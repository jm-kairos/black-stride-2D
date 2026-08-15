# FleetControl

**Responsibility:** Owns the player fleet — membership and the active/spawned split, selection,
per-ship move and attack orders, FTL jumps, the autopilot that executes those orders, pilot input
for the manually-flown ship, hull-vs-hull collision response, and the shared steering primitives.
It explicitly does not own *input interpretation* (RtsControl decides what a click means and
issues the order; this executes it), does not own the ship data model (ShipCombatModel), and
does not own NPC movement — although LocalAgentAi is the sole external consumer of `steering.h`.

**Public interface:** `sandbox/source/sim/fleet.h` — `FLEET_MAX_SHIPS`, `JUMP_RADIUS_DEFAULT`,
`enum ShipType`, `enum FleetStance`, `struct ShipFlight`, `flight_telemetry_tick`,
`struct FleetShip`, `class Fleet`
(membership, piloting, selection, orders, stance, jump, `update_autopilot`, `simulate_all`).
`sandbox/source/sim/ship_control.h` — `control_ship_global`, `resolve_ship_collision`,
`piloted_ship_origin`.
`sandbox/source/sim/steering.h` — `namespace steering`: `arrive`, `seek`, `flee`, `standoff`,
`separation`, `apply`, `apply_face`, `control_face`.
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
- **An attack order fires EVERYTHING; `auto_skip` is the whole player-vs-autopilot fire
  arbitration.** Every ship `update_autopilot` drives fires both missiles and ballistics under
  an attack order — the hull the player is hand-flying is simply skipped (`auto_skip`), and
  manual gunnery is attached-only (detached, LMB is RTS selection — see ShipCombatModel), so
  there is no per-hull gunnery flag and a released hull fights like any other wingman.
  Ballistics fire as a BROADSIDE: each ballistic mount that bears trains onto its own lead
  solution and fires through `ship_weapon_fire_state` / `ship_hardpoint_fire`, the same
  validated per-weapon path as the manual trigger loop. The lead is the NPC gunner's two-pass
  relative-velocity solve (`sim/ai_ship.cpp`), so fleet gunners shoot like the NPCs do.
  Missiles keep their original absolute-velocity lead. Both branches gate on
  `stance != FLEET_STANCE_HOLD_FIRE`.
- **ROE is orthogonal to BOTH stance and orders: it decides whether a ship picks its own
  fight.** `update_engagement` runs before `update_attack` for every autopilot-driven hull and
  self-acquires the nearest hostile ship inside the acquisition envelope (longest reach aboard
  × 1.5, drop hysteresis × 1.3) — `ROE_WEAPONS_FREE` (the default: a warship that watches its
  fleet get shot without responding was the bug), `ROE_RETURN_FIRE` (only factions with a live
  aggression entry — see the Fleet's decaying aggressor table, fed by CombatArena's hit path),
  `ROE_HOLD` (the legacy designation-only behaviour). A player designation always outranks the
  pass (`auto_acquired` marks ROE targets; designations never carry it), an unarmed hull never
  self-acquires, and PASSIVE/HOLD_FIRE stances never self-acquire regardless of ROE. Firing
  still runs through `update_attack`'s gates unchanged — ROE changes WHO gets shot at, never
  HOW. Hostility is `galaxy_history_faction_is_hostile`, not a hardcoded faction compare.
  Set from the editor panel's "ROE (selected)" combo via `set_selected_roe` (the
  `set_selected_stance` pattern); each self-acquisition logs "X: engaging Y".
- **An auto-acquired hunt overrides a standing move order (AGGRESSIVE only).** When an
  AGGRESSIVE ship's SELF-acquired target sits beyond the approach standoff while a move order
  is live, `update_attack` clears the move ("X: breaking station to engage" in the log) so the
  chase can run. Without this a stale move order pinned the hull in place forever — the
  systematic case is a formation slot the separation post-pass never lets a ship close to
  within `update_move`'s 60-unit arrival deadband, so the order never self-clears and
  `need_approach` (which requires `!has_move_target`) stays false while the ship tracks a
  target it never closes on. Player-DESIGNATED attack+move keeps the deliberate
  strafe-while-firing coexistence — only ROE-acquired hunts break station.
