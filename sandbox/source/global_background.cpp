#include "global_background.h"
#include "parallax_layer.h"
#include "mapped_system_layer.h"
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
    layer_count = 3;
    // Layer 0: far decorative starfield (procedural shader, parallax 0.008, zoom_scale 0.70)
    auto* layer0 = new StarfieldLayer(0, 0.008f, 0.70f, 0xDEADBEEFu);
    layer0->set_game_state(gs);
    layers[0] = layer0;
    // Layer 1: mid-distance decorative starfield (procedural shader, parallax 0.02, zoom_scale 0.90)
    auto* layer1 = new StarfieldLayer(1, 0.02f, 0.90f, 0xCAFEBABEu);
    layer1->set_game_state(gs);
    layers[1] = layer1;
    // Layer 2: mapped system star + planets (parallax 0.30, more distant than foreground)
    layers[2] = new MappedSystemLayer(gs, star_fx);
}
// =====================================================================================
void GlobalBackground::shutdown()
{
    for (i32 i = 0; i < layer_count; ++i) {
        if (layers[i]) {
            delete layers[i];
            layers[i] = nullptr;
        }
    }
    layer_count = 0;
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
    for (i32 i = 0; i < layer_count; ++i) {
        if (!layers[i]) continue;
        // Respect editor-panel debug toggles.
        if (gs) {
            if (i == 0 && !gs->bg_layer0_enabled) continue;
            if (i == 1 && !gs->bg_layer1_enabled) continue;
            if (i == 2 && !gs->bg_layer2_enabled) continue;
        }
        ParallaxLayer* layer = layers[i];
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
        if (layer->is_custom_gpu) {
            layer->draw(layer_cam, fb_w, fb_h, dt, elapsed_time, layerBlur);
        } else {
            renderer_set_camera(layer_cam);
            layer->draw(layer_cam, fb_w, fb_h, dt, elapsed_time, layerBlur);
        }
    }
    renderer_set_camera(cam);
}
