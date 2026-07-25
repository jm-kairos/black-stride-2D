#include "render/gameplay_overlays.h"
#include "game.h"

#include "core/view_transform.h" // game_true_world_to_render, render_from_hierpos
#include "core/cursor_world.h"   // mouse_true_hierpos
#include "sim/editor_tools.h"    // edit_entity_position, gizmo_*
#include "sim/heat_map.h"        // draw_ship_metaballs
#include "sim/combat_arena.h"    // ENEMY_DETECTOR_RADIUS
#include "render/ship_render.h"  // draw_collider_outline
#include "render/sensor_overlay.h" // sensor_overlay_draw
#include "render/defense_laser_overlay.h" // defense_laser_overlay_draw
#include "core/render_layers.h" // LAYER_UI / LAYER_GIZMO / LAYER_DEBUG
#include "sim/galaxy_history.h"  // galaxy_history_faction_label / faction_is_hostile
#include "sim/ai_ship.h"         // ShipArchetype (role marker shapes)
#include "text.h"                // text_draw (patrol label)
#include <renderer/renderer.h>
#include <renderer/camera2d.h>   // camera2d_world_to_screen (patrol label placement)
#include <math.h>

using namespace bs_math;

// ---- Generic "unidentified object" marker -------------------------------------------------
// Used by the discovery system for ships/stations the player has not yet scanned at close range.
static void draw_unidentified_marker(Vec2 center, f32 zoom_inv, u32 layer) {
    f32 r = 14.0f * zoom_inv;
    f32 h = r * 0.55f;
    const bs_color COL = bs_color{ 0.85f, 0.85f, 0.85f, 0.75f };
    renderer_draw_circle(center, r, 20, 1.5f, COL, layer);
    renderer_draw_line(Vec2{ center.x - h, center.y - h }, Vec2{ center.x + h, center.y + h }, 1.5f, COL, layer);
    renderer_draw_line(Vec2{ center.x + h, center.y - h }, Vec2{ center.x - h, center.y + h }, 1.5f, COL, layer);
}

// ---- Static enemy reticle -----------------------------------------------------------------
// Persistent red marker around the enemy ship so the hostile stays locatable at any zoom, plus a
// faint ring showing its detector radius (the range at which it opens fire on the flagship).
static void draw_enemy_marker(game_state* s) {
    const Ship* enemy = &s->fleet_state.enemy_ship;
    Vec2 center = render_from_hierpos(s, &enemy->origin);
    f32  zoom   = (s->camera_state.camera.zoom > 0.0001f) ? s->camera_state.camera.zoom : 1.0f;

    const bs_color RED = bs_color{ 1.0f, 0.25f, 0.25f, 0.95f };
    // Screen-constant reticle so it never shrinks to nothing when far away.
    f32 r = 22.0f / zoom;
    renderer_draw_circle(center, r, 20, 2.0f, RED, LAYER_UI);
    // Corner brackets just outside the ring.
    f32 br = r * 1.5f;      // bracket corner offset
    f32 c  = br * 0.5f;     // bracket arm length
    // Top-left
    renderer_draw_line(Vec2{ center.x - br, center.y + br - c }, Vec2{ center.x - br, center.y + br }, 2.0f, RED, LAYER_UI);
    renderer_draw_line(Vec2{ center.x - br, center.y + br }, Vec2{ center.x - br + c, center.y + br }, 2.0f, RED, LAYER_UI);
    // Top-right
    renderer_draw_line(Vec2{ center.x + br - c, center.y + br }, Vec2{ center.x + br, center.y + br }, 2.0f, RED, LAYER_UI);
    renderer_draw_line(Vec2{ center.x + br, center.y + br }, Vec2{ center.x + br, center.y + br - c }, 2.0f, RED, LAYER_UI);
    // Bottom-right
    renderer_draw_line(Vec2{ center.x + br, center.y - br + c }, Vec2{ center.x + br, center.y - br }, 2.0f, RED, LAYER_UI);
    renderer_draw_line(Vec2{ center.x + br, center.y - br }, Vec2{ center.x + br - c, center.y - br }, 2.0f, RED, LAYER_UI);
    // Bottom-left
    renderer_draw_line(Vec2{ center.x - br + c, center.y - br }, Vec2{ center.x - br, center.y - br }, 2.0f, RED, LAYER_UI);
    renderer_draw_line(Vec2{ center.x - br, center.y - br }, Vec2{ center.x - br, center.y - br + c }, 2.0f, RED, LAYER_UI);
    // Faint detector-radius ring (world-space radius).
    const bs_color FAINT = bs_color{ 1.0f, 0.30f, 0.30f, 0.18f };
    renderer_draw_circle(center, ENEMY_DETECTOR_RADIUS, 48, 1.5f, FAINT, LAYER_UI);
}

