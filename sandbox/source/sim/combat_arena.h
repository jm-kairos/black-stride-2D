#pragma once
#include <defines.h>

struct game_state;

// Combat-arena subsystem: the projectile pool, the combat-entity mirror of the fleet + enemy,
// and the proximity "encounter" trigger. This owns the SIMULATION half only. Rendering stays
// split across render/ship_scene.cpp (combat-entity quads + enemy sensor FX), sim/heat_map.cpp
// (radiation metaballs) and render/game_hud.cpp (the encounter modal). Weapon firing stays in
// game_update's input path.

// One-time init (game_init): init the projectile pool, register the enemy ship + every fleet
// ship as combat entities, and set the sensor / heat-signature / encounter tunables.
void combat_arena_init(game_state* s);

// Per-frame (game_update, real dt independent of pause): proximity encounter detection. Pauses
// the sim (time_scale = 0) and pushes an action-log line when the enemy first comes into reach.
void combat_arena_update_encounter(game_state* s);

// Per-frame (game_update, sim_dt): hardcoded demo motion — enemy ship orbits the flagship.
void combat_arena_update_enemy_orbit(game_state* s, f32 dt);

// Per-frame (game_update, sim_dt): mirror each combat entity's position/velocity from its ship,
// scale drone heat by speed, and push the heat-map position trail. Run AFTER fleet integration.
void combat_arena_sync_entities(game_state* s, f32 sim_dt);

// Per-frame (game_update, sim_dt): advance projectiles and resolve projectile vs combat-entity
// collisions. Run AFTER combat_arena_sync_entities so entity positions are current.
void combat_arena_update_projectiles(game_state* s, f32 sim_dt);
