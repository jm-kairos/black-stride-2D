# CombatArena

**Responsibility:** Owns combat resolution — the combat-entity mirror of the fleet, enemy and
NPCs; the proximity encounter trigger; the static enemy AI; missile steering; projectile
advancement and hit resolution; the point-defense laser; and fleet-wide sensor detection. It
explicitly does not own weapons or projectiles as *objects* (ShipCombatModel), does not own NPC
behaviour (LocalAgentAi), and does not draw anything — `combat_arena.h` states the rendering half
is split across ShipRendering, InWorldOverlays and the HUD, and that weapon firing stays in
`game_update`'s input path.

**Public interface:** `sandbox/source/sim/combat_arena.h` — `ENEMY_DETECTOR_RADIUS`;
`combat_arena_init`, `_rebuild_player_entities`, `_update_encounter`, `_update_enemy_orbit`,
`_update_enemy_ai`, `_sync_entities`, `_update_projectiles`.
`sandbox/source/sim/point_defense.h` — `point_defense_update`.
`sandbox/source/sim/sensor_system.h` — `struct SensorContact`, `sensor_reading`,
`sensor_gather_hostile_contacts`.
Used from outside: `combat_arena.h` by 3 subsystems, `point_defense.h` by 1
(FrameOrchestrator), `sensor_system.h` by 1 (InWorldOverlays).

**Depends on:** ShipCombatModel, GalaxyHistory, LocalAgentAi, ActionLog, Geometry2D,
GameStateModel; engine `math/math_utils.h`, `math/bs_hierpos.h`, `defines.h`.
**Depended on by:** DevPanels, InWorldOverlays, FrameOrchestrator.

**Key invariants:**
- **A four-step call order spans three files and is documented only in comments.**
  `point_defense.h` states `point_defense_update` must run *after*
  `combat_arena_sync_entities()` (so positions are current) and *before*
  `combat_arena_update_projectiles()` (so destroyed threats never advance or collide that
  frame); `combat_arena.h` states `sync_entities` runs after fleet integration. Nothing
  enforces any of it. Getting it wrong lets intercepted missiles still hit.
- **The combat-entity array is a manually partitioned window:** slot 0 is the enemy, slots
  1..N mirror the active fleet, and `npc_combat_base` marks where LocalAgentAi appends.
  `combat_arena_rebuild_player_entities` must be called whenever the active fleet count changes
  or the two windows overlap — `combat_arena.h` says so, and DevPanels' "multiple ship command"
  checkbox is the caller that must remember.
- **Entities hold raw `Ship*` back-pointers**, which is why FleetControl's fixed array is
  documented as a stability guarantee.
- **Both enemy-AI fire sites spawn through `ship_hardpoint_fire`, not `Weapon::fire`.** The
  bearing-weapon shot and the missile loop each pass their hardpoint index, which is what lets a
  multi-barrel weapon fire from its barrels for an NPC exactly as it does for the player.
  Calling `Weapon::fire` directly here would still shoot, and would silently collapse every
  barrel back onto the mount centre — see ShipCombatModel's spawner invariant.
- **Every projectile this subsystem destroys goes through `ProjectileSystem::retire`**, not
  through a hand-rolled `active = FALSE; --count`. All three sites here (hull hit, flak burst
  killing hostile ordnance, and the flak shell consumed by its own burst) pass the *reason* the
  shot stopped, because only this code knows it — the shell hit armour, or was burned down, or
  fused. That reason picks the termination effect. Flak additionally passes
  `s->flak_tuning.burst_radius` as the effect size, so the airburst is drawn at the radius that
  actually did damage rather than at the 3-unit shell's own radius; it is the only place in the
  game that shows the player how far the screen reaches.
- **Detection is a fleet-wide union computed by max** — a contact's confidence is the strongest
  single reading over all friendly ships, so overlapping coverage widens the picture without
  double-counting (`sensor_system.h`).
