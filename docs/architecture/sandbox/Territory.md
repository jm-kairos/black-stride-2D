# Territory

**Responsibility:** Owns the Voronoi partition of the galaxy into star-system territories — the
vendored diagram generator, the wrapper that builds cells from star positions, the containment
query, the wireframe and Delaunay-lane drawing, and the cursor hover effect over cells. It
explicitly does not own galaxy ownership or borders in the *political* sense (that is
GalaxyHistory's `node_owner`); these are geometric cells around star positions. It also does not
own the nearest-site query the rest of the game uses — `galaxy_spatial.h` records that the
spatial grid **replaced** the O(N) Voronoi nearest-site scan and its fixed 64-site cap.

**Public interface:** `sandbox/source/sim/voronoi_galaxy.h` — `VORONOI_MAX_SITES`,
`VORONOI_LAYER_CELESTIAL`, `VORONOI_LAYER_UI`; `GalaxyVEdge`, `GalaxyDEdge`, `GalaxyVCell`,
`GalaxyVoronoi`; `generate_galaxy_voronoi`, `find_system_by_cell`, `draw_voronoi_edges`,
`draw_delaunay_lanes`.
`sandbox/source/render/voronoi_cell_hover_effect.h` — `update_cell_hover_effect`,
`draw_cell_hover_effect`.
`sandbox/source/jc_voronoi.h` — the vendored library (`jcv_diagram_generate`, `jcv_diagram_free`,
`jcv_diagram_get_sites`, …), consumed only by `sim/voronoi_galaxy.cpp`.
Used from outside: `voronoi_galaxy.h` by 2 subsystems, `voronoi_cell_hover_effect.h` by 2.

**Depends on:** CoordinateFrames, GameStateModel; engine `math/bs_hierpos.h`,
`math/math_utils.h`, `renderer/renderer.h`, `renderer/renderer_types.h`, `defines.h`.
**Depended on by:** GalaxyMapRendering, FrameOrchestrator, GameStateModel.

**Key invariants:**
- **`sim/voronoi_galaxy.cpp` is the single TU that defines `JC_VORONOI_IMPLEMENTATION`.** The
  library body is emitted exactly once; a second definer would produce duplicate symbols.
  Nothing enforces it.
- **Cell vertices must be ordered so consecutive array entries are adjacent on the polygon.**
  `sort_verts_by_angle` does more than sort: after an O(n²) bubble sort by `atan2` it finds the
  largest angular gap and rotates the array so the gap sits at the boundary. The comment
  explains why — without it the `atan2` wraparound leaves consecutive entries non-adjacent and
  the drawn edges cross.
- **`generate_galaxy_voronoi` must run after `systems[]` and `system_count` are initialised** —
  stated in the header.
- **`VORONOI_MAX_SITES` is 64**, matching the galaxy hot-cache size, so the diagram only ever
  covers the materialised neighbourhood rather than the ~10,000-node galaxy.
- **`hovered_cell` and `hover_head_dist` are owned by a different file.** They live on
  `GalaxyVoronoi` but are annotated "written by `voronoi_cell_hover_effect.cpp`" — presentation
  state inside a simulation structure, maintained by convention.
- Cell picking is by nearest *site*, not polygon containment — correct for a Voronoi diagram by
  definition, and it means the pick never touches the computed geometry.

**Extension points:** A new cell-derived visual follows `voronoi_cell_hover_effect`: read
`GalaxyVoronoi::cells[]`, route vertices through `render_from_hierpos` (or
`hierpos_from_vec2` + `hierpos_diff`, as the hover effect does), and draw on a
`VORONOI_LAYER_*`. A new query over the partition belongs in `voronoi_galaxy.h` alongside
`find_system_by_cell`. Changing the diagram's precision or clipping means the `JCV_*`
configuration macros at the top of `jc_voronoi.h`, which are deliberately overridable.

**Known limitations / tech debt:**
- **`jc_voronoi.h` is vendored third-party code inside the game's own source tree** (MIT,
  Mathias Westerdahl). Unlike the engine's vendored libraries — isolated under
  `engine/vendor` and compiled with relaxed warnings — it compiles as part of `sandbox.exe`
  under `-Wall -Werror`, and it allocates internally with `malloc`/`free`, entirely outside the
  engine's tagged allocator. A `_useralloc` variant exists but is unused.
- **`JCV_REAL_TYPE` is left at `float`**, so the diagram works in `f32` at galaxy scale — the
  precision concern that motivated the entire `HierPos2` refactor. Positions are fed in as
  `f32` `Vec2`.
- **The 64-site cap is a leftover scale.** The header's comment says "Fortune's sweep-line
  algorithm (O(N log N)) for ~50 star systems", which no longer matches the galaxy's size;
  `galaxy_spatial.h` explicitly records replacing this for nearest-site queries.
- **`GalaxyVoronoi` is a large fixed POD** — 512 Voronoi edges, 512 Delaunay edges, and 64 cells
  of 32 vertices each, roughly 40 KB embedded by value.
- **It declares its own layer constants** (`VORONOI_LAYER_CELESTIAL`, `VORONOI_LAYER_UI`) with
  the comment "must match game.cpp" — a third parallel layer vocabulary alongside
  `core/render_layers.h` and `sim/rts_controls.cpp`'s hand-copied `50`.
- **It mixes generation, query and rendering in one subsystem**, unlike the surrounding
  sim/render split — `draw_voronoi_edges` and `draw_delaunay_lanes` submit draws directly from a
  `sim/` file.
- `update_cell_hover_effect`'s pulse accumulator wraps at `100 * PI` although its comment says
  "wrap at 2π to avoid fp drift" — the code and comment disagree, though both bound the drift.
- The `zoom` parameter of `update_cell_hover_effect` is accepted and explicitly discarded,
  documented as "retained for the call signature".
- The header's comment describes a "rotating neon trail" that the implementation does not draw;
  it renders a uniform pulsing outline.
- Cell vertices are stored as `Vec2` and round-tripped through `HierPos2` on every frame for
  every edge.

**Source paths:** `sandbox/source/sim/voronoi_galaxy.{cpp,h}`,
`sandbox/source/render/voronoi_cell_hover_effect.{cpp,h}`, `sandbox/source/jc_voronoi.h`

**Last verified:** 2026-08-07, commit `812680c`
