#include "core/cursor_world.h"
#include "game.h"
#include "core/view_transform.h" // game_screen_to_true_world / game_screen_to_true_hierpos
#include <core/input.h>          // input_get_mouse_position
#include <renderer/camera2d.h>   // camera2d_screen_to_world

using namespace bs_math;

// World-space position under the mouse cursor.
Vec2 mouse_world(const game_state* s) {
    i32 mx, my;
    input_get_mouse_position(&mx, &my);
    return camera2d_screen_to_world(&s->camera_state.camera, s->fb_width, s->fb_height, Vec2{ (f32)mx, (f32)my });
}

// True (simulation) world position under the mouse cursor. Equals mouse_world in the legacy
// global path (render space == true world); under the unified path it inverts the compression /
// camera_hierpos transform so callers that hit-test against true-world entity positions (edit
// picking, piloting/aiming) stay correct.
Vec2 mouse_true_world(const game_state* s) {
    i32 mx, my;
    input_get_mouse_position(&mx, &my);
    return game_screen_to_true_world(s, Vec2{ (f32)mx, (f32)my });
}

// HierPos2 (precision-safe) cursor position in true world space.
HierPos2 mouse_true_hierpos(const game_state* s) {
    i32 mx, my;
    input_get_mouse_position(&mx, &my);
    return game_screen_to_true_hierpos(s, Vec2{ (f32)mx, (f32)my });
}