- **Point-defense engagement range is live-coupled to `sensors.layer0_radius`** when
  `DefenseLaser::range` is 0, so installing a sensor module changes PD reach. `sim/ship.h`
  argues the Layer 0 choice deliberately ("last-ditch screen… must not reach identification
  range").
- **A capacitor reserve floor blocks new locks but lets an existing one burn to its dwell end**,
  so PD throttles itself before bottoming the bank; running dry mid-dwell drops the lock and is
  described in `sim/point_defense.cpp` as the intended saturation failure mode.
- `ship_sensor_range` is set to `SENSOR_LAYER1_RADIUS` in `combat_arena_init` so an enemy hull
  resolves into a real sprite exactly when the sensor suite says it is identified — a deliberate
  numeric coupling between rendering and the sensor model.

**Extension points:** A new combat entity kind is a registration in
`combat_arena_rebuild_player_entities` (or an append past `npc_combat_base` for agent-owned
ones) plus handling in `_sync_entities` and `_update_projectiles`. A new point-defense doctrine
axis follows the existing three: a `u8` field on `DefenseLaser`, a scaling or scoring branch in
`point_defense_update`, and a HUD chip round-tripped through the `bs_rml_hud_state` action
grammar — stance, priority and gate tier are all built that way and multiply into 27
configurations from one code path. A new detection rule belongs in `sensor_reading` /
`sensor_gather_hostile_contacts`, which are pure and have one consumer.

**Known limitations / tech debt:**
- **`combat_arena_init` sets ~20 tuning fields belonging to other subsystems** — sensor range,
  heat-map palette, colours, metaball radius and threshold, tail length and fade, warp strength,
  venn sharpness, and the encounter flags. It is the de-facto initialiser for the heat map and
  sensor FX as well as combat.
- **Both the superseded and current enemy behaviours are exported.**
  `combat_arena_update_enemy_orbit` is described as "hardcoded demo motion" and
  `_update_enemy_ai` as the static AI that "replaces the orbit demo" — both still declared.
- `combat_arena.h` names `render/game_hud.cpp` as the home of the encounter modal; **no such
  file exists** in the tree.
- **`sensor_system.h` contradicts its implementation**: the header says "Only contacts with
  confidence > 0 are written", but `sensor_gather_hostile_contacts` writes every active
  non-player projectile, with an in-code comment explaining Layer 2 is unbounded so distant
  returns are still tracked at confidence 0. The code is the newer behaviour.
- Sensor gathering is O(active projectiles × fleet size) every frame over the full
  `MAX_PROJECTILES` pool, with no active list.
- "Hostile" in the sensor path is a single hardcoded comparison against `FACTION_PLAYER`, not a
  diplomacy query — unlike the projectile hit path, which does consult
  `galaxy_history_faction_is_hostile`.
- **`ENEMY_DETECTOR_RADIUS` (40000) must stay inside the ballistic engagement envelope**
  (19k–58k units of reach after the 0.15 range compression).
  `combat_arena_update_enemy_ai` gates firing on alignment and cooldown but **not on reach**, so
  a detector radius beyond its own shells' reach makes the enemy fire continuously at a flagship
  it cannot hit, draining its capacitor and filling the field with shells that expire short. It
  was 120000, chosen to exceed the sensor layers *for detection testing* — a debugging value that
  sat on the critical path of the fight. Its comment also cited a "50k sensor layer" that no
  longer exists (the layers are 250k/500k/1M).
- **The point-defense gate fractions `{0.6, 0.8, 1.0}` are the third copy of those numbers**,
  alongside identical tables in `render/defense_laser_overlay.cpp` and documented tiers in the
  engine's `renderer/bs_rml.h`.
- `point_defense_update` calls `ship_turret_aim_at`, so a simulation tick drives presentation
  state directly.
- The whole subsystem assumes exactly one enemy hull (`fleet_state.enemy_ship`) alongside the
  NPC population; the encounter trigger and the static AI are both written against that single
  hull.

**Source paths:** `sandbox/source/sim/combat_arena.{cpp,h}`,
`sandbox/source/sim/point_defense.{cpp,h}`, `sandbox/source/sim/sensor_system.{cpp,h}`

**Last verified:** 2026-08-09, working tree on `game` (the four projectile-destruction sites
across this subsystem and `point_defense.cpp` now retire through the pool's own API)
