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
`ship_first_free_hardpoint`, `ship_hardpoint_fire_origin`, `ship_hardpoint_fire`,
`ship_hardpoint_can_aim`,
`ship_select_bearing_weapon`, `ship_turret_aim_at`, `ship_update_turrets`;
`WeaponFireState`, `ship_weapon_fire_state`, `ship_hardpoint_in_selection`,
`ship_select_weapon_override`, `ship_clear_weapon_override`, `ship_hardpoint_in_group`,
`ship_nth_group_weapon`.
`sim/weapon.h` — `Weapon`, `BallisticWeapon`, `MissileLauncher`, `WeaponKind`, `FireMode`,
`weapon_effective_reach`. `sim/weapon_def.h` — `WeaponDef`, `WeaponRegistry`,
`weapon_registry_load`, `weapon_registry_resolve_textures`, `weapon_registry_find`,
`weapon_instantiate`.
`sim/module.h` — `ModuleDef`, `ModuleRegistry`, `module_registry_load`, `module_registry_find`.
`sim/projectile.h` — `Projectile`, `ProjectileKind`, `ProjectileSystem`, `MAX_PROJECTILES`.
`render/ship_visual.h` — `ShipVisual`, `VisualLayer`, `ship_visual_load`,
`ship_visual_resolve_textures`.
`ship.h` is included by 7 other subsystems; `weapon.h` by 6.

**Depends on:** GameStateModel; engine `math/math_utils.h`, `math/bs_hierpos.h`,
`renderer/renderer.h`, `renderer/renderer_types.h`, `core/logger.h`, `defines.h`.
It has **out-degree 0** to other sandbox subsystems — the most self-contained large cluster.
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
- **Manual fire control is gated on camera ATTACHMENT, not on `view.mode`.** `game_update`'s
  piloting branch has no mode test: the arena↔galaxy flip is a label over one coordinate space,
  so flying, turret traverse, the fire-group row and trigger firing all keep working at any
  zoom in either look. What separates the two control schemes is `free_camera_active`, which
  every block inside gates on — `control_ship_global` self-guards (`sim/ship_control.cpp:21`),
  and the number row and the trigger are suppressed while detached because RtsControl owns the
  left button there (it is box/click selection, not fire). Detached engagement therefore runs
  through attack orders, which honour the override.
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
- **`ship_hardpoint_fire` is the one per-shot SPAWNER, the twin of `ship_weapon_fire_state`
  being the one per-shot validator.** The manual trigger (`game.cpp`), the RTS attack order
  (`sim/fleet.cpp`) and both combat-arena gunners route through it, so where a weapon's shots
  physically leave the hull is answered once. That is what makes barrels a data question: a
  `.weapon` def listing `muzzle` offsets gets them on the player's guns, the autopilot's and
  the NPCs' together, with no fire site aware that barrels exist. Callers still validate and
  spend the capacitor first; the spawner only spawns.
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
- **`Weapon::disabled` is declared with no producer.** There is no subsystem-damage model yet;
  the flag exists so the validator and the hub's Disabled state are correct the day one lands.
- **Weapon instances point their `name`/`icon` into the registry's pool storage**, which
  `sim/weapon_def.h` justifies as safe because the fixed pool never reallocates. The registry
  therefore must outlive every weapon — it does, living in `game_state`.
- **Module defs are shared by pointer across ships** and carry no per-instance state
  (`sim/module.h`), which is what makes sharing safe.
- Sensor layers are strictly ordered `l0 < l1 < l2`; `sim/ship.h` notes the editor enforces it.

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
**A new module** is the same shape via `assets/modules/modules.list`.
**A new hull** is a `.ship` file with `hardpoint <id> <accepts> <size> <x> <y> <facing> <arc>`
lines. A genuinely new weapon *behaviour* needs a `Weapon` subclass plus a `WeaponKind` tag and
a branch in `weapon_instantiate` — note `weapon_effective_reach` downcasts on that tag rather
than dispatching virtually, so a new subclass must be added there too or it falls into the
ballistic branch. A new projectile behaviour needs a `ProjectileKind` and handling in the
combat-arena steering pass, not here.
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
- `ProjectileSystem::glow_override` is a mutable pointer set by a *render* pass
  (`render/gameplay_overlays.cpp:151`) into a simulation object, and its identity determines GPU
  batch merging.
- `point_defense.cpp` frees projectile slots directly (`p.active = FALSE; --count`) rather than
  through an API, and `ProjectileSystem::update` recounts from scratch each tick, quietly
  repairing the counter.
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

**Last verified:** 2026-08-07, commit `e83a88d` + the weapon-hub group-filter change (added
`ship_hardpoint_in_group` / `ship_nth_group_weapon`, removed `ship_nth_mounted_weapon`) + the
cannon mount-art change (added the `mount_art*` fields and `weapon_registry_resolve_textures`) +
the multi-barrel change (added `muzzle*` fields, `ship_hardpoint_fire`, the
`fire` -> `spawn_shot` + `begin_cooldown` split, and `trident_mk1` as the first salvo weapon)
