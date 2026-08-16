# Point-Defense & Missile Combat — Design + Implementation Plan

> **Status: phases A–E implemented** (see the log below). This document remains the
> design reference; tuning lives in the editor panel (MISSILES / CAPACITOR / FLAK
> sections) and the constants noted per phase.
>
> Combat controls: `P` cycles PD stance (HOLD/STANDARD/OVERDRIVE); `T` toggles the
> active fire group between AP and FLAK; PD priority + engagement gate are set from
> the flagship inspector's "Point defense" chips.
>
> Implementation log:
> - **A — Guided missiles**: fire-and-seek `PROJ_MISSILE` (steering pass in
>   `combat_arena.cpp`, flight model `game_state::missile_tuning`), `MissileLauncher`
>   (`weapon.cpp`), enemy launcher auto-mounted, flagship launcher in stash slot 4,
>   `ic-missile` emblem.
> - **B — Capacitor**: `Ship::cap_*` baseline→derived (`ship_recompute_stats`),
>   optional `capacitor <max> <regen>` in `.ship` files, `Weapon::cap_cost()`
>   (cannon 4 / missile 25), fire-site gating, PD drain 6/s + 15% reserve floor.
> - **C — Doctrine**: `DefenseLaser` stance/priority/gate_tier; OVERDRIVE = 2x dps,
>   3x drain, 0.5x retarget; TTI / missiles-first / nearest scoring; inspector chips
>   round-tripped via `pd:*` HUD actions.
> - **D — Flak**: `MODE_FLAK` on `BallisticWeapon` (0.6x speed, 1.4s life), fuse +
>   linear-falloff burst pass vs hostile ordnance (`game_state::flak_tuning`), flak
>   never damages hulls.
> - **E — Feedback**: fleet-panel capacitor bar + PD doctrine line (amber off-STANDARD),
>   attributed logs (`PD: missile intercepted`, `MISSILE HIT -- PD holding / capacitor
>   dry / PD saturated`, `Flak burst: N ordnance destroyed`), gated engagement ring in
>   `defense_laser_overlay.cpp`.
> - **F — Pool equipment (2026-08-12)**: the PD DEVICE became fleet-pool inventory like
>   the weapons — `game_state::fleet_pd_stock` (one device), mounted onto a defense
>   hardpoint from any ship's inspector. `DefenseLaser::enabled` now means "device
>   mounted on this hull" (default FALSE; set only by the mount/unmount paths), and
>   `point_defense_update` gates on `enabled && point_defense_mount >= 0`. Doctrine
>   UI, the status lines and the engagement ring all follow the mount. The per-hull
>   `DefenseLaser` struct remains the device's tuning/doctrine storage. "More PD
>   mounts = more beams" (Phase C) still holds — scaling now means acquiring devices.
> - **G — Autonomous flak screen + PD localization (2026-08-15)**: flak became the
>   fleet's AUTONOMOUS outer defense layer (`sim/flak_screen.cpp`, ticked before PD in
>   the arena): under the per-ship FLAK AUTO doctrine every ballistic mount screens
>   inbound hostile ordnance with per-shot flak rounds (a `fire_mode` swap around
>   `ship_hardpoint_fire` — AP guns keep their AP loadout for hull work), dedicated
>   `MODE_FLAK` mounts are pure screens that pre-aim their sector when idle, defense
>   preempts offense via per-tick `Ship::flak_screen_claimed` claims the attack
>   autopilot yields to, and side discipline scores candidates inside a fit window
>   around the mount's authored facing so a starboard gun never fires across the hull.
>   PD's range decoupled from the sensor suite: `DefenseLaser::range` defaults to
>   5000 (0 = legacy Layer-0 coupling), making PD the inner point layer inside the
>   flak envelope (`weapon_flak_reach` ≈ 8–10k). The T-key manual flak channel is
>   unchanged on top.
> - **H — Engagement doctrine surface (2026-08-16)**: doctrine went in-game. The
>   inspector's DOCTRINE tab gains an Engagement panel — ROE (weapons free / return
>   fire / hold), flak doctrine (auto screen / manual), missile policy (free /
>   conserve / hold) and gun cap floor (off / 15% / 25% / 40%) as chip rows with
>   tooltip prose, actions `doc:*`. The fleet roster rows gain a 5th chip cycling the
>   ship's ROE (`froe:R`, selection-wide like the stance chips). `.ship` cards can
>   author doctrine defaults (`roe` / `flak` / `missiles` / `cap_floor` lines) applied
>   to player fleet spawns via `FleetShip::apply_card_doctrine`.
> - **I — Missile offense buildout (2026-08-16/17)**: missiles became a first-class
>   offensive system. The Alpha Strike fleet skill (`sim/skill_system.cpp`, see
>   SkillSystem.md) volleys every committed tube at one designated hull, claiming the
>   mounts (`Ship::skill_claimed`, the flak-claim contract) so the attack autopilot
>   yields them. `weapon_engage_range` split tactical range from flight endurance
>   (card `engage_range`; approach / ROE acquisition / avoid / HUD ring read it, the
>   fire gate keeps `weapon_effective_reach`). Missiles COLD-LAUNCH: card
>   `eject_speed` + `ignition_delay` — expelled along the firing cell's facing, an
>   unpowered plume-less coast, then ignition and a turn-rate-limited arc onto the
>   stored launch aim until the seeker locks (`combat_arena.cpp`). The Vanguard grew
>   a five-cell missile-only nose battery (`missile` accepts kind, spine cell L), and
>   the catalog grew Skewer (S, quick-cycle), Hydra (M, 3-tube salvo cluster) and the
>   Heavy Torpedo made mountable. PD/flak counterplay is unchanged — a cold round is
>   burnable the whole way.