- **Stance is orthogonal to orders: it survives one and constrains the next.** The distinction
  that carries the design is AUTONOMY, not aggression — Aggressive and Defensive both fight and
  differ only in whether the ship will leave its station to do it, which is what lets "hold here
  and shoot what comes" be expressed without a second order type. Passive *clears* an attack
  order rather than silently ignoring it, so the HUD never shows a live order the ship is
  declining; Hold Fire keeps tracking, slewing and station-keeping and stops only at the
  trigger, because shadowing a target while not authorised to shoot is a different thing from
  refusing the engagement. Default is Aggressive: it is the pre-stance behaviour, and any other
  default would silently re-tune how every existing escort fights.
- **A move order flies in TWO regimes, split by distance.** Beyond
  `max(3 x own bounding radius, 1500)` (`RTS_MOVE_FACE_DIST_MUL/MIN`) the ship
  TURN-AND-BURNS: it rotates its nose onto the destination, lights the main drive once
  within `RTS_MOVE_FACE_ANGLE`, keeps retro thrust available at any angle so leftover speed
  bleeds while turning, and TRIMS cross-track drift at `RTS_CRUISE_TRIM_FRAC` (0.25) of
  full thrust — side jets read as course corrections, never as a second drive axis. Inside
  the band the original strafing controller survives as the PRECISION regime (final
  approach, docking-scale hops, formation-slot corrections), where sliding on RCS beats
  swinging a cruiser's nose around twice; the band sits above the vanguard's ~410-unit
  full-speed braking distance, so main-engine cutoff hands over to RCS before the retro
  brake begins. The nose is only steered when NO attack order holds it on a target —
  move+attack keeps the old broadside strafe by design (strafe toward the destination
  while the guns track), because two writers on the angle would fight every frame.
  Escorts get the same courtesy in catch-up: far off the station ring (>3x station) they
  face their TRAVEL direction and ride the main drive, swinging guns back onto the
  escortee once near the ring.
- **The three position orders are mutually exclusive by construction.** Each `order_*` clears
  the others, so at most one of `update_move` / `update_escort` / `update_avoid` drives a ship
  in a frame — they all write velocity, and two of them running would fight every tick. Avoid is
  the only self-terminating order: it clears itself once the ship is outside the threat's reach.
  *(`order_move` predated escort/avoid and did not clear them until 2026-08-13 — the stale
  escort was masked behind `update_move`'s priority until arrival, then silently resumed, and
  the roster labelled the ship with the order it was not executing.)*
- **Every control path RECORDS the thrust it commands on `ShipFlight`'s telemetry channels,
  and the exhaust pass draws ONLY from them.** `thrust_cmd`/`turn_cmd` are ship-local,
  normalized to the hull's own caps, accumulated with `+=` by whoever writes velocity that
  tick — the pilot (`control_ship_global`), `update_move`/`update_attack`'s strafing
  controllers, the separation post-pass, and `steering::apply_impl` (which covers escort,
  avoid and every NPC agent at one site). `flight_telemetry_tick` consumes them exactly once
  per ship per tick — from `FleetShip::simulate` for fleet ships, from `ai_ships_update` for
  agents — easing the `*_vis` copies the render tier reads (ShipRendering's exhaust pass, and
  since 2026-08-15 SceneOrchestration's frame-lighting collector, which projects the same
  telemetry onto the authored nozzles to place a thruster-burn point light) and zeroing the
  commands, so a tick nobody commands anything decays the jets to zero. Purely visual: nothing
  in the simulation reads the telemetry back, and a coasting ship (this sim has no drag)
  correctly burns nothing. The channels answer "what are the engines doing", never "how fast is the hull
  moving" — the mistake the old speed-driven exhaust made. The same tick also eases
  `heat_vis`, the main-drive THERMAL state, toward the forward burn on a seconds timescale
  (rise ~1.1/s, fall ~0.35/s): it lags the throttle the way a nozzle bell lags its flame,
  and the burn-vs-heat gap is how the exhaust pass detects a cold ignition.
- **The linear speed cap is `ship_speed_cap` — the ship's player-selected speed-limit
  gear — never `motion.max_speed` directly.** The simulate clamp, both `update_move`
  regimes, `update_attack`, escort/avoid's steering calls and the separation post-pass all
  read the accessor, so the gear governs the autopilot AND the pilot seat together (the
  piloted hull's only cap is simulate's clamp). `motion.accel/decel` are untouched: a high
  gear just accelerates longer, and `update_move`'s `sqrt(2·decel·dist)` braking envelope
  starts the burn-down proportionally further out with no arrival-logic change. Gears are
  card data (`speed_limits`, ShipCombatModel); the player shifts them from the fleet
  panel's SPEED LIMIT chips, which target the selection while detached.
