#include "dyna_eval.h"
#define OBS_SIZE OBS_DIM
#define NUM_ATNS 2
#define ACT_SIZES {1, 1}
#define OBS_TENSOR_T FloatTensor

#define Env DynaEval
#include "vecenv.h"

void my_init(Env* env, Dict* kwargs) {
    env->num_agents = 1;
    env->arena_size        = dict_get(kwargs, "arena_size")->value;
    env->max_steps         = (int)dict_get(kwargs, "max_steps")->value;
    env->dt                = dict_get(kwargs, "dt")->value;
    env->difficulty        = (int)dict_get(kwargs, "difficulty")->value;
    env->world_seed_base   = (int)dict_get(kwargs, "world_seed_base")->value;
    env->gamma_d           = dict_get(kwargs, "gamma_d")->value;
    env->beta              = dict_get(kwargs, "beta")->value;
    env->sigma_o           = dict_get(kwargs, "sigma_o")->value;
    env->success_bonus     = dict_get(kwargs, "success_bonus")->value;
    env->collision_penalty = dict_get(kwargs, "collision_penalty")->value;
    env->goal_radius       = dict_get(kwargs, "goal_radius")->value;
    env->open_front        = (int)dict_get(kwargs, "open_front")->value;
    env->v_max_clip        = dict_get(kwargs, "v_max_clip")->value;
    env->goal_box_half     = dict_get(kwargs, "goal_box_half")->value;
    env->a_max             = dict_get(kwargs, "a_max")->value;
    env->alpha_max         = dict_get(kwargs, "alpha_max")->value;
    init(env);
}

void my_log(Log* log, Dict* out) {
    dict_set(out, "perf", log->perf);
    dict_set(out, "score", log->score);
    dict_set(out, "episode_return", log->episode_return);
    dict_set(out, "episode_length", log->episode_length);
    dict_set(out, "success", log->success);
    dict_set(out, "collision", log->collision);
    dict_set(out, "timeout", log->timeout);
    dict_set(out, "n", log->n);
}
