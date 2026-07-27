#pragma once
#include <defines.h>
#include <math/math_utils.h>
#include "sim/ship.h"
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
// Default FTL jump range (world units) for a jump-capable ship. Live-tunable via the editor's
// FLEET JUMP slider (Fleet::set_all_jump_radius). ~= GALAXY_GRID_CELL (2.5e8) inter-system spacing so
// one jump reaches a neighbouring star system (real inter-system travel).
#define JUMP_RADIUS_DEFAULT 250000000.0f
// Forward declaration: game_state is defined in game.h (autopilot needs projectiles +
// combat entities for attack orders).
struct game_state;
// Ship type identifier used for combat-mode emblems. Only DRONE is populated today;
// EXTRACTOR is reserved for future implementation.
enum ShipType {
    SHIP_TYPE_DRONE = 0,
    SHIP_TYPE_EXTRACTOR,
};
// Global-mode inertial flight dynamics (Starsector-style). The ship's POSE (origin, angle)
// lives in Ship; these are the integrators that drive it. The ship coasts -- there is no
// passive linear drag, only the active brake (C) / reverse (S).
struct ShipFlight {
    bs_math::Vec2 velocity;          // world-space linear velocity
    f32           angular_velocity;  // rad/s, CCW positive; auto-stabilizes when A/D released
};
// One player ship: pose + dynamics + selection + per-ship order state.
// Move and attack targets are independent: a ship can strafe toward a destination while its
// nose tracks and fires at a designated target.
struct FleetShip {
    Ship          ship;
    ShipFlight    flight;
    ShipType      ship_type;
    b8            selected;
    b8            has_move_target;   // TRUE when a move order is active
    b8            has_attack_target; // TRUE when an attack order is active
    b8            jump_capable;      // TRUE when this ship can perform FTL jumps
    f32           jump_radius;       // world-space FTL jump range (units)
    bs_math::HierPos2 move_target;   // world-space formation slot / destination
    Ship*         attack_target;     // validated against combat entities each frame
    // Integrate the rigid-body pose from the flight velocities. Mirrors the legacy
    // simulate_ship: auto-stabilizes residual spin when turn_commanded is FALSE.
    void simulate(f32 dt, b8 turn_commanded);
    // Autopilot a Move order: strafe/thrust toward move_target.
    void update_move(f32 dt);
    // Autopilot an Attack order: rotate nose toward attack_target and fire when aligned.
    // If no move target is set, this also approaches the target to maintain engagement range.
    void update_attack(game_state* s, f32 dt);
    // Clear a single order type.
    void clear_move_target();
    void clear_attack_target();
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
    // Number of ships ever spawned (high-water mark). set_count can restore up to this.
    i32        spawned_count() const { return m_spawned; }
    // Truncate / restore the ACTIVE ship count without destroying escort data beyond it.
    // Clamps to [1, spawned_count()]; resets piloting to the flagship and clears selection
    // on any hidden slots so single-ship mode behaves like only the flagship exists.
    void       set_count(i32 n);
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
    i32  select_in_box(bs_math::HierPos2 p0, bs_math::HierPos2 p1);
    // Select the single ship under the world point (nearest hit). Clears others. Returns TRUE
    // when a ship was hit, FALSE when the point was empty (selection cleared).
    b8   select_at_point(bs_math::HierPos2 world);
    i32  selected_count() const;
    i32  first_selected() const;   // index of the first selected ship, or -1
    // ---- Orders (formation-aware) ----------------------------------------------------
    void order_move(bs_math::HierPos2 target);   // selected ships -> formation slots around target
    void order_attack(Ship* target);          // selected ships -> attack target
    void clear_order(i32 idx);                 // cancel a ship's order (e.g. when piloted)
    // ---- FTL jump --------------------------------------------------------------------
    // Among the selected, jump-capable ships, find the smallest jump radius and the index of
    // the ship that owns it (the jump circle's center; ties resolve to the first selected).
    // Returns FALSE when no selected ship is jump-capable. Outputs are untouched on FALSE.
    b8   selected_min_jump(i32* out_center_idx, f32* out_radius) const;
    // Teleport every selected, jump-capable ship to `point`, preserving each ship's offset
    // from the center ship (formation preserved). Clears their orders and zeroes velocity so
    // they do not drift after the jump.
    void jump_selected(i32 center_idx, bs_math::HierPos2 point);
    // Broadcast a jump radius to every ship (editor slider).
    void set_all_jump_radius(f32 radius);
    // ---- Simulation ------------------------------------------------------------------
    // Run autopilot for every ordered ship except piloted_idx (-1 = none piloted).
    void update_autopilot(game_state* s, f32 dt, i32 piloted_idx);
    // Integrate every ship's pose. The piloted ship uses flagship_turn_commanded; the rest
    // auto-stabilize their spin (turn_commanded = FALSE).
    void simulate_all(f32 dt, b8 piloted_turn_commanded, i32 piloted_idx);
private:
    FleetShip m_ships[FLEET_MAX_SHIPS];
    i32       m_count;
    i32       m_spawned;   // high-water mark of ships ever added (escort data survives truncation)
    i32       m_piloted_idx;
};
