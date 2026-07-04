#include "global_background.h"
#include "mapped_system_layer.h"
#include "nebula_layer.h"
#include "starfield_layer.h"
#include "starfield_generator.h"
#include "game.h"
#include "star_fx.h"
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
#include <core/logger.h>
#include <math.h>
#include <stdlib.h>
// =====================================================================================
void GlobalBackground::init(game_state* gs, StarFxSystem* star_fx)
{
    this->gs = gs;
    // Starfield: single virtual-quadtree starfield (procedural multi-LOD shader). World-locked
    // (parallax 1.0, zoom_scale 1.0) — the LOD levels themselves provide apparent depth, so the
    // old far/mid parallax pair is collapsed into this one continuous field that renders across
    // BOTH the arena and galaxy-map zoom ranges.
    starfield = new StarfieldLayer(0, 1.0f, 1.0f, 0xDEADBEEFu);
    starfield->set_game_state(gs);
    // Nebula: alpha-blended nebula/dust clouds (parallax 0.02, zoom_scale 0.90)
    nebula = new NebulaLayer(1, 0.02f, 0.90f, 0xBADDCAFEu);
    nebula->set_game_state(gs);
    // Mapped system star + planets
    mapped_system = new MappedSystemLayer(gs, star_fx);
}
// =====================================================================================
void GlobalBackground::shutdown()
{
    delete starfield;      starfield = nullptr;
    delete nebula;         nebula = nullptr;
    delete mapped_system;  mapped_system = nullptr;
}
// =====================================================================================
void GlobalBackground::notify_system_changed(i32 system_idx)
{
    if (mapped_system) mapped_system->on_system_changed(system_idx);
}
// =====================================================================================
template <typename LayerT>
static void draw_bg_layer(LayerT* layer, const Camera2D& cam, u16 fb_w, u16 fb_h,
                          f32 dt, f32 elapsed_time, const bs_math::Vec2& baseBlur)
{
    // Compute virtual camera for this layer.
    Camera2D layer_cam = cam;
    layer_cam.position.x = cam.position.x * layer->parallax;
    layer_cam.position.y = cam.position.y * layer->parallax;
    layer_cam.zoom *= layer->zoom_scale;
    // Blur is stronger for distant (low-parallax) layers.
    float parallaxFactor = 1.0f - layer->parallax;
    bs_math::Vec2 layerBlur = bs_math::Vec2{
        baseBlur.x * parallaxFactor / (layer_cam.zoom > 0.0001f ? layer_cam.zoom : 1.0f),
        baseBlur.y * parallaxFactor / (layer_cam.zoom > 0.0001f ? layer_cam.zoom : 1.0f)
    };
    if (!layer->is_custom_gpu) {
        renderer_set_camera(layer_cam);
    }
    layer->draw(layer_cam, fb_w, fb_h, dt, elapsed_time, layerBlur);
}
// =====================================================================================
void GlobalBackground::draw(const Camera2D& cam, u16 fb_w, u16 fb_h,
                            f32 dt, f32 elapsed_time)
{
    // Motion blur: distant layers (low parallax) streak more when camera moves.
    // blur = velocity * (1 - parallax) / layer_zoom, in screen pixels.
    bs_math::Vec2 baseBlur = bs_math::Vec2{0,0};
    if (gs) {
        baseBlur = bs_math::Vec2{
            gs->player_flight().velocity.x * dt,
            gs->player_flight().velocity.y * dt
        };
    }
    // Back-to-front, respecting editor-panel debug toggles.
    if (starfield && (!gs || gs->render.bg_layer0_enabled)) {
        draw_bg_layer(starfield, cam, fb_w, fb_h, dt, elapsed_time, baseBlur);
    }
    if (nebula && (!gs || gs->render.bg_nebula_enabled)) {
        draw_bg_layer(nebula, cam, fb_w, fb_h, dt, elapsed_time, baseBlur);
    }
    if (mapped_system && (!gs || gs->render.bg_layer2_enabled)) {
        draw_bg_layer(mapped_system, cam, fb_w, fb_h, dt, elapsed_time, baseBlur);
    }
    renderer_set_camera(cam);
}

