# FleetControl

**Responsibility:** Owns the player fleet — membership and the active/spawned split, selection,
per-ship move and attack orders, FTL jumps, the autopilot that executes those orders, pilot input
for the manually-flown ship, hull-vs-hull collision response, and the shared steering primitives.
It explicitly does not own *input interpretation* (RtsControl decides what a click means and
issues the order; this executes it), does not own the ship data model (ShipCombatModel), and
does not own NPC movement — although LocalAgentAi is the sole external consumer of `steering.h`.

**Public interface:** `sandbox/source/sim/fleet.h` — `FLEET_MAX_SHIPS`, `JUMP_RADIUS_DEFAULT`,
`enum ShipType`, `struct ShipFlight`, `struct FleetShip`, `class Fleet` (membership, piloting,
selection, orders, jump, `update_autopilot`, `simulate_all`).
`sandbox/source/sim/ship_control.h` — `control_ship_global`, `resolve_ship_collision`,
`piloted_ship_origin`.
`sandbox/source/sim/steering.h` — `namespace steering`: `arrive`, `seek`, `flee`, `standoff`,
`apply`, `apply_face`.
Used from outside: `fleet.h` by 4 subsystems, `ship_control.h` by 2 (CameraControl,
FrameOrchestrator), `steering.h` by 1 (LocalAgentAi only).

**Depends on:** ShipCombatModel, GameStateModel; engine `math/math_utils.h`,
`math/bs_hierpos.h`, `core/input.h`, `defines.h`.
**Depended on by:** CoordinateDiagnostics, RtsControl, LocalAgentAi, CameraControl,
InWorldOverlays, FrameOrchestrator, GameStateModel.

**Key invariants:**
- **The fixed `FleetShip m_ships[FLEET_MAX_SHIPS]` array is a pointer-stability guarantee, not
  just a cap.** `sim/fleet.h` states ships live in a fixed array "so `Ship*` pointers handed to
  the combat / render systems stay stable for the lifetime of the fleet". `CombatEntity` holds
  raw `Ship*`, so this is load-bearing — any move to a growable container breaks combat.
- **Member 0 is the flagship**, assumed throughout (`Fleet::flagship()` returns `m_ships[0]`,
  and `piloted_ship_origin` falls back to index 0 on an out-of-range piloted index).
- **Turn auto-stabilisation lives in `FleetShip::simulate`, deliberately not in
  `control_ship_global`.** `sim/ship_control.cpp` explains why at length: the integrator runs in
  both modes while the control function runs only while piloting, so a stabiliser there would
  let spin accumulated in one mode rotate the ship forever in the other. The two files implement
  complementary halves of one behaviour, and `control_ship_global`'s `b8` return is the signal
  that couples them.
- **`control_ship_global` mutates velocities only, never the pose.** Integration is
  `simulate`'s job, which is what lets a ship keep coasting when nobody is piloting it.
- **`resolve_ship_collision` must run after pose integration** — stated in a comment in
  `sim/ship_control.cpp`, enforced only by call order in `game_update`. The enemy hull is treated
  as immovable: fleet ships are pushed out by the full SAT minimum-translation vector and only
  the inward velocity component is cancelled.
- **`set_count` truncates without destroying escort data**, keeping an `m_spawned` high-water
  mark so the editor's "multiple ship command" toggle can hide and restore escorts instantly.
  That is why `game_init` spawns four escorts and then immediately sets the count to 1.
- Attack range is `weapon_effective_reach * RTS_ATTACK_REACH_FRAC` (0.85) with a 10000-unit
  fallback for an unarmed hull — the RTS half of ShipCombatModel's single-source-of-truth
  guarantee.
- **An attack order is the MISSILE interface: `update_attack` fires GUIDED ORDNANCE ONLY.**
  Unguided offence belongs to the player's left button (`game.cpp`'s fire loop) in *both* control
  modes. Auto-firing the guns here would restore the passive "designate and it shoots for you"
  model in the exact mode where the player has the most attention free to aim — and would beat
  them at it, because the first-order lead solution below is something the manual path
  deliberately does not do. A missile creates no such asymmetry: it steers itself after launch,
  so there is no aiming skill to take away. The gate is `w->wkind == WEAPON_KIND_MISSILE`.
