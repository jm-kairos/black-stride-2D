#include "game.h"

#include <core/logger.h>

b8 game_init(game* game_inst){
    IBX_LOG_DEBUG("game_init was called !")
    return TRUE;
}

b8 game_update(game* game_inst, f32 dt){
    IBX_LOG_DEBUG("game_update was called !")
    return TRUE;
}

b8 game_render(game* game_inst, f32 dt){
    IBX_LOG_DEBUG("game_render was called !");
    return TRUE;
}

void game_on_resize(game* game_inst, u32 width, u32 height){
    IBX_LOG_DEBUG("game_on_resize was called !");
}