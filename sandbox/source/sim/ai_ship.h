#pragma once
#include <defines.h>

struct game_state;

// =====================================================================================
// General Ship AI (NPC agents).
//
// NPC ships are live, moving, Ship-backed agents (see NpcShip in state/game_state.h) driven by a
// small behavior state machine + steering, parameterised by a per-archetype AiProfile. The same
// code governs every ship type; only the profile data (and which FSM states are enabled) differ,
// so new ship kinds are added as data rows -- no new AI code.
//
// Phase A (this file's first cut): a bounded population manager garrisons the player's CURRENT
// owned system with a few agents that ORBIT the home star and fire on the player only when their
// civilization is hostile. Phases B/C add the full FSM (pursue/attack/return/evade/investigate),
// shared steering, and archetype variety.
// =====================================================================================

enum ShipArchetype : u8 {
    ARCHETYPE_PATROL = 0,   // system garrison: orbits home, engages hostiles
    ARCHETYPE_WARSHIP,      // (Phase C) aggressive line combatant
    ARCHETYPE_INTERCEPTOR,  // (Phase C) fast pursuer
    ARCHETYPE_TRADER,       // civilian hauler: loiters / routes, does not attack
    ARCHETYPE_SCOUT,        // (Phase C) shadows + flees
    ARCHETYPE_PIRATE,       // (Phase C) aggressive in wild space
    ARCHETYPE_MINER,        // civilian miner: works asteroid belts (mines per-system asteroids)
    ARCHETYPE_COUNT
};

// Behavior FSM states (Phase A uses PATROL + a lightweight ATTACK-in-place; B/C use the rest).
enum AiState : u8 {
    AI_PATROL = 0,   // loiter / orbit home (or cycle planet waypoints)
    AI_INVESTIGATE,  // move to a last-known contact position
    AI_PURSUE,       // close on a target
    AI_ATTACK,       // hold standoff and fire
    AI_EVADE,        // flee when badly hurt
    AI_RETURN,       // return to home when leashed too far
    AI_TRADE_DOCK,   // trader bound to a contract: fly to the contract's station
    AI_TRADE_DOCKED  // trader bound to a contract: halted at the station, loading/unloading
};

// Per-archetype tunables: the single knob that makes different ship types behave differently with
// the same code. Distances are world units, speeds units/s, angles radians.
struct AiProfile {
    f32 max_speed;      // linear speed cap
    f32 accel;          // thrust magnitude
    f32 turn_rate;      // rad/s nose slew
    f32 sensor_range;   // perception radius (Phase B)
    f32 engage_range;   // opens fire within this
    f32 standoff;       // preferred distance from target while attacking
    f32 min_range;      // never fire closer than this
    f32 patrol_speed;   // cruise speed while patrolling
    f32 patrol_radius;  // orbit radius around home
    f32 aggression;     // 0..1 willingness to leave patrol and engage (Phase B)
    f32 flee_hp_frac;   // flee below this fraction of max_hp (Phase C)
    f32 leash_range;    // return home beyond this distance from home (Phase B)
    f32 fire_period;    // seconds between shots
    u16 enabled_states; // bitmask of AiState the archetype may enter (Phase B/C)
};

const AiProfile& ai_profile(u8 archetype);

// Lifecycle / per-frame.
void ai_ships_init(game_state* s);                 // load the hull template, clear the pool
void ai_ships_update(game_state* s, f32 dt);        // population manager + perceive/think/steer/fire
void ai_ships_register_combat(game_state* s);       // rebuild the NPC window in combat_entities[]

// Apply damage to the NPC backing combat_entities[].npc_index; destroys + raids the civ at <=0 hp.
void ai_ship_damage(game_state* s, i32 npc_index, f32 dmg);
