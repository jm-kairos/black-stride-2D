#include "ui/editor_ui.h"
#include "game.h"
#include "render/debug_overlay.h" // g_debug_cell_grid
#include "sim/action_log.h"       // action_log_push
#include "sim/combat_arena.h"     // combat_arena_rebuild_player_entities (multi-ship toggle)
#include "core/coord_diag.h"        // coord_diag_is_enabled / set_enabled / last_violations
#include "core/cursor_world.h"     // mouse_true_hierpos (hardpoint cursor placement)
#include "render/ship_render.h"    // draw_hardpoint_highlight (gizmo selection)
#include <renderer/bs_ui.h>    // bs_ui_* panel/widget API
#include <renderer/bs_rml.h>   // bs_rml_set_sharpen (UI sharpen slider)
#include <core/logger.h>       // BS_LOG_INFO (.ship hardpoint line dump)
#include "core/view_transform.h" // g_zoom_out_speed_gain
#include "render/sensor_overlay.h" // g_sensor_fade_distance
#include <stdio.h>             // snprintf
#include <math.h>             // sqrt

using namespace bs_math;

// Rebuild the '|'-joined accepts list ("weapon|defense") from a MODULE_TYPE_* mask —
// the exact reverse of ship_def_load's parse_module_mask (sim/ship_def.cpp), for .ship
// line round-tripping.
static void hardpoint_accepts_string(u32 mask, char* buf, i32 cap) {
    static const struct { u32 bit; const char* name; } KINDS[] = {
        { MODULE_TYPE_WEAPON,  "weapon"  },
        { MODULE_TYPE_DEFENSE, "defense" },
        { MODULE_TYPE_SENSOR,  "sensor"  },
        { MODULE_TYPE_ENGINE,  "engine"  },
        { MODULE_TYPE_UTILITY, "utility" },
    };
    i32 n = 0;
    buf[0] = '\0';
    for (i32 i = 0; i < 5; ++i) {
        if (!(mask & KINDS[i].bit)) continue;
        n += snprintf(buf + n, (size_t)(cap - n), "%s%s", (n > 0) ? "|" : "", KINDS[i].name);
        if (n >= cap - 1) break;
    }
    if (buf[0] == '\0') snprintf(buf, (size_t)cap, "utility");
}

// Per-entity glow parameter editor. Rendered inline inside build_editor_panel for each glow
// target; id_suffix keeps widget ids unique across the ship/exhaust/bullet/global blocks.
static void draw_glow_editor(bs_glow_params* gp, const char* id_suffix) {
    char lbl[64];
    auto mk = [&](const char* base) { snprintf(lbl, sizeof(lbl), "%s##%s", base, id_suffix); return lbl; };
    bs_ui_slider_float(mk("Intensity"), &gp->intensity, 0.0f, 3.0f);
    bs_ui_slider_float(mk("Falloff"),   &gp->falloff,   1.0f, 20.0f);
    f32 gt[3] = { gp->glow_tint.r, gp->glow_tint.g, gp->glow_tint.b };
    if (bs_ui_color_edit3(mk("Glow Tint"), gt)) {
        gp->glow_tint.r = gt[0]; gp->glow_tint.g = gt[1]; gp->glow_tint.b = gt[2];
    }
    bs_ui_slider_float(mk("Head Mult"),    &gp->head_mult,     0.0f, 8.0f);
    bs_ui_slider_float(mk("Head Falloff"), &gp->head_falloff,  0.5f, 10.0f);
    bs_ui_slider_float(mk("Head Range"),   &gp->head_range,    0.0f, 1.0f);
    bs_ui_slider_float(mk("Distort Amp"),  &gp->distort_amp,   0.0f, 0.3f);
    bs_ui_slider_float(mk("Wave Speed"),   &gp->wave_speed,    0.0f, 50.0f);
    bs_ui_slider_float(mk("Wave Freq"),    &gp->wave_freq,     0.0f, 30.0f);
    bs_ui_slider_float(mk("Jitter Speed"), &gp->jitter_speed, 0.0f, 100.0f);
    bs_ui_slider_float(mk("Jitter Freq"),  &gp->jitter_freq,   0.0f, 60.0f);
    f32 tc[3] = { gp->temp_cool.r, gp->temp_cool.g, gp->temp_cool.b };
    if (bs_ui_color_edit3(mk("Cool (tail)"), tc)) {
        gp->temp_cool.r = tc[0]; gp->temp_cool.g = tc[1]; gp->temp_cool.b = tc[2];
    }
    f32 tw[3] = { gp->temp_warm.r, gp->temp_warm.g, gp->temp_warm.b };
    if (bs_ui_color_edit3(mk("Warm (mid)"), tw)) {
        gp->temp_warm.r = tw[0]; gp->temp_warm.g = tw[1]; gp->temp_warm.b = tw[2];
    }
    f32 th[3] = { gp->temp_hot.r, gp->temp_hot.g, gp->temp_hot.b };
    if (bs_ui_color_edit3(mk("Hot (head)"), th)) {
        gp->temp_hot.r = th[0]; gp->temp_hot.g = th[1]; gp->temp_hot.b = th[2];
    }
    if (bs_ui_button(mk("Reset Defaults"), TRUE)) {
        *gp = bs_glow_params{
            1.0f, 6.0f, 4.0f, 2.5f, 0.80f, 0.08f, 15.0f, 8.0f, 45.0f, 24.0f,
            bs_color{ 1.0f, 0.85f, 0.5f, 1.0f },
            bs_color{ 0.90f, 0.15f, 0.02f, 1.0f },
            bs_color{ 1.0f, 0.45f, 0.05f, 1.0f },
            bs_color{ 1.0f, 0.98f, 0.90f, 1.0f }
        };
    }
}

