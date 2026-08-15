#pragma once

#include "defines.h"

struct Game;

// Application configuration.
struct ApplicationConfig{
    // Window starting x axis, if applicable.
    i16 start_pos_x;
    // Window starting y axis, if applicable.
    i16 start_pos_y;
    // Window starting width, if applicable.
    i16 start_width;
    // Window starting height, if applicable.
    i16 start_height;
    // Start as a borderless fullscreen window at the desktop resolution. start_pos/size then
    // only describe the windowed-mode geometry the window would fall back to.
    b8 fullscreen;
    // Application name used in windowing, if applicable.
    const char* name;
};

bs__api__ b8 application_init(Game* game_inst);

bs__api__ b8 application_run();