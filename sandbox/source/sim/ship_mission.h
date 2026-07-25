#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>

struct game_state;
struct ShipMission;

// =====================================================================================
// ship_mission.h — cross-system Ship AI: the MACRO traveler tier (Step 2).
//
// A ShipMission (defined in state/game_state.h) is a persistent, player-independent agent that
// walks the galaxy lane graph toward a simple objective. This module owns their motion: it seeds
// a handful of debug travelers at generation time and advances them continuously along their lane
// routes on the shared sim clock. Rendering (moving map pips) lives in render/galaxy_map_render.cpp
// and reuses ship_mission_position(); materialisation into live NpcShip agents is a later step.
//
// The macro tier is deliberately separate from the local live-agent tier (sim/ai_ship.*): macro
// travelers exist everywhere at all times and move in in-game hours, whereas NpcShips are transient
// and only exist in the player's current system.
// =====================================================================================

// Deterministically spawn the initial pool of cross-system travelers between owned nodes, routing
// each toward a first destination. Call once at generation end (after garrison seeding), when the
// galaxy node/lane graph and civ ownership are finalised. No-op if the mission pool is unallocated.
void ship_missions_seed(game_state* s);

// Advance every active mission through its natural-AI stage machine (fly to station -> load ->
// fly to the jump circle -> jump between systems -> cross intermediate systems -> dock at the
// market). Speeds come from GalaxyState::ai_speed_in_system / ai_speed_jump (u/s; 1 real second
// == 1 in-game hour at 1x, so sim_dt_hours doubles as seconds). Call each frame after the galaxy
// history tick. No-op when paused (sim_dt_hours <= 0).
void ship_missions_update(game_state* s, f32 sim_dt_hours);

// World position of a mission right now: the relevant station anchor during dock/cooldown stages,
// else the continuously-integrated ShipMission::pos. Pure read; used by rendering and (later)
// live-agent materialisation.
bs_math::HierPos2 ship_mission_position(const game_state* s, const ShipMission& m);

// Step 4 writeback: the live NpcShip form of mission `mission_id` was destroyed in the local arena.
// Permanently retires the macro mission (frees its pool slot) so it no longer travels or renders.
// No-op for an out-of-range or already-inactive id.
void ship_mission_notify_destroyed(game_state* s, i32 mission_id);
