#pragma once
#include <defines.h>
#include <math/math_utils.h>
struct Camera2D;
struct game_state; // forward declaration for editor tunables
// =====================================================================================
// Procedural starfield layer — single fullscreen shader draw per layer.
// No CPU star generation, no VBO upload, no tile culling.
// =====================================================================================
struct StarfieldLayer {
    u32  id;
    f32  parallax;
    f32  zoom_scale;
    b8   is_custom_gpu;
    u32  seed;
    game_state* gs = nullptr; // for editor panel tunables
    StarfieldLayer(u32 _id, f32 _parallax, f32 _zoom_scale, u32 _seed);
    void set_game_state(game_state* s) { gs = s; }
    void draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
              f32 dt, f32 elapsed_time,
              const bs_math::Vec2& blur = bs_math::Vec2{0,0});
};
