#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include <renderer/renderer_types.h>
// =====================================================================================
// Base struct for a parallax background layer in MODE_GLOBAL.
// Each layer has a parallax multiplier (0.0 = screen-locked, 1.0 = world-locked)
// and an optional zoom_scale for depth-cue size modulation.
// =====================================================================================
struct ParallaxLayer {
    u32  id;            // render order (lower = behind)
    f32  parallax;      // 0.0 = fixed to screen, 1.0 = locked to world
    f32  zoom_scale;    // additional size multiplier for depth cue (1.0 = no effect)
    b8   is_custom_gpu; // TRUE if this layer bypasses the sprite batch
    ParallaxLayer(u32 _id, f32 _parallax, f32 _zoom_scale, b8 _custom)
        : id(_id), parallax(_parallax), zoom_scale(_zoom_scale), is_custom_gpu(_custom) {}
    virtual void draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
                      f32 dt, f32 elapsed_time,
                      const bs_math::Vec2& blur = bs_math::Vec2{0,0}) = 0;
    virtual ~ParallaxLayer() = default;
};
