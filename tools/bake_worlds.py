#!/usr/bin/env python3
"""Bake DynaBARN .world + plugin .so files into a flat per-world binary that
our dyna_eval C-side env can mmap-load.

The plugins are shipped as compiled .so files (no .cc source available). Each
plugin's Load() function calls CreateKeyFrame(time) followed by Translation(
Vector3d(x, y, 0)) for each waypoint. We extract those (time, x, y) values by
disassembling the .so and walking the call sequence:

  movsd ADDR(%rip),%xmm0     ; load time
  call CreateKeyFrame@plt
  ...
  movsd ADDR(%rip),%xmm0     ; load x
  movsd %xmm0, -OFF(%rbp)
  movsd ADDR(%rip),%xmm0     ; load y
  movsd %xmm0, -OFF(%rbp)
  ...
  call Translation@plt

ADDR is the rip-relative comment objdump prints (e.g. "# 0x170e8"). We map
that virtual address to a file offset via readelf and read the 8 bytes there
as a little-endian double.

Output binary layout (per-world file, all little-endian):

  uint32  magic       = 0x44425257 ("DBRW")
  uint32  version     = 1
  uint32  n_obstacles
  for each obstacle:
      uint32 n_waypoints
      for each waypoint:
          float32 t
          float32 x
          float32 y
"""
from __future__ import annotations

import argparse
import json
import re
import shutil
import struct
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


MAGIC = 0x44425257   # 'DBRW' (little-endian → bytes "WRBD" when written; doesn't matter as long as we match in C)
VERSION = 1


@dataclass
class Waypoint:
    t: float
    x: float
    y: float


@dataclass
class Trajectory:
    waypoints: list[Waypoint]


@dataclass
class World:
    name: str
    plugins: list[str]              # plugin .so filenames in SDF order
    trajectories: list[Trajectory]


# ----------------------------------------------------------------------------
# ELF helpers
# ----------------------------------------------------------------------------
def section_va_to_offset(so_path: Path) -> list[tuple[int, int, int]]:
    """Return list of (vaddr, file_off, size) per loaded section."""
    out = subprocess.check_output(["readelf", "-WS", str(so_path)]).decode()
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+\S+\s+\S+\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            va = int(m.group(1), 16)
            off = int(m.group(2), 16)
            sz = int(m.group(3), 16)
            rows.append((va, off, sz))
    return rows


def va_to_file_offset(sections: list[tuple[int, int, int]], va: int) -> int:
    for va_lo, off, sz in sections:
        if va_lo <= va < va_lo + sz:
            return off + (va - va_lo)
    raise ValueError(f"va 0x{va:x} not in any section")


def read_double_at(blob: bytes, file_off: int) -> float:
    return struct.unpack_from("<d", blob, file_off)[0]


# ----------------------------------------------------------------------------
# Disassembly scan
# ----------------------------------------------------------------------------

# Match a double-typed load from rip-relative memory. Two forms occur:
#   movsd  0x6ca3(%rip),%xmm0   # 170e8 <symbol>
#   mov    0x4242(%rip),%rax    # 17018 <symbol>
# Both load 8 bytes of immediate data; we extract the virtual-address comment.
DOUBLE_LOAD_RE = re.compile(
    r"^\s*([0-9a-f]+):\s+.*\b(?:movsd|mov)\s+0x[0-9a-f]+\(%rip\),%(?:xmm0|rax)\s+#\s+([0-9a-f]+)\b"
)
# Calls into the PLT for our two targets
CALL_RE = re.compile(r"^\s*([0-9a-f]+):\s+.*call\s+[0-9a-f]+\s+<(.+)>\s*$")
CKF_SYMBOL = "_ZN6gazebo6common13PoseAnimation14CreateKeyFrameEd@plt"
TRANS_SYMBOL = "_ZN6gazebo6common12PoseKeyFrame11TranslationERKN8ignition4math2v47Vector3IdEE@plt"


def extract_waypoints(so_path: Path) -> Trajectory:
    blob = so_path.read_bytes()
    sections = section_va_to_offset(so_path)
    disasm = subprocess.check_output(
        ["objdump", "-d", "--no-show-raw-insn", str(so_path)],
        # --no-show-raw-insn doesn't exist in old binutils; fall back if needed
    ).decode("utf-8", errors="replace") if shutil.which("objdump") else ""
    # The --no-show-raw-insn variant may not be supported; fallback re-runs:
    if not disasm:
        disasm = subprocess.check_output(["objdump", "-d", str(so_path)]).decode()

    waypoints: list[Waypoint] = []
    # State machine. Each waypoint consists of (per generator output):
    #   1. load `time` into xmm0  (movsd ADDR(rip), or mov ADDR(rip) + restore, or pxor for t=0)
    #   2. call CreateKeyFrame
    #   3. load `x` from a rip-relative const into xmm0
    #   4. load `y` from a rip-relative const into xmm0
    #   5. call Translation
    # We track the immediates loaded between calls.
    last_immediate: float | None = None     # most-recent scalar load
    xy_buf: list[float] = []                # rip-relative loads since last call
    pending_time: float | None = None
    pxor_recent = False                     # whether xmm0 was just zeroed
    for line in disasm.splitlines():
        # pxor %xmm0,%xmm0  ⇒ sets xmm0 = 0.0
        if "pxor   %xmm0,%xmm0" in line or "pxor %xmm0,%xmm0" in line:
            last_immediate = 0.0
            pxor_recent = True
            continue
        mm = DOUBLE_LOAD_RE.match(line)
        if mm:
            va = int(mm.group(2), 16)
            try:
                off = va_to_file_offset(sections, va)
                val = read_double_at(blob, off)
            except (ValueError, struct.error):
                continue
            last_immediate = val
            xy_buf.append(val)
            if len(xy_buf) > 4:
                xy_buf = xy_buf[-4:]
            pxor_recent = False
            continue
        mc = CALL_RE.match(line)
        if not mc:
            continue
        symbol = mc.group(2)
        if symbol == CKF_SYMBOL:
            # Time is what was in xmm0 at the call site.
            pending_time = last_immediate if last_immediate is not None else 0.0
            xy_buf = []        # reset; x/y come AFTER this call
            pxor_recent = False
            continue
        if symbol == TRANS_SYMBOL:
            if pending_time is None or len(xy_buf) < 2:
                pending_time = None
                xy_buf = []
                continue
            x = xy_buf[-2]
            y = xy_buf[-1]
            waypoints.append(Waypoint(t=float(pending_time), x=float(x), y=float(y)))
            pending_time = None
            xy_buf = []
            continue
    return Trajectory(waypoints=waypoints)


