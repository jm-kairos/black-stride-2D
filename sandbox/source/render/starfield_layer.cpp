#include "render/starfield_layer.h"
#include "game.h"
#include "core/view_transform.h" // game_camera_center_hierpos
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
using namespace bs_math;
// =====================================================================================
StarfieldLayer::StarfieldLayer(u32 _id, f32 _parallax, f32 _zoom_scale, u32 _seed)
    : id(_id), parallax(_parallax), zoom_scale(_zoom_scale), is_custom_gpu(TRUE),  // custom GPU path
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
    params.density    = gs ? gs->render.starfield_lod_density    : 0.06f;
    params.size_mul   = gs ? gs->render.starfield_lod_size       : 1.0f;
    params.brightness_mul = gs ? gs->render.starfield_lod_brightness : 1.0f;

    // ---- Virtual-quadtree LOD ----------------------------------------------------------
    // Pass the camera center as a precise HierPos2 split (integer cell + local offset) so the
    // shader can reduce sample coordinates per LOD level without f32 snapping far from origin.
    // The field is now continuous across the whole zoom range (arena AND galaxy map): the old
    // `brightness_mul *= view_arena_w` kill-gate is removed and LOD weighting drives visibility.
    if (gs) {
        bs_math::HierPos2 cam_hp = game_camera_center_hierpos(gs);
        params.cam_cell  = bs_math::Vec2{ (f32)cam_hp.cell.x, (f32)cam_hp.cell.y };
        params.cam_local = cam_hp.local;
    } else {
        params.cam_cell  = bs_math::Vec2{ 0.0f, 0.0f };
        params.cam_local = bs_math::Vec2{ 0.0f, 0.0f };
    }
    params.base_cell   = 64.0f;   // finest LOD cell size (world units) — fixed: base_cell*256 == HierPos cell
    params.lod_factor  = gs ? gs->render.starfield_lod_factor    : 4.0f;    // quadtree: each level 4x coarser
    params.target_px   = gs ? gs->render.starfield_lod_target_px : 40.0f;   // desired on-screen cell size for LOD selection
    params.lod_levels  = gs ? gs->render.starfield_lod_levels    : 4.0f;    // active LOD slots accumulated per pixel
    params.parallax_near    = gs ? gs->render.starfield_parallax_near    : 0.5f;  // finest-level scroll speed (0..1)
    params.parallax_falloff = gs ? gs->render.starfield_parallax_falloff : 0.75f; // coarser levels scroll slower

    // Star dazzle: gs->star_pos is already the hero star's render-space offset from the camera
    // center, which matches the shader's per-pixel `offset` frame, so pass it directly.
    if (gs) {
        params.star_rel = gs->star_pos;
        params.dazzle_inner     = gs->render.star_dazzle_inner_radius;
        params.dazzle_outer     = gs->render.star_dazzle_outer_radius;
        params.dazzle_intensity = gs->render.star_dazzle_intensity;
    } else {
        params.star_rel = bs_math::Vec2{0,0};
        params.dazzle_inner = 0.0f;
        params.dazzle_outer = 0.0f;
        params.dazzle_intensity = 0.0f;
    }
    renderer_draw_starfield(&params);
}
