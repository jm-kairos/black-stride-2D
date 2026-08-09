# RenderLayerTable

**Responsibility:** Owns the game's draw-order vocabulary — the named layer constants every
sandbox draw site passes to the engine's sprite `layer` field. It explicitly does not own what
those numbers *mean* to the renderer: the engine interprets the same integer as draw order, as
the bloom split (`BS_LAYER_BLOOM_THRESHOLD`), and via `renderer_set_lights`'s `unlit_layer`
argument as the fullbright cutoff. This header only names the values the game uses.

**Public interface:** `sandbox/source/core/render_layers.h` — `LAYER_STARFIELD_FAR` (0),
`LAYER_STARFIELD_MID` (2), `LAYER_MAPPED_SYSTEM` (3), `LAYER_SHIP` (10), `LAYER_CELESTIAL` (11),
`LAYER_PROJECTILE` (12), `LAYER_PROJECTILE_FX` (13), `LAYER_UI` (50), `LAYER_HUD_TEXT` (100),
`LAYER_DEBUG` (`BS_LAYER_BLOOM_THRESHOLD`), `LAYER_GIZMO` (`BS_LAYER_BLOOM_THRESHOLD + 1`).
Included by **7 subsystems**.

**Depends on:** engine `renderer/renderer_types.h` (for `BS_LAYER_BLOOM_THRESHOLD`).
**Depended on by:** CoordinateDiagnostics, FrameOrchestrator, GalaxyMapRendering,
InWorldOverlays, SceneOrchestration, ShipRendering, SystemContentRendering.

**Key invariants:**
- **Layers below `BS_LAYER_BLOOM_THRESHOLD` go through bloom; layers at or above it are drawn
  after composite.** Enforced engine-side, in the backend's `end_frame` batch split. The game's
  half of that contract is that `LAYER_DEBUG` and `LAYER_GIZMO` are *derived* from the engine
  macro rather than written as literals, so debug overlays stay pinned to the cutoff wherever it
  moves.
- **Constants are `static`, not `extern`** — the header states each TU gets its own copy to
  avoid ODR and link concerns. That is deliberate; it also means they are not visible by symbol
  name in a debugger and produce unused-variable pressure in TUs that use only a few.
- **A layer number decides THREE things at once, and the two weapon-fire layers are where that
  bites.** Below `BS_LAYER_BLOOM_THRESHOLD` a sprite blooms; below the `unlit_layer` argument
  `frame_lighting` passes to `renderer_set_lights` (also `LAYER_UI`, also 50) it is *lit*. So
  `LAYER_PROJECTILE` (12) and `LAYER_PROJECTILE_FX` (13) buy bloom for weapon fire and inherit
  scene lighting as the price; both opt back out per-sprite with the shader's `custom.z = 1`
  self-emissive flag. Any new layer placed under 50 for bloom must make the same choice, and
  getting it wrong is invisible in the arena look (which submits no lights) and only shows on
  the galaxy-map side.
- Ordering must remain ascending by intended depth (starfield → system → ship → celestial →
  weapon fire → UI → HUD text). Nothing enforces it; a mis-numbered constant produces a silent
  z-order bug.

**Extension points:** A new layer is one `static const u32` line. Place it by value relative to
the existing constants, and derive from `BS_LAYER_BLOOM_THRESHOLD` (as `LAYER_DEBUG` and
`LAYER_GIZMO` do) if it must sit on a specific side of the bloom split rather than at a fixed
number.

**Known limitations / tech debt:**
- **`LAYER_UI` (50) and `LAYER_DEBUG` currently collide.** `LAYER_UI` is written as the literal
  `50` while `LAYER_DEBUG` is `BS_LAYER_BLOOM_THRESHOLD`, which is also 50. Two spellings of the
  same number by different means — they would silently diverge if the engine constant changed,
  which is precisely the scenario the derived form was meant to survive.
- **Two other layer vocabularies exist and neither uses this header.**
  `sim/voronoi_galaxy.h` declares its own `VORONOI_LAYER_CELESTIAL` (2) and
  `VORONOI_LAYER_UI` (50) with the comment "must match game.cpp"; and
  `sim/rts_controls.cpp` hardcodes `HOVER_CIRCLE_LAYER = 50` with the comment "same as
  `LAYER_UI` in game.cpp". Three independent definitions of the same constants, two of them
  hand-copied.
- `render/out_sensor_detection_fx.cpp` defines a private `LAYER_FX = 15` and uses five
  consecutive layers from it, none of them named here — so the 11–50 range has an undocumented
  occupant. It is why the weapon-fire layers are 12/13 rather than the more natural 15/16, and
  it is a live hazard: the next module to pick a number in that gap has nothing to read.
- `LAYER_MOUNT_ART` is a *third* privately-defined layer (`render/ship_render.cpp`, `LAYER_SHIP
  + 1` = 11), colliding with `LAYER_CELESTIAL`.
- `render/debug_overlay.cpp` draws its parity checkerboard on the bare literal layer `1`, which
  has no name in this table.
- The file is one of only two single-header subsystems in the sandbox and arguably belongs
  inside whatever owns rendering policy — but it is consumed by seven subsystems including
  `sim/` ones, so no obvious owner exists. *Inferred:* that its `core/` placement reflects "used
  everywhere" rather than a deliberate layering decision.

**Source paths:** `sandbox/source/core/render_layers.h`

**Last verified:** 2026-08-09, working tree on `game` (adds `LAYER_PROJECTILE` /
`LAYER_PROJECTILE_FX` under the bloom threshold)