- **`FleetShip::simulate` is the ONLY pose integrator for fleet ships.** Escort and avoid go
  through `steering::control_face` — the same control law as `apply_face` minus the pose add —
  because `simulate_all` integrates every member every frame and the integrating form moved
  those ships twice per frame (escorts flew at double speed). `apply`/`apply_face` remain the
  all-in-one form for agents with no other integrator (LocalAgentAi).
- **Fleet ships hold a minimum separation, sized from BOTH hulls.** A post-pass in
  `update_autopilot` walks member pairs; centers closer than the sum of the two bounding radii
  (`FLEET_MIN_SEPARATION_MUL` = 1.0 — bounding circles never overlap) get pushed apart,
  velocity-only, deliberately below the formation spacing (2.5x) and escort station ring (2.6x)
  so holding a slot never fights the ring. A ship with a live order gets an accel-scaled
  additive nudge its controller absorbs; an orderless ship is DRIVEN toward
  `steering::separation`'s ramp, which fades to zero at the ring edge so it glides to rest just
  outside instead of coasting away forever. The piloted ship is never adjusted — its partner
  does all the yielding, the same immovability convention `resolve_ship_collision` applies to
  the enemy hull.
- **Escort and avoid size themselves from the OTHER hull, never a constant.** Station-keeping is
  a multiple of the escortee's bounding radius (a corvette shadowing a cruiser must sit further
  out just to clear the hull), and the avoid clear range is the threat's own longest
  `weapon_effective_reach` — the same function the HUD ring and the engagement logic use, so
  running from a sniper means running further than running from a knife-fighter.
- **This reaches only the PLAYER's own fleet.** `update_autopilot` walks `Fleet::m_ships`; NPC
  agents fire through `sim/ai_ship.cpp` and the static enemy through `sim/combat_arena.cpp`.
  Their shots still lead. So the enemy out-shoots an unskilled player and loses to a skilled one,
  which is the intended shape — and no player-vs-NPC branch was needed to get it.
- **The hull turns only when turning buys something — turrets aim themselves.** `update_attack`
  computes the minimal hull rotation that unmasks a weapon (per-hardpoint authored
  `facing`/`arc`, edge minus `ROE_ARC_MARGIN`): ZERO when any mount already bears from the
  current heading, the nearest arc edge otherwise — so a broadside hull presents its flank,
  never its bow. During an AGGRESSIVE approach the plan is clamped into `RTS_BURN_CONE`
  (0.6 rad) around the closing vector: the ship CRABS to keep a side mount unmasked while the
  main drive still closes at >= cos(0.6) efficiency, with cross-track damping at cruise-trim
  strength so the approach stays a line. The old nose-alignment fire gate
  (`RTS_ATTACK_FACE_ANGLE`) is REMOVED: per-mount bearing + the validator's slew convergence
  are the real alignment, so a converged side turret fires regardless of where the nose
  points. Only the minimum-range check remains global.
- **The engaging mount tracks the TARGET; firing ballistic mounts track their own LEAD point.**
  `update_attack` aims `whp` (the bearing weapon) at the hull, and the ballistic broadside aims
  each mount it intends to fire at its own lead solution instead — not the hull, because the
  validator's slew gate compares the barrel against the direction FIRED, so a mount trained on
  the hull with a wide lead angle would hold `WEAPON_FIRE_SLEWING` forever. The aim runs
  outside the fire gates, so turrets keep tracking under HOLD FIRE and while the nose is still
  swinging on. *(The old "only whp, never the player's cursor mounts" restriction existed to
  avoid stomping the piloted hull's cursor aim; that conflict is structurally gone — the
  attached hull never runs `update_attack` at all, and detached there is no cursor aim.)*
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
target). Escort and avoid are the newer worked examples. **Not every command needs a new order
type**: `order_rally` is `order_escort` aimed at member 0, because regroup and escort are the
same behaviour with a different target, and "screen a point" is a move order plus the Defensive
stance. A fourth near-duplicate `update_*` would have earned nothing. A new steering behaviour is a pure function in `namespace steering` returning a desired
velocity, consumed through `steering::apply` or `apply_face` — those two own accel-clamping,
speed capping, nose slew and pose integration. New per-hull motion feel is data: `ShipMotion` is
resolved from the hull's size class at load, so a new `ShipSizeClass` row changes flight
characteristics without touching this code.

