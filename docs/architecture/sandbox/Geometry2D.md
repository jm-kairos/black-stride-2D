# Geometry2D

**Responsibility:** Owns two stateless 2D geometry predicates — even-odd point-in-polygon and
point-to-segment distance. It owns nothing else: no state, no allocation, no `game_state`, no
callbacks. `geom2d.h` opens by advertising exactly that ("Pure 2D geometry helpers (no
`game_state`)"), making the absence of the sandbox's dominant dependency a stated property
rather than an accident.

**Public interface:** `sandbox/source/core/geom2d.h` — `point_in_polygon(Vec2 p, const Vec2*
verts, i32 n)`, `point_to_segment(Vec2 p, Vec2 a, Vec2 b)`.

**Depends on:** engine `math/math_utils.h` (for `Vec2`, `clampf`, `vec2_*`) and `defines.h`.
Nothing else — along with DeterministicRng it is one of only two sandbox subsystems that do not
reach GameStateModel.
**Depended on by:** CombatArena (hull hit-testing), WorldEditor (edit picking),
FrameOrchestrator.

**Key invariants:**
- **The polygon must not repeat its first vertex.** `point_in_polygon` wraps with
  `j = n - 1` in `core/geom2d.cpp`, so a closing duplicate vertex would be counted twice.
  The header says "closed polygon" without stating this. Unenforced.
- **The `(verts[i].y > p.y) != (verts[j].y > p.y)` test is load-bearing, not an optimisation.**
  It is what guarantees the subsequent division by `verts[j].y - verts[i].y` has a non-zero
  denominator. Removing or reordering it introduces a divide-by-zero.
- `point_to_segment` special-cases a degenerate segment (`ab2 < 0.0001f`) by returning the
  point-to-`a` distance.
- `n < 3` returns `FALSE` rather than reading out of bounds.

**Extension points:** A new predicate is a declaration in `geom2d.h` and a definition in
`geom2d.cpp` built on the engine's `vec2_*` free functions and `clampf`, keeping the
no-`game_state` property intact. That property is the reason this subsystem is usable from both
`sim/` and `render/` without pulling in the god struct, so any addition that needs game state
belongs elsewhere.

**Known limitations / tech debt:**
- `point_in_polygon` takes a raw pointer and count with **no ownership or lifetime note**, and
  the two callers pass differently-sourced arrays (a ship's collider corners in CombatArena; an
  editor selection polygon in WorldEditor).
- The header names its two consumers in a comment, which is the only record of who depends on
  it — there is no other index.
- Only two predicates exist. Callers needing anything more (segment intersection, convex hull,
  SAT) have rolled their own: `ships_collide` in `sim/ship.cpp` implements SAT with a
  minimum-translation vector independently, and `sim/voronoi_galaxy.cpp` has its own
  angular vertex sort. *Inferred:* that this module was extracted opportunistically from
  `game.cpp` rather than designed as the geometry layer — the comment in `game.cpp` recording
  the move ("`point_in_polygon` / `point_to_segment` now live in `core/geom2d.cpp`") supports
  that reading but does not state it.
- No test coverage; the project has no test harness for sandbox code.

**Source paths:** `sandbox/source/core/geom2d.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
