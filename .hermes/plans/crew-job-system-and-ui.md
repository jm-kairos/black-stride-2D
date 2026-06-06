# Plan — Crew Job System + UI System

> **Status: Phases 1–3 COMPLETE & verified. Phases 4–6 pending.**
> Phase 1 added the engine `renderer_create_texture` + `camera2d_world_to_screen` seams and the
> sandbox `font8x8.h` / `text.{h,cpp}` bitmap-text layer; built clean (`-Wall -Werror`) and
> screenshot-verified pinned + upright under camera rotation (local heading) and zoom in both
> modes. The temporary HUD probe was removed; `text_init()` runs at startup (atlas bake), no
> text drawn until Phase 2 consumes it.
>
> Phase 2 added the screen-anchored widget layer `sandbox/source/ui.{h,cpp}`: the camera-cancel
> `ui_quad`, the `UiWidget` POD + `UiContext`, the immediate-build / retained-draw lifecycle
> (`ui_begin` / `ui_panel` / `ui_label` / `ui_button` / `ui_update` / `ui_draw`), screen-space
> hit-testing, and `ui_wants_mouse()`. Built clean (`-Wall -Werror`) and screenshot-verified via
> a temporary `[Test]`-button probe across all three acceptance checks: (a) panel pinned
> top-left under pan/zoom and ship-heading rotation in both modes; (b) button recolors
> NORMAL→HOT→HELD on hover/press (`btn_1_idle`/`btn_2_hot`/`btn_3_held` shots); (c) input
> precedence — clicking the button does NOT leak to world picking (leak test B/C/D:
> click-crew→ring appears; click-button→ring persists; click-empty-floor→ring clears, proving
> the gate is specific, not a broken deselect). The probe + `[TEMP DIAG]` cursor-dot block were
> then stripped from `game.cpp`; the integration seams remain wired (`ui_draw` in `game_render`,
> `ui_wants_mouse()` gates on both crew select+move in `update_crew_command`). The widget set is
> dormant (`s->ui` zero-init at game init → safe no-op `ui_draw` / `ui_wants_mouse`) until
> Phase 5 wires the real Crew Job Panel. Verified clean: upper-left corner empty, no debug dots,
> world unaffected (`p2_clean_final.png`).
>
> Remaining systems still greenfield: no station concept, and no job code anywhere in the tree.
> This plan turns the `# Crew Job System` spec plus
> a `# UI System` into a phased, buildable, individually-verifiable implementation, with the
> UI architecture adapted from the Parallel Realities "Widgets" tutorial into the project's
> C-with-structs (Kohi-style) idiom.
>
> Baseline it builds on: the global→local angular-velocity carry-over fix (uncommitted in
> `sandbox/source/game.cpp` — `control_ship_global` returns `b8 turn_commanded`, stabilizer
> lives in `simulate_ship`). Phase 4 must preserve and re-verify that fix.

---

## 0. The two-sentence summary

In **global mode**, ship movement must require a crew member actively piloting at a control
station; assigning a **PilotingJob** makes the crew autonomously pathfind to the helm, begin
piloting, and enable flight — and when nobody is piloting, flight input is inert (the ship
only coasts). A **screen-anchored Crew Job Panel**, built on a new minimal text + widget
layer, lets the player view crew state and view/assign/remove/reorder jobs.

---

## 1. Decisions to confirm before Phase 1 (recommended defaults chosen)

These are genuine forks. Each has a default picked so the plan is actionable as-is; flag any
you want changed.

1. **Text rendering approach** → **Recommended: procedural bitmap-font atlas built at init,
   uploaded through a new one-line frontend wrapper `renderer_create_texture(pixels,w,h)`.**
   The backend vtable *already* has `create_texture(pixels,w,h)`
   (`renderer_backend.h:35`); the frontend only exposes `renderer_load_texture(path)` (PNG).
   We embed a compact 8×8 ASCII font (glyphs 32–126) as a `static const` bit array, expand it
   to an RGBA atlas once at init, and draw each character as **one tinted quad** with computed
   UVs. Zero art assets, one tiny additive engine function, 1 quad/char (scales fine under the
   16384 batch cap). *Alternatives:* ship a `font.png` (no engine change, adds a binary asset)
   or per-pixel quads (no engine change, no asset, ~35× more quads). Picking the wrapper means
   one conscious, minimal engine-surface touch.