**Known limitations / tech debt:**
- **The autopilot only PARTLY uses `steering`.** `update_escort` and `update_avoid` go through
  `steering::standoff` / `flee` and `apply_face`, but `update_move` and `update_attack` still
  implement their own strafing controller — desired velocity, velocity error, `RTS_STRAFE_GAIN`,
  then projection onto local thrusters — so the fleet and the NPC AI move through two locomotion
  implementations
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

**Last verified:** 2026-08-15, working tree on `game` (same day, latest: the beyond-reach chase
— instrumented live run with the debug strike ring moved to 40-48k: a parked flagship
self-acquired at 41k (beyond its 36k gauss reach), engaged the approach (`appr=1`), spun up
and closed 41k -> 34k -> 31k into the standoff, kill confirmed; the hunt-overrides-move guard
added the same session shares that validated distance condition. Earlier: turret-first gunnery —
the unconditional nose-on rotation and the hull-alignment fire gate are replaced by minimal
arc-unmask steering + the cone-clamped crab approach; observed live in a player session: the
flagship with port+starboard cannons under WEAPONS FREE autonomously killed a dozen pirate
hulls in sequence, one "engaging" acquisition per kill. Same day, earlier: ROE self-acquisition —
`EngageRoe` + `update_engagement` + the Fleet's decaying aggressor table; live-verified: a
flagship with a freshly mounted gauss, given only a MOVE order toward the raider, self-acquired
("Iron Meridian: engaging Pirate Raider" in the action log), flipped its roster row to ATTACK,
broke toward the target and drained capacitor firing, while the four unarmed escorts correctly
stayed passive. RETURN_FIRE and HOLD verified at code level only. Same day, earlier: every linear speed
cap routes through `ship_speed_cap` — the selected speed-limit gear from the hull's card —
live-verified: gear 5 (4000 u/s) took a move order past 1787 u/s and still braked to a
clean arrival, gear default reproduced the old 240 cap. Same day, earlier: move orders become
two-regime — `update_move` turn-and-burns beyond the hull-derived face distance and keeps
the strafing controller as the precision regime inside it, and `update_escort` faces the
travel direction during catch-up. Autopilot turns gain an angular ARRIVE: pose-stepped
slew is capped by `sqrt(2 * turn_accel * |error|)` in `update_move`/`update_attack` — the
rotational twin of the linear braking envelope — and by a proportional `SLEW_EASE_RAD`
taper in `steering::apply_impl`, so a nose settles onto its goal heading instead of
snapping from full rate to a dead stop in one tick; live-verified: a fleet-wide far move order rotated all
five hulls onto the destination heading and they cruised nose-first at max speed, roster
MOVE across the board. Earlier: ShipFlight grows `heat_vis`, the
slow-eased main-drive thermal scalar behind the exhaust pass's bell glow and ignition-flare
detection). Previously 2026-08-14 (ShipFlight grows the thruster-telemetry
channels — `thrust_cmd`/`turn_cmd` accumulated by every velocity-writing control path,
`flight_telemetry_tick` easing `thrust_vis`/`turn_vis` for the exhaust pass; `Fleet::add` and
`jump_selected` now reset the whole `ShipFlight`, so a jump also zeroes residual spin;
live-verified: pilot W/E/A/S each light the correct jets, coasting at 240 u/s shows none, a
move order brakes on RCS and arrives dark). Previously 2026-08-13 (the `player_gunnery` plumb is REMOVED —
manual gunnery became attached-only with the command overlay's retirement, so `auto_skip`
alone arbitrates player-vs-autopilot fire and `update_autopilot` is back to three parameters;
`order_move` now clears escort/avoid, making the position-order mutual exclusion actually hold.
Previously 2026-08-12: attack orders fire the ballistic broadside with NPC-style
relative-velocity lead through the shared validator/spawner pair — live-verified: escort gauss
fire under attack orders, HOLD FIRE round-trip; this session live-verified the escort→move
label handoff and detached no-fire)
