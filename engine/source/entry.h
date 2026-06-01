#pragma once

#include "core/application.h"
#include "core/logger.h"
#include "core/memory/ibx_memory.h"
#include "game_types.h"

// Externally-defined function to create a game
// The engine dll does not know how this function is defined.
// It just knows that it is defined externally.
extern b8 game_create(game* out_game);

/**
 * The main entry point of the application. 
 */
int main(void){

    ibx_memory_initialize();

    game game_inst;
    if (!game_create(&game_inst))
    {
        IBX_LOG_FATAL("Could not create game !");
        return -1;
    }
    
    if (!game_inst.init || !game_inst.update || !game_inst.render || !game_inst.on_resize)
    {
        IBX_LOG_FATAL("The game's function pointers must be assigned !");
        return -2;
    }

    // Initialization.
    if(!application_init(&game_inst)){
        IBX_LOG_INFO("Application failed to create !")
        return 1;
    };

    // Begin the game loop.
    if(!application_run()){
        IBX_LOG_INFO("Application did not terminate correctly !")
        return 2; 
    }

    ibx_memory_terminate();

    return 0;
}