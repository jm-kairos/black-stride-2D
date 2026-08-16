#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
struct StarfieldLayer;
struct NebulaLayer;
struct MappedSystemLayer;
struct StarFxSystem;
struct game_state;
// =====================================================================================
// Manages all parallax background layers for MODE_GLOBAL.
// Owns the layer stack and draws them back-to-front each frame.
// =====================================================================================
struct GlobalBackground {
    StarfieldLayer*    starfield;
    NebulaLayer*       nebula;
    MappedSystemLayer* mapped_system;
    game_state*        gs; // retained for velocity-based motion blur
    void init(game_state* gs, StarFxSystem* star_fx);
    void shutdown();
    void draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
              f32 dt, f32 elapsed_time);
    // Notify the mapped-system layer that the active system changed. Lets sim/ callers trigger
    // the render-side hand-off without depending on the concrete MappedSystemLayer type.
    void notify_system_changed(i32 system_idx);
};
