#pragma once
#include <defines.h>

struct game_state;

// Detector radius (world units) of the static enemy: it opens fire on the flagship only while
// the flagship is within this range. Shared by the firing AI and the overlay danger ring.
// Set well beyond the flagship's 50k sensor layer so the enemy actually fires and its
// projectiles sweep inward across Layer 2 -> Layer 1 -> Layer 0 for detection testing.
#define ENEMY_DETECTOR_RADIUS 120000.0f

// Combat-arena subsystem: the projectile pool, the combat-entity mirror of the fleet + enemy,
// and the proximity "encounter" trigger. This owns the SIMULATION half only. Rendering stays
// split across render/ship_scene.cpp (combat-entity quads + enemy sensor FX), sim/heat_map.cpp
// (radiation metaballs) and render/game_hud.cpp (the encounter modal). Weapon firing stays in
// game_update's input path.

// One-time init (game_init): init the projectile pool, register the enemy ship + every fleet
// ship as combat entities, and set the sensor / heat-signature / encounter tunables.
void combat_arena_init(game_state* s);

// Rebuild the persistent (non-NPC) combat-entity window: enemy ship (slot 0) + the ACTIVE fleet
// ships, and recompute npc_combat_base. Call after the active fleet count changes (e.g. the
// multi-ship editor toggle) so the NPC window stays packed against the player window.
void combat_arena_rebuild_player_entities(game_state* s);

// Per-frame (game_update): proximity encounter detection. Flags the encounter panel and pushes an
// action-log line when the enemy first comes into reach (the sim keeps running — no auto-pause).
void combat_arena_update_encounter(game_state* s);

// Per-frame (game_update, sim_dt): hardcoded demo motion — enemy ship orbits the flagship.
void combat_arena_update_enemy_orbit(game_state* s, f32 dt);

// Per-frame (game_update, sim_dt): STATIC enemy AI. The enemy never translates; it rotates to
// track the flagship (turret) and fires its weapon periodically while the flagship is inside
// ENEMY_DETECTOR_RADIUS. Replaces the orbit demo.
void combat_arena_update_enemy_ai(game_state* s, f32 dt);

// Per-frame (game_update, sim_dt): mirror each combat entity's position/velocity from its ship,
// scale drone heat by speed, and push the heat-map position trail. Run AFTER fleet integration.
void combat_arena_sync_entities(game_state* s, f32 sim_dt);

// Per-frame (game_update, sim_dt): advance projectiles and resolve projectile vs combat-entity
// collisions. Run AFTER combat_arena_sync_entities so entity positions are current.
void combat_arena_update_projectiles(game_state* s, f32 sim_dt);
