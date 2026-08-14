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
`PdStance`/`PdPriority`, `ShipSizeClass`, `ShipMotion`; `ship_recompute_stats`,
`ship_try_spend_cap`, `ship_capacitor_update`, `ship_collider_corners`, `ship_bounding_radius`,
`ship_local_dir`, `ships_collide`, `hardpoint_accepts`, `hardpoint_fits_module`,
`ship_first_free_hardpoint`, `ship_hardpoint_fire_origin`, `ship_muzzle_origin`,
`ship_hardpoint_aim_goal`, `ship_hardpoint_unit`, `ship_hardpoint_fire`,
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
`sim/ship_def.h` — `ShipDef`, `ShipRegistry`, `SHIP_REGISTRY_MAX`, `ship_registry_load`,
`ship_registry_resolve_textures`, `ship_registry_find`, `ship_instantiate` (the old
direct-from-file `ship_load` is retired — every hull is a registry card).
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
- **The point-defense DEVICE is fleet-pool equipment, not a per-hull fixture.** The fleet owns
  `game_state::fleet_pd_stock` devices (init 1); the bay shows the PD tile while stock > 0,
  mounting decrements it and unmounting/evicting increments it, exactly like the weapon stash.
  The per-hull `DefenseLaser` struct is only the device's tuning/doctrine storage, inert unless
  mounted: `enabled` defaults FALSE and is set only by the mount/unmount paths, and the
  simulation gates on `enabled && point_defense_mount >= 0` (`sim/point_defense.cpp`) so an
  editor override can never arm a hull without the device. The PD range ring
  (`render/defense_laser_overlay.cpp`) and the missile-hit attribution (`sim/combat_arena.cpp`)
  carry the same mount test. The HUD reads the mount too: `pd_visible`, the inspector's PD
  status line and the piloted panel's `fleet_pd_label` all show doctrine only for a hull that
  carries the device, and read `"PD --"` otherwise.
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
  traverse arc, **slew convergence**, cooldown, reach and capacitor affordability into one
  `WeaponFireState` and spends nothing — the caller commits via `ship_try_spend_cap` only after
  `WEAPON_FIRE_READY`.
  *(Until the micro-selection work the manual path never checked reach at all, so a click far
  beyond the drawn range ring still fired and still drained the bank.)*
- **The arc test and the slew test are different questions, and both are needed.**
  `ship_hardpoint_can_aim` answers "can this mount ever point there"; `WEAPON_FIRE_SLEWING`
  answers "does it, yet". Without the second, a held trigger fired the instant the target
  entered the arc while `mount_aim` was still mid-rotation, and since the muzzle resolves
  against `mount_aim` (below) the round left the correct barrel tip at a visible angle to the
  barrel. The tolerance is deliberately a VISUAL one — it bounds how far the barrel may be off
  the shot, not how far the shot may be off the target, because the round still flies exactly
  along `world_dir`. ~3 degrees is imperceptible on the art and comfortably above one frame's
  slew (a medium mount turns 2.2 rad/s, so 0.037 rad at 60 fps), which is what keeps a turret
  tracking a moving cursor firing instead of stuttering every time the goal shifts.
  *Note this reaches the autopilot too, by design — that is what routing every rule through one
  validator buys. It does NOT reach `combat_arena`'s enemy gunner or `ai_ship.cpp`'s agent
  gunner, both of which bypass the validator; neither matters today because the enemy hull has
  no hardpoints and agents fire from their hull origin with no turret to disagree with.*
- **The trigger and cursor turret-traverse are ATTACHED-ONLY; zoom and render look never gate
  them.** With the command overlay retired, box/click selection owns the left button whenever
  the camera is detached (RtsControl), so `game_update`'s traverse+fire block tests
  `free_camera_active` — the button has exactly one meaning per control mode, at any zoom, in
  either look. (It was briefly mode-independent while selection lived behind the bounded
  overlay moment; the mode split replaced that arbitration.) The traverse goes with the
  trigger deliberately: a barrel tracking a cursor that cannot fire would be a lie. The
  fire-group number row still runs in both modes, and `control_ship_global` still self-guards
  on `free_camera_active` (`sim/ship_control.cpp:21`), so detached always means the autopilot
  flies.
- **Manual gunnery exists only in the pilot seat; everything else is automated.** The hull the
  player is hand-flying is skipped by the autopilot entirely (`update_autopilot`'s
  `auto_skip`), and that skip is the whole player-vs-autopilot fire arbitration — there is no
  per-hull gunnery flag any more. Every autopilot-driven ship fires missiles AND ballistics
  under an attack order (`sim/fleet.cpp`), including the last-piloted hull the moment it is
  released, and point defense engages on its own doctrine wherever the fleet's PD device is
  mounted. `WeaponKind` still expresses the guided/unguided distinction at every fire site.
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
  the same hardpoint-half-extent units as `mount_art_size` — and both now resolve through
  `ship_hardpoint_unit`, which folds in the slot's `art_scale` — so art and shot origins scale
  together and cannot drift apart. The two can still differ *in angle* mid-traverse, but
  `WEAPON_FIRE_SLEWING` now bounds that gap to the aim tolerance rather than letting it run to
  the full width of the arc.
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
- **Ship instances make the same bargain:** `Ship::def` and `Ship::vessel_name` point into the
  ship registry's fixed pool (`game_state::ship_registry`). This also FIXED a latent dangler —
  the old `ship_load` aimed `vessel_name` at its own stack line buffer, masked only because
  every spawn site overwrote the name with a string literal.
