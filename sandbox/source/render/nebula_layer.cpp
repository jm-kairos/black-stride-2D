#include "render/nebula_layer.h"
#include "game.h"
#include "core/view_transform.h" // game_camera_center_hierpos
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
using namespace bs_math;
// =====================================================================================
NebulaLayer::NebulaLayer(u32 _id, f32 _parallax, f32 _zoom_scale, u32 _seed)
    : id(_id), parallax(_parallax), zoom_scale(_zoom_scale), is_custom_gpu(TRUE),  // custom GPU path
      seed(_seed)
{
}
// =====================================================================================
void NebulaLayer::draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
                       f32 dt, f32 elapsed_time,
                       const bs_math::Vec2& blur)
{
    (void)dt;
    (void)elapsed_time;
    (void)blur;
    bs_nebula_params params{};
    params.cam        = cam;
    params.zoom_scale = zoom_scale;
    params.layer_id   = id;
    params.seed       = seed;
    params.fb_w       = fb_w;
    params.fb_h       = fb_h;
    params.intensity     = gs ? gs->render.nebula_intensity     : 0.5f;
    params.dust_intensity = gs ? gs->render.nebula_dust_intensity : 0.4f;
    // Continuous across the whole zoom range (arena AND galaxy map): the LOD-driven base
    // frequency keeps cloud detail resolved at every scale, so the old `*= view_arena_w`
    // kill-gate is removed. Pass the camera center as a precise HierPos2 split so the shader
    // can reduce sample coordinates without f32 snapping far from the origin.
    if (gs) {
        bs_math::HierPos2 cam_hp = game_camera_center_hierpos(gs);
        params.cam_cell  = bs_math::Vec2{ (f32)cam_hp.cell.x, (f32)cam_hp.cell.y };
        params.cam_local = cam_hp.local;
    } else {
        params.cam_cell  = bs_math::Vec2{ 0.0f, 0.0f };
        params.cam_local = bs_math::Vec2{ 0.0f, 0.0f };
    }
    params.gas_color_a      = gs ? gs->render.nebula_gas_color_a : bs_color{0.05f, 0.20f, 0.40f, 1.0f};
    params.gas_color_b      = gs ? gs->render.nebula_gas_color_b : bs_color{0.10f, 0.60f, 0.35f, 1.0f};
    params.gas_color_c      = gs ? gs->render.nebula_gas_color_c : bs_color{0.85f, 0.75f, 0.15f, 1.0f};
    params.dust_color       = gs ? gs->render.nebula_dust_color  : bs_color{0.02f, 0.015f, 0.015f, 1.0f};
    params.gas_brightness_mul = gs ? gs->render.nebula_gas_brightness_mul : 1.0f;
    params.highlight_power    = gs ? gs->render.nebula_highlight_power    : 1.0f;
    params.palette_shift      = gs ? gs->render.nebula_palette_shift      : 0.0f;
    params.swirl_strength     = gs ? gs->render.nebula_swirl_strength     : 1.0f;
    params.falloff_radius     = gs ? gs->render.nebula_falloff_radius     : 2.0f;
    params.band_strength      = gs ? gs->render.nebula_band_strength      : 0.5f;
    params.lod_target         = gs ? gs->render.nebula_lod_target         : 3500.0f;
    params.parallax           = gs ? gs->render.nebula_parallax           : 0.08f;
    params.biome_strength     = gs ? gs->render.nebula_biome_strength     : 0.6f;
    params.biome_scale        = gs ? gs->render.nebula_biome_scale        : 55000.0f;
    params.biome_hue_spread   = gs ? gs->render.nebula_biome_hue_spread   : 0.5f;
    params.zoom_detail        = gs ? gs->render.nebula_zoom_detail        : 0.6f;
    params.zoom_saturation    = gs ? gs->render.nebula_zoom_saturation    : 0.5f;
    renderer_draw_nebula(&params);
}
