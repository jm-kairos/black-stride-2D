#include "starfield_layer.h"
#include "game.h"
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
using namespace bs_math;
// =====================================================================================
StarfieldLayer::StarfieldLayer(u32 _id, f32 _parallax, f32 _zoom_scale, u32 _seed)
    : ParallaxLayer(_id, _parallax, _zoom_scale, TRUE),  // custom GPU path
      seed(_seed)
{
}
// =====================================================================================
void StarfieldLayer::draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
                          f32 dt, f32 elapsed_time,
                          const bs_math::Vec2& blur)
{
    (void)dt;
    (void)elapsed_time;
    (void)blur;
    bs_starfield_params params{};
    params.cam        = cam;
    params.zoom_scale = zoom_scale;
    params.layer_id   = id;
    params.seed       = seed;
    params.fb_w       = fb_w;
    params.fb_h       = fb_h;
    params.layer_data = nullptr; // procedural path
    params.pass_index = 0;
    params.density    = gs ? gs->starfield_lod_density    : 0.06f;
    params.size_mul   = gs ? gs->starfield_lod_size       : 1.0f;
    params.brightness_mul = gs ? gs->starfield_lod_brightness : 1.0f;
    // Star dazzle: transform real star position into this layer's parallax shader space.
    // The shader world is: layer_world = cam.position + (screen - center) / (zoom * zoom_scale),
    // so the star's layer-world position is cam.position + (star_world - real_cam.position) / zoom_scale.
    if (gs && parallax > 0.0001f && zoom_scale > 0.0001f) {
        Vec2 real_cam_pos = vec2_scale(cam.position, 1.0f / parallax);
        params.star_pos = vec2_add(cam.position,
                                   vec2_scale(vec2_sub(gs->star_pos, real_cam_pos),
                                              1.0f / zoom_scale));
        params.dazzle_inner     = gs->star_dazzle_inner_radius;
        params.dazzle_outer     = gs->star_dazzle_outer_radius;
        params.dazzle_intensity = gs->star_dazzle_intensity;
    } else {
        params.star_pos = bs_math::Vec2{0,0};
        params.dazzle_inner = 0.0f;
        params.dazzle_outer = 0.0f;
        params.dazzle_intensity = 0.0f;
    }
    renderer_draw_starfield(&params);
}