void build_editor_panel(game_state* s) {
    if (bs_ui_begin_panel("EDITOR PANEL", BS_UI_ANCHOR_TOP_LEFT, 12.0f, BsUiType::BS_UI_TYPE_EDITOR)) {
        // ---- EDIT MODE -------------------------------------------------------------------------
        // When active, left-click selects a ship or light in the world and drag repositions it.
        // Flight simulation is suspended while active so dragged poses stay put.
        const f32 EM[4] = { 0.55f, 0.85f, 0.95f, 1.0f };
        bs_ui_text_colored(EM[0], EM[1], EM[2], EM[3], "EDIT MODE");
        bool edit_on = s->editor.edit_mode_active ? true : false;
        bs_ui_checkbox("Edit mode active", &edit_on);
        s->editor.edit_mode_active = edit_on ? TRUE : FALSE;
        // ---- SHIP COMMAND --------------------------------------------------------------------
        // Default: the player commands only the flagship. Enabling this reveals the escort wing
        // (already spawned) and lets the RTS layer box-select / order the whole fleet.
        bs_ui_separator();
        const f32 SC[4] = { 0.55f, 0.95f, 0.65f, 1.0f };
        bs_ui_text_colored(SC[0], SC[1], SC[2], SC[3], "SHIP COMMAND");
        bool multi_on = s->editor.multi_ship_enabled ? true : false;
        bs_ui_checkbox("Multiple ship command", &multi_on);
        if ((multi_on ? TRUE : FALSE) != s->editor.multi_ship_enabled) {
            s->editor.multi_ship_enabled = multi_on ? TRUE : FALSE;
            i32 n = s->editor.multi_ship_enabled ? s->fleet_state.fleet.spawned_count() : 1;
            s->fleet_state.fleet.set_count(n);
            s->fleet_state.fleet.clear_selection();
            s->fleet_state.fleet.set_piloted(0);
            combat_arena_rebuild_player_entities(s);
            action_log_push(s, s->editor.multi_ship_enabled
                            ? "Multiple ship command ENABLED"
                            : "Single ship command (flagship only)");
        }
        // ---- DISCOVERIES ---------------------------------------------------------------------
        bool draw_sensor_on = s->editor.draw_discovery_sensor_range ? true : false;
        bs_ui_checkbox("Draw discovery sensor range", &draw_sensor_on);
        s->editor.draw_discovery_sensor_range = draw_sensor_on ? TRUE : FALSE;
        bs_ui_text("Discovery uses Sensor Layer 1 (identification range).");
        if (bs_ui_button("Open Discoveries", TRUE)) s->show_discoveries = !s->show_discoveries;
        // ---- UI FONT KIT ---------------------------------------------------------------------
        // Swaps the in-game RmlUi typefaces live (0 = Neon, 1 = Clean, 2 = Minimal, 3 = Frontier).
        bs_ui_separator();
        const f32 UK[4] = { 0.70f, 0.80f, 0.95f, 1.0f };
        bs_ui_text_colored(UK[0], UK[1], UK[2], UK[3], "UI FONT KIT");
        bs_ui_combo("Font kit", &s->ui_font_kit, "Neon\0Clean\0Minimal\0Frontier\0");
        // CAS-lite sharpening of the UI skin/icon atlases (fonts are never sharpened).
        if (bs_ui_slider_float("UI sharpen", &s->ui_sharpen, 0.0f, 1.0f))
            bs_rml_set_sharpen(s->ui_sharpen);
        // ---- MISSILES --------------------------------------------------------------------------
        // Live flight-model tuning for guided missiles (Phase A; docs/POINT_DEFENSE_AND_MISSILES.md).
        bs_ui_separator();
        const f32 MS[4] = { 0.95f, 0.60f, 0.40f, 1.0f };
        bs_ui_text_colored(MS[0], MS[1], MS[2], MS[3], "MISSILES");
        bs_ui_slider_float("Turn rate (rad/s)##msl", &s->missile_tuning.turn_rate,       0.2f,   6.0f);
        bs_ui_slider_float("Accel##msl",             &s->missile_tuning.accel,           0.0f,  8000.0f);
        bs_ui_slider_float("Max speed##msl",         &s->missile_tuning.max_speed,    1000.0f, 16000.0f);
        bs_ui_slider_float("Seeker cone (deg)##msl", &s->missile_tuning.seeker_cone_deg, 10.0f,  180.0f);
        bs_ui_slider_float("Seeker range##msl",      &s->missile_tuning.seeker_range, 1000.0f, 2000000.0f);
        // ---- FLAK ------------------------------------------------------------------------------
        // Proximity-burst screen tuning (Phase D; toggled per fire group with the T key).
        bs_ui_separator();
        const f32 FK[4] = { 0.85f, 0.85f, 0.60f, 1.0f };
        bs_ui_text_colored(FK[0], FK[1], FK[2], FK[3], "FLAK");
        bs_ui_slider_float("Fuse radius##flak",  &s->flak_tuning.fuse_radius,   20.0f, 400.0f);
        bs_ui_slider_float("Burst radius##flak", &s->flak_tuning.burst_radius,  50.0f, 800.0f);
        bs_ui_slider_float("Burst damage##flak", &s->flak_tuning.burst_damage,   0.5f,  10.0f);
        // ---- CAPACITOR -------------------------------------------------------------------------
        // Shared energy budget (Phase B): live flagship readout + baseline/PD-drain tuning.
        bs_ui_separator();
        const f32 CP[4] = { 0.60f, 0.90f, 0.70f, 1.0f };
        bs_ui_text_colored(CP[0], CP[1], CP[2], CP[3], "CAPACITOR");
        {
            Ship& flag = s->player_ship();
            char capbuf[96];
            snprintf(capbuf, sizeof(capbuf), "Flagship: %.0f / %.0f (regen %.1f/s)",
                     flag.cap_current, flag.cap_max, flag.cap_regen);
            bs_ui_text(capbuf);
            if (bs_ui_slider_float("Cap max##cap",   &flag.cap_max_base,   20.0f, 400.0f)) ship_recompute_stats(&flag);
            if (bs_ui_slider_float("Cap regen##cap", &flag.cap_regen_base,  0.0f,  40.0f)) ship_recompute_stats(&flag);
            bs_ui_slider_float("PD drain /s##cap",   &flag.point_defense.cap_drain_per_s, 0.0f, 30.0f);
            bs_ui_slider_float("PD reserve##cap",    &flag.point_defense.reserve_floor,   0.0f,  0.5f);
            // Read-only doctrine state (set via the inspector chips / the P stance key).
            static const char* PD_ST[3] = { "HOLD", "STANDARD", "OVERDRIVE" };
            static const char* PD_PR[3] = { "impact-time", "missiles-first", "nearest" };
            static const char* PD_GT[3] = { "60%", "80%", "100%" };
            char pdbuf[96];
            snprintf(pdbuf, sizeof(pdbuf), "PD doctrine: %s / %s / gate %s",
                     PD_ST[flag.point_defense.stance % 3], PD_PR[flag.point_defense.priority % 3],
                     PD_GT[flag.point_defense.gate_tier % 3]);
            bs_ui_text(pdbuf);
        }
        // ---- LIGHTS ----------------------------------------------------------------------------
        // Spawn / remove / edit the editor-managed 2D point lights.
        bs_ui_separator();
        const f32 LT[4] = { 0.95f, 0.85f, 0.55f, 1.0f };
        bs_ui_text_colored(LT[0], LT[1], LT[2], LT[3], "LIGHTS");
        // Scene-global ambient floor.
        f32 amb[3] = { s->render.light_ambient.r, s->render.light_ambient.g, s->render.light_ambient.b };
        if (bs_ui_color_edit3("Ambient##light_amb", amb)) {
            s->render.light_ambient.r = amb[0]; s->render.light_ambient.g = amb[1]; s->render.light_ambient.b = amb[2];
        }
        // Spawn a new light at the camera center (the visible world center), select it.
        if (bs_ui_button("Add Light", TRUE)) {
            bs_light2d nl{};
            nl.position  = s->camera_state.camera.position;
            nl.radius    = 320.0f;
            nl.intensity = 1.6f;
            nl.color     = bs_color{ 1.00f, 0.92f, 0.78f, 1.0f }; // warm default
            nl.enabled   = TRUE;
            s->render.lights.push_back(nl);
            s->render.light_selected = (i32)s->render.lights.size() - 1;
        }
        // Per-light rows. Collect a remove request and apply it AFTER the loop so we never erase
        // while iterating. Button/widget ids are suffixed "##<i>" to stay unique per row.
        i32 remove_idx = -1;
        for (size_t i = 0; i < s->render.lights.size(); ++i) {
            bs_light2d& L = s->render.lights[i];
            char id[32], label[48];
            snprintf(label, sizeof(label), "Light %zu%s", i, ((i32)i == s->render.light_selected) ? " *" : "");
            bs_ui_text_colored(LT[0], LT[1], LT[2], LT[3], label);
            snprintf(id, sizeof(id), "Select##%zu", i);
            if (bs_ui_button_sized(id, 60.0f, TRUE)) s->render.light_selected = (i32)i;
            bs_ui_same_line();
            snprintf(id, sizeof(id), "Remove##%zu", i);
            if (bs_ui_button_sized(id, 60.0f, TRUE)) remove_idx = (i32)i;
            bool on = L.enabled ? true : false;
            snprintf(id, sizeof(id), "Enabled##%zu", i);
            bs_ui_checkbox(id, &on);
            L.enabled = on ? TRUE : FALSE;
            snprintf(id, sizeof(id), "Radius##%zu", i);
            bs_ui_slider_float(id, &L.radius, 16.0f, 1200.0f);
            snprintf(id, sizeof(id), "Intensity##%zu", i);
            bs_ui_slider_float(id, &L.intensity, 0.0f, 4.0f);
            f32 col[3] = { L.color.r, L.color.g, L.color.b };
            snprintf(id, sizeof(id), "Color##%zu", i);
            if (bs_ui_color_edit3(id, col)) { L.color.r = col[0]; L.color.g = col[1]; L.color.b = col[2]; }
            bs_ui_separator();
        }
        if (remove_idx >= 0 && remove_idx < (i32)s->render.lights.size()) {
            s->render.lights.erase(s->render.lights.begin() + remove_idx);
            // Keep the selection valid after the shift.
            if (s->render.light_selected == remove_idx)      s->render.light_selected = -1;
            else if (s->render.light_selected > remove_idx)  s->render.light_selected -= 1;
        }
        // ---- Per-Entity Glow Controls --------------------------------------------------------
        bs_ui_separator();
        const f32 GL[4] = { 1.0f, 0.75f, 0.35f, 1.0f };
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "SHIP GLOW");
        draw_glow_editor(&s->player_ship().glow, "ship");
        bs_ui_separator();
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "EXHAUST GLOW");
        draw_glow_editor(&s->render.exhaust_glow, "exhaust");
        bs_ui_separator();
        // One editor per VISUAL FAMILY. These are what give a rail slug its cold blue and a
        // missile motor its shimmer, so they are tuned against each other rather than alone --
        // hence three panels in a row instead of one "bullet glow".
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "PROJECTILE GLOW - SHELL");
        draw_glow_editor(&s->render.projectile_glow[VFX_SHELL], "pglow_shell");
        bs_ui_separator();
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "PROJECTILE GLOW - SLUG");
        draw_glow_editor(&s->render.projectile_glow[VFX_SLUG], "pglow_slug");
        bs_ui_separator();
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "PROJECTILE GLOW - ORDNANCE");
        draw_glow_editor(&s->render.projectile_glow[VFX_ORDNANCE], "pglow_ordnance");
        // Also keep the global fallback editable for entities that don't set an override.
        bs_ui_separator();
        bs_ui_text_colored(GL[0], GL[1], GL[2], GL[3], "GLOBAL GLOW (fallback)");
        draw_glow_editor(&s->render.glow_params, "global");
        // ---- HDR BLOOM -------------------------------------------------------------------------
        bs_ui_separator();
        const f32 BL[4] = { 0.75f, 0.55f, 0.95f, 1.0f };
        bs_ui_text_colored(BL[0], BL[1], BL[2], BL[3], "HDR BLOOM");
        bool bloom_on = s->render.bloom_enabled ? true : false;
        bs_ui_checkbox("Enabled##bloom", &bloom_on);
        s->render.bloom_enabled = bloom_on ? TRUE : FALSE;
        bs_ui_slider_float("Threshold##bloom", &s->render.bloom_threshold, 0.0f, 2.0f);
        bs_ui_slider_float("Intensity##bloom", &s->render.bloom_intensity, 0.0f, 2.0f);
        bool dyn_bloom = s->render.dynamic_bloom ? true : false;
        bs_ui_checkbox("Dynamic (speed-driven)", &dyn_bloom);
        s->render.dynamic_bloom = dyn_bloom ? TRUE : FALSE;
        // ---- BACKGROUND LAYERS (debug toggles) -----------------------------------------------
        const f32 BG[4] = { 0.75f, 0.55f, 0.35f, 1.0f };
        bs_ui_text_colored(BG[0], BG[1], BG[2], BG[3], "BACKGROUND LAYERS");
        bool layer0_on = s->render.bg_layer0_enabled ? true : false;
        bool nebula_on = s->render.bg_nebula_enabled ? true : false;
        bool layer1_on = s->render.bg_layer1_enabled ? true : false;
        bool layer2_on = s->render.bg_layer2_enabled ? true : false;
        bs_ui_checkbox("Far starfield (p=0.008)", &layer0_on);
        bs_ui_checkbox("Nebula / dust (p=0.02)", &nebula_on);
        bs_ui_checkbox("Mid starfield (p=0.02)", &layer1_on);
        bs_ui_checkbox("Mapped system (p=0.30)", &layer2_on);
        s->render.bg_layer0_enabled = layer0_on ? TRUE : FALSE;
        s->render.bg_nebula_enabled = nebula_on ? TRUE : FALSE;
        s->render.bg_layer1_enabled = layer1_on ? TRUE : FALSE;
        bool planets_on     = s->render.celestial_draw_planets ? true : false;
        bool testsprites_on = s->render.celestial_draw_testsprites ? true : false;
        bs_ui_checkbox("Planets + orbit rings", &planets_on);
        bs_ui_checkbox("Test sprites (demo dots)", &testsprites_on);
        s->render.celestial_draw_planets     = planets_on ? TRUE : FALSE;
        s->render.celestial_draw_testsprites = testsprites_on ? TRUE : FALSE;
        s->render.bg_layer2_enabled = layer2_on ? TRUE : FALSE;
        // Depth-based parallax for the celestial backdrop (stars/planets/orbits/test sprites).
        // depth 0 = foreground (1:1 with camera), depth 1 = fully locked to the shared anchor. Keep
        // co-located pairs matched: planets == orbit lines, test sprites == star.
        bool parallax_on = s->render.bg_parallax_enabled ? true : false;
        bs_ui_checkbox("Depth parallax (celestial)", &parallax_on);
        s->render.bg_parallax_enabled = parallax_on ? TRUE : FALSE;
        bs_ui_slider_float("Depth: star", &s->render.depth_star, 0.0f, 1.0f);
        bs_ui_slider_float("Depth: planets", &s->render.depth_planet, 0.0f, 1.0f);
        bs_ui_slider_float("Depth: orbit lines", &s->render.depth_orbit, 0.0f, 1.0f);
        bs_ui_slider_float("Depth: test sprites", &s->render.depth_testsprite, 0.0f, 1.0f);
        bs_ui_slider_float("Parallax fade zoom", &s->render.bg_parallax_fade_zoom, 0.0f, 0.30f);
        bs_ui_separator();
        const f32 UC[4] = { 0.45f, 0.90f, 0.95f, 1.0f };
        bs_ui_text_colored(UC[0], UC[1], UC[2], UC[3], "COORDINATE SPACE (STEP 1)");
        bool coord_diag_on = coord_diag_is_enabled() ? true : false;
        bs_ui_checkbox("Log coord diagnostics (bin/coord_diag.txt)", &coord_diag_on);
        coord_diag_set_enabled(coord_diag_on ? TRUE : FALSE);
        if (coord_diag_on) {
            char cd[64];
            snprintf(cd, sizeof(cd), "Invariant violations (last dump): %d", coord_diag_last_violations());
            f32 vr = coord_diag_last_violations() > 0 ? 0.95f : 0.45f;
            f32 vg = coord_diag_last_violations() > 0 ? 0.35f : 0.90f;
            bs_ui_text_colored(vr, vg, 0.40f, 1.0f, cd);
        }
        bool cell_grid_on = g_debug_cell_grid ? true : false;
        bs_ui_checkbox("Draw HierPos2 cell grid (global)", &cell_grid_on);
        g_debug_cell_grid = cell_grid_on ? TRUE : FALSE;
        bs_ui_separator();
        const f32 NEB[4] = { 0.85f, 0.55f, 0.95f, 1.0f };
        bs_ui_text_colored(NEB[0], NEB[1], NEB[2], NEB[3], "NEBULA");
        bs_ui_slider_float("Intensity", &s->render.nebula_intensity, 0.0f, 1.0f);
        bs_ui_slider_float("Dust intensity", &s->render.nebula_dust_intensity, 0.0f, 1.0f);
        bs_ui_slider_float("Gas brightness", &s->render.nebula_gas_brightness_mul, 0.0f, 3.0f);
        bs_ui_slider_float("Highlight power", &s->render.nebula_highlight_power, 0.0f, 3.0f);
        bs_ui_slider_float("Palette shift", &s->render.nebula_palette_shift, 0.0f, 1.0f);
        bs_ui_slider_float("Warp strength", &s->render.nebula_swirl_strength, 0.0f, 2.0f);
        bs_ui_slider_float("Region density", &s->render.nebula_falloff_radius, 0.0f, 1.0f);
        bs_ui_slider_float("Filament sharpness", &s->render.nebula_band_strength, 0.0f, 1.0f);
        bs_ui_slider_float("LOD feature scale", &s->render.nebula_lod_target, 500.0f, 12000.0f);
        bs_ui_slider_float("Parallax", &s->render.nebula_parallax, 0.0f, 0.5f);
        bs_ui_slider_float("Biome strength", &s->render.nebula_biome_strength, 0.0f, 1.0f);
        bs_ui_slider_float("Biome scale", &s->render.nebula_biome_scale, 10000.0f, 600000.0f);
        bs_ui_slider_float("Biome hue spread", &s->render.nebula_biome_hue_spread, 0.0f, 1.0f);
        bs_ui_slider_float("Zoom detail", &s->render.nebula_zoom_detail, 0.0f, 1.0f);
        bs_ui_slider_float("Zoom saturation", &s->render.nebula_zoom_saturation, 0.0f, 1.0f);
        f32 col_a[3] = { s->render.nebula_gas_color_a.r, s->render.nebula_gas_color_a.g, s->render.nebula_gas_color_a.b };
        if (bs_ui_color_edit3("Base gas##nebula", col_a)) {
            s->render.nebula_gas_color_a.r = col_a[0]; s->render.nebula_gas_color_a.g = col_a[1]; s->render.nebula_gas_color_a.b = col_a[2];
        }
        f32 col_b[3] = { s->render.nebula_gas_color_b.r, s->render.nebula_gas_color_b.g, s->render.nebula_gas_color_b.b };
        if (bs_ui_color_edit3("Mid gas##nebula", col_b)) {
            s->render.nebula_gas_color_b.r = col_b[0]; s->render.nebula_gas_color_b.g = col_b[1]; s->render.nebula_gas_color_b.b = col_b[2];
        }
        f32 col_c[3] = { s->render.nebula_gas_color_c.r, s->render.nebula_gas_color_c.g, s->render.nebula_gas_color_c.b };
        if (bs_ui_color_edit3("Highlight gas##nebula", col_c)) {
            s->render.nebula_gas_color_c.r = col_c[0]; s->render.nebula_gas_color_c.g = col_c[1]; s->render.nebula_gas_color_c.b = col_c[2];
        }
        f32 col_d[3] = { s->render.nebula_dust_color.r, s->render.nebula_dust_color.g, s->render.nebula_dust_color.b };
        if (bs_ui_color_edit3("Dust##nebula", col_d)) {
            s->render.nebula_dust_color.r = col_d[0]; s->render.nebula_dust_color.g = col_d[1]; s->render.nebula_dust_color.b = col_d[2];
        }
        bs_ui_separator();
        const f32 CAM[4] = { 0.55f, 0.95f, 0.75f, 1.0f };
        bs_ui_text_colored(CAM[0], CAM[1], CAM[2], CAM[3], "CAMERA");
        bs_ui_slider_float("Zoom smoothing", &s->camera_state.zoom_smooth_rate, 4.0f, 40.0f);
        bs_ui_separator();
        const f32 SF[4] = { 0.55f, 0.85f, 0.95f, 1.0f };
        bs_ui_text_colored(SF[0], SF[1], SF[2], SF[3], "STARFIELD LOD");
        bs_ui_slider_float("Density", &s->render.starfield_lod_density, 0.0f, 0.5f);
        bs_ui_slider_float("Size", &s->render.starfield_lod_size, 0.25f, 3.0f);
        bs_ui_slider_float("Brightness", &s->render.starfield_lod_brightness, 0.0f, 3.0f);
        bs_ui_slider_float("LOD target px", &s->render.starfield_lod_target_px, 12.0f, 120.0f);
        bs_ui_slider_float("LOD levels", &s->render.starfield_lod_levels, 1.0f, 6.0f);
        bs_ui_slider_float("LOD factor", &s->render.starfield_lod_factor, 2.0f, 8.0f);
        bs_ui_slider_float("Parallax near", &s->render.starfield_parallax_near, 0.0f, 1.0f);
        bs_ui_slider_float("Parallax falloff", &s->render.starfield_parallax_falloff, 0.2f, 1.0f);
        bs_ui_separator();
        bs_ui_text_colored(SF[0], SF[1], SF[2], SF[3], "STAR DAZZLE");
        bs_ui_slider_float("Inner radius", &s->render.star_dazzle_inner_radius, 0.0f, 50000.0f);
        bs_ui_slider_float("Outer radius", &s->render.star_dazzle_outer_radius, 0.0f, 100000.0f);
        bs_ui_slider_float("Intensity", &s->render.star_dazzle_intensity, 0.0f, 1.0f);
        bool star_light_on = s->render.star_light_enabled ? true : false;
        bs_ui_checkbox("Star volumetric light", &star_light_on);
        s->render.star_light_enabled = star_light_on ? TRUE : FALSE;
        if (s->render.star_light_enabled) {
            bs_ui_slider_float("Star light intensity", &s->render.star_light_intensity_mul, 0.0f, 4.0f);
            bs_ui_slider_float("Star light radius",    &s->render.star_light_radius_mul, 0.1f, 4.0f);
        }
        s->render.star_fx.build_ui();
        // Opens the free-floating per-type Planet Editor window (movable/resizable).
        if (bs_ui_button("Planet Properties...", TRUE))
            s->render.star_fx.show_planet_editor = s->render.star_fx.show_planet_editor ? FALSE : TRUE;
        // Opens the System Inspector (evolved bodies + chronicle of the current system).
        if (bs_ui_button("System Inspector...", TRUE))
            s->galaxy.show_system_inspector = !s->galaxy.show_system_inspector;
        // ---- TRAVEL ----------------------------------------------------------------------------
        bs_ui_separator();
        const f32 TR[4] = { 0.55f, 0.95f, 0.75f, 1.0f };
        bs_ui_text_colored(TR[0], TR[1], TR[2], TR[3], "TRAVEL");
        b8 was_travel = s->travel_enabled;
        bool travel_on = s->travel_enabled ? true : false;
        bs_ui_checkbox("Enable Continuous Travel", &travel_on);
        s->travel_enabled = travel_on ? TRUE : FALSE;
        if (s->travel_enabled != was_travel)
            action_log_push(s, "Continuous travel %s", s->travel_enabled ? "enabled" : "disabled");
        if (s->travel_enabled) {
            // Auto-init travel on first enable if not already active.
            if (!s->travel.active && s->travel.progress == 0.0f) {
                travel_init(&s->travel, s->player_ship().origin, Vec2{ 50000.0f, 0.0f });
            }
            bool paused = s->travel_paused ? true : false;
            bs_ui_checkbox("Pause Travel", &paused);
            s->travel_paused = paused ? TRUE : FALSE;
            bs_ui_slider_float("Speed", &s->travel.speed, 0.01f, 2.0f);
            i32 ease_idx = (i32)s->travel.ease_mode;
            if (bs_ui_combo("Ease Mode", &ease_idx, "Linear\0Smoothstep\0Quad In/Out\0")) {
                if (ease_idx >= 0 && ease_idx < TRAVEL_EASE_COUNT)
                    s->travel.ease_mode = (TravelEaseMode)ease_idx;
            }
            bs_ui_separator();
            char prog_buf[48];
            snprintf(prog_buf, sizeof(prog_buf), "Progress: %.1f%%", s->travel.progress * 100.0f);
            bs_ui_text(prog_buf);
            bs_ui_text("Hierarchical Position:");
            char cell_buf[48];
            snprintf(cell_buf, sizeof(cell_buf), "  Cell: (%lld, %lld)",
                     s->travel.current.cell.x, s->travel.current.cell.y);
            bs_ui_text(cell_buf);
            char local_buf[48];
            snprintf(local_buf, sizeof(local_buf), "  Local: (%.1f, %.1f)",
                     s->travel.current.local.x, s->travel.current.local.y);
            bs_ui_text(local_buf);
            char world_buf[64];
            snprintf(world_buf, sizeof(world_buf), "  World: (%.1f, %.1f)",
                     (f32)s->travel.world_x, (f32)s->travel.world_y);
            bs_ui_text(world_buf);
            // Distance to final destination.
            f64 dest_x, dest_y;
            bs_math::hierpos_to_f64(&s->travel.destination, BS_HIERPOS_CELL_SIZE, &dest_x, &dest_y);
            f64 dx = dest_x - s->travel.world_x;
            f64 dy = dest_y - s->travel.world_y;
            f64 dist = sqrt(dx * dx + dy * dy);
            char dist_buf[48];
            snprintf(dist_buf, sizeof(dist_buf), "  Dist to dest: %.1f", (f32)dist);
            bs_ui_text(dist_buf);
            if (bs_ui_button("Reset Travel", s->travel.progress > 0.0f ? TRUE : FALSE)) {
                travel_reset(&s->travel);
            }
        }
        // ---- SYSTEM VIEW -----------------------------------------------------------------------
        bs_ui_separator();
        const f32 SV[4] = { 0.95f, 0.55f, 0.35f, 1.0f };
        bs_ui_text_colored(SV[0], SV[1], SV[2], SV[3], "SYSTEM VIEW");
        bs_ui_checkbox("Animate scale",     &s->galaxy.map_anim_scale);
        bs_ui_checkbox("Animate rotation",  &s->galaxy.map_anim_rotate);
        bs_ui_checkbox("Animate alpha",     &s->galaxy.map_anim_alpha);
        bs_ui_checkbox("Animate thickness", &s->galaxy.map_anim_thickness);
        bs_ui_checkbox("Draw hyperjump range", &s->galaxy.map_draw_jump_range);
        if (s->galaxy.map_draw_jump_range) {
            bs_ui_slider_float("Range (units)", &s->galaxy.map_jump_range, 0.0f, 40000000.0f);
        }
        bs_ui_checkbox("Draw sensor range", &s->galaxy.map_draw_sensor_range);
        if (s->galaxy.map_draw_sensor_range) {
            bs_ui_slider_float("Sensor range (units)", &s->galaxy.map_sensor_range, 10000.0f, 500000.0f);
        }
        // Natural AI trader speeds (u/s of simulation; 1 real second == 1 in-game hour at 1x).
        bs_ui_slider_float("AI in-system speed (u/s)", &s->galaxy.ai_speed_in_system, 1000.0f, 1000000.0f);
        bs_ui_slider_float("AI jump speed (u/s)",      &s->galaxy.ai_speed_jump,      10000.0f, 100000000.0f);
        bs_ui_checkbox("Draw system lanes", &s->galaxy.map_draw_lanes);
        // War room (G): objective-coded mission glyphs, red war-frontier lanes, garrison/risk labels.
        bs_ui_checkbox("War room overlay [G]", &s->galaxy.map_war_room);
        // Progressive zoom-out speed: higher = each wheel notch covers more zoom the further out you are.
        bs_ui_slider_float("Zoom-out speed gain", &g_zoom_out_speed_gain, 0.0f, 5.0f);
        bool show_mb = (bool)s->show_metaball_ui;
        bs_ui_slider_float("Ship sensor range (units)", &s->ship_sensor_range, 0.0f, 8000000.0f);
        // ---- Three-layer sensor overlay radii (Layer 0 < Layer 1 < Layer 2, press V) --------
        // The sliders edit the hull BASELINE (sensors_base); the effective suite is re-derived
        // per ship by ship_recompute_stats so mounted sensor modules keep their multipliers.
        {
            SensorSuite& sen = s->player_ship().sensors_base;
            const f32 GAP = 1000.0f; // enforced minimum separation between adjacent layers
            f32 l0 = sen.layer0_radius, l1 = sen.layer1_radius, l2 = sen.layer2_radius;
            b8 changed = FALSE;
            // Dynamic bounds keep l0 < l1 < l2: each slider is clamped by its neighbours.
            changed |= bs_ui_slider_float("Sensor Layer 0 radius", &l0, 1000.0f,   l1 - GAP);
            changed |= bs_ui_slider_float("Sensor Layer 1 radius", &l1, l0 + GAP,  l2 - GAP);
            changed |= bs_ui_slider_float("Sensor Layer 2 falloff", &l2, l1 + GAP,  12000000.0f);
            if (changed) {
                // Re-assert ordering in case a typed value momentarily broke the bounds.
                if (l1 < l0 + GAP) l1 = l0 + GAP;
                if (l2 < l1 + GAP) l2 = l1 + GAP;
                sen.layer0_radius = l0; sen.layer1_radius = l1; sen.layer2_radius = l2;
                // Propagate the baseline to the whole fleet, then recompose each ship's
                // effective sensors from baseline x its own mounted modules.
                Fleet& fleet = s->fleet_state.fleet;
                for (i32 i = 0; i < fleet.count(); ++i) {
                    fleet.at(i).ship.sensors_base = sen;
                    ship_recompute_stats(&fleet.at(i).ship);
                }
            }
        }
        // Unbounded Layer 2: distance (world units) at which distant contact blips fade to the dim floor.
        bs_ui_slider_float("Sensor blip fade distance", &g_sensor_fade_distance, 500000.0f, 20000000.0f);
        // ---- Point-defense laser (auto-intercepts incoming hostile projectiles) --------------
        // Range is intentionally NOT a slider: it is live-coupled to each ship's Layer 1 radius
        // (DefenseLaser.range stays 0), so the Layer 1 slider above drives the laser range too.
        {
            DefenseLaser& pd = s->player_ship().point_defense;
            bool pd_on = (bool)pd.enabled;
            bs_ui_checkbox("Defense laser enabled", &pd_on);
            b8 changed = (pd.enabled != (b8)pd_on);
            pd.enabled = (b8)pd_on;
            changed |= bs_ui_slider_float("Laser DPS",            &pd.damage_per_second, 0.0f, 100.0f);
            changed |= bs_ui_slider_float("Laser dwell (s)",      &pd.dwell_time,        0.02f, 1.0f);
            changed |= bs_ui_slider_float("Laser retarget cd (s)", &pd.retarget_cooldown, 0.0f, 0.5f);
            char rng[64];
            snprintf(rng, sizeof(rng), "Laser range = Layer 0 (%.0f)", s->player_ship().sensors.layer0_radius);
            bs_ui_text(rng);
            if (changed) {
                // Propagate tuning (not runtime state) to the whole fleet; range stays 0/live-coupled.
                // `enabled` is deliberately NOT propagated: it now means "the fleet's PD device is
                // mounted on this hull" and belongs to the mount/unmount paths. The checkbox still
                // overrides the flagship's own flag as a dev switch; the simulation additionally
                // gates on the mount, so an unmounted hull stays inert regardless.
                Fleet& fleet = s->fleet_state.fleet;
                for (i32 i = 0; i < fleet.count(); ++i) {
                    DefenseLaser& d = fleet.at(i).ship.point_defense;
                    d.damage_per_second = pd.damage_per_second;
                    d.dwell_time        = pd.dwell_time;
                    d.retarget_cooldown = pd.retarget_cooldown;
                }
            }
        }
        // ---- SHIP AI TELEMETRY (Phase 7) -----------------------------------------------------
        // Read-only counters for the autonomous universe: NPC pool pressure, what the live agents
        // are actually doing, the macro mission mix per objective, and the civ economy that funds
        // it. This is the pass that tells you at a glance whether the sim is healthy or starving.
        {
            bs_ui_separator();
            bs_ui_text_colored(0.62f, 0.82f, 1.0f, 1.0f, "SHIP AI TELEMETRY");

            static const char* ARCH_NAME[7]  = { "patrol", "warship", "intercept", "trader", "scout", "pirate", "miner" };
            static const char* STATE_NAME[10] = { "patrol", "investig", "pursue", "attack", "evade",
                                                  "return", "dock", "docked", "leg", "deliver" };
            i32 arch_n[7]  = { 0, 0, 0, 0, 0, 0, 0 };
            i32 state_n[10] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
            i32 live = 0, undiscovered = 0;
            for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {
                const NpcShip& n = s->npc_ships[i];
                if (!n.active) continue;
                ++live;
                if (!n.discovered) ++undiscovered;
                if (n.archetype < 7) ++arch_n[n.archetype];
                if (n.state < 10)    ++state_n[n.state];
            }
            char buf[256];
            snprintf(buf, sizeof(buf), "NPC pool: %d / %d live (%d unidentified)", live, (i32)NPC_SHIP_MAX, undiscovered);
            bs_ui_text(buf);
            snprintf(buf, sizeof(buf), "hulls  %s %d  %s %d  %s %d  %s %d",
                     ARCH_NAME[0], arch_n[0], ARCH_NAME[1], arch_n[1], ARCH_NAME[2], arch_n[2], ARCH_NAME[4], arch_n[4]);
            bs_ui_text(buf);
            snprintf(buf, sizeof(buf), "       %s %d  %s %d  %s %d",
                     ARCH_NAME[3], arch_n[3], ARCH_NAME[6], arch_n[6], ARCH_NAME[5], arch_n[5]);
            bs_ui_text(buf);
            snprintf(buf, sizeof(buf), "states %s %d  %s %d  %s %d  %s %d  %s %d",
                     STATE_NAME[0], state_n[0], STATE_NAME[1], state_n[1], STATE_NAME[2], state_n[2],
                     STATE_NAME[3], state_n[3], STATE_NAME[4], state_n[4]);
            bs_ui_text(buf);
            snprintf(buf, sizeof(buf), "       %s %d  %s %d  %s %d  %s %d",
                     STATE_NAME[6], state_n[6], STATE_NAME[7], state_n[7], STATE_NAME[8], state_n[8],
                     STATE_NAME[9], state_n[9]);
            bs_ui_text(buf);

            // ---- Macro tier: mission mix + the Phase 6 economy that pays for it ----
            const auto& g = s->galaxy;
            i32 m_trade = 0, m_rein = 0, m_pat = 0, m_raid = 0, m_pirate = 0;
            i32 m_hull_wait = 0, m_escort = 0, m_bound = 0;
            for (i32 i = 0; i < g.mission_count; ++i) {
                const ShipMission& m = g.missions[i];
                if (!m.active) { if (m.awaiting_hull) ++m_hull_wait; continue; }
                if (m.ship_slot >= 0) ++m_bound;
                if (m.owner < 0) { ++m_pirate; continue; }
                switch (m.objective) {
                    case OBJ_TRADE:     ++m_trade; if (m.escorted) ++m_escort; break;
                    case OBJ_REINFORCE: ++m_rein;  break;
                    case OBJ_PATROL:    ++m_pat;   break;
                    case OBJ_RAID:      ++m_raid;  break;
                    default: break;
                }
            }
            snprintf(buf, sizeof(buf), "missions %d/%d  trade %d (esc %d)  reinf %d  patrol %d  raid %d  pirate %d",
                     g.mission_count, g.mission_capacity, m_trade, m_escort, m_rein, m_pat, m_raid, m_pirate);
            bs_ui_text(buf);
            snprintf(buf, sizeof(buf), "materialised %d   awaiting a replacement hull %d", m_bound, m_hull_wait);
            bs_ui_text(buf);

            f32 budget_total = 0.0f; i32 hulls_total = 0, alive_civs = 0;
            for (i32 c = 0; c < g.civ_count && c < GALAXY_CIV_MAX; ++c) {
                if (g.civs[c].status != 0) continue;
                ++alive_civs;
                budget_total += g.civs[c].mil_budget;
                hulls_total  += (i32)g.civs[c].hull_pool;
            }
            i32 risky = 0; f32 worst = 0.0f; i32 worst_node = -1;
            for (i32 i = 0; i < NODE_RISK_MAX; ++i) {
                if (g.node_risks[i].node < 0) continue;
                ++risky;
                if (g.node_risks[i].risk > worst) { worst = g.node_risks[i].risk; worst_node = g.node_risks[i].node; }
            }
            snprintf(buf, sizeof(buf), "economy  %d civs  war chests %.0f cr  hulls on the slips %d",
                     alive_civs, budget_total, hulls_total);
            bs_ui_text(buf);
            if (worst_node >= 0) snprintf(buf, sizeof(buf), "lane risk  %d node(s) marked, worst N%d (%.2f)", risky, worst_node, worst);
            else                 snprintf(buf, sizeof(buf), "lane risk  no nodes currently marked");
            bs_ui_text(buf);
        }

        // ---- HARDPOINT EDITOR (live .ship authoring gizmo) -----------------------------------
        // Edits the FLAGSHIP's hardpoint skeleton in place: nudge a slot's position/facing/arc
        // with sliders (or drop it under the cursor), watch the overlay update live, then dump
        // ready-to-paste `hardpoint` lines to the log for the .ship file.
        bs_ui_separator();
        const f32 HPC[4] = { 0.95f, 0.75f, 0.35f, 1.0f };
        bs_ui_text_colored(HPC[0], HPC[1], HPC[2], HPC[3], "HARDPOINT EDITOR");
        static bool hp_edit_on = false;
        static i32  hp_sel     = 0;
        static bool hp_snap    = false;
        bs_ui_checkbox("Edit flagship hardpoints", &hp_edit_on);
        if (hp_edit_on) {
            Ship& fship = s->player_ship();
            if (fship.hardpoint_count <= 0) {
                bs_ui_text("This hull has no hardpoints.");
            } else {
                if (hp_sel >= fship.hardpoint_count) hp_sel = fship.hardpoint_count - 1;
                if (hp_sel < 0) hp_sel = 0;
                if (bs_ui_button("< Prev", TRUE))
                    hp_sel = (hp_sel + fship.hardpoint_count - 1) % fship.hardpoint_count;
                bs_ui_same_line();
                if (bs_ui_button("Next >", TRUE))
                    hp_sel = (hp_sel + 1) % fship.hardpoint_count;
                HardpointDef& hp = fship.hardpoints[hp_sel];
                char acc[64];
                hardpoint_accepts_string(hp.accepts, acc, sizeof(acc));
                char hdr[128];
                snprintf(hdr, sizeof(hdr), "%d/%d  '%s'  %s  [%s]",
                         hp_sel + 1, fship.hardpoint_count, hp.id,
                         hardpoint_size_name(hp.size), acc);
                bs_ui_text(hdr);
                // Position sliders span the hull footprint (ship-local units == art pixels).
                f32 hw = fship.size_local.x * 0.5f;
                f32 hh = fship.size_local.y * 0.5f;
                bs_ui_slider_float("Pos X (px)", &hp.pos_local.x, -hw, hw);
                bs_ui_slider_float("Pos Y (px)", &hp.pos_local.y, -hh, hh);
                f32 fac_deg = hp.facing * BS_RAD2DEG;
                f32 arc_deg = hp.arc * BS_RAD2DEG;
                if (bs_ui_slider_float("Facing (deg)", &fac_deg, -180.0f, 180.0f))
                    hp.facing = fac_deg * BS_DEG2RAD;
                if (bs_ui_slider_float("Arc (deg)", &arc_deg, 0.0f, 360.0f))
                    hp.arc = arc_deg * BS_DEG2RAD;
                // Mount art size. Scales everything this slot draws -- authored turret art,
                // the procedural turret/dish rectangles, and the barrel origins with them, so
                // shots keep leaving the muzzle at any scale. Slot semantics are untouched:
                // an M slot still takes M modules and the selection box stays slot-sized.
                bs_ui_slider_float("Art scale", &hp.art_scale, 0.25f, 2.0f);
                bs_ui_same_line();
                if (bs_ui_button("Reset##artscale", TRUE)) hp.art_scale = 1.0f;
                // Cursor placement: the slot rides the mouse (inverse pose -> ship-local px)
                // until the checkbox is turned off.
                bs_ui_checkbox("Slot follows cursor", &hp_snap);
                if (hp_snap) {
                    Vec2 lp = ship_world_to_local(&fship, mouse_true_hierpos(s));
                    hp.pos_local = Vec2{ clampf(lp.x, -hw, hw), clampf(lp.y, -hh, hh) };
                }
                draw_hardpoint_highlight(&fship, hp_sel, s->elapsed_time);
                // The scene only draws the skeleton while the flagship inspector is open, so
                // keep it visible here whenever hardpoint editing is on.
                draw_hardpoint_overlay(&fship, 1.5f);
                if (bs_ui_button("Log .ship hardpoint lines", TRUE)) {
                    BS_LOG_INFO("---- flagship hardpoints (paste into the .ship file) ----");
                    for (i32 i = 0; i < fship.hardpoint_count; ++i) {
                        const HardpointDef& h = fship.hardpoints[i];
                        char a[64];
                        hardpoint_accepts_string(h.accepts, a, sizeof(a));
                        // The scale is emitted only when it differs from 1.0, so a hull whose
                        // mounts are all default-sized still dumps the original 7-field lines
                        // and the .ship file stays as terse as it was.
                        if (h.art_scale > 1.001f || h.art_scale < 0.999f) {
                            BS_LOG_INFO("hardpoint %s %s %s %.1f %.1f %.1f %.1f %.3f",
                                        h.id, a, hardpoint_size_name(h.size),
                                        h.pos_local.x, h.pos_local.y,
                                        h.facing * BS_RAD2DEG, h.arc * BS_RAD2DEG, h.art_scale);
                        } else {
                            BS_LOG_INFO("hardpoint %s %s %s %.1f %.1f %.1f %.1f",
                                        h.id, a, hardpoint_size_name(h.size),
                                        h.pos_local.x, h.pos_local.y,
                                        h.facing * BS_RAD2DEG, h.arc * BS_RAD2DEG);
                        }
                    }
                    action_log_push(s, "Hardpoint lines written to the log.");
                }
                bs_ui_text("Edits are live on the flagship only.");
            }
        } else {
            hp_snap = false;
        }
        bs_ui_text("Out-of-sensor FX:");
        bs_ui_slider_float("FX sweep speed", &s->out_sensor_fx.sweep_speed, 0.0f, 5.0f);
        bs_ui_slider_float("FX intensity", &s->out_sensor_fx.intensity, 0.0f, 2.0f);
        bs_ui_slider_float("FX radius scale", &s->out_sensor_fx.radius_scale, 0.1f, 2.0f);
        bs_ui_slider_float("FX glow intensity", &s->out_sensor_fx.overlay_glow.intensity, 0.0f, 4.0f);
        bs_ui_slider_float("FX glow falloff", &s->out_sensor_fx.overlay_glow.falloff, 0.5f, 10.0f);
        bs_ui_text("FX color:");
        bs_ui_slider_float("FX R", &s->out_sensor_fx.color.r, 0.0f, 1.0f);
        bs_ui_slider_float("FX G", &s->out_sensor_fx.color.g, 0.0f, 1.0f);
        bs_ui_slider_float("FX B", &s->out_sensor_fx.color.b, 0.0f, 1.0f);
        bs_ui_checkbox("Radiation detector", &show_mb);
        s->show_metaball_ui = (b8)show_mb;
        if (s->show_metaball_ui) {
            bs_ui_slider_float("BaseDetectionRadius", &s->base_detection_radius, 5000.0f, 50000.0f);
            bs_ui_slider_float("Heat signature radius", &s->heat_signature_radius, 0.0f, 50000.0f);
            bs_ui_slider_float("Threshold",           &s->metaball_threshold,     0.1f, 5.0f);
            bs_ui_slider_float("Heat map intensity",  &s->heat_map_intensity,     0.0f, 1.0f);
            bs_ui_slider_float("Tail length",         &s->heat_tail_length,       0.0f, 16.0f);
            bs_ui_slider_float("Tail fade",           &s->heat_tail_fade,         0.5f, 4.0f);
            bs_ui_slider_float("Heat warp",           &s->heat_warp_strength,     0.0f, 200.0f);
            bs_ui_slider_float("Venn sharpness",        &s->heat_map_venn_sharpness, 0.0f, 1.0f);
            const char* palette_names = "Rainbow\0Thermal\0Blackbody\0Custom\0";
            bs_ui_combo("Heat palette", &s->heat_palette, palette_names);
            if (s->heat_palette < 0) s->heat_palette = 0;
            if (s->heat_palette >= BS_HEAT_PALETTE_COUNT) s->heat_palette = BS_HEAT_PALETTE_COUNT - 1;
            bs_ui_color_edit3("Heat edge color",  &s->heat_color_low.r);
            bs_ui_color_edit3("Heat center color", &s->heat_color_high.r);
            bs_ui_slider_float("Color falloff power", &s->heat_color_falloff_power, 0.25f, 4.0f);
            char timing_buf[64];
            snprintf(timing_buf, sizeof(timing_buf), "Heat map CPU: %.2f ms", s->heat_map_cpu_ms);
            bs_ui_text(timing_buf);
            snprintf(timing_buf, sizeof(timing_buf), "Frame time: %.2f ms", s->frame_ms);
            bs_ui_text(timing_buf);
        }
        // Zoom is controlled by mouse wheel (scroller) in system view.
    }
    // ---- FLEET JUMP ----------------------------------------------------------------------
    bs_ui_separator();
    const f32 FJ[4] = { 0.55f, 0.95f, 0.65f, 1.0f };
    bs_ui_text_colored(FJ[0], FJ[1], FJ[2], FJ[3], "FLEET JUMP");
    {
        Fleet& fleet = s->fleet_state.fleet;
        f32 jr = (fleet.count() > 0) ? fleet.at(0).jump_radius : JUMP_RADIUS_DEFAULT;
        f32 jr_prev = jr;
        bs_ui_slider_float("Jump radius (units)", &jr, 10000.0f, 4000000000.0f);
        if (jr != jr_prev) fleet.set_all_jump_radius(jr);
        i32 center_idx = -1;
        f32 min_r = 0.0f;
        char buf[96];
        if (fleet.selected_min_jump(&center_idx, &min_r)) {
            snprintf(buf, sizeof(buf), "Selected: %d   Min radius: %.0f",
                     fleet.selected_count(), min_r);
        } else {
            snprintf(buf, sizeof(buf), "Selected: %d   (no jump-capable ship)",
                     fleet.selected_count());
        }
        bs_ui_text(buf);
    }
    bs_ui_end_panel();
}

