# SystemContentRendering

**Responsibility:** Owns the drawing of per-system ambient content — natural asteroids, resource
nodes, ambient dust decorations, and civilian stations — under a strict sprite budget. It
explicitly does not own the *generation* of that content (the arrays live on `StarSystem`,
produced by GalaxyGeneration and materialised by GalaxyRuntime), does not own station identity
or layout (GalaxyRuntime), and does not own the discovery state it reads to decide how a station
is drawn (Discovery).

**Public interface:** `sandbox/source/render/system_content_render.h` —
`draw_system_stations`, `draw_system_asteroids`, `draw_system_resources`,
`draw_system_decorations`. Four passes, all called only by SceneOrchestration.

**Depends on:** CoordinateFrames, RenderLayerTable, GalaxyRuntime, GameStateModel; engine
`renderer/renderer.h`, `renderer/camera2d.h`, `math/bs_hierpos.h`, `defines.h`.
**Depended on by:** SceneOrchestration.

**Key invariants:**
- **The engine's 16384-sprite batch must not be blown.** This is the module's organising
  constraint and the comments say so twice with reasoning: per-system detail "numbers in the
  thousands per system", so drawing every materialised system's belt at galaxy zoom would
  exceed the batch. Three mechanisms stack, all in `system_content_render.cpp`:
  `SYSTEM_DETAIL_MIN_SCREEN_PX` (48) skips a whole system whose belt is smaller than that on
  screen; `LOD_DOT_PX` (3) collapses a per-entity ring or polygon to a single quad; and
  `adaptive_segments` scales ring tessellation with on-screen radius, clamped to [6, base].
  This is the clearest case in the codebase of an engine limit shaping game-side rendering.
- **Everything is gated on sensor visibility**, so ambient scenery disappears outside sensor
  range rather than merely dimming — `system_sensor_vis` returns the same cubic falloff the
  celestial layers use.
- **Station appearance encodes discovery state**: a pulsing antenna marker until scanned inside
  the flagship's Layer 1 radius, then a filled circle in the owner civ's colour. Documented in
  the header as part of the interface contract.
- **Stations are drawn only in owned systems**, while asteroids and dust appear in every system
  and resources concentrate in belt and mid zones — the scope differences are stated per
  function in the header and are the only record of them.
- Asteroid silhouettes use per-vertex jitter generated with the object, so shapes are
  deterministic and stable across frames without being stored per frame.
- Everything draws on `LAYER_CELESTIAL`, below the bloom threshold, so all of it participates in
  bloom.

**Extension points:** A new ambient content type follows the existing four exactly: a fixed
array on `StarSystem` with a `SYSTEM_*_MAX` capacity, a deterministic generator during
materialisation, and a draw pass here that applies the same three LOD mechanisms plus the
sensor-visibility gate, then a call added to `render_scene`. Reuse `system_belt_screen_px`,
`adaptive_segments` and `on_screen` rather than reimplementing them — they are the budget
discipline, not incidental helpers.

**Known limitations / tech debt:**
- **It duplicates two helpers that already exist elsewhere.** `on_screen` is a near-copy of
  `is_on_screen` in `render/mapped_system_layer.cpp`, and `system_sensor_vis` mirrors that
  file's `get_sensor_visibility_global` — with a comment acknowledging the parallel rather than
  sharing the code.
- The three LOD thresholds (48 px, 3 px, and the `screen_r * 0.6` segment scale) are file-static
  constants with no editor exposure, despite being the main lever on frame cost for this content.
- Nothing reports when a system's detail is skipped, so a wide view silently omits content with
  no diagnostic — the same silent-truncation pattern the engine's effect queues have.
- The four passes each iterate every materialised system's full array before culling, so cost
  scales with cache occupancy rather than with what is visible.
- It reads the flagship's `sensors.layer1_radius` for the station scan test, duplicating the
  range rule Discovery owns — two modules deciding what "scanned" means from the same field.
- All four take a non-const `game_state*` despite being read-only.

**Source paths:** `sandbox/source/render/system_content_render.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
