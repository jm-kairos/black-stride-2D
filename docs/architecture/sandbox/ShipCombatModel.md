# ShipCombatModel

**Responsibility:** Owns the ship as a simulated object — the rigid-body pose, the hardpoint
skeleton, mountable modules and weapons, the sensor suite, the capacitor, turret traverse, the
collider, the visual-layer model, and the projectiles weapons spawn. It owns the three data-file
formats these are loaded from (`.ship`, `.module`, `.weapon`) and their registries. It
explicitly does not own *who* is flying (FleetControl), does not own combat resolution or
damage (CombatArena), and does not own drawing — `render/ship_visual` here is the appearance
*data model*; the passes that consume it are ShipRendering.

*(This subsystem is a merge of what were separately ShipModel and Weapons; the split produced a
4-edge cycle because `Ship` owns `Weapon*` and `ship.cpp` includes `weapon.h`. See
`sandbox-subsystems.md` §Settled calls.)*

**Public interface:** `sandbox/source/sim/ship.h` — `Ship`, `HardpointDef`, `HardpointSize`,
`MODULE_TYPE_*`, `VesselFaction` and the `FACTION_*` sentinels, `SensorSuite`, `DefenseLaser`,
`PdStance`/`PdPriority`, `ShipSizeClass`, `ShipMotion`; `ship_load`, `ship_recompute_stats`,
`ship_try_spend_cap`, `ship_capacitor_update`, `ship_collider_corners`, `ship_bounding_radius`,
`ship_local_dir`, `ships_collide`, `hardpoint_accepts`, `hardpoint_fits_module`,
`ship_first_free_hardpoint`, `ship_hardpoint_fire_origin`, `ship_muzzle_origin`,
`ship_hardpoint_fire`,
`ship_hardpoint_can_aim`, `ship_select_bearing_weapon`, `ship_turret_aim_at`,
`ship_update_turrets`;
`WeaponFireState`, `ship_weapon_fire_state`, `ship_hardpoint_in_selection`,
`ship_select_weapon_override`, `ship_clear_weapon_override`, `ship_hardpoint_in_group`,
`ship_nth_group_weapon`.
`sim/weapon.h` — `Weapon`, `BallisticWeapon`, `MissileLauncher`, `WeaponKind`, `FireMode`,
`weapon_effective_reach`, `Weapon::cooldown_duration_s`. `sim/weapon_def.h` — `WeaponDef`,
`WeaponRegistry`, `MuzzlePattern`,
`WEAPON_MAX_MUZZLES`, `VfxFamily`, `weapon_registry_load`, `weapon_registry_resolve_textures`,
`weapon_registry_find`, `weapon_instantiate`.
`sim/module.h` — `ModuleDef`, `ModuleRegistry`, `module_registry_load`, `module_registry_find`.
`sim/projectile.h` — `Projectile`, `ProjectileKind`, `ProjectileSystem`, `MAX_PROJECTILES`,
`PROJ_TRAIL_SAMPLES`, `PROJ_TRAIL_INTERVAL`, `ProjectileSystem::retire`, `ProjectileSystem::fx`.
`render/ship_visual.h` — `ShipVisual`, `VisualLayer`, `ship_visual_load`,
`ship_visual_resolve_textures`.
`ship.h` is included by 7 other subsystems; `weapon.h` by 6.

