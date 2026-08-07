# CelestialParallax

**Responsibility:** Owns the depth-parallax offset applied to celestial backdrop bodies — the
single shared anchor chosen once per frame, the offset computation, and the zoom-driven fade
that turns the effect off at map scale. It explicitly does not own gameplay positioning:
`celestial_parallax.h` draws the line sharply — stars, planets, orbit lines and test sprites are
parallaxed; ships, enemies, projectiles and FX are excluded and stay at parallax 1.0 via
`render_from_hierpos`. It does not own the drawing of anything it offsets.

**Public interface:** `sandbox/source/sim/celestial_parallax.h` —
`celestial_shared_anchor`, `celestial_center_render`, `celestial_parallax_fade`.
Three functions, used from outside by **5 subsystems** — which is why this was split out of
Backdrop rather than left inside it.

**Depends on:** GalaxyRuntime (`galaxy_nearest_node`), GameStateModel; engine
`math/bs_hierpos.h`, `defines.h`.
**Depended on by:** Backdrop, SceneOrchestration, ShipRendering, GalaxyMapRendering,
FrameOrchestrator.

**Key invariants:**
- **All celestial bodies must be parallaxed against ONE shared anchor.** This is the whole
  design. A per-system anchor would collapse every system onto the screen centre as depth
  approaches 1 and fuse them; one shared anchor keeps the depth term common so inter-system
  layout stays rigid at any depth. Stated in both the header and the `.cpp`.
- **The anchor is computed once per frame and cached** in `game_state::celestial_anchor` (by
  `game_render`), which is what makes the per-frame consistency guarantee hold. Consumers must
  pass that cached value, not recompute it.
- **The Map and Arena renderers must call `celestial_center_render` with identical arguments** or
  a seam appears across the cross-fade. Stated in the header; unenforceable.
- **A special case exists purely for the planet-approach camera.** While a planet is captured
  the camera is deliberately offset from its star, so "nearest node to the camera" would snap the
  anchor onto a *neighbour* star and break the correction — so the anchor is pinned to the
  captured system instead. Without this the approach maths silently mis-centres the planet.
- **Parallax fades out at low zoom by design**, because on the galaxy map it would tear ships
  away from their own systems. The band is `[bg_parallax_fade_zoom, ×2.8]`, chosen to match the
  arena/map view fade defaults — but deliberately kept as a *separate* ramp so the disappear
  point can be tuned alone.
- `celestial_parallax_fade` returns 0 when parallax is disabled, so callers get "no parallax"
  consistently.
- Both differences use `hierpos_diff`, keeping the computation precision-safe far from the
  origin.

**Extension points:** A new parallaxed element calls `celestial_center_render(s, &center,
&s->celestial_anchor, depth)` and adds its own local offset — the pattern every consumer
follows, with `depth` drawn from `s->render.depth_star` / `depth_planet` / `depth_orbit` /
`depth_testsprite`. A new depth tier is a field on `game_state::RenderState` plus an editor
slider; the function itself needs no change. `celestial_parallax_fade` is exposed specifically
so camera code can cancel a followed body's parallax shift, which is the pattern for anything
that must stay screen-centred while parallax is active.

**Known limitations / tech debt:**
- **It lives under `sim/` but is purely a render-space transform.** Its five consumers are all
  render-side or the frame orchestrator; nothing in the simulation uses it. Clustered here by
  responsibility rather than directory.
- **The "identical arguments" requirement between the map and arena renderers is unenforceable**
  and is one of several numerical agreements those two passes must maintain by hand (see also
  Backdrop's dark-pocket occlusion weight and the four star-scaling rules).
- The `×2.8` band width is a magic multiplier in `parallax_fade_weight` with a comment
  explaining what it was matched to, but no link to the constants it mirrors.
- `celestial_shared_anchor` calls `galaxy_nearest_node` every frame; the result is cached by the
  caller, so this is fine today, but nothing prevents a consumer from calling it directly per
  element.
- The header lists "test sprites" among parallaxed elements — a dev-only feature gated by
  `celestial_draw_testsprites`, which defaults to `FALSE` in `game_init`.
- Three functions, no state, no allocation: this is one of the cleanest modules in the sandbox,
  and its main risk is that the invariants it protects are enforced entirely by callers.

**Source paths:** `sandbox/source/sim/celestial_parallax.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