2. **Control-station model** → **Recommended: a new `TILE_HELM` tile placed in
   `assets/ship.tmap`.** Reuses the entire tile/nav/rigid-body stack for free (the station
   rides the ship pose, A* already routes to tiles). A `stations[]` list is derived at load so
   multiple stations/types generalize later. *Alternative:* a standalone stations array in
   `game_state` (more bookkeeping, no reuse).

3. **Piloting completion semantics** → **Recommended: PilotingJob is *persistent*** — it
   reaches `JOB_EXECUTING` and stays there until interrupted/unassigned (it does not
   auto-`COMPLETED`). The spec's "if nobody is actively piloting, movement is disabled" implies
   a held state, not a one-shot.

4. **Queue dispatch order** → **Recommended: FIFO by default, with a `priority` field that,
   when nonzero, pulls a job ahead.** Spec says priority is *optional*; this honors it without
   forcing a full scheduler.

5. **What the pilot gate covers** → **Recommended: gate *all* global thrust + turn (W/S/Q/E/C
   and A/D)** on an active pilot, for one consistent rule. (Coasting still happens — only *new*
   thrust/torque is gated; the pure-inertial pillar is untouched.)

6. **Crew count** → **Recommended: keep one crew member now**, but build the job API, the
   runner loop, and the panel's crew list as `for each crew` so N crew is a data change, not a
   rewrite. (`game_state` already notes the "multi-crew seam".)

---

## 2. Architecture

### 2.1 The one hard fact that shapes the whole UI

The renderer has **a single camera and one view-projection per frame**, rebuilt at flush:

```
renderer_backend_sdlgpu.cpp:803  Mat4 view_proj = camera2d_view_proj(&g_sdl.camera, swap_w, swap_h);
renderer_backend_sdlgpu.cpp:838  SDL_PushGPUVertexUniformData(cmd, 0, &view_proj, sizeof(Mat4));  // identical per draw-run
```

There is **no separate screen-space pass**. And in local mode the camera *rotates with the
ship*:

```
game.cpp:426  s->camera.rotation = s->ship.angle * (1.0f - s->roof_alpha);
```

So a naive world-space panel would slide and spin with the ship. The UI must **counter the
live camera per frame** so it stays pinned to the screen. The exact inverse is already in the
engine and is load-bearing:

```
camera2d_view_proj:      view = scale(zoom) * rotateZ(-rotation) * translate(-position)
camera2d_screen_to_world: view_pt = {sx - hw, hh - sy};  world = position + rotateZ(+rotation) * (view_pt / zoom)
```

**Camera-cancel recipe for a screen-anchored quad** (this is the crux of the UI layer):

- **Position**: `world = camera2d_screen_to_world(cam, fb_w, fb_h, screen_px)` — places the
  pivot at a fixed screen pixel.
- **Size**: author in screen pixels, then `sprite.size = screen_size * (1.0f / cam.zoom)` —
  cancels the view's `scale(zoom)`.
- **Rotation**: `sprite.rotation = cam.rotation` — cancels the view's `rotateZ(-rotation)`, so
  the quad lands axis-aligned on screen.

All three fold into one helper, `ui_quad(screen_rect, color, layer)`. **Hit-testing stays in
pure screen space** (mouse pixels vs. widget rect) — no inverse transform, no rotation math,
robust under any ship pose. Draw on a high layer (`LAYER_UI = 100`, `LAYER_UI_TEXT = 101`;
current max is `LAYER_ROOF = 10`).

### 2.2 UI architecture — Parallel Realities widget model, in the C idiom

The tutorial's pattern (typed `Widget` with a `void* data` extension, a factory that dispatches
on a type tag, a `doWidgets()` / `drawWidgets()` split, and an "active widget" for navigation)
maps cleanly onto our constraints once translated away from OOP/heap/JSON:

- **`UiWidget` POD** — `type` tag (`UI_LABEL`, `UI_BUTTON`, `UI_ROW`), screen-space `rect`
  (x,y,w,h px, top-left origin), `char label[32]`, flags (`visible`, `enabled`, `hot`), an
  `action` enum, and an `i32 param` (e.g. crew index / job slot). We use an **action enum +
  param dispatched in one switch** instead of per-widget function pointers — extensible,
  warning-clean under `-Wall -Werror`, no closures.
- **`do` / `draw` split** — `ui_update(...)` consumes mouse, sets `hot`, returns which action
  fired; `ui_draw(...)` emits quads + text. Mirrors the tutorial's two-phase widget loop.
- **Retained set built in code, not JSON.** The tutorial loads widget sets from JSON; we have
  no JSON parser and "minimal UI" doesn't justify adding one. Data-driven widget definitions
  are an explicit **deferred seam** (§6).
- **Input precedence** — `ui_wants_mouse()` returns true when the cursor is over any visible
  widget; `update_crew_command` is gated on `!ui_wants_mouse()` so clicking a button never also
  selects/moves crew underneath.

### 2.3 Crew Job System — data model (C idiom)

The user's `class` / `std::queue` / `enum class` / virtual examples are *intent*; we render
them as POD structs, enum tags, fixed-size arrays, and free functions (consistent with
`ship`/`nav`/`crew`, and warning-clean).

```c
// job.h  (new)
typedef enum JobType  { JOB_NONE = 0, JOB_PILOTING /*, JOB_REPAIR, JOB_MAN_STATION ... */ } JobType;
typedef enum JobState { JOB_QUEUED = 0, JOB_MOVING_TO_TARGET, JOB_EXECUTING,
                        JOB_COMPLETED, JOB_FAILED, JOB_INTERRUPTED } JobState;

typedef struct Job {
    JobType  type;
    JobState state;
    b8       has_target;       // does this job resolve to a tile target?
    i32      target_col, target_row;   // station tile (ship-local grid)
    i32      priority;         // 0 = normal; higher pulls ahead of FIFO
} Job;

typedef enum SkillType { SKILL_PILOTING = 0, SKILL_COUNT } SkillType;
typedef struct SkillSet { u8 level[SKILL_COUNT]; } SkillSet;   // progression = stub for now
```

`Crew` (in `game.h`) is **extended** (not replaced) — it keeps `position/velocity/radius/path*`
and gains:

```c
SkillSet skills;
Job      queue[CREW_MAX_JOBS];   // fixed-size ring/array
i32      job_count;
Job      current;                // active job (type JOB_NONE when idle)
b8       has_current;
b8       is_active_pilot;        // set true only while current==PILOTING && EXECUTING
```

**Job API** (`crew_jobs.h/.cpp`, free functions): `crew_enqueue_job`, `crew_remove_job(idx)`,
`crew_reorder_job(idx, dir)`, `crew_clear_jobs`, and the runner
`crew_update_jobs(game_state*, crew*, dt)`.

**The job runner** — runs **every frame in both modes** (like `simulate_crew`, so a crew keeps
executing while you're zoomed out piloting). State machine:

```
idle & queue non-empty   -> pop best (priority then FIFO) into `current`; state = QUEUED
QUEUED (PILOTING)        -> resolve helm tile; A* from crew tile to it; state = MOVING_TO_TARGET
                            (no path / no helm -> state = FAILED)
MOVING_TO_TARGET         -> when arrived (path_len==0 && within arrive radius) -> state = EXECUTING
EXECUTING (PILOTING)     -> is_active_pilot = TRUE; persists (does not self-complete)
INTERRUPTED / FAILED     -> is_active_pilot = FALSE; clear current (drop or optionally re-queue)
```

A **new move order interrupts** the active job (sets `current.state = JOB_INTERRUPTED`,
`is_active_pilot = FALSE`) — so manually walking the pilot away disables flight, which is the
intended emergent behavior.

### 2.4 The control station

Add a `TILE_HELM` tile (glyph `H`):

- `ship.cpp` `char_to_tile`: `case 'H': return TILE_HELM;`
- `ship.h` `TileType`: add `TILE_HELM`.
- `ship_tile_is_walkable`: include `TILE_HELM` (crew stands on it).
- `ship_tile_is_solid`: exclude it. `ship_tile_is_structure`: it's non-empty, so included (roof
  silhouette unaffected).
