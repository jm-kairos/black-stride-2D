# Crew Job System — Multi-Session Roadmap

> **Baseline:** Phases 1–5 COMPLETE. The system is live: `TILE_HELM`, `JOB_PILOTING`, queue ops, the per-frame runner, the pilot gate on `control_ship_global`, and the ImGui Crew Job Panel (assign / remove / reorder / cancel) are all built and running. Two crew members are spawned and the panel targets whichever is selected.
>
> **Goal:** Finish Phase 6, extend job types beyond piloting, and harden the system for save/load.

---

## Session A — Close Phase 6 (Docs + Spec + Hardening)

**Scope:** documentation, edge-case fixes, and regression verification. No new gameplay.

- [ ] Write `docs/CREW_JOB_SYSTEM_DESIGN_NOTES.md` (mirrors `MAP_GENERATION_DESIGN_NOTES.md`).
- [ ] Update `blackstride-prototype-spec` to graduate the crew control model:
  - Local control = select + pathfinding + job assignment (was WASD-direct).
  - Global flight requires an active pilot at the helm.
- [ ] Edge-case pass in `crew_jobs.cpp`:
  - Assign while already walking → new job interrupts current (already does, verify).
  - Helm destroyed / tile overwritten mid-job → `JOB_FAILED` path (defensive).
  - Empty queue + `has_current` cleared → runner stays idle (already does, verify).
- [ ] Regression: confirm global→local spin-carryover still settles; confirm coasting is identical with/without pilot.
- [ ] Delete or archive the stale `.hermes/plans/crew-job-system-and-ui.md` (replaced by this roadmap).

**Definition of done:** Build clean (`-Wall -Werror`), design doc committed, spec patched.

---

## Session B — New Job: Repair + `TILE_REPAIR_BAY`

**Scope:** first non-piloting job to prove the generic tile↔job seam.

- [ ] Add `TILE_REPAIR_BAY` to `ship.h` enum; update `ship.cpp` (`char_to_tile 'R'`, walkable, color).
- [ ] Place one `R` in `assets/ship.tmap` (engine room or lower deck).
- [ ] Add `JOB_REPAIR` to `job.h`; extend every `switch` in `crew_jobs.cpp` (type name, station tile, station name, runner behavior).
- [ ] Runner behavior for Repair:
  - `QUEUED` → resolve repair bay tile → A* → `MOVING_TO_TARGET`.
  - `EXECUTING` → duration-based (e.g. 5s), then auto-`COMPLETED` (unlike Piloting which is persistent).
  - On complete, emit a log; crew becomes idle and picks next job.
- [ ] Panel: "Assign Repair" button appears; progress bar works for duration-based jobs.
- [ ] Add `ship_repair_progress` or a generic `job_duration` field on `Job` so future timed jobs reuse the same mechanic.

**Definition of done:** Screenshot shows crew walking to repair bay, progress bar filling over 5s, "Completed" state, then idle.

---

## Session C — New Job: Man Station + `TILE_GUN_DECK`

**Scope:** second non-piloting job; test persistent vs. timed semantics in the same crew.

- [ ] Add `TILE_GUN_DECK` (char `G`); place in `ship.tmap`.
- [ ] Add `JOB_MAN_STATION` — persistent like Piloting (crew holds the station until interrupted).
- [ ] Verify mixed queue works: e.g. queue `[Repair, Man Station, Piloting]` → crew repairs, then walks to gun deck and holds, then walks to helm and holds.
- [ ] Panel: show station name per row; disable reorder on an in-flight job? (decide: yes, safer).

**Definition of done:** Click-through screenshot sequence of the mixed queue executing end-to-end.

---

## Session D — Skill System Gameplay Effects

**Scope:** make `SkillSet` levels actually matter.

- [ ] Add `JOB_ENGINEERING` (optimizes thrust?) or keep it simple: `SKILL_PILOTING` reduces helm-walk time or increases thrust efficiency.
- [ ] Store skill gain: each completed job increments the relevant skill XP; level up at thresholds.
- [ ] Runner reads skill level:
  - Higher piloting = faster path-walk speed to the helm? (touches `simulate_crew` max speed).
  - Higher repair = shorter repair duration.
- [ ] Panel: show crew skill level next to name.

**Definition of done:** Two crews with different piloting levels; the higher-level one reaches the helm measurably faster.

---

## Session E — Multi-Crew Panel + Job Ownership

**Scope:** the panel currently shows one selected crew; generalize to a roster view.

- [ ] Roster header: list all crew with name + current job + skill summary.
- [ ] Click a crew in the roster to select them (same as world left-click).
- [ ] "Assign to all" opt-in (e.g. emergency repair: every idle crew gets the job).
- [ ] Verify `crew_resolve_deadlocks` handles multi-crew station-rush gracefully.

**Definition of done:** Roster renders N rows; click-select works; assign-to-all distributes jobs.

---

## Session F — Save/Load Integration

**Scope:** crew job queues and skill levels must survive save/load.

- [ ] Extend save format with `Crew` fields: `skills`, `queue[]`, `job_count`, `current`, `has_current`, `is_active_pilot`.
- [ ] On load: restore queue in order; if `current.state == EXECUTING` and type is persistent, re-resolve station tile and resume (don't replay walk). If `MOVING_TO_TARGET`, re-run A* from current tile (path may differ if ship changed).
- [ ] Regression: save with active pilot, load, confirm `is_active_pilot` is TRUE and flight works immediately.

**Definition of done:** Save/load round-trip; loaded crew resumes exact job state.

---

## Deferred / Out of Scope (named seams)

- JSON/data-driven widget definitions.
- Scrollable panels (only needed if crew count > ~6).
- Job schedules, emergency overrides, multi-step chained jobs.
- Non-station jobs (e.g. "wander", "sleep").
- Skill progression curves (linear stub is fine for now).

---

## Acceptance Criteria for the Whole Roadmap

- Every session builds clean under `-Wall -Werror`.
- Every session ends with at least one screenshot verifying the new behavior.
- The pilot gate and pure-inertial pillar are never regressed.
- The old `.hermes/plans/crew-job-system-and-ui.md` is replaced by this living roadmap.
