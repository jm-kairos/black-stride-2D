#pragma once

// ============================================================================================
// game_modules.h — aggregate include of every extracted gameplay/render/ui module header.
//
// This is the module-include cascade that used to live at the bottom of game.h. It is now
// included ONLY by game.cpp (the frame orchestrator that actually calls into all modules).
//
// Modules themselves must NOT rely on this aggregate: each module .cpp forward-declares
// `struct game_state;` (via game.h for the full definition) and includes the SPECIFIC peer
// module headers it calls. Keeping the cascade out of game.h means changing one module header
// no longer forces every translation unit that includes game.h to recompile, and it makes each
// module's real peer dependencies explicit instead of satisfied by a hidden transitive include.
// ============================================================================================

// Galaxy coordinate helpers (find_nearest_system / galaxy_to_system_local /
// world_to_galaxy_pos / get_system_zone).
#include "core/galaxy_coords.h"

// Galaxy-map subsystem: procedural generation + per-frame map upkeep (galaxy_map_init /
// galaxy_map_sync_entities / galaxy_map_update_orbits).
#include "sim/galaxy_map.h"

// Combat-arena subsystem: projectiles, combat entities, encounter detection (combat_arena_init /
// combat_arena_update_encounter / combat_arena_update_enemy_orbit / combat_arena_sync_entities /
// combat_arena_update_projectiles).
#include "sim/combat_arena.h"

// Rolling HUD action log data (action_log_push). The presentation panel is ui/action_log_panel.h.
#include "sim/action_log.h"

// Celestial depth parallax (celestial_center_render / celestial_shared_anchor).
#include "sim/celestial_parallax.h"
// Cosmetic zoom-space compression + the screen<->true-world<->render-space transforms
// (compression_factor, cosmetic_compress, game_compression_factor, view_arena_weight,
// game_screen_to_true_world/render, game_camera_center, and the HierPos2 variants).
#include "core/view_transform.h"
// DEBUG HierPos2 cell-grid overlay (draw_hierpos_cell_grid).
#include "render/debug_overlay.h"
// Mouse cursor -> world helpers (mouse_world / mouse_true_world / mouse_true_hierpos).
#include "core/cursor_world.h"
// Piloted-ship control + collision (control_ship_global / resolve_ship_collision / piloted_ship_origin).
#include "sim/ship_control.h"
// Pure 2D geometry helpers (point_in_polygon / point_to_segment).
#include "core/geom2d.h"
// In-world entity editor (update_edit_mode + gizmo geometry).
#include "sim/editor_tools.h"
// System/galaxy-map fleet emblems (draw_fleet_emblems).
#include "render/ship_render.h"
// Arena-side HUD overlays (draw_encounter_panel / draw_nav_ship_hud).
#include "render/game_hud.h"
// In-world gameplay overlays (draw_gameplay_overlays).
#include "render/gameplay_overlays.h"
// Galaxy-map look render pass (draw_galaxy_map_look).
#include "render/galaxy_map_render.h"
// Per-frame lighting assembly + submission (submit_frame_lighting).
#include "render/frame_lighting.h"
// Procedural parallax background pass (draw_parallax_background).
#include "render/parallax_background.h"
// Ship-rendering pass (draw_ship_scene).
#include "render/ship_scene.h"
// Scene render orchestrator: owns the ordered world-drawing passes (render_scene).
#include "render/scene_renderer.h"
// Radiation heat map (draw_ship_metaballs).
#include "sim/heat_map.h"
// Camera zoom + view-mode controller (update_zoom_and_mode).
#include "sim/camera_controller.h"
// Editor / debug UI panels (build_editor_panel / build_transform_panel / draw_time_control_panel / build_profiler_panel).
#include "ui/editor_ui.h"
// Action Log HUD panel (build_action_log_panel) — presentation half of sim/action_log.
#include "ui/action_log_panel.h"
