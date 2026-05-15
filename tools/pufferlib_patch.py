"""Encoder classes for our DynaBARN costmap observation, appended to
PufferLib's pufferlib/models.py.

Why patch puffer's file rather than ship our own module: load_policy uses
`getattr(pufferlib.models, args['torch']['encoder'])` so the class must
live there.

Re-apply this patch after any PufferLib upgrade. To install:

  docker exec puffertank bash -c "
    cat /puffertank/host/dyna_barn/tools/pufferlib_patch.py \\
        >> /puffertank/pufferlib/pufferlib/models.py
  "

The marker line below lets us check whether the patch is already applied
before re-appending (idempotent install via grep).
"""

# ───── DYNA_BARN PATCH BEGIN ─────  (do not remove this marker line)
import torch
import torch.nn as nn

class CostmapEncoder(nn.Module):
    """Encoder for our flat-packed costmap observation.

    Obs layout (flat, last dim):
        [costmap (G*G floats) | v | w | goal_dx_body | goal_dy_body]
    where G = 64 by default.

    Reshapes the costmap into (B, 1, G, G), runs a small conv stack, and
    concatenates with the (v, w, goal) extras before a final MLP.
    """

    def __init__(self, obs_size, hidden_size=128, grid=64, n_extras=4):
        super().__init__()
        self.grid = grid
        self.n_extras = n_extras
        self.grid_dim = grid * grid
        assert obs_size == self.grid_dim + n_extras, (
            f"CostmapEncoder: obs_size={obs_size} != grid({grid}^2)+extras({n_extras})="
            f"{self.grid_dim + n_extras}"
        )
        # Small CNN — three strided convs collapse 64×64 → 8×8, then flatten.
        self.conv = nn.Sequential(
            nn.Conv2d(1,  16, kernel_size=5, stride=2, padding=2),  # 64 → 32
            nn.ReLU(),
            nn.Conv2d(16, 32, kernel_size=3, stride=2, padding=1),  # 32 → 16
            nn.ReLU(),
            nn.Conv2d(32, 64, kernel_size=3, stride=2, padding=1),  # 16 →  8
            nn.ReLU(),
            nn.Flatten(),
        )
        conv_out = 64 * 8 * 8
        self.fc = nn.Sequential(
            nn.Linear(conv_out + n_extras, hidden_size),
            nn.ReLU(),
        )

    def forward(self, observations):
        # observations: (B, G*G + n_extras), float32
        B = observations.shape[0]
        grid = observations[:, :self.grid_dim].view(B, 1, self.grid, self.grid)
        extras = observations[:, self.grid_dim:]
        z = self.conv(grid)
        z = torch.cat([z, extras], dim=-1)
        return self.fc(z)


class CostmapEncoder64(CostmapEncoder):
    """Alias for default 64×64 + 4-extras layout (our COSTMAP_SIZE + OBS_EXTRA)."""
    def __init__(self, obs_size, hidden_size=128):
        super().__init__(obs_size, hidden_size, grid=64, n_extras=4)
# ───── DYNA_BARN PATCH END ─────