// ---- General Ship AI: NPC agent markers ---------------------------------------------------
// A civ-coloured, role-shaped marker (screen-constant) over every live NPC ship so they stay locatable
// at any zoom: patrols/combat = triangle (points along heading), miners = square, traders = circle.
// Hostile agents get an extra outer ring.
static void draw_npc_ship_markers(game_state* s) {
    f32 zoom = (s->camera_state.camera.zoom > 0.0001f) ? s->camera_state.camera.zoom : 1.0f;
    f32 zoom_inv = 1.0f / zoom;
    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {
        NpcShip& n = s->npc_ships[i];
        if (!n.active) continue;
        Vec2 center = render_from_hierpos(s, &n.ship.origin);
        if (!n.discovered) {
            draw_unidentified_marker(center, zoom_inv, LAYER_UI);
            continue;
        }
        bs_color base = (n.faction >= 0 && n.faction < s->galaxy.civ_count)
                          ? s->galaxy.civs[n.faction].color
                          : bs_color{ 0.95f, 0.35f, 0.30f, 1.0f };
        bs_color col = bs_color{ base.r * 0.5f + 0.5f, base.g * 0.5f + 0.5f, base.b * 0.5f + 0.5f, 1.0f };
        b8 hostile = galaxy_history_faction_is_hostile(s, n.faction);
        f32 r  = 16.0f / zoom;
        f32 th = hostile ? 2.5f : 1.5f;
        if (n.archetype == ARCHETYPE_MINER) {
            renderer_draw_rect_outline(center, Vec2{ r * 1.7f, r * 1.7f }, th, col, LAYER_UI);   // square
        } else if (n.archetype == ARCHETYPE_TRADER) {
            renderer_draw_circle(center, r, 16, th, col, LAYER_UI);                              // circle
        } else {
            // Triangle pointing along the ship's heading (angle 0 => nose +Y).
            Vec2 nose = vec2_rotate(Vec2{ 0.0f,  r * 1.2f }, n.ship.angle);
            Vec2 perp = vec2_rotate(Vec2{ r * 0.9f, 0.0f }, n.ship.angle);
            Vec2 back = vec2_rotate(Vec2{ 0.0f, -r * 0.8f }, n.ship.angle);
            Vec2 p0 = vec2_add(center, nose);
            Vec2 p1 = vec2_add(center, vec2_add(back, perp));
            Vec2 p2 = vec2_add(center, vec2_sub(back, perp));
            renderer_draw_line(p0, p1, th, col, LAYER_UI);
            renderer_draw_line(p1, p2, th, col, LAYER_UI);
            renderer_draw_line(p2, p0, th, col, LAYER_UI);
        }
        if (hostile) renderer_draw_circle(center, r * 2.0f, 4, 1.5f, col, LAYER_UI);
    }
}

