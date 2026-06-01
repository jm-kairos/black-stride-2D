#include "game.h"

#include <entry.h>

#include <core/memory/ibx_memory.h>

// Define the function to create a game
b8 game_create(game* out_game){

    // Application configuration
    out_game->app_config.name = "Ibex Engine Sandbox";
    out_game->app_config.start_pos_x = 100;
    out_game->app_config.start_pos_y = 100;
    out_game->app_config.start_width = 1280;
    out_game->app_config.start_height = 720;
    out_game->init = game_init;
    out_game->update = game_update;
    out_game->render = game_render;
    out_game->on_resize = game_on_resize;

    // Create the game state.
    out_game->state = ibx_memory_allocator(sizeof(game_state), EMemoryTag::MEMORY_TAG_GAME);

    return TRUE;
}