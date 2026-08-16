# CoordinateDiagnostics

**Responsibility:** Owns verification of the hierarchical coordinate system — a throttled
per-frame dump of every gameplay object's cell/local coordinates with runtime invariant checks,
and a visual overlay of the coordinate lattice around the flagship. Both exist to prove the
simulation runs in cell space rather than lossy absolute `f32`. It explicitly does not own the
coordinate system itself (the engine's `HierCoords`) or the transforms
(CoordinateFrames) — it consumes both in order to check them.

**Public interface:** `sandbox/source/core/coord_diag.h` — `coord_diag_update`,
`coord_diag_set_enabled`, `coord_diag_is_enabled`, `coord_diag_last_violations`.
`sandbox/source/render/debug_overlay.h` — `g_debug_cell_grid` (mutable global toggle),
`draw_hierpos_cell_grid`.
Used from outside: `debug_overlay.h` by 3 subsystems, `coord_diag.h` by 2.

**Depends on:** CoordinateFrames (`render_from_hierpos`), BitmapText, RenderLayerTable,
FleetControl, ShipCombatModel, GameStateModel; engine `math/bs_hierpos.h`,
`renderer/renderer.h`, `renderer/camera2d.h`, `defines.h`.
**Depended on by:** DevPanels (both toggles), ShipRendering (draws the grid),
FrameOrchestrator.

**Key invariants:**
- **The overlay must route through the same transform entities use.** Every grid line is built
  from `HierPos2` corners passed through `render_from_hierpos` — "the exact path entities use" —
  so a broken floating-origin rebase makes the grid visibly shear or jitter against the ships.
  Its diagnostic value depends entirely on it *not* taking a shortcut. Stated in both
  `debug_overlay.h` and the `.cpp`.
- **Two named invariants encode the coordinate refactor's contract.** INV1: a canonical `local`
  folds within ±half a cell (`INV_LOCAL_BOUND`). INV3: the camera-relative render offset stays
  small (`INV_RENDER_BOUND` 5.0e6), proving floating-origin rebasing works — a huge value would
  mean an absolute-`f32` position leaked into the render path. Checked in `diag_check`.
- **INV3 is deliberately suppressed when the camera is decoupled** — free camera active, or a
  view mode other than `MODE_SYSTEM` — because a detached camera legitimately sits far from
  entities. The comment is explicit that enforcing it there would be a false positive.
- **Everything is compiled out under `#else` when `BS_DEBUG` is undefined**, with empty stubs
  preserving the link surface so call sites need no guards. Both build scripts always pass
  `-DBS_DEBUG`, so the stub branch is currently dead.
- The dump is throttled to `DIAG_INTERVAL` (0.25 s, ~4 Hz) and caps projectile logging at the
  first 8 active entries to bound output.

**Extension points:** A new invariant is a check inside `diag_check` (per-entity) or
`coord_diag_update` (global), incrementing the violation count so
`coord_diag_last_violations` surfaces it in the editor panel. A new entity class to cover is
another loop in `coord_diag_update` following the fleet / enemy / combat-entity / projectile
loops. A new lattice visualisation belongs in `draw_hierpos_cell_grid` and must use
`render_from_hierpos` for the same reason the existing lines do.

**Known limitations / tech debt:**
- **INV2 does not exist.** The numbering runs INV1, INV3 — implying a check that was dropped or
  never written. Nothing records which.
- **It writes to disk from inside the frame loop.** `coord_diag_update` appends to
  `coord_diag.txt`, opening and closing the file on every dump. Append mode means a long run
  grows the file without bound, and `coord_diag.h` documents the path as `bin/coord_diag.txt`
  while the implementation opens a bare relative name — the two agree only because the working
  directory is `bin/`.
- **It reaches into five unrelated subsystems' state** — camera, fleet, enemy ship, combat
  entities, projectile pool — making it a read-only consumer that nonetheless knows the layout
  of much of the game.
- **A mutable global toggle wired across three modules.** `g_debug_cell_grid` is defined in
  `render/debug_overlay.cpp`, written by DevPanels' editor panel, and read by ShipRendering.
- The grid draws a fixed 7×7 window (`R = 3`) around the flagship and emits up to ~245 primitives
  per frame (196 lines, up to 24 quads, 49 text labels), all competing for the shared
  16384-sprite budget.
- Highlighting scans the whole fleet per cell — 49 × fleet-size comparisons per frame.
- The parity checkerboard draws on the bare literal layer `1`, which has no name in
  `core/render_layers.h`.
- `coord_diag.cpp` uses `fopen_s`, an MSVC-specific function, unlike the sandbox's other file
  readers which use `fopen` behind `_CRT_SECURE_NO_WARNINGS`.
- `debug_overlay.h`'s comment still claims the overlay includes "a HUD readout"; that part moved
  to an RmlUi document, recorded only in the `.cpp`'s closing comment.
- The two files are grouped as one subsystem because they verify the same thing, not because
  they share code — they have no include edge between them.

**Source paths:** `sandbox/source/core/coord_diag.{cpp,h}`,
`sandbox/source/render/debug_overlay.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
