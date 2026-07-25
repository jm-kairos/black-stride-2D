#pragma once
#include <defines.h>

struct game_state;

// =====================================================================================
// Single-ship discovery system.
//
// On first entry to a system, NPC garrison ships and per-system stations render as generic
// "unidentified" markers (see render/ship_scene.cpp, render/gameplay_overlays.cpp and
// render/system_content_render.cpp). As the player's ship closes within its Ship::sensors.layer1_radius
// of such an object it becomes discovered: rendered normally, appended to the Discoveries browser
// feed, and remembered so re-visits show it identified.
//
// Persistence:
//   - NPC agents: keyed by (home_node, spawn_seed) in game_state.npc_discovered[].
//   - Per-system stations: keyed by SystemStation::discovered (persists with the StarSystem cache).
// =====================================================================================

// Discovery now uses the ship's existing Layer 1 sensor radius (Ship::sensors.layer1_radius),
// which is the identification range. It is no longer a separate constant or a dedicated field.

// TRUE if the NPC agent identified by (home_node, seed) has already been discovered this run.
b8 discovery_npc_is_known(const game_state* s, i32 home_node, u64 seed);

// Per-frame (game_update): scan the player ship's surroundings and discover any close-enough NPC
// agents and per-system stations, logging each newly discovered object.
void discovery_update(game_state* s, f32 dt);

// Append an entry to the Discoveries browser feed (oldest rolls off at capacity). Also pushes a
// transient Action Log line for immediate HUD feedback.
void discovery_log_push(game_state* s, const char* name, const char* system, u8 kind);
