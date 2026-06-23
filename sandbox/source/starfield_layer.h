#pragma once
#include "parallax_layer.h"
struct game_state; // forward declaration for editor tunables
// =====================================================================================
// Procedural starfield layer — single fullscreen shader draw per layer.
// No CPU star generation, no VBO upload, no tile culling.
// =====================================================================================
struct StarfieldLayer : ParallaxLayer {
    u32  seed;
    game_state* gs = nullptr; // for editor panel tunables
    StarfieldLayer(u32 _id, f32 _parallax, f32 _zoom_scale, u32 _seed);
    void set_game_state(game_state* s) { gs = s; }
    void draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
              f32 dt, f32 elapsed_time,
              const bs_math::Vec2& blur = bs_math::Vec2{0,0}) override;
};
