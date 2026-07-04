#include "render/game_hud.h"
#include "game.h"

#include "sim/action_log.h"     // action_log_push
#include "core/galaxy_coords.h" // get_system_zone
#include <renderer/bs_ui.h>
#include <stdio.h>
#include <math.h>

using namespace bs_math;

// ---- Encounter panel (centered, modal) -----------------------------------------------
void draw_encounter_panel(game_state* s) {
    if (s->encounter_active && !s->editor.edit_mode_active) {
        if (bs_ui_begin_panel("ENCOUNTER", BS_UI_ANCHOR_CENTER, 12.0f, BsUiType::BS_UI_TYPE_GAME)) {
            bs_ui_text_colored(1.0f, 1.0f, 1.0f, 1.0f, s->fleet_state.enemy_ship.vessel_name);
            bs_ui_separator();
            Vec2 delta = hierpos_diff(&s->player_ship().origin, &s->fleet_state.enemy_ship.origin, BS_HIERPOS_CELL_SIZE);
            f32 dist   = vec2_length(delta);
            char info[128];
            snprintf(info, sizeof(info), "Distance: %.1f m", dist);
            bs_ui_text_colored(0.8f, 0.8f, 0.8f, 1.0f, info);
            char faction_line[128];
            snprintf(faction_line, sizeof(faction_line), "Faction: %s",
                     vessel_faction_name(s->fleet_state.enemy_ship.faction));
            bs_ui_text_colored(0.8f, 0.8f, 0.8f, 1.0f, faction_line);
            bs_ui_text_colored(0.7f, 0.7f, 0.7f, 1.0f,
                                vessel_faction_desc(s->fleet_state.enemy_ship.faction));
            bs_ui_separator();
            if (bs_ui_button("Engage",  TRUE)) {
                s->encounter_active = FALSE;
                s->time_scale = 1.0f;
                action_log_push(s, "Engage selected.");
            }
            if (bs_ui_button("Avoid",   TRUE)) {
                s->encounter_active = FALSE;
                s->time_scale = 1.0f;
                action_log_push(s, "Avoid selected.");
            }
            if (bs_ui_button("Hail",    TRUE)) {
                s->encounter_active = FALSE;
                s->time_scale = 1.0f;
                action_log_push(s, "Hail selected.");
            }
            if (bs_ui_button("Observe", TRUE)) {
                s->encounter_active = FALSE;
                s->time_scale = 1.0f;
                action_log_push(s, "Observe selected.");
            }
        }
        bs_ui_end_panel();
    }
}

// ---- Navigation HUD + Ship HUD (arena-side affordance, hidden in edit mode) -------------
// Shown on the arena side of the blend band; suppressed on the galaxy-map side to avoid
// cluttering the map. Uses the arena weight midpoint so it fades in with the arena look.
void draw_nav_ship_hud(game_state* s) {
    if (s->view_arena_w > 0.5f && !s->editor.edit_mode_active) {
        i32 nearest = find_system_by_cell(&s->galaxy.map_entities[0].galaxy_pos, &s->galaxy.galaxy_voronoi, s->galaxy.systems);
        f64 sx, sy, nx, ny;
        hierpos_to_f64(&s->galaxy.map_entities[0].galaxy_pos, BS_HIERPOS_CELL_SIZE, &sx, &sy);
        hierpos_to_f64(&s->galaxy.systems[nearest].galaxy_center, BS_HIERPOS_CELL_SIZE, &nx, &ny);
        f64 dist = sqrt((sx - nx) * (sx - nx) + (sy - ny) * (sy - ny));
        char dist_buf[64];
        if (dist >= 1000000.0) {
            snprintf(dist_buf, sizeof(dist_buf), "%.2f M u", dist / 1000000.0);
        } else if (dist >= 1000.0) {
            snprintf(dist_buf, sizeof(dist_buf), "%.2f k u", dist / 1000.0);
        } else {
            snprintf(dist_buf, sizeof(dist_buf), "%.0f u", dist);
        }
        if (bs_ui_begin_hud_panel("NAV HUD", BS_UI_ANCHOR_TOP_RIGHT, 16.0f)) {
            const f32 label_x = 60.0f;
            const f32 dim_a   = 0.60f;
            const f32 bright_a= 1.0f;
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "SECTOR");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a, "Alpha");
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "SYS");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a,
                               s->galaxy.systems[s->galaxy.current_system].name ? s->galaxy.systems[s->galaxy.current_system].name : "?");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x + 110.0f);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "|");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x + 128.0f);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a, dist_buf);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "ZONE");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x);
            char zone_buf[16];
            snprintf(zone_buf, sizeof(zone_buf), "%d", get_system_zone(s, s->galaxy.current_system));
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a, zone_buf);
        }
        bs_ui_end_hud_panel();

        // ---- Ship properties HUD (global mode only) -------------------------------------------
        if (!s->camera_state.free_camera_active) {
        f32 speed = vec2_length(s->player_flight().velocity);
        char speed_buf[64];
        snprintf(speed_buf, sizeof(speed_buf), "%.1f u/s", speed);
        if (bs_ui_begin_hud_panel("SHIP HUD", BS_UI_ANCHOR_TOP_RIGHT, 110.0f)) {
            const f32 label_x = 60.0f;
            const f32 dim_a   = 0.60f;
            const f32 bright_a= 1.0f;
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, dim_a, "SPEED");
            bs_ui_same_line();
            bs_ui_set_cursor_pos_x(label_x);
            bs_ui_text_colored(0.35f, 0.80f, 0.95f, bright_a, speed_buf);
        }
        bs_ui_end_hud_panel();
        }
    }
}
