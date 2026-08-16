# SkillSystem

**Responsibility:** Owns hotkey-triggered fleet abilities: the skill-card registry
(`assets/skills/*.skill` behind `assets/skills/skills.list`), the hotbar slot bindings, the
modal entity-targeting state, the fleet-level cooldowns, and the pending volley ripple that
executes the one v1 effect (`missile_volley` / "Alpha Strike"). It explicitly does not own
input *interpretation* (game.cpp's number row and RtsControl's cast click decide what a press
means and call in), does not render (the hotbar is RmlUi, filled by `game_push_hud`), and does
not spawn projectiles on its own authority — every launch runs ShipCombatModel's canonical
`ship_weapon_fire_state` → `ship_try_spend_cap` → `ship_hardpoint_fire` chain.

**Public interface:** `sandbox/source/sim/skill_def.h` — `SkillDef` / `SkillRegistry`,
`skill_registry_load`, `skill_registry_find` (the ship/module/weapon registry twin, fourth of
the line; manifest order = hotbar slot order = number key order).
`sandbox/source/sim/skill_system.h` — `SkillSystemState` (lives in `game_state.skills`),
`skill_system_init`, `skill_system_update`, `skill_system_hotkey`,
`skill_system_cancel_targeting`, `skill_system_cast_entity`, `skill_system_collect_tubes`.

**Depends on:** ShipCombatModel (fire chain, turret aim, `weapon_lead_dir`), FleetControl
(roster, selection), CombatArena (`combat_entities` for target validation), ActionLog,
GameStateModel; engine `defines.h`, `math_utils.h`.
**Depended on by:** FrameOrchestrator (tick, number row, ESC, HUD push, action drain),
RtsControl (cast click, cancel arbitration).

**Data flow:** card → registry (init) → slot (init) → targeting (hotkey / `skill:N` click) →
volley (cast click) → fire chain (ripple tick), with the HUD reading slots + volley each frame.

**Key invariants:**
- **Tick order contract (game.cpp):** `skill_system_update` runs AFTER the weapon-cooldown /
  capacitor block and BEFORE `rts_controls.update` and `update_autopilot` — so the skill
  claims it asserts this tick are in place when `update_attack` reads them, and a cast made
  this frame executes against current cooldown state.
- **An executing volley CLAIMS its mounts** (`Ship::skill_claimed`, the flak-claim contract:
  cleared and re-asserted every skill tick while the entry is pending, released the moment it
  fires or the volley ends). `update_attack` yields a claimed mount — no aim assertion, no
  autopilot fire — so a standing attack order on a different target cannot hold a volley tube
  off the player's designated one. An explicit cast outranks the standing order for exactly
  the tubes it committed, and only for the volley's lifetime.
- **The volley is reload-tolerant and bounded.** A cast commits every mounted, operational
  missile tube (reload state deliberately ignored — under `MISSILE_FREE` the autopilot
  respends each tube the frame it readies, which used to pin the badge at zero all fight).
  Pending entries train on the target every tick and retry each stagger beat; transient
  states (`RELOADING / SLEWING / NO_BEARING / OUT_OF_RANGE / STARVED`) simply stay pending,
  permanent ones (participant gone, tube unmounted) skip, and `SKILL_VOLLEY_TIMEOUT_S` (10 s)
  abandons stragglers so claims can never be held open forever.
- **The volley never cheats.** Every launch runs the shared validator, then the capacitor
  commit, then the shared spawner — nothing fires faster than its reload or without power.
  No new `WeaponFireState` exists for skills.
- **Doctrine is not consulted.** The cast path never reads `missile_policy` / `roe` / `stance`
  / `cap_fire_floor` — an explicit cast is player intent, the manual-trigger contract. (The
  flak claim still wins: that is a survival invariant, not doctrine.)
- **Cooldown commits on the FIRST actual launch** (`fired` 0 → 1), never at cast. A volley
  ending with `fired == 0` costs nothing and fires the denial feedback instead.