// ---- Gameplay overlays (projectiles, RTS, gizmos, travel, sensor, heat map) ---------------
// Drawn in ALL looks: under the unified coordinate space gameplay is continuous across the
// arena <-> galaxy-map blend, so these overlays must not pop at the discrete render-mode
// boundary. rts_controls.draw() runs exactly once here (previously duplicated per mode).
void draw_gameplay_overlays(game_state* s) {
    // These overlays draw in the same render space as the ship sprites: a pure translation by
    // -camera_hierpos. render_from_hierpos / game_true_world_to_render apply that transform for
    // hierpos / point positions respectively.

    // ---- Projectiles ---------------------------------------------------------------------
    s->profiler.begin(PROF_PROJECTILES_DRAW);
    s->projectiles.glow_override = &s->render.bullet_glow;
    s->projectiles.render(LAYER_UI, &s->camera_state.camera_hierpos);
    s->profiler.end(PROF_PROJECTILES_DRAW);

    // ---- RTS controls overlay (hover selection, etc.) ------------------------------------
    s->rts_controls.draw();

    // ---- Static enemy reticle (always visible so the hostile stays locatable) ------------
    draw_enemy_marker(s);

    // ---- General Ship AI: civ-coloured markers over every live NPC agent -------------------
    draw_npc_ship_markers(s);

    // ---- Sensor system: flagship layer rings + incoming-projectile radar contacts --------
    sensor_overlay_draw(s);

    // ---- Point-defense laser beams (populated this frame by sim/point_defense.cpp) --------
    defense_laser_overlay_draw(s);

    // ---- EDIT MODE selection highlight ----------------------------------------------------
    // When active, every editor light gets a tinted marker and a faint radius circle so they
    // are easy to locate. The selected entity (ship or light) gets a bright highlight on top.
    if (s->editor.edit_mode_active) {
        f32 zoom_inv = 1.0f / ((s->camera_state.camera.zoom > 0.0001f) ? s->camera_state.camera.zoom : 1.0f);
        // All lights: small tinted marker + faint radius circle.
        for (size_t i = 0; i < s->render.lights.size(); ++i) {
            const bs_light2d& L = s->render.lights[i];
            if (!L.enabled) continue;
            bs_color col = L.color;
            bs_color mkr = bs_color{ col.r, col.g, col.b, 0.40f };
            bs_color rad = bs_color{ col.r, col.g, col.b, 0.15f };
            f32 r_mkr = 12.0f * zoom_inv;
            renderer_draw_circle(game_true_world_to_render(s, L.position), r_mkr, 8, 2.0f, mkr, LAYER_GIZMO);
            renderer_draw_circle(game_true_world_to_render(s, L.position), L.radius, 32, 1.5f, rad, LAYER_GIZMO);
        }
        // Selected entity: bright highlight on top.
        const bs_color SEL = bs_color{ 0.30f, 0.95f, 1.00f, 1.0f };
        if (s->editor.edit_selection.kind == EDIT_SHIP) {
            const Ship* sel = (s->editor.edit_selection.index == 0) ? &s->player_ship() : &s->fleet_state.enemy_ship;
            draw_collider_outline(sel, SEL, 3.0f);
        } else if (s->editor.edit_selection.kind == EDIT_LIGHT &&
                   s->editor.edit_selection.index >= 0 &&
                   s->editor.edit_selection.index < (i32)s->render.lights.size()) {
            Vec2 p = game_true_world_to_render(s, s->render.lights[s->editor.edit_selection.index].position);
            f32  r = 24.0f * zoom_inv;
            renderer_draw_circle(p, r, 24, 2.0f, SEL, LAYER_GIZMO);
            renderer_draw_line(Vec2{ p.x - r, p.y }, Vec2{ p.x + r, p.y }, 2.0f, SEL, LAYER_GIZMO);
            renderer_draw_line(Vec2{ p.x, p.y - r }, Vec2{ p.x, p.y + r }, 2.0f, SEL, LAYER_GIZMO);
        }
        // ---- GIZMOS (translation arrows + rotation ring) ----------------------------------
        if (s->editor.edit_selection.kind != EDIT_NONE) {
            bs_math::HierPos2 sel_hp = edit_entity_position(s, s->editor.edit_selection);
            Vec2 origin = render_from_hierpos(s, &sel_hp);
            f32 axis_len = gizmo_axis_len(zoom_inv);
            f32 arrow_sz = gizmo_arrow_size(zoom_inv);
            // Which gizmo part is currently under the cursor? Visual feedback must match the
            // hit-test logic in edit_pick_gizmo so the user knows what will activate on click.
            EditDragMode hover = edit_pick_gizmo(s, mouse_true_hierpos(s));
            // Rotation ring — sized to extend past the entity bounds + 30 px screen padding.
            f32 ring_r = 0.0f;
            if (s->editor.edit_selection.kind == EDIT_SHIP) {
                const Ship* sh = (s->editor.edit_selection.index == 0) ? &s->player_ship() : &s->fleet_state.enemy_ship;
                ring_r = gizmo_ring_radius_ship(sh, zoom_inv);
            } else {
                ring_r = gizmo_ring_radius_light(zoom_inv);
            }
            bs_color ring_col = (hover == EDIT_DRAG_ROTATE)
                ? bs_color{ 0.30f, 0.95f, 1.00f, 1.0f }   // bright cyan on hover
                : bs_color{ 0.90f, 0.90f, 0.90f, 0.60f }; // gray normally
            f32 ring_thick = (hover == EDIT_DRAG_ROTATE) ? 2.5f : 1.5f;
            renderer_draw_circle(origin, ring_r, 32, ring_thick, ring_col, LAYER_GIZMO);
            // X axis (red) and Y axis (green) — brighten and thicken on hover.
            bs_color x_col = (hover == EDIT_DRAG_AXIS_X)
                ? bs_color{ 1.00f, 1.00f, 1.00f, 1.0f }   // white on hover
                : bs_color{ 1.00f, 0.20f, 0.20f, 1.0f };  // red normally
            bs_color y_col = (hover == EDIT_DRAG_AXIS_Y)
                ? bs_color{ 1.00f, 1.00f, 1.00f, 1.0f }   // white on hover
                : bs_color{ 0.20f, 1.00f, 0.30f, 1.0f };  // green normally
            f32 axis_thick = 2.5f;
            if (hover == EDIT_DRAG_AXIS_X || hover == EDIT_DRAG_AXIS_Y)
                axis_thick = 4.0f;
            Vec2 x_end = vec2_add(origin, Vec2{ axis_len, 0.0f });
            Vec2 y_end = vec2_add(origin, Vec2{ 0.0f, axis_len });
            renderer_draw_line(origin, x_end, axis_thick, x_col, LAYER_GIZMO);
            renderer_draw_line(origin, y_end, axis_thick, y_col, LAYER_GIZMO);
            // Arrow heads follow their parent axis color.
            renderer_draw_line(x_end, Vec2{ x_end.x - arrow_sz, x_end.y - arrow_sz * 0.5f }, 2.0f, x_col, LAYER_GIZMO);
            renderer_draw_line(x_end, Vec2{ x_end.x - arrow_sz, x_end.y + arrow_sz * 0.5f }, 2.0f, x_col, LAYER_GIZMO);
            renderer_draw_line(y_end, Vec2{ y_end.x - arrow_sz * 0.5f, y_end.y - arrow_sz }, 2.0f, y_col, LAYER_GIZMO);
            renderer_draw_line(y_end, Vec2{ y_end.x + arrow_sz * 0.5f, y_end.y - arrow_sz }, 2.0f, y_col, LAYER_GIZMO);
        }
    }

    // ---- Travel debug overlay (editor-gated) -----------------------------------------------
    if (s->travel_enabled) {
        Vec2 origin_world = render_from_hierpos(s, &s->travel.origin);
        Vec2 dest_world   = render_from_hierpos(s, &s->travel.destination);
        Vec2 ship_world   = render_from_hierpos(s, &s->travel.current);
        // Path line: faint cyan.
        bs_color path_col = bs_color{ 0.35f, 0.90f, 0.95f, 0.6f };
        renderer_draw_line(origin_world, dest_world, 1.5f, path_col, LAYER_UI);
        // Origin marker: green circle.
        renderer_draw_circle(origin_world, 12.0f, 16, 2.0f, bs_color{ 0.35f, 0.95f, 0.45f, 0.9f }, LAYER_UI);
        // Destination marker: red circle.
        renderer_draw_circle(dest_world,   12.0f, 16, 2.0f, bs_color{ 0.95f, 0.35f, 0.35f, 0.9f }, LAYER_UI);
        // Current ship marker: yellow circle.
        renderer_draw_circle(ship_world,    8.0f, 16, 2.0f, bs_color{ 1.00f, 0.95f, 0.35f, 0.9f }, LAYER_UI);
    }

    // ---- Sensor range circle (gameplay affordance, drawn in all looks) ------------------
    if (s->galaxy.map_draw_sensor_range) {
        bs_color sensor_col = bs_color{ 0.45f, 0.90f, 0.40f, 0.30f };
        renderer_draw_circle(render_from_hierpos(s, &s->player_ship().origin), s->galaxy.map_sensor_range, 64, 2.0f, sensor_col, LAYER_UI);
    }
    // ---- Discovery sensor radius (identifies unknown ships/stations when crossed) --------
    if (s->editor.draw_discovery_sensor_range) {
        bs_color disc_col = bs_color{ 0.40f, 0.85f, 0.95f, 0.25f };
        renderer_draw_circle(render_from_hierpos(s, &s->player_ship().origin), s->player_ship().sensors.layer1_radius, 64, 2.0f, disc_col, LAYER_UI);
    }

    // ---- Metaball movement UI (global mode only) ------------------------------------------
    s->profiler.begin(PROF_HEAT_MAP);
    draw_ship_metaballs(s);
    s->profiler.end(PROF_HEAT_MAP);
}