Design principle (agreed): **automated hands, player brain.** PD tracking is never manual;
player agency lives in doctrine (stances/priorities), a shared resource (capacitor),
geometry (positioning/screens), and a manual skill channel (flak cannons).

## Baseline at planning time (historical — superseded by the log above)

- `sim/point_defense.cpp`: fully automated single-beam PD per ship — acquires *nearest*
  hostile projectile in Layer-1 sensor range, dwell 0.15s, retarget cooldown 0.08s,
  DPS vs projectile HP. Free to run (no resource), always-on (`enabled`).
  *(No longer true: phase B priced it, phase C rescored it, phase F made the device
  pool equipment that must be mounted.)*
- `sim/projectile.h`: dumbfire ballistic only; has `hp/max_hp` (PD-ready), no guidance.
- No capacitor/energy system on `Ship`. No weapon fire modes. Missiles don't exist.
- Fire groups 1–5 + gm-matrix already wired through HUD; fleet panel has room for a
  stance chip; `defense_laser_overlay` draws beams.

Dependency order: **A (missiles) → B (capacitor) → C (doctrine) → D (flak) → E (feedback)**.
A and B are independent and could swap; C/D depend on both.

---

## Phase A — Guided missiles (the threat that motivates everything)

**Sim**
- `Projectile` += `kind` (`PROJ_SHELL` / `PROJ_MISSILE`), `target_ship` (fleet index or
  entity id; -1 = dumbfire), `turn_rate`, `accel`, `max_speed`. Missiles: high `hp`
  (~3–4× shell), high `radiation_emission` (hot engine = sensor-visible: counterplay
  via detection), finite lifetime → self-destruct.
- Guidance in the projectile update: pure-pursuit first (steer velocity toward target's
  live position, clamped by `turn_rate`); proportional navigation later if pursuit is
  too easy to outrun. Target dead/out of range → go dumbfire until lifetime expires.
- `MissileLauncher : Weapon` (weapon.h/.cpp): salvo size, per-tube reload, lock range
  (Layer-1 coupled like PD); `icon = "ic-missile"`.
- AI: ship-AI fire sites gain launcher support (same fire path as cannons).

**Assets**: `ic-missile` emblem in `tools/ui_icon_gen.py` + `ui-icons` sheet (reserved
slot exists; rectilinear grammar: finned dart on the family base plate).

