#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
struct ParallaxLayer;
struct StarFxSystem;
struct game_state;
// =====================================================================================
// Manages all parallax background layers for MODE_GLOBAL.
// Owns the layer stack and draws them back-to-front each frame.
// =====================================================================================
struct GlobalBackground {
    ParallaxLayer* layers[4];
    i32            layer_count;
    game_state*    gs; // retained for velocity-based motion blur
    void init(game_state* gs, StarFxSystem* star_fx);
    void shutdown();
    void draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
              f32 dt, f32 elapsed_time);
};
