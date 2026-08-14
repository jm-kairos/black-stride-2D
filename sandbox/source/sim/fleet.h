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
// FLEET JUMP slider (Fleet::set_all_jump_radius). ~= GALAXY_GRID_CELL (2e9) inter-system spacing so
// one jump reaches a neighbouring star system (real inter-system travel).
#define JUMP_RADIUS_DEFAULT 2000000000.0f
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
    // ---- Thruster telemetry (visual-only: the exhaust pass draws from it) ---------------
    // Every control path RECORDS the thrust it commands here, so the drawn flames answer
    // "what are the engines doing" rather than "how fast is the hull moving" -- a coasting
    // ship burns nothing, a braking ship fires its bow thrusters, a strafing ship its side
    // ones. SHIP-LOCAL axes normalized to the hull's own caps: thrust_cmd.x = starboard
    // strafe, .y = forward thrust, turn_cmd CCW-positive, all nominally -1..1 (writers
    // ACCUMULATE with +=, so a tick with several contributors can exceed it; the consumer
    // clamps). flight_telemetry_tick consumes them once per simulation tick -- fleet ships
    // from FleetShip::simulate, NPC agents from ai_ships_update -- easing the *_vis copies
    // the exhaust pass reads and zeroing the commands for the next tick's writers, so a
    // tick nobody commands anything decays the jets to zero. Rendering never writes these.
    bs_math::Vec2 thrust_cmd;        // this tick's commanded thrust, ship-local, -1..1
    f32           turn_cmd;          // this tick's commanded turn, -1..1 (CCW positive)
    bs_math::Vec2 thrust_vis;        // smoothed thrust the exhaust pass draws
    f32           turn_vis;          // smoothed turn the exhaust pass draws
    // Main-drive THERMAL state, 0..1: eased toward the forward-thrust burn far slower than
    // thrust_vis (seconds, not tenths), so it lags the throttle the way a nozzle bell lags
    // its flame. The exhaust pass reads it two ways: sustained burn heats the bell glow
    // toward white-hot, and burn >> heat is the ignition transient -- a drive lighting from
    // cold -- which draws as a brief overbright flare. Visual only, like the rest.
    f32           heat_vis;
};
// Consume one simulation tick's thruster commands: clamp, ease the *_vis copies toward them
// (fast attack / slower release so taps read as puffs, not strobes), then zero the commands.
// Call exactly once per ship per tick, AFTER every control path has written.
void flight_telemetry_tick(ShipFlight* fl, f32 dt);
// Standing posture for a fleet ship: what it does when nobody is issuing it a fresh order.
//
// Modelled on DefenseLaser's stance/priority/gate trio rather than invented fresh -- that one
// already proves the shape in this codebase: a u8 on the owning struct, a branch in the update
// that consumes it, and a HUD chip round-tripped through the action grammar.
//
// The distinction that matters is AUTONOMY, not aggression. Aggressive and Defensive both fight;
// they differ in whether the ship will leave its station to do it. That is what makes a stance
// useful next to an order: the order says where, the stance says how far the ship will stray
// from it.
enum FleetStance : u8 {
    FLEET_STANCE_AGGRESSIVE = 0, // engage designated targets and close to weapons range
    FLEET_STANCE_DEFENSIVE,      // engage, but hold station -- fire from where the order put it
    FLEET_STANCE_PASSIVE,        // never engage; movement orders only
    FLEET_STANCE_HOLD_FIRE,      // manoeuvre normally, weapons tight (shadow a target, don't shoot)
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
    u8            stance;            // FleetStance; honoured by update_attack
    // ---- Standing orders ---------------------------------------------------------------
    // Distinct from move_target because their destination is not a POINT -- it tracks another
    // hull every frame. A move order to where the escortee currently is would be stale the
    // moment it moved, which is the whole reason these are separate order types rather than
    // the RTS layer re-issuing move orders every tick.
    b8            has_escort_target; // TRUE when following a friendly at station-keeping range
    Ship*         escort_target;     // validated against the fleet each frame
    b8            has_avoid_target;  // TRUE when actively keeping away from a hostile
    Ship*         avoid_target;      // validated against combat entities each frame
    bs_math::HierPos2 move_target;   // world-space formation slot / destination
    Ship*         attack_target;     // validated against combat entities each frame
    // Integrate the rigid-body pose from the flight velocities. Mirrors the legacy
    // simulate_ship: auto-stabilizes residual spin when turn_commanded is FALSE.
    void simulate(f32 dt, b8 turn_commanded);
    // Autopilot a Move order: strafe/thrust toward move_target.
    void update_move(f32 dt);
    // Autopilot an Attack order: rotate nose toward attack_target and fire when aligned --
    // missiles AND ballistics, through the shared validated fire path. If no move target is
    // set, this also approaches the target to maintain engagement range. The hull the player
    // is hand-flying never runs this at all (update_autopilot skips it), which is the whole
    // player-vs-autopilot fire arbitration now: manual gunnery is attached-only.
    void update_attack(game_state* s, f32 dt);
    // Autopilot an Escort order: hold station near escort_target, using steering::standoff so the
    // ship settles into a band rather than oscillating on the exact radius.
    void update_escort(f32 dt);
    // Autopilot an Avoid order: run from avoid_target until outside the disengage band.
    void update_avoid(f32 dt);
    // Clear a single order type.
    void clear_move_target();
    void clear_attack_target();
    void clear_escort_target();
    void clear_avoid_target();
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
    // Apply a FleetStance to every selected ship. Stance is orthogonal to orders -- it survives
    // one and constrains the next -- so this deliberately does not touch order state.
    void set_selected_stance(u8 stance);
    // Selected ships escort `target` (a friendly hull) at station-keeping range.
    void order_escort(Ship* target);
    // Selected ships break off and keep away from `target`.
    void order_avoid(Ship* target);
    // Selected ships regroup on the flagship. Deliberately NOT a new order type: it is
    // order_escort aimed at member 0, because "regroup" and "escort" are the same behaviour
    // with a different target, and a fourth near-duplicate update_* would earn nothing.
    void order_rally();
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
    // Run autopilot for every ordered ship except auto_skip (-1 = drive them all, i.e. the
    // camera is detached and nobody is hand-flown). The skipped hull is the ONLY one whose
    // guns belong to the player: detached, manual gunnery does not exist (LMB is selection),
    // so a released hull fights like any other wingman.
    void update_autopilot(game_state* s, f32 dt, i32 auto_skip);
    // Integrate every ship's pose. The piloted ship uses flagship_turn_commanded; the rest
    // auto-stabilize their spin (turn_commanded = FALSE).
    void simulate_all(f32 dt, b8 piloted_turn_commanded, i32 piloted_idx);
private:
    FleetShip m_ships[FLEET_MAX_SHIPS];
    i32       m_count;
    i32       m_spawned;   // high-water mark of ships ever added (escort data survives truncation)
    i32       m_piloted_idx;
};
