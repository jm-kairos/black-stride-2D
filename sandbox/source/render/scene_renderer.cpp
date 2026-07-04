#include "render/scene_renderer.h"
#include "game.h"
// Explicit peer includes (do not rely on the game.h module-include cascade): each pass this
// orchestrator calls is declared by its own header.
#include "render/galaxy_map_render.h"   // draw_galaxy_map_look
#include "render/parallax_background.h" // draw_parallax_background
#include "render/frame_lighting.h"      // submit_frame_lighting
#include "render/ship_scene.h"          // draw_ship_scene
#include "render/gameplay_overlays.h"   // draw_gameplay_overlays

#include <renderer/renderer.h>          // renderer_set_camera

void render_scene(game_state* s, f32 dt) {
    renderer_set_camera(s->camera_state.camera);

    s->profiler.begin(PROF_STARS);
    // ---- Galaxy-map look render (was MODE_SYSTEM) -- render/galaxy_map_render.cpp
    draw_galaxy_map_look(s, dt);
    s->profiler.end(PROF_STARS);

    // ---- Procedural parallax background (starfield + nebula) -- render/parallax_background.cpp.
    // Updates s->star_pos (read by the lighting + ship passes below).
    draw_parallax_background(s, dt);

    // ---- Per-frame lighting assembly + submission -- render/frame_lighting.cpp.
    // Runs after the star position is set and before the lit sprite passes.
    submit_frame_lighting(s);

    // ---- Ship rendering -- render/ship_scene.cpp. Runs after the star position + lighting.
    draw_ship_scene(s);

    // ---- Gameplay overlays (projectiles, RTS, gizmos, travel, sensor, heat map) --
    // render/gameplay_overlays.cpp.
    draw_gameplay_overlays(s);
}