- A color in `color_for_tile` (e.g. a lit cyan console).
- Helper `ship_find_first_tile(ship, TILE_HELM, &col, &row)` (or build a `stations[]` list at
  load). Place exactly one `H` on the bridge in `assets/ship.tmap` (top interior room).

### 2.5 The pilot gate

`control_ship_global` today applies thrust/turn whenever `mode == GLOBAL`. Change it to a no-op
for thrust **and** torque unless a crew member has `is_active_pilot == TRUE`. The existing
`b8 turn_commanded` return value (from the prior angular fix) naturally becomes `FALSE` when
there's no pilot → `simulate_ship` keeps auto-stabilizing the spin to zero and linear velocity
keeps coasting. **No new drag, no forced stop** — the pure-inertial pillar holds; only the
*input authority* is gated.

---

## 3. File inventory

**New (sandbox):**
- `sandbox/source/font8x8.h` — embedded 8×8 ASCII bitmap (static const; public-domain style).
- `sandbox/source/text.h` / `text.cpp` — atlas build at init + `text_draw(screen_x,y,scale,color,str)` and `text_measure`.
- `sandbox/source/ui.h` / `ui.cpp` — camera-cancel `ui_quad`, `UiWidget`, `ui_begin/update/draw/end`, `ui_button`, `ui_label`, `ui_wants_mouse`, screen-space hit test.
- `sandbox/source/job.h` — `JobType`, `JobState`, `Job`, `SkillType`, `SkillSet`.
- `sandbox/source/crew_jobs.h` / `crew_jobs.cpp` — queue ops + the per-frame job runner.
- `sandbox/source/crew_panel.h` / `crew_panel.cpp` — the Crew Job Panel screen (composes ui + crew_jobs).

**Modified (sandbox):**
- `sandbox/source/ship.h` / `ship.cpp` — `TILE_HELM` + walkable/color/find-helm.
- `assets/ship.tmap` — place one `H` (and bump the legend comment).
- `sandbox/source/game.h` — extend `Crew` with skills/queue/current/is_active_pilot; `CREW_MAX_JOBS`.
- `sandbox/source/game.cpp` — call `crew_update_jobs` in update; gate `control_ship_global`; draw the panel in render; gate `update_crew_command` on `!ui_wants_mouse()`; init helm/skills.

**Modified (engine — single, additive, minimal):**
- `engine/source/renderer/renderer.h` / `renderer.cpp` — surface
  `renderer_create_texture(const u8* pixels, u32 w, u32 h)` as a thin forward to the existing
  `state.backend.create_texture(...)` vtable entry. No backend change (the function already
  exists at `renderer_backend_sdlgpu.cpp:592`).

> Build picks up new `.cpp` automatically (both build scripts glob `*.cpp`). Any sandbox file
> using `fopen`/`sscanf` needs `#define _CRT_SECURE_NO_WARNINGS` at the top (sandbox does not
> define it globally) — none of the new files should need file I/O, but note it for safety.

---

## 4. Phasing (each phase builds clean and is verified before the next)

Verification is **screenshot-only** via `scripts/capture-window.ps1 -Proc sandbox -Out <abs.png>`
(PrintWindow `PW_RENDERFULLCONTENT`) plus the synthetic-input harness; Windows logging uses
`WriteConsoleA`/`OutputDebugStringA`, not stdout. Kill `sandbox.exe` before each rebuild and
after each verify. Build: `cmd //c build-all.bat 2>&1 | tail -6` → expect "All assemblies built
successfully."

### Phase 1 — Text foundation
- Add `renderer_create_texture` (engine wrapper). Build engine alone first to isolate the seam.
- Embed `font8x8.h`; build the RGBA atlas at game init; implement `text_draw`/`text_measure`.
- Temporary probe: draw `"HELLO 0123 local"` top-left each frame.
- **Verify:** screenshot shows legible text pinned to the screen; rotate the ship (A/D in
  global) and zoom — text stays put, upright, fixed size. Remove the probe after.