**Acceptance**: hostile ship launches; missile arcs to the player; PD engages it
automatically; cannon fire can also kill it (it's just a high-HP projectile).

## Phase B — Capacitor: the shared budget that makes automation a decision

**Sim** (`ship.h` + new `sim/capacitor.cpp` or folded into ship update)
- `Ship` += `cap_max`, `cap_current`, `cap_regen` (baseline authored in `.ship`,
  recomputed via `ship_recompute_stats()` so future capacitor modules multiply it —
  same composition pattern as sensors).
- Costs: cannon shot = flat cost on fire; PD = drain *per beam-second* (stance-scaled,
  Phase C); missile launch = large flat cost.
- Starvation rules: below a PD reserve floor the laser stops acquiring; a weapon that
  can't afford its shot simply doesn't fire (no partial shots). Regen is continuous.
- Order of ops: PD drain happens inside `point_defense_update` (already ticked before
  projectile advance).

**Acceptance**: sustained cannon fire + active PD visibly compete; easing off the
trigger restores PD effectiveness within seconds.

## Phase C — PD doctrine: stances, priorities, engagement gate

**`DefenseLaser` extensions** (`ship.h`, logic in `point_defense.cpp`)
- `stance`: `PD_HOLD` (no fire, no drain — stealth/heat discipline) /
  `PD_STANDARD` / `PD_OVERDRIVE` (2× DPS, ~3× capacitor drain, halved retarget
  cooldown). Replaces the raw `enabled` bool (HOLD subsumes it; editor keeps a master
  override).
- `priority`: `PD_PRI_TTI` (default: min time-to-impact = dist / closing-speed toward
  *this ship*) / `PD_PRI_GUIDED_FIRST` (missiles before shells, TTI within class) /
  `PD_PRI_NEAREST` (legacy). Replaces the nearest-scan scoring loop — one score
  function, ~20 lines.
- `gate`: engagement range as a fraction of the resolved range (0.5–1.0). Engage-early
  wastes dwell on far targets; engage-late is capacitor-efficient but unforgiving.
  (Arc/sector gating deliberately **cut** — turret is a 360° dome; arcs add micro
  without a matching decision.)
- Saturation is emergent (dwell + retarget cooldown + capacitor); no artificial beam
  cap needed while PD remains one beam per mounted PD. More PD mounts = more beams,
  which keeps loadout as the scaling lever.

**Controls**
- One combat keybind cycles stance (pattern: existing F-key toggles in `game.cpp`
  ~2080); logs "[PD] OVERDRIVE" etc.
- Doctrine (priority, gate) set in the flagship inspector — new "Point defense" section
  under the Arsenal tab: three-way stance row + priority row (clickable rows, same
  `hud_action` pattern as fire groups; snapshot fields + actions `pd:stance:N` /
  `pd:pri:N`).

## Phase D — Flak: the manual skill channel

- `BallisticWeapon` += `fire_mode`: `MODE_AP` (current) / `MODE_FLAK`. Flak shells:
  slower, short lifetime, proximity fuse — when any *hostile* projectile is within
  `fuse_radius`, detonate: radial HP damage to projectiles in `burst_radius` (shells
  and missiles alike). No ship damage in v1 (keeps it purely defensive; revisit later).
- Fuse check lives in the projectile update pass (`combat_arena_update_projectiles`),
  scanning hostile projectiles only — pool is small (`MAX_PROJECTILES`), O(n²) worst
  case is fine at current sizes; spatial bucket only if profiling says so.
- Mode is toggled per weapon from the inspector (weapon tile context or a mode chip on
  the fire-group matrix column — decide at UI review). Assigning flak weapons to their
  own fire group is the intended playstyle: number key = defensive barrage under
  manual lead. High skill = leading the salvo; PD remains the reliable floor.

## Phase E — Readable feedback (agency requires attribution)

- **Fleet panel**: capacitor bar (reuse the planet-gauge recessed-channel styling) +
  PD stance chip line ("PD: OVERDRIVE", amber when not STANDARD). New
  `bs_rml_hud_state` fields (fleet_cap_w, fleet_pd_stance) — small ABI bump, engine +
  sandbox rebuilt together as in Phase 4.
- **Action log attribution**: `PD: missile destroyed`, `MISSILE HIT — PD saturated`,
  `MISSILE HIT — capacitor dry`, `PD holding (stance)`. Attribution decided at impact
  time from PD state (was it starved? was every dwell window occupied?).
- **Overlay**: dashed engagement ring at the gated PD range (extend
  `defense_laser_overlay`; reuse the discovery-sensor ring drawing); missile threat
  markers can ride the existing projectile rendering with a distinct tint.

---

## Starting tuning values (all editor-exposed where sliders already exist)

| Knob | Value | Rationale |
|---|---|---|
| Missile hp | 3.5 (shell = 1.0) | ~0.3s of STANDARD PD dwell to kill |
| Missile turn_rate | ~90°/s | outrunnable by corvettes, not cruisers |
| PD OVERDRIVE | 2× DPS, 3× drain, 0.04s retarget | burst insurance, not a sustain mode |
| Capacitor | 100 cap, 8/s regen; cannon 4/shot, PD 6/beam-s (STANDARD), missile 25 | PD sustain ≈ 75% uptime alone; heavy overlap starves |
| PD reserve floor | 15% | PD dies last, feels intentional |
| Flak fuse/burst radius | ~1.5× / 3× shell radius | rewards leading, punishes spray |
| Salvo to saturate 1 PD | 3 missiles inside one dwell+cooldown window | escorts/flak required vs volleys |

## Validation per phase

Each phase: engine untouched except Phase E's snapshot fields; sandbox rebuild +
`-Wall -Werror` clean; manual scenario via the editor (spawn hostile, grant launcher).
A: missile chases and PD kills it. B: PD starves under sustained fire. C: stances
change outcomes vs a fixed 3-missile salvo (HOLD = hits, OVERDRIVE = clean sweep,
STANDARD = 1 leaker). D: flak wall stops a salvo PD alone cannot. E: every missile
outcome appears in the log with a cause.

## Deliberately cut (and why)

- Manual PD aiming — execution, not decision.
- Per-missile target assignment / mid-fight arc editing — micromanagement with no
  interesting tradeoff.
- PD ammo — capacitor already prices automation; two currencies obscure attribution.
- Beam-count caps — saturation already emerges from dwell windows + capacitor.
