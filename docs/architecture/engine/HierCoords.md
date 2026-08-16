# HierCoords

**Responsibility:** Owns precision-safe galaxy-scale positions — the `HierPos2` representation
(an `i64` grid cell plus a small `Vec2` local offset), the canonicalisation rule that keeps the
local part bounded, and the conversions and arithmetic that preserve exactness far from the
origin. It explicitly does not own the flat `Vec2`/`Mat4` primitives it builds on (MathCore), nor
any rendering or camera logic. Notably it has **no engine-side consumer at all**: it exists
purely as a service to the game.

**Public interface:** `engine/source/math/bs_hierpos.h` — `struct GridCell`, `struct HierPos2`;
`BS_HIERPOS_CELL_SIZE` (16384.0f), `BS_HIERPOS_HALF_CELL`;
`hierpos_from_vec2`, `hierpos_to_vec2`, `hierpos_to_f64`, `hierpos_normalize`, `hierpos_lerp`,
`hierpos_add_f64`, `hierpos_diff`, `bs_hierpos_selftest` (all `bs__api__`); plus the `inline`
convenience forms `hierpos_add_vec2` and the three default-cell-size overloads.

**Depends on:** Foundation, MathCore.
**Depended on by:** **nothing engine-side.** 23 sandbox files include the header;
`hierpos_diff` alone is referenced in 31 sandbox files and `BS_HIERPOS_CELL_SIZE` in 33.

**Key invariants:**
- **The canonical form is `local ∈ [-half, +half)`** — enforced at a single point.
  `hierpos_from_f64` (`bs_hierpos.cpp:7-24`) floors into a cell then folds the remainder
  (`:17-18`), and *every* public conversion and mutation funnels through it
  (`:27,44,60,66`). That single-point discipline is what makes the invariant hold rather than
  being a per-function obligation.
- The fold is deliberately asymmetric: `+half` rounds up into the next cell while `-half` stays
  in the current one. Three cases in `bs_hierpos_selftest` (`bs_hierpos.cpp:90,93,94`) exist
  only to lock that down.
- **`hierpos_to_vec2` is the designated lossy path** and is safe only near the origin —
  documented at `bs_hierpos.h:25-26`. Nothing enforces it. `hierpos_diff` exists so callers can
  get an `f32` offset between two far-apart points without going through absolute world space;
  this is the floating-origin technique the whole render path depends on.
- `hierpos_to_f64` null-checks its two out-parameters (`bs_hierpos.cpp:39-40`); no function
  null-checks its `HierPos2*` inputs.

**Extension points:** Adding an operation means a `bs__api__` declaration plus an implementation
that widens to `f64`, operates, and re-canonicalises through `hierpos_from_f64` — the pattern
every existing function follows. A default-cell-size convenience form can be added as an
`inline` overload alongside `bs_hierpos.h:53-55`, but see the tech-debt note about what that
does to the DLL boundary. `bs_hierpos_selftest` (`bs_hierpos.cpp:76-192`) is the place to add an
invariant check; it is genuinely run — `sandbox/source/game.cpp:652` calls it under `BS_DEBUG`
and treats failure as fatal.

**Known limitations / tech debt:**
- **The `inline` overloads silently decide whether a call crosses the DLL.** `bs_hierpos.h`
  declares each conversion twice: a `bs__api__` form taking `cell_size` (`:23,27,50`) and an
  `inline` default-cell-size form (`:53,54,55`). So an *argument count* — not an API decision —
  determines whether the sandbox calls into `engine.dll` or executes an engine function body
  compiled into `sandbox.exe`. `hierpos_add_vec2` (`:44`, used in 15 sandbox files) has no
  exported form at all and is always compiled into the caller.
- Those inline forms **bake `BS_HIERPOS_CELL_SIZE` into every caller's object code**, so
  changing the constant requires recompiling the game, not just the DLL — and there is no
  version check that would catch a mismatch (`engine-api-boundary.md` §9).
- `hierpos_add_vec2` is the only mutation helper that cannot take a custom cell size, so
  velocity integration is hard-bound to the default.
- `HierPos2` and `Vec2` cross the DLL **by value** (`bs_hierpos.h:23,33`), making their layout
  part of the ABI with no assertions guarding it.
- `hierpos_normalize` (`bs_hierpos.h:33`) is exported and has no callers anywhere.
- The `f32 t` loop counters in `bs_hierpos_selftest` accumulate `+= 0.1f`
  (`bs_hierpos.cpp:128,157`), so `t` never lands exactly on 1.0 and the endpoint is untested.
- **The selftest hardcodes gameplay-scale constants** — a "travel-scale, ~50k units" case
  described in-comment as "the exact scenario used by the travel system"
  (`bs_hierpos.cpp:138-141`). An engine-side test pinned to a sandbox feature's numbers.
- The header calls flat `Vec2` positions "legacy" (`bs_hierpos.h:22,25`), marking
  `hierpos_from_vec2`/`hierpos_to_vec2` as migration bridges — so their presence in a file is a
  signal about that file's conversion status. *Inferred:* that the migration is incomplete
  rather than a permanent dual representation; the header does not say so outright.

**Source paths:** `engine/source/math/bs_hierpos.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