# ----------------------------------------------------------------------------
# SDF parsing
# ----------------------------------------------------------------------------
PLUGIN_RE = re.compile(r"<plugin\s+[^>]*filename=['\"]([^'\"]+)['\"]")


def parse_world(world_path: Path) -> list[str]:
    """Return the list of plugin .so filenames in SDF order."""
    text = world_path.read_text()
    return PLUGIN_RE.findall(text)


# ----------------------------------------------------------------------------
# Binary write
# ----------------------------------------------------------------------------
def write_world(out_path: Path, world: World) -> None:
    with out_path.open("wb") as f:
        f.write(struct.pack("<III", MAGIC, VERSION, len(world.trajectories)))
        for traj in world.trajectories:
            f.write(struct.pack("<I", len(traj.waypoints)))
            for wp in traj.waypoints:
                f.write(struct.pack("<fff", wp.t, wp.x, wp.y))


# ----------------------------------------------------------------------------
# Driver
# ----------------------------------------------------------------------------
def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--worlds-dir", required=True,
                   help="DynaBARN_worlds_60/ directory")
    p.add_argument("--plugins-dir", required=True,
                   help="all_cylinder_plugins/ directory")
    p.add_argument("--out-dir", required=True, help="output dir for baked worlds")
    p.add_argument("--limit", type=int, default=0,
                   help="bake only the first N worlds (0 = all)")
    p.add_argument("--report", default=None,
                   help="write JSON summary to this path")
    args = p.parse_args()

    worlds_dir = Path(args.worlds_dir)
    plugins_dir = Path(args.plugins_dir)
    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    # Cache extracted trajectories by plugin name so we don't re-disassemble.
    traj_cache: dict[str, Trajectory] = {}

    world_files = sorted(worlds_dir.glob("world_*.world"),
                         key=lambda p: int(re.search(r"world_(\d+)\.world", p.name).group(1)))
    if args.limit:
        world_files = world_files[:args.limit]

    summary = {"worlds": [], "errors": []}
    for wp_path in world_files:
        idx = int(re.search(r"world_(\d+)\.world", wp_path.name).group(1))
        plugins = parse_world(wp_path)
        trajs: list[Trajectory] = []
        for plugin_name in plugins:
            if plugin_name not in traj_cache:
                so_path = plugins_dir / plugin_name
                if not so_path.exists():
                    summary["errors"].append(f"missing plugin: {so_path}")
                    traj_cache[plugin_name] = Trajectory(waypoints=[])
                else:
                    traj_cache[plugin_name] = extract_waypoints(so_path)
            trajs.append(traj_cache[plugin_name])
        world = World(name=wp_path.stem, plugins=plugins, trajectories=trajs)
        out_path = out_dir / f"world_{idx:03d}.bin"
        write_world(out_path, world)

        wp_counts = [len(t.waypoints) for t in trajs]
        max_t = max((t.waypoints[-1].t for t in trajs if t.waypoints), default=0.0)
        all_xy = [(wp.x, wp.y) for t in trajs for wp in t.waypoints]
        xs = [v[0] for v in all_xy] if all_xy else [0.0]
        ys = [v[1] for v in all_xy] if all_xy else [0.0]
        summary["worlds"].append({
            "idx": idx,
            "name": wp_path.stem,
            "out": str(out_path),
            "n_obstacles": len(plugins),
            "n_waypoints_total": sum(wp_counts),
            "n_waypoints_per_obs_min": min(wp_counts) if wp_counts else 0,
            "n_waypoints_per_obs_max": max(wp_counts) if wp_counts else 0,
            "max_t": max_t,
            "x_min": min(xs), "x_max": max(xs),
            "y_min": min(ys), "y_max": max(ys),
        })
        print(f"world_{idx:03d}: {len(plugins)} obstacles, "
              f"{sum(wp_counts)} total waypoints, "
              f"max_t={max_t:.1f}s, x∈[{min(xs):.1f},{max(xs):.1f}], "
              f"y∈[{min(ys):.1f},{max(ys):.1f}]")

    if args.report:
        Path(args.report).write_text(json.dumps(summary, indent=2))
        print(f"\nWrote report: {args.report}")
    if summary["errors"]:
        print(f"\n{len(summary['errors'])} errors:", file=sys.stderr)
        for e in summary["errors"][:10]:
            print("  " + e, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
