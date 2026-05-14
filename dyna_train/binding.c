#include "dyna_train.h"
#define OBS_SIZE OBS_DIM
#define NUM_ATNS 2
#define ACT_SIZES {1, 1}              // 2 continuous action dims
#define OBS_TENSOR_T FloatTensor

#define Env DynaTrain
#include "vecenv.h"

void my_init(Env* env, Dict* kwargs) {
    env->num_agents = 1;
    env->arena_size         = dict_get(kwargs, "arena_size")->value;
    env->max_steps          = (int)dict_get(kwargs, "max_steps")->value;
    env->dt                 = dict_get(kwargs, "dt")->value;
    env->num_obstacles_min  = (int)dict_get(kwargs, "num_obstacles_min")->value;
    env->num_obstacles_max  = (int)dict_get(kwargs, "num_obstacles_max")->value;
    env->order_min          = (int)dict_get(kwargs, "order_min")->value;
    env->order_max          = (int)dict_get(kwargs, "order_max")->value;
    env->speed_min          = dict_get(kwargs, "speed_min")->value;
    env->speed_max          = dict_get(kwargs, "speed_max")->value;
    env->std_min            = dict_get(kwargs, "std_min")->value;
    env->std_max            = dict_get(kwargs, "std_max")->value;
    env->min_init_goal_dist = dict_get(kwargs, "min_init_goal_dist")->value;
    env->gamma_d            = dict_get(kwargs, "gamma_d")->value;
    env->beta               = dict_get(kwargs, "beta")->value;
    env->sigma_o            = dict_get(kwargs, "sigma_o")->value;
    env->success_bonus      = dict_get(kwargs, "success_bonus")->value;
    env->collision_penalty  = dict_get(kwargs, "collision_penalty")->value;
    env->goal_radius        = dict_get(kwargs, "goal_radius")->value;
    env->mw_poly            = (int)dict_get(kwargs, "mw_poly")->value;
    env->mw_linear          = (int)dict_get(kwargs, "mw_linear")->value;
    env->mw_reciprocating   = (int)dict_get(kwargs, "mw_reciprocating")->value;
    env->mw_sinusoidal      = (int)dict_get(kwargs, "mw_sinusoidal")->value;
    env->mw_random_walk     = (int)dict_get(kwargs, "mw_random_walk")->value;
    env->mw_stationary      = (int)dict_get(kwargs, "mw_stationary")->value;
    env->amp_max            = dict_get(kwargs, "amp_max")->value;
    env->freq_min           = dict_get(kwargs, "freq_min")->value;
    env->freq_max           = dict_get(kwargs, "freq_max")->value;
    env->walk_step_std      = dict_get(kwargs, "walk_step_std")->value;
    env->reciprocate_min_dist = dict_get(kwargs, "reciprocate_min_dist")->value;
    env->train_v_max        = dict_get(kwargs, "train_v_max")->value;
    env->train_v_min        = dict_get(kwargs, "train_v_min")->value;
    env->train_w_max        = dict_get(kwargs, "train_w_max")->value;
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
