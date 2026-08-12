#include "render/scene_renderer.h"
#include "game.h"
// Explicit peer includes (do not rely on the game.h module-include cascade): each pass this
// orchestrator calls is declared by its own header.
#include "render/galaxy_map_render.h"   // draw_galaxy_map_look
#include "render/parallax_background.h" // draw_parallax_background
#include "render/frame_lighting.h"      // submit_frame_lighting
#include "render/ship_scene.h"          // draw_ship_scene
#include "render/system_content_render.h" // per-system asteroids / resources / dust / stations
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

    // ---- Per-system natural asteroids (every system) -- render/system_content_render.cpp.
    draw_system_asteroids(s);

    // ---- Per-system resource nodes (belt/mid zones) + ambient dust -- render/system_content_render.cpp.
    draw_system_resources(s);
    draw_system_decorations(s);

    // ---- Per-system civilian stations (owned systems only) -- render/system_content_render.cpp.
    draw_system_stations(s);

    // ---- Ship rendering -- render/ship_scene.cpp. Runs after the star position + lighting.
    draw_ship_scene(s);

    // ---- Gameplay overlays (projectiles, RTS, gizmos, travel, sensor, heat map) --
    // render/gameplay_overlays.cpp.
    draw_gameplay_overlays(s);

    // ---- Ship portrait (inspector) -- render/ship_scene.cpp. Captures the inspected hull
    // into the engine's offscreen portrait target for the full-screen inspector's center
    // zone. Runs LAST: it needs draw_ship_scene's render_pos, and it temporarily swaps the
    // submitted camera. No-op (and free) while the inspector is closed.
    ship_portrait_submit(s);
}
