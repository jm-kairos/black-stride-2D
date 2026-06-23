#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include "ship.h"
// =====================================================================================
// Fleet system.
//
// A Fleet is a container of player-controlled ships. Each FleetShip owns its own rigid-body
// pose (Ship), inertial flight dynamics (ShipFlight), selection flag, and a per-ship order
// (move / attack). The RTS control layer selects subsets of the fleet (box / click) and
// issues orders; the fleet's autopilot drives each ordered ship toward its target while the
// player may still manually pilot one ship at a time.
//
// Member 0 is the FLAGSHIP (the historical single "player ship"); the rest are spawned
// alongside it. The ships live in a fixed array so Ship* pointers handed to the combat /
// render systems stay stable for the lifetime of the fleet.
// =====================================================================================
#define FLEET_MAX_SHIPS 8
// Forward declaration: game_state is defined in game.h (autopilot needs projectiles +
// combat entities for attack orders).
struct game_state;
// Global-mode inertial flight dynamics (Starsector-style). The ship's POSE (origin, angle)
// lives in Ship; these are the integrators that drive it. The ship coasts -- there is no
// passive linear drag, only the active brake (C) / reverse (S).
struct ShipFlight {
    bs_math::Vec2 velocity;          // world-space linear velocity
    f32           angular_velocity;  // rad/s, CCW positive; auto-stabilizes when A/D released
};
// What a single fleet ship is currently doing under RTS command.
enum class FleetOrder {
    None = 0,
    Move,    // autopilot to move_target (formation slot)
    Attack,  // pursue + fire on attack_target
};
// One player ship: pose + dynamics + selection + per-ship order state.
struct FleetShip {
    Ship          ship;
    ShipFlight    flight;
    b8            selected;
    FleetOrder    order;
    bs_math::Vec2 move_target;     // world-space formation slot (Move)
    Ship*         attack_target;   // validated against combat entities each frame (Attack)
    // Integrate the rigid-body pose from the flight velocities. Mirrors the legacy
    // simulate_ship: auto-stabilizes residual spin when turn_commanded is FALSE.
    void simulate(f32 dt, b8 turn_commanded);
    // Autopilot a Move order: rotate-to-face then thrust/brake toward move_target.
    void update_move(f32 dt);
    // Autopilot an Attack order: approach to engagement range and fire on attack_target.
    void update_attack(game_state* s, f32 dt);
};
class Fleet {
public:
    Fleet();
    void init();
    // ---- Membership ------------------------------------------------------------------
    i32        count() const { return m_count; }
    FleetShip& at(i32 i) { return m_ships[i]; }
    const FleetShip& at(i32 i) const { return m_ships[i]; }
    FleetShip& flagship() { return m_ships[0]; }
    // Append a new (zero-initialized) ship slot and return it for the caller to populate.
    // Returns the flagship slot if the fleet is already full (never overflows).
    FleetShip& add();
    // Map a Ship* (e.g. stored by a CombatEntity) back to its flight state. NULL if not ours.
    ShipFlight* flight_for_ship(const Ship* ship);
    // ---- Piloting --------------------------------------------------------------------
    // The fleet member the player is manually piloting. Defaults to 0 (flagship).
    i32        piloted_index() const { return m_piloted_idx; }
    FleetShip* piloted() { return (m_piloted_idx >= 0 && m_piloted_idx < m_count) ? &m_ships[m_piloted_idx] : nullptr; }
    void       set_piloted(i32 idx);
    // ---- Selection -------------------------------------------------------------------
    void clear_selection();
    b8   is_selected(i32 idx) const { return (idx >= 0 && idx < m_count) ? m_ships[idx].selected : FALSE; }
    void set_selected(i32 idx, b8 selected);
    b8   any_selected() const;
    // Select every ship whose origin lies inside the world-space box. Returns count selected.
    i32  select_in_box(bs_math::Vec2 p0, bs_math::Vec2 p1);
    // Select the single ship under the world point (nearest hit). Clears others. Returns TRUE
    // when a ship was hit, FALSE when the point was empty (selection cleared).
    b8   select_at_point(bs_math::Vec2 world);
    i32  selected_count() const;
    i32  first_selected() const;   // index of the first selected ship, or -1
    // ---- Orders (formation-aware) ----------------------------------------------------
    void order_move(bs_math::Vec2 target);   // selected ships -> formation slots around target
    void order_attack(Ship* target);          // selected ships -> attack target
    void clear_order(i32 idx);                 // cancel a ship's order (e.g. when piloted)
    // ---- Simulation ------------------------------------------------------------------
    // Run autopilot for every ordered ship except piloted_idx (-1 = none piloted).
    void update_autopilot(game_state* s, f32 dt, i32 piloted_idx);
    // Integrate every ship's pose. The piloted ship uses flagship_turn_commanded; the rest
    // auto-stabilize their spin (turn_commanded = FALSE).
    void simulate_all(f32 dt, b8 piloted_turn_commanded, i32 piloted_idx);
private:
    FleetShip m_ships[FLEET_MAX_SHIPS];
    i32       m_count;
    i32       m_piloted_idx;
};