- **`ship_registry_resolve_textures` must run BEFORE `ai_ships_init`** (both in `game_init`):
  the NPC templates copy the def's texture handles at instantiate time, and every `NpcShip` is
  struct-copied from those templates. The fleet/enemy instances are built even earlier — before
  the resolve — so they keep their own per-instance `ship_visual_resolve_textures` pass later
  in `game_init`, exactly as before.
- **A card's `hull` line is an override, not the only source of HP.** `ShipDef::hull_authored`
  records whether the line was present; CombatArena seeds player/enemy entities from
  `Ship::hull_max_hp` unconditionally (default 100 = the old literal), while LocalAgentAi keeps
  its per-archetype literals for any hull whose card does not author `hull` — so NPC balance is
  untouched until a card deliberately opts in.
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
**A new hull is a `.ship` card in `assets/ships/ships.list`** — an `id` line (required; the
registry key spawn sites pass to `ship_registry_find`) plus optional stat lines, each
defaulting to the pre-card behaviour: `hull <max_hp>` (entity health; default 100),
`motion <max_speed> <accel> <decel> <turn_accel> <max_turn>` (overrides the class tuning
table), `sensors <l0> <l1> <l2>` (baseline suite, validated strictly increasing),
`desc "..."`, and market-forward `price`/`tier`. `assets/ships/ship/ship.ship`
(`vanguard_cruiser` — by design a high-tier LATE-game hull; the starting fleet flies it only
until a dedicated starter hull card exists) is the worked example. The art/skeleton lines are
unchanged, including
`hardpoint <id> <accepts> <size> <x> <y> <facing> <arc> [art_scale]` lines. The trailing scale is
optional and presentational — it multiplies everything the slot draws *and* the barrel origins
with it, so a rescaled mount still fires from its muzzle; omit it and the slot behaves exactly
as before it existed, which is why every pre-existing 7-field line is untouched. The hardpoint
editor writes it into its "Log .ship hardpoint lines" dump only when it differs from 1.0. A genuinely new weapon *behaviour* needs a `Weapon` subclass plus a `WeaponKind` tag and
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
- Three hand-rolled `sscanf` parsers with near-identical helpers (`ship_def.cpp`, `module.cpp`,
  `weapon_def.cpp`); the latter two duplicate four functions almost verbatim. All three define
  `_CRT_SECURE_NO_WARNINGS` locally. (The ship parser moved whole from `ship.cpp` into
  `ship_def.cpp` with the registry work; the duplication stands.)
- Parse failures are graded and mostly silent: unknown `type`/`size` tokens warn and keep a
  default, registry overflow past 32 drops the tail, `.svis` layers past 8 are skipped.
- `WeaponDef::price` and `tier` are declared and marked "market-forward: unused v1".
- `sim/ship.h` references `docs/POINT_DEFENSE_AND_MISSILES.md` three times for rules not stated
  in code.

**Source paths:** `sandbox/source/sim/ship.{cpp,h}`, `sandbox/source/sim/ship_def.{cpp,h}`,
`sandbox/source/sim/module.{cpp,h}`,
`sandbox/source/sim/weapon.{cpp,h}`, `sandbox/source/sim/weapon_def.{cpp,h}`,
`sandbox/source/sim/projectile.{cpp,h}`, `sandbox/source/render/ship_visual.{cpp,h}`

**Last verified:** 2026-08-13, working tree on `game` (hulls become registry stat cards —
`sim/ship_def.{h,cpp}` adds `ShipDef`/`ShipRegistry` mirroring the weapon/module pattern, a
manifest at `assets/ships/ships.list`, and `ship_instantiate`; `ship_load` is retired, every
spawn site converted, and the card grows `id`/`desc`/`hull`/`motion`/`sensors`/`price`/`tier`
lines with behaviour-preserving defaults; live-verified: 5 cards load, all instantiate sites
log, an authored `hull 140` flows through to the instance and back to 100 on revert. Same day,
earlier: manual gunnery becomes ATTACHED-ONLY —
the trigger and cursor traverse gate on `free_camera_active` now that detached LMB is RTS
selection, and the autopilot's `auto_skip` is the entire fire arbitration; live-verified:
attached LMB fires and drains the capacitor, detached LMB only box-selects. Previously
2026-08-12: the point-defense becomes fleet-pool equipment — `DefenseLaser::enabled` defaults
FALSE, the simulation and its two mirroring consumers gate on the mount, and
`game_state::fleet_pd_stock` backs the bay tile and the mount/unmount/evict paths; AI wingmen
fire ballistics under attack orders — see FleetControl)