- **This reaches only the PLAYER's own fleet.** `update_autopilot` walks `Fleet::m_ships`; NPC
  agents fire through `sim/ai_ship.cpp` and the static enemy through `sim/combat_arena.cpp`.
  Their shots still lead. So the enemy out-shoots an unskilled player and loses to a skilled one,
  which is the intended shape — and no player-vs-NPC branch was needed to get it.
- **Only the ENGAGING mount tracks the designated target.** `update_attack` used to slew every
  mounted turret onto the target, which now fights the player: `game_update` aims the fire
  selection at the cursor earlier in the same frame and `update_autopilot` runs after it, so the
  loop overwrote that aim every frame. Narrowing it to `whp` leaves the launcher holding its
  target while the guns go where they are pointed.
- **The autopilot honours `Ship::weapon_override`.** When the player has micro-selected a single
  weapon, `update_attack` engages with that mount alone *and* takes its reach as the approach
  distance — so the hub choice decides both what fires and where the ship parks. The field is
  `-1` on every AI hull and every escort, so their "best weapon that bears" behaviour is
  untouched. Firing goes through `ship_weapon_fire_state`, the same validator the manual path
  uses, and spawns through `ship_hardpoint_fire`, the same spawner.

**Extension points:** A new order type means a flag and target field on `FleetShip`, an
`order_*` method on `Fleet`, an autopilot `update_*` invoked from `update_autopilot`, and a
`clear_*`; the existing move and attack orders are the template, and the header notes they are
independent by design (a ship can strafe toward a destination while its nose tracks a different
target). A new steering behaviour is a pure function in `namespace steering` returning a desired
velocity, consumed through `steering::apply` or `apply_face` — those two own accel-clamping,
speed capping, nose slew and pose integration. New per-hull motion feel is data: `ShipMotion` is
resolved from the hull's size class at load, so a new `ShipSizeClass` row changes flight
characteristics without touching this code.

**Known limitations / tech debt:**
- **The autopilot does not use `steering::apply`.** `sim/fleet.cpp` implements its own strafing
  controller — desired velocity, velocity error, `RTS_STRAFE_GAIN`, then projection onto local
  thrusters — so the fleet and the NPC AI move through two different locomotion implementations
  despite `steering.h` describing itself as "one locomotion layer, reused by any Ship-backed
  agent". The player goes through a third path (`control_ship_global`).
- **`piloted_ship_origin` asks `s->rts_controls.piloted_index()`**, so a `sim` module depends on
  the RTS control object to answer "who is the player".
- `control_ship_global` reads the engine input singleton directly for W/S/Q/E/C/A/D, so the
  flight control scheme is hardcoded at those call sites with no binding indirection.
- The heading convention (`angle 0` → nose +Y, via `atan2f(-x, y)`) is encoded independently in
  three files: `ship_control.cpp`, `steering.cpp` and `render/ship_render.cpp`.
- `steering.cpp` carries magic constants with no tuning hook — a 1.0-unit arrival deadband in
  `arrive`, and `standoff`'s band width of `standoff_dist * 0.25 + 1`.
- `fleet.cpp`'s ten autopilot constants are file-static `constexpr` with no editor exposure,
  unlike most tuning in this codebase.
- `find_combat_entity_for_ship` linear-scans the combat entity array every frame per ordered
  ship to validate an attack target.
- `JUMP_RADIUS_DEFAULT` (2e9) is calibrated to `GALAXY_GRID_CELL` in `sim/galaxy_gen.h` so one
  jump reaches a neighbouring system — a numeric relationship across two headers with no shared
  constant.
- `ShipType::SHIP_TYPE_EXTRACTOR` is declared and documented as unpopulated.
- `Fleet` is one of only two real C++ classes in the sandbox (with `RtsControls`), using private
  members and accessors where everything else is a POD struct.

**Source paths:** `sandbox/source/sim/fleet.{cpp,h}`, `sandbox/source/sim/ship_control.{cpp,h}`,
`sandbox/source/sim/steering.{cpp,h}`

**Last verified:** 2026-08-09, commit `b1baf31` (attack orders fire guided ordnance
only; turret tracking narrowed to the engaging mount)