**Depends on:** GameStateModel, `core/projectile_fx.h`; engine `math/math_utils.h`,
`math/bs_hierpos.h`, `renderer/renderer.h`, `renderer/renderer_types.h`, `core/logger.h`,
`defines.h`.
Its only sandbox edge is the Tier 0 FX ring — a dependency-free POD buffer, so the cluster is
still effectively self-contained and still points strictly *down* the tier order. The
alternative considered was having the render tier reach in, which would have inverted it.
**Depended on by:** CombatArena, CoordinateDiagnostics, Discovery, FleetControl, InWorldOverlays,
RtsControl, ShipRendering, LocalAgentAi, FrameOrchestrator, GameStateModel.
*(The per-header counts above are not the same question: `game.cpp` reaches this cluster through
`weapon.h` and `projectile.h` without including `ship.h`, so FrameOrchestrator is a dependant of
the subsystem while sitting outside `ship.h`'s seven.)*

**Key invariants:**
- **The rigid-body relation is fixed:** `world = origin + rotate(local * world_scale, angle)`,
  inverse `local = rotate(world - origin, -angle) / world_scale`. Stated as formulas in
  `sim/ship.h` and relied on by every render, collision and hardpoint calculation.
- **Derived stats must be recomputed after any mount change.** `sensors` and the capacitor pair
  are re-derived by `ship_recompute_stats()` from `sensors_base` / `cap_*_base` times mounted
  modules; `ship.h` instructs "tune the baseline, not it". `ship_load` calls it itself, but every
  other mount/unmount site must call it by hand. Unenforced.
- **A hardpoint holds at most one occupant across three parallel arrays** — `mounts[]`,
  `point_defense_mount`, `module_mounts[]`. Maintained by convention across the loadout code
  with no central check.
- **A module mounts on slots of its own size or larger.** Encoded in `hardpoint_fits_module`
  (`sim/ship.cpp`) and documented in `sim/module.h` — but re-implemented independently in two
  drag-validation paths (see tech debt).
- **`weapon_effective_reach` is the single source of truth** for "can this weapon hit at
  distance d". `sim/weapon.h` says so explicitly, and it holds: the RTS attack order gates
  firing on it (`sim/fleet.cpp`) and the HUD range ring draws it
  (`render/gameplay_overlays.cpp`), so what the player sees matches what the ship does. It
  prefers the authored `.weapon` def over live instance values so editing a data file moves both.
- **`ship_weapon_fire_state` is the one per-shot validator**, and all three fire-control
  surfaces go through it: the manual held-trigger loop in `game.cpp`, the autopilot attack order
  in `sim/fleet.cpp`, and the selection hub's per-tile status. It folds operational status,
  traverse arc, cooldown, reach and capacitor affordability into one `WeaponFireState` and
  spends nothing — the caller commits via `ship_try_spend_cap` only after `WEAPON_FIRE_READY`.
  *(Until the micro-selection work the manual path never checked reach at all, so a click far
  beyond the drawn range ring still fired and still drained the bank.)*
- **The trigger is mode-independent; only FLIGHT is gated on camera attachment.** `game_update`'s
  piloting branch has no mode test and no longer has a detach test either: turret traverse, the
  fire-group number row and trigger firing all run at any zoom, in either look, attached or
  detached. The left button is the ballistic trigger in **both** control modes — it was
  previously suppressed while detached only because RtsControl's box/click selection owned the
  button there, and that selection is retired. Detached is in fact where aiming is *easiest*: the
  autopilot is flying, so the player's whole attention is on the shot. What still separates the
  two schemes is that `control_ship_global` self-guards on `free_camera_active`
  (`sim/ship_control.cpp:21`), so detached still means the autopilot flies.
- **Unguided offence is the player's; guided ordnance and defences are automated.** That one rule
  decides every fire site: ballistics require the trigger, missiles fire from an attack order
  (`sim/fleet.cpp`), point defense engages on its own doctrine. `WeaponKind` already expresses
  the distinction, so a new weapon inherits the answer without reopening it.
- **`proj_life` IS the flight time to maximum reach**, because `weapon_effective_reach` is
  `proj_speed * proj_life`. Tuning engagement distance and tuning shot travel time are therefore
  the same edit, and the whole ballistic catalog was scaled by a uniform 0.15 (reach 19k–58k,
  flight 2.1–3.6 s) precisely so every authored balance relationship between the four ballistics
  survived — `trident_mk1` documents itself as a third of `longlance_rail`'s reach, and a
  per-weapon retune would have silently broken that. Compress range by scaling the catalog, not
  weapon by weapon.
- **The manual trigger is level-triggered, not edge-triggered.** Holding the left button
  re-runs the whole selection loop every frame; per-weapon rate limiting comes entirely from
  `WEAPON_FIRE_RELOADING`, so a weapon fires at its own authored rate and nothing bypasses the
  validator. The press edge survives only to gate the action-log feedback — `action_log_push`
  neither dedups nor rate limits, so reporting per frame would flood the buffer.
- **`Ship::weapon_override` is the fire selection, and `ship_hardpoint_in_selection` is how you
  ask.** `-1` means "All" — the active group's members, the long-standing default. `>= 0` is a
  single hardpoint index *within that group*. Selecting a fire group with the number row clears
  it, and `ship_groups_sanitize` drops it both when the loadout editor empties that slot and
  when the fire-group matrix takes the weapon out of the active group. Every consumer (fire
  loop, turret traverse, autopilot, reach ring, hull digits, hub) reads the helper rather than
  testing the group mask, so they cannot disagree about what fires.
- **The override can never point outside the active group**, and that is enforced at the three
  *write* sites (hub commit, `ship_select_weapon_group`, `ship_groups_sanitize`) rather than by
  re-checking membership at the fire sites — the readers still treat it as an absolute hardpoint
  index. `ship_select_weapon_override` has exactly one caller, the hub, which is what makes a
  write-side invariant tractable. Any second producer must uphold it.
- **`ship_hardpoint_in_group` is the only place the group-membership bit is tested.**
  `ship_group_size`, `ship_nth_group_weapon`, `ship_select_weapon_group`'s re-home and
  `ship_hardpoint_in_selection`'s no-override branch all route through it, so "what is in this
  group" has one answer. `ship_nth_group_weapon` is its enumerate form and is what makes the
  micro-selection hub offer the active group instead of the whole hull.
- **A limited-traverse turret slews in ARC-RELATIVE space, never along the shortest angle.**
  `ship_update_turrets` interpolates between two offsets already clamped to `[-arc/2, +arc/2]`,
  which confines the barrel to the arc by construction. Using `wrap_pi` on the absolute angles
  instead — the obvious formulation, and the one that shipped — caps a turn at 180 degrees, so
  on an arc *wider* than 180 the two edges are further apart through the arc than around the
  back of it and the turret cuts across its own blocked wedge. That is invisible on the 180
  degree `bow_gun` (every legal pair is <= 180 apart) and shows on the 210 degree `port_gun` /
  `stbd_gun` as the barrel swinging the wrong way round. Full-circle mounts keep the
  shortest-angle path: with no blocked wedge it is both legal and what a 360 degree mount
  should do.
- **`ship_hardpoint_fire` is the one per-shot SPAWNER, the twin of `ship_weapon_fire_state`
  being the one per-shot validator.** The manual trigger (`game.cpp`), the RTS attack order
  (`sim/fleet.cpp`) and both combat-arena gunners route through it, so where a weapon's shots
  physically leave the hull is answered once. That is what makes barrels a data question: a
  `.weapon` def listing `muzzle` offsets gets them on the player's guns, the autopilot's and
  the NPCs' together, with no fire site aware that barrels exist. Callers still validate and
  spend the capacitor first; the spawner only spawns.
- **`ship_muzzle_origin` is the ONE place barrel geometry is computed**, and it exists because a
  second consumer arrived: `ship_hardpoint_fire` spawns from it, and the charge-up VFX pass
  (InWorldOverlays) draws the pre-fire glow on it. Had the render side re-derived the same
  half-extent/`mount_aim` maths, the two would have agreed at rest and drifted apart *only while
  a turret was traversing* — a bug that is invisible in a screenshot and obvious in motion.
  Passing `-1` returns the hardpoint centre, which is what keeps weapons with no authored
  muzzles behaving exactly as they did before barrels existed.
- **`cooldown_duration_s()` exists so callers can recover ABSOLUTE time-to-ready**, since
  `cooldown_progress()` is a fraction and a fixed-length anticipation window is not. The same
  0.4 s of charge-up is 24% of a trident's cycle and 4% of a torpedo's; keying the window off the
  fraction would have made the torpedo glow for most of a nine-second reload.
- **A muzzle resolves against `mount_aim`, not against the direction being fired.** The two
  differ while a turret is still traversing, and the muzzle must sit where ShipRendering draws
  the barrel — the shot is watched leaving the art, not leaving the aim vector. Offsets are in
  the same hardpoint-half-extent units as `mount_art_size`, so art and shot origins scale
  together and cannot drift apart.
- **`Weapon::fire` is now a non-virtual composition of two virtuals** — `spawn_shot` (put one
  projectile in the world) and `begin_cooldown` — because `fire` gated on `ready()` and set the
  cooldown itself, so a salvo calling it per barrel would have fired exactly once. Every
  existing caller of `fire` keeps its behaviour; only a multi-barrel salvo takes the split path,
  checking readiness once and cooling down once.
- **`spawn` / `spawn_missile` are where the MUZZLE FLASH is emitted, and that is the whole
  reason it reaches every gun.** They are the two functions through which a projectile enters
  the world, so an effect raised there covers the manual trigger, the autopilot, both
  combat-arena gunners *and* `sim/ai_ship.cpp`'s agent gunner — the one fire site documented
  below as bypassing `ship_hardpoint_fire` entirely. Emitting from the spawner instead would
  have silently skipped NPC agents, and a future fire path would have to remember to opt in.
- **`retire` is the one place a shot STOPS for a reason**, the mirror of `spawn` being where it
  starts. It frees the slot and records the termination effect; the four callers (hull hit,
  flak burst, flak self-consume, PD kill) pass the *reason* because only they know it. Lifetime
  expiry is deliberately not routed through it and stays inside `update`, silently: a shell
  running out of range should fizzle, not detonate.
- **The whole VFX layer hangs off one nullable pointer.** `ProjectileSystem::fx` is wired once
  in `combat_arena_init` and never rewritten. Setting it to `nullptr` disables every launch and
  termination effect in the game and must leave damage, collision and physics bit-identical.
  That is the removability test for the feature, and it is why the emit calls take no non-const
  simulation state.
- **`Weapon::disabled` is declared with no producer.** There is no subsystem-damage model yet;
  the flag exists so the validator and the hub's Disabled state are correct the day one lands.
- **Weapon instances point their `name`/`icon` into the registry's pool storage**, which
  `sim/weapon_def.h` justifies as safe because the fixed pool never reallocates. The registry
  therefore must outlive every weapon — it does, living in `game_state`.
- **Module defs are shared by pointer across ships** and carry no per-instance state
  (`sim/module.h`), which is what makes sharing safe.
- Sensor layers are strictly ordered `l0 < l1 < l2`; `sim/ship.h` notes the editor enforces it.

- **Guided rounds carry a position history; unguided ones deliberately do not.** `Projectile::
  trail` is `PROJ_TRAIL_SAMPLES` past positions stored as `Vec2` OFFSETS from `position` (8 bytes
  a sample against 24 for a `HierPos2`, and trivially precision-safe as a short local vector no
  matter how far from the origin the fight is), rebased by each tick's movement. Recorded only
  for `PROJ_MISSILE`, so the per-tick cost scales with missiles in flight rather than with the
  512-slot pool, and read only by `ProjectileSystem::render`. A shell flies a straight line and
  its single stretched quad is already exactly right; a missile is steered every tick, so a
  straight streak reports a path it never flew.
  *Sampled on a fixed 25 ms clock, not per frame* — a per-frame history would make trail length
  a readout of the player's framerate.
- **A weapon's VISUAL family is authored data, and defaults so hard that five of six defs say
  nothing.** `WeaponDef::vfx_family` picks one of three looks — `shell` (inert kinetic round),
  `slug` (rail-driven), `ordnance` (powered and guided) — and is resolved *after* parsing from
  `kind`, so ballistic becomes shell and missile becomes ordnance with nothing written. Only
  `longlance_rail` opts out. The field is read by `ProjectileSystem::render` and the FX pass and
  by nothing else: two projectiles differing only in `vfx_family` fly, collide and damage
  identically. A `slug` is never inferred from `proj_speed` — a railgun is a design statement
  about a weapon, not a threshold.
  *(0xFF is the "unauthored" sentinel during parsing, because `VFX_SHELL` is 0 and a zeroed
  struct would otherwise be indistinguishable from an explicit `vfx_family shell`.)*
- **A FLAK round keeps the SHELL look regardless of its gun's family.** `BallisticWeapon::
  spawn_shot` forces it: what is in flight is a fused proximity round and what it does is burst,
  so borrowing a `slug` gun's visual language would misdescribe it.

**Extension points:** **A new weapon is a data file** — a `.weapon` text file listed in
`assets/weapons/weapons.list`; `weapon_instantiate` builds a `BallisticWeapon` or
`MissileLauncher` from `def->kind`, and `sim/weapon.cpp` records that "the old hardcoded
factories are gone". **Its in-world turret look is part of that data file too** — an optional
`mount_art` path plus `mount_art_size` / `mount_art_pivot`, which ShipRendering draws instead of
its procedural rectangles. Omit them and the weapon keeps the rectangles, which is why adding
cannon art left railguns and missile racks alone. **Barrels are the same kind of edit** —
repeatable `muzzle <right> <forward>` lines plus `muzzle_pattern sequential|salvo`. Any count up
to `WEAPON_MAX_MUZZLES` works and no fire site changes to add a six-barrel gun; measure the
offsets off the art's alpha rather than guessing, as `gauss_mk1` and `autocannon_mk1` record in
their comments. Choose `salvo` only for a def tuned for it — it empties every barrel on one
capacitor charge, multiplying damage per trigger pull by the barrel count; `trident_mk1` is the
worked example, balanced around its 3-shell volley rather than its per-shell damage.
**Its visual family is one more optional line** — `vfx_family shell|slug|ordnance`, which picks
which of the three looks its shots speak and is resolved from `kind` when omitted (ballistic →
shell, missile → ordnance). Purely cosmetic: nothing in the simulation reads it. Five of the six
catalog defs author nothing; only `longlance_rail` opts out, because a rail slug is a design
statement about a weapon rather than something to infer from `proj_speed`.
**A new module** is the same shape via `assets/modules/modules.list`.
**A new hull** is a `.ship` file with `hardpoint <id> <accepts> <size> <x> <y> <facing> <arc>`
lines. A genuinely new weapon *behaviour* needs a `Weapon` subclass plus a `WeaponKind` tag and
a branch in `weapon_instantiate`. The subclass implements `spawn_shot` and `begin_cooldown`,
**not** `fire` — `fire` is now a non-virtual composition of those two, so it cannot be
overridden and a subclass defining only `fire` will not compile. Note also that
`weapon_effective_reach` downcasts on the `WeaponKind` tag rather than dispatching virtually, so
a new subclass must be added there too or it falls into the ballistic branch. A new projectile
behaviour needs a `ProjectileKind` and handling in the combat-arena steering pass, not here.
**A new condition on whether a weapon may fire is a `WeaponFireState` value plus a branch in
`ship_weapon_fire_state`** — never a check bolted onto one fire site. All three fire-control
surfaces route through that one function, so a rule added there reaches the manual trigger, the
autopilot and the selection hub's status readout together; a rule added anywhere else silently
applies to one of the three. The validator must stay side-effect-free — it is called every frame
per tile by the hub *and* every frame per selected mount by the manual trigger while it is held:
it reports affordability, and the caller commits via `ship_try_spend_cap`.

**Known limitations / tech debt:**
- **Two parallel faction systems coexist** mid-migration: the legacy `VesselFaction` enum (for
  visuals and friendly fire) and the unified `i16 faction_id` where non-negative values index
  civs and three negative sentinels encode player, pirate and unset. Both must be set correctly
  at every spawn site; diplomacy resolves only from the latter.
- **The size-fit rule is implemented three times** — `hardpoint_fits_module` here,
  `arsenal_drag_fits` in `render/ship_scene.cpp` (acknowledged in-comment as a mirror of
  `arsenal_drop_on_slot`), and `arsenal_drop_on_slot` itself in `game.cpp`.
- **`Ship` is enormous** (~3 MB with embedded arrays), and `game_state` holds two by value —
  which is what forced placement-new in `game_init`. `ShipVisual` alone carries 512 bytes of
  path strings per layer × 8 layers, retained after load though the paths are only needed during
  resolution.
- **Texture resolution is a mandatory second phase, now in two places.** `ship_visual_load` and
  `weapon_registry_load` both record only path strings; `ship_visual_resolve_textures` and
  `weapon_registry_resolve_textures` must run later, after the renderer is live. Skipping the
  first leaves every hull handle at 0, which the engine silently renders as the 1×1 white
  texture; skipping the second is quieter — the mount just falls back to procedural art, which
  looks like "the artist's PNG never landed" rather than like a bug.
- `projectile.cpp`'s `init` bakes two textures on the **stack** (128×512 and 128×128, ~320 KB
  combined) — unlike `text.cpp` and `star_fx.cpp`, which use `static` buffers to avoid exactly
  this.
- **A size-L weapon cannot be mounted anywhere in the game.** `assets/ships/ship/ship.ship` is
  the only hull with authored hardpoints, and its three weapon slots are all MEDIUM (its one
  LARGE slot is the engine bay). `longlance_rail` is therefore dead content today: it loads into
  the registry, shows a stat card, and fits nothing. Any new L weapon inherits the same problem —
  which is why `trident_mk1` is size M. The fix is a hull with an L weapon hardpoint, not a
  change here.
- **One fire site stays off `ship_hardpoint_fire`:** the NPC-agent gunner
  (`sim/ai_ship.cpp:2193`) calls `Weapon::fire` with `sh->origin`, because a transient agent
  shoots from its hull position rather than through a hardpoint. Consistent today — those hulls
  draw no mount art either, so there is no barrel to disagree with — but a weapon's barrels are
  silently ignored there, and that is the site to fix if agents ever grow turrets.
- `spawn_missile` duplicates `spawn`'s entire free-slot loop, with a comment explaining the
  reason: `spawn` does not report which slot it filled.
- ~~`ProjectileSystem::glow_override` is a mutable pointer set by a *render* pass into a
  simulation object~~ — **fixed**: the member is gone and `render` takes a per-family glow table
  as a parameter instead. Pointer identity still determines GPU batch merging, so the table must
  point at long-lived storage; that constraint moved to the call site rather than disappearing.
- ~~`point_defense.cpp` frees projectile slots directly rather than through an API~~ — **fixed**:
  it and the three `combat_arena.cpp` sites now go through `ProjectileSystem::retire`.
  `ProjectileSystem::update` still recounts from scratch each tick, quietly repairing the
  counter, so the invariant remains unenforced rather than checked.
- Three hand-rolled `sscanf` parsers with near-identical helpers (`ship.cpp`, `module.cpp`,
  `weapon_def.cpp`); the latter two duplicate four functions almost verbatim. All three define
  `_CRT_SECURE_NO_WARNINGS` locally.
- Parse failures are graded and mostly silent: unknown `type`/`size` tokens warn and keep a
  default, registry overflow past 32 drops the tail, `.svis` layers past 8 are skipped.
- `WeaponDef::price` and `tier` are declared and marked "market-forward: unused v1".
- `sim/ship.h` references `docs/POINT_DEFENSE_AND_MISSILES.md` three times for rules not stated
  in code.

**Source paths:** `sandbox/source/sim/ship.{cpp,h}`, `sandbox/source/sim/module.{cpp,h}`,
`sandbox/source/sim/weapon.{cpp,h}`, `sandbox/source/sim/weapon_def.{cpp,h}`,
`sandbox/source/sim/projectile.{cpp,h}`, `sandbox/source/render/ship_visual.{cpp,h}`

**Last verified:** 2026-08-10, working tree on `game` (adds `ProjectileSystem::retire` / `::fx`
and the spawn-side muzzle flash; `ship_muzzle_origin` factored out as the one barrel-geometry
site; `vfx_family` and the guided-round trail history added to the data model; `render` gains an
incandescent head, takes a per-family glow table as a parameter, loses the `glow_override`
member, and loses the travelling fake muzzle flash)
