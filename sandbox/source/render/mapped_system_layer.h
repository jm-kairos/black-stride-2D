#pragma once
#include <defines.h>
#include <math/math_utils.h>
struct Camera2D;
struct StarSystem;
struct StarFxSystem;
struct game_state;
// =====================================================================================
// Renders the current star system (star + planets) as a background layer in MODE_GLOBAL.
// Reads live StarSystem data — no copy, always synchronized with System Mode.
// =====================================================================================
struct MappedSystemLayer {
    u32 id;
    f32 parallax;
    f32 zoom_scale;
    b8  is_custom_gpu;
    game_state* gs;
    StarFxSystem* star_fx;
    i32 current_system;
    MappedSystemLayer(game_state* _gs, StarFxSystem* _star_fx);
    void on_system_changed(i32 system_idx);
    void draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
              f32 dt, f32 elapsed_time,
              const bs_math::Vec2& blur = bs_math::Vec2{0,0});
};
