#pragma once

#include "defines.h"

struct game;

// Application configuration.
typedef struct application_config{
    // Window starting x axis, if applicable.
    i16 start_pos_x;
    // Window starting y axis, if applicable.
    i16 start_pos_y;
    // Window starting width, if applicable.
    i16 start_width;
    // Window starting height, if applicable.
    i16 start_height;
    // Application name used in windowing, if applicable.
    const char* name;
} application_config;

ibx__api__ b8 application_init(game* game_inst);

ibx__api__ b8 application_run();