### Phase 2 — Screen-anchored UI primitives + widget core  ✅ COMPLETE & verified
- Implement `ui_quad` (camera-cancel), `UiWidget`, `ui_begin/update/draw/end`, `ui_button`,
  `ui_label`, `ui_wants_mouse`.
- Temporary probe: a static panel with one `[Test]` button that recolors on hover/click and
  logs the click.
- **Verify:** panel + button stay pinned under ship rotation/zoom in both modes; hover/click
  states correct; clicking the button does **not** also select/move the crew (precedence works).
- **Result:** all three checks passed by screenshot (pinning; NORMAL→HOT→HELD recolor; leak
  test B/C/D shows the UI click is consumed while a real world click still deselects). Probe +
  `[TEMP DIAG]` dot stripped; seams (`ui_draw`, `ui_wants_mouse` gates) retained; widget set
  dormant until Phase 5. Final build clean under `-Wall -Werror`.

### Phase 3 — Job data model + station + runner (no panel yet)  ✅ COMPLETE & verified
- Add `TILE_HELM` (ship.h/.cpp + color + walkable + find-helm); place `H` in `ship.tmap`.
- Add `job.h`; extend `Crew`; implement `crew_jobs.*` (queue ops + runner state machine).
- Temporary: at init, enqueue one `PilotingJob` on the crew.
- **Verify:** screenshot/log shows the crew auto-paths to the helm (existing green path line to
  the `H` tile), advances `QUEUED → MOVING_TO_TARGET → EXECUTING`, and sets `is_active_pilot`.
  Confirm via the periodic `BS_LOG_INFO` (extend it to print crew job state + pilot flag).
>
> **Done.** New `sandbox/source/job.h` (JobType/JobState/Job/SkillSet, `JOB_PILOTING` persistent),
> `crew_jobs.{h,cpp}` (queue ops `crew_enqueue_job`/`crew_remove_job`/`crew_reorder_job`/
> `crew_clear_jobs` + the per-frame `crew_update_jobs` runner: idle→pop best (priority then FIFO)→
> `ship_find_first_tile(TILE_HELM)`→`nav_find_path`→`MOVING_TO_TARGET`→`EXECUTING`, persistent for
> PILOTING, interrupts to `JOB_INTERRUPTED` if a manual move order repopulates the path).
> `TILE_HELM` added to `ship.h` enum + `ship.cpp` (`char_to_tile 'H'`, walkable=TRUE, teal color
> in `game.cpp`) + `ship_find_first_tile` helper. `Crew` extended in `game.h` (`SkillSet skills`,
> `Job queue[CREW_MAX_JOBS]`, `job_count`, `current`, `has_current`, `is_active_pilot`). `game.cpp`
> hooks: temp PILOTING job enqueued in `game_init`, `crew_update_jobs` called each frame before
> `simulate_crew`, periodic `BS_LOG_INFO` extended with `job=<state> pilot=<0|1>`. The runner runs
> in BOTH modes (like `simulate_crew`) so the crew keeps heading to / manning the station while you
> fly. Built clean under `-Wall -Werror`; screenshot (`bin/phase3_shot.png`) shows the crew having
> auto-pathed from mid-ship (row 8) up to the helm tile (row 2, no green path line remaining ⇒ path
> consumed ⇒ `JOB_EXECUTING`), confirming the full `QUEUED → MOVING_TO_TARGET → EXECUTING` run with
> `is_active_pilot` set. Pilot-flag gating of flight itself lands in Phase 4.

### Phase 4 — Gate global flight on an active pilot
- Gate `control_ship_global` thrust+torque on `is_active_pilot`; thread the existing
  `turn_commanded` return so it's `FALSE` when ungated.
- **Verify (three checks):**
  1. **No pilot:** in global mode, W/S/Q/E/A/D produce no new motion; a moving ship still
     coasts; a spinning ship still auto-stabilizes to zero (prior angular fix intact).
  2. **Pilot active:** after the crew reaches the helm, thrust/turn work normally.
  3. **Regression:** re-run the global→local spin-carryover check from
     `bin/verify_spin_carryover.ps1` — still passes (spin stops in local).