- **`Ship*` safety is the attack_target convention.** `SkillVolley::target` is re-validated
  against the combat-entity pool every tick (a dead NPC stops being re-registered and vanishes
  a frame later); participants are stored as `(fleet_idx, hp_index)` INDICES and re-validated
  at launch time, because the fleet's active count can shrink (`Fleet::set_count`) and
  loadouts mutate mid-ripple.
- **"What you see is what fires":** the HUD badge and the cast run the SAME query
  (`skill_system_collect_tubes`), so the number on the button is the number of missiles a
  cast will put in space over the ripple (not the instantaneous loaded count).
- **The number row is mode-split** on `camera_state.free_camera_active` (+ `!recentering`):
  detached = skill hotbar 1–9, attached = fire groups 1–5. This deliberately re-takes the
  detached row that the fire groups had claimed (see the retired note in `rts_controls.cpp`);
  detached group selection lives in the HUD's `group:` rows.
- **Armed modes are mutually exclusive:** arming a skill drops jump mode and vice versa; both
  drop on camera attach. Cancel gestures — re-press, ESC, X, RMB, empty-space LMB — are all
  consumed (no order issued, no attack order cleared, no selection change, no app quit).

**HUD action grammar:** `skill:N` (hotbar click) drains into `skill_system_hotkey(s, N)` — the
same entry point as key N+1, so click and key cannot diverge. Snapshot fields:
`skills_visible`, `skill_count`, `skill[9]` (`bs_rml_skill_slot`), `skill_target_visible`,
`skill_target_label`; markup `#skillbar` / `#skilltarget` in `assets/ui/hud.rml`. The cooldown
strip is the `fleet_cap_w` string-bind recipe (`cd_w` must always hold a valid CSS length).
`#jumpmode` and `#skilltarget` share the banner slot at `bottom: 104px` — legal because the
two armed modes are mutually exclusive.

**Extension points:**
- **A new skill effect** is one `SkillEffect` enum value, one branch in the cast/ripple path,
  and one `.skill` card (plus a `skills.list` line, which is also its hotbar slot). Cards tune;
  code implements — the `WeaponKind` split.
- **`targeting instant|point`** are parsed and defaulted but have no v1 producer: INSTANT
  needs a branch in `skill_system_hotkey` (execute on press), POINT needs a cast-at-position
  sibling of `skill_system_cast_entity` and a world click in RtsControl's targeting block.
- **`scope fleet`** on a card forces fleet-wide participation regardless of selection.
- Time-on-target (delaying near ships so all missiles arrive together) would replace the fixed
  `stagger` beat with per-entry release times computed at cast — the queue shape already
  supports it.

**Known limitations / tech debt:**
- **Missiles are fire-and-seek** (ShipCombatModel): the lead-solved aim biases seeker
  acquisition toward the clicked hull, but the skill cannot *guarantee* the clicked target
  takes every hit — identical to the autopilot's launches. Not a bug.
- The piloted hull's manual cursor-traverse can still contest a volley tube's aim while
  ATTACHED (manual aim asserts before the skill tick each frame and wins) — rare, since
  casting is detached-only; the tube retries until the timeout either way.
- The fourth `sscanf` parser (`skill_def.cpp`) re-duplicates the rstrip/quoted-string helpers
  per the standing decision recorded in ShipCombatModel.md.
- One volley in flight fleet-wide (`SkillSystemState::volley` is a single slot); a second cast
  during a ripple is denied. Fine for v1's one skill; a per-slot volley array is the fix if
  two skills ever need to overlap.
- The hostility test for the cast click inherits `m_hovered_enemy_idx`'s legacy
  `ce->faction != player` check (RtsControl), not `faction_id` /
  `galaxy_history_faction_is_hostile` — consistent with the RMB attack order, and the same
  Feature-B migration seam.
- Cooldowns tick with `sim_dt`, so a cast enqueued while paused (RtsControl runs on real dt)
  holds until unpause. Intended: reloads behave the same way.

**Source paths:** `sandbox/source/sim/skill_def.{cpp,h}`,
`sandbox/source/sim/skill_system.{cpp,h}`, `assets/skills/`

**Last verified:** 2026-08-16
