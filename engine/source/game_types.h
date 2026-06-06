#pragma once

#include "core/application.h"

/**
 * Represents the basic game state in a game.
 * Called for creation by the application.
 */
struct Game {
    ApplicationConfig app_config;
    b8 (*init)(Game* game_inst);
    b8 (*update)(Game* game_inst, f32 dt);
    b8 (*render)(Game* game_inst, f32 dt);
    void (*on_resize)(Game* game_inst, u32 width, u32 height);
    // Game-specific game state. Created and managed by the game.
    VOID_PTR state; 
};