### Phase 5 — The Crew Job Panel
- Implement `crew_panel.*`: crew list with **state labels** (Idle / Walking to station /
  Piloting / Performing task / Interrupted), the selected crew's **job queue** (type + state +
  priority), and controls: **[Assign Piloting]**, per-row **[x] remove** and **[▲/▼] reorder**.
  Wire buttons to `crew_enqueue_job` / `crew_remove_job` / `crew_reorder_job`.
- Replace the Phase 3 hardcoded enqueue with panel-driven assignment.
- Add a keyboard fallback (e.g. `P` = assign piloting to selected crew) so the synthetic-input
  harness can drive it deterministically.
- **Verify (full click-through):** assign piloting via button → crew walks → "Piloting" → global
  flight enabled; remove the job mid-pilot → flight disabled, ship coasts; enqueue two jobs →
  reorder → order changes; state labels track reality at each step. Capture a screenshot per
  state.

### Phase 6 — Polish, regression, docs
- State color-coding; edge cases (assign while walking; interrupt-on-new-move-order; empty
  queue; helm missing → `JOB_FAILED` shown, not a crash). Confirm the multi-crew loop/seam is
  intact (drop in a 2nd crew locally to smoke-test the list renders N rows, then revert).
- Update `blackstride-prototype-spec` to graduate scope (job system + minimal UI now in scope;
  note the deferred seams). Patch `blackstride-build-verify` with any new pitfalls (e.g. the
  camera-cancel recipe, UI input precedence).
- Write `docs/CREW_JOB_SYSTEM_DESIGN_NOTES.md` (mirrors the existing design-notes docs).
- Optional: commit the uncommitted prior angular fix + this feature as coherent commits.

---

## 5. Risks & pitfalls (front-loaded)

- **Camera-cancel is the highest-risk code.** Get §2.1 exactly right or the panel drifts/spins.
  Mitigation: Phase 1/2 verify pinning under deliberate ship rotation before any job logic.
- **`-Wall -Werror`.** Unused params (`(void)x;`), missing enum cases in switches, sign/narrowing
  on `u16 fb_*`. Build after every file.
- **Input precedence.** Forgetting `!ui_wants_mouse()` makes button clicks leak into crew
  select/move. Covered by a Phase 2 verify.
- **Preserve the pure-inertial pillar.** The gate removes *input authority*, never adds drag and
  never zeroes linear velocity. Coasting must look identical with/without a pilot.
- **Preserve the prior angular fix.** It's uncommitted; Phase 4 must re-verify spin still bleeds
  to zero when ungated, and the local-carryover check still passes.
- **Batch cap (16384 sprites).** 1 quad/char keeps a full panel in the low hundreds of quads —
  fine. (This is *why* the atlas font beats per-pixel quads.)
- **Helm reachability.** Place `H` on a floor-connected tile so A* can always route; a walled-off
  helm should surface as `JOB_FAILED`, not a hang.

---

## 6. Explicitly out of scope / deferred seams (named, not built)

JSON-/data-driven widget definitions; scrollable multi-crew panels; skill *gameplay* effects
and progression curves (struct + stub increment only); job priorities beyond a single
pull-ahead field; schedules, emergency overrides, multi-step/chained jobs, and non-piloting job
types (`JOB_REPAIR`, `JOB_MAN_STATION`, …) — all left as enum/struct seams the architecture
already accommodates. Text is intentionally a minimal mono bitmap font (no kerning, no
Unicode).

---

## 7. Definition of done

- Global flight is impossible until a crew member is assigned a PilotingJob, walks to the helm,
  and reaches `EXECUTING`; it becomes impossible again the moment piloting is interrupted —
  shown empirically via screenshots of the ship accelerating only while piloted and coasting
  otherwise.
- The Crew Job Panel (screen-pinned in both modes) shows live crew state and supports
  assign / remove / reorder, all verified by click-through screenshots.
- Builds clean under `-Wall -Werror`; the prior angular-carryover fix and the pure-inertial
  pillar are both intact and re-verified.
- `blackstride-prototype-spec` updated to reflect the graduated scope.