// =====================================================================================
// Transform panel: standalone window showing the selected entity's name, world position,
// angle, and HierPos2 galaxy coordinates. Appears in edit mode when an entity is selected.
// =====================================================================================
void build_transform_panel(game_state* s) {
    if (!s->editor.edit_mode_active || s->editor.edit_selection.kind == EDIT_NONE) return;
    if (bs_ui_begin_panel("TRANSFORM", BS_UI_ANCHOR_TOP_RIGHT, 12.0f, BsUiType::BS_UI_TYPE_EDITOR)) {
        const f32 TF[4] = { 0.95f, 0.55f, 0.35f, 1.0f };
        bs_ui_text_colored(TF[0], TF[1], TF[2], TF[3], "TRANSFORM");
        bs_math::HierPos2 world_pos = bs_math::HierPos2{};
        f32   angle_deg = 0.0f;
        const char* name = "?";
        if (s->editor.edit_selection.kind == EDIT_SHIP) {
            const Ship* sh = (s->editor.edit_selection.index == 0) ? &s->player_ship() : &s->fleet_state.enemy_ship;
            world_pos = sh->origin;
            angle_deg = sh->angle * (180.0f / 3.14159265f);
            name = (sh->vessel_name && sh->vessel_name[0]) ? sh->vessel_name
                 : (s->editor.edit_selection.index == 0) ? "Player Ship" : "Enemy Ship";
        } else if (s->editor.edit_selection.kind == EDIT_LIGHT) {
            const bs_light2d& L = s->render.lights[s->editor.edit_selection.index];
            world_pos = hierpos_from_vec2(L.position, BS_HIERPOS_CELL_SIZE);
            name = "Light";
        }
        // Entity name
        char name_buf[64];
        snprintf(name_buf, sizeof(name_buf), "Name: %s", name);
        bs_ui_text(name_buf);
        // World position (read-only text)
        char pos_buf[64];
        f64 wpx, wpy; hierpos_to_f64(&world_pos, BS_HIERPOS_CELL_SIZE, &wpx, &wpy);
        snprintf(pos_buf, sizeof(pos_buf), "Position: %.1f, %.1f", wpx, wpy);
        bs_ui_text(pos_buf);
        // Angle (ships only)
        if (s->editor.edit_selection.kind == EDIT_SHIP) {
            char ang_buf[48];
            snprintf(ang_buf, sizeof(ang_buf), "Angle: %.1f deg", angle_deg);
            bs_ui_text(ang_buf);
        }
        // HierPos2 galaxy coordinates
        bs_math::HierPos2 gal = world_pos;
        char cell_buf[64];
        snprintf(cell_buf, sizeof(cell_buf), "Sector: %lld, %lld", gal.cell.x, gal.cell.y);
        bs_ui_text(cell_buf);
        char local_buf[64];
        snprintf(local_buf, sizeof(local_buf), "Local: %.1f, %.1f", gal.local.x, gal.local.y);
        bs_ui_text(local_buf);
        bs_ui_separator();
        if (bs_ui_button("Deselect", TRUE)) {
            s->editor.edit_selection = EditSelection{ EDIT_NONE, -1 };
            s->editor.edit_drag.active = FALSE;
            s->editor.edit_drag.mode   = EDIT_DRAG_NONE;
        }
    }
    bs_ui_end_panel();
}

// PROFILER panel -- bottom-left, collapsible per-subsystem CPU timing readout (Profiler system).
void build_profiler_panel(game_state* s) {
    if (bs_ui_begin_panel("PROFILER", BS_UI_ANCHOR_BOTTOM_LEFT, 12.0f, BsUiType::BS_UI_TYPE_GAME)) {
        s->profiler.build_ui();
    }
    bs_ui_end_panel();
}
