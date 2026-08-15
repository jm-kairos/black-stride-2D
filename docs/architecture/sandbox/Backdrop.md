# Backdrop

**Responsibility:** Owns the parallax backdrop layer stack — the container that holds and
sequences the layers, the procedural starfield, the nebula, the mapped current system (star,
planets, moons), and the pass that drives them with a parallax-appropriate camera. It explicitly
does not own the depth-parallax *math* (CelestialParallax, split out because four render
clusters consume it), does not own star and planet *appearance* (CelestialFx owns `draw_star`
and `draw_planet_3d`), and does not own the galaxy-map look (GalaxyMapRendering draws the same
stars from the other side of the blend).

**Public interface:** `sandbox/source/render/global_background.h` — `struct GlobalBackground`
(`init`, `shutdown`, `draw`, `notify_system_changed`); embedded in `game_state`.
`sandbox/source/render/parallax_background.h` — `draw_parallax_background`.
`sandbox/source/render/mapped_system_layer.h`, `starfield_layer.h`, `nebula_layer.h` — the three
layer types, known only to `GlobalBackground`.
Used from outside: `global_background.h` by 2, `parallax_background.h` by 2,
`mapped_system_layer.h` by 1.

**Depends on:** CelestialParallax, CelestialFx, CoordinateFrames, GalaxyRuntime,
GameStateModel; engine `renderer/renderer.h`, `renderer/camera2d.h`, `math/math_utils.h`,
`math/bs_hierpos.h`, `renderer/renderer_types.h`, `core/logger.h`, `defines.h`.
**It is the sandbox's heaviest engine consumer: 22 boundary edges.**
**Depended on by:** SceneOrchestration, CameraControl (`notify_system_changed`),
FrameOrchestrator, GameStateModel.

**Key invariants:**
- **`draw_parallax_background` writes `s->star_pos`, which two later passes read.** This is the
  producer half of SceneOrchestration's pass-order dependency.
- **Background layers must be driven by the absolute camera centre, not the render residual.**
  `bg_cam_for_parallax` substitutes `game_camera_center` because under floating origin
  `camera.position` is only a residual — without it the background would sit still while the
  ship moves. The comment in `parallax_background.cpp` states this.
- **The camera must be restored twice.** `GlobalBackground::draw` restores whatever camera it was
  handed (the effective-centre one), and `draw_parallax_background` then restores the
  render-space camera for the batched sprite passes. Each is correct only in combination.
- **The mapped-system layer tracks the *camera's* system, not the ship's.** A long comment
  records the bug that forced this: using the ship's system drew the star ~1e8 units off-screen
  while the map renderer faded the viewed system's star, so stars appeared to "fade to nothing"
  when zooming into a remote system.
- **This layer and GalaxyMapRendering draw the same star simultaneously**, each faded by the
  complementary blend weight, relying on additive blending to sum to a continuous
  full-intensity star. The opaque dark pocket uses a separate saturating weight
  (`view_arena_w / 0.45` clamped) so at least one pass fully occludes at every zoom — a
  cross-module numerical agreement with no shared constant.
- **`draw_bg_layer` is a template over a duck-typed contract.** The three layer types share no
  base class; they are unified structurally by having `parallax`, `zoom_scale`, `is_custom_gpu`
  and `draw(...)`. Enforced only at instantiation.
- Layers flagged `is_custom_gpu` skip `renderer_set_camera` because they pass camera parameters
  through their own uniform block instead.
- `renderer_set_draw_alpha` must be restored to 1.0 — `mapped_system_layer` does so at the end,
  deliberately being the last background layer.
- **The god-ray (volumetric sun shaft) submission lives here, next to the streak source** —
  `mapped_system_layer.cpp` fills `bs_godray_params` from the drawn star's parallaxed screen
  position, hero-floor-sized radius, colour and sensor fade, weighted by `view_arena_w` so the
  deep-map look keeps its existing volumetric star light. Occluder band is `LAYER_SHIP`; the
  engine's mapped batch always occludes with depth-map transmission. Submitting from the star
  draw site (not `frame_lighting`) is what keeps the shafts pixel-aligned with the drawn star.

**Extension points:** **A new layer** is a struct with the four duck-typed fields and a
`draw(cam, fb_w, fb_h, dt, elapsed_time, blur)` method, allocated in `GlobalBackground::init`,
freed in `shutdown`, and drawn in the back-to-front sequence in `GlobalBackground::draw` behind
an editor toggle. `NebulaLayer` and `StarfieldLayer` are the template for a shader-driven layer
(`is_custom_gpu = TRUE`, pack a `bs_*_params` struct, call the engine draw); `MappedSystemLayer`
is the template for a sprite-based one. New shader tunables are fields on
`game_state::RenderState` copied into the params block in the layer's `draw`.

**Known limitations / tech debt:**
- **The only sandbox code using raw `new`/`delete`.** All three layers are heap-allocated in
  `init` and freed in `shutdown`, bypassing the engine's tagged allocator entirely — so
  background memory is invisible to `bs_memory`'s accounting, and a missed `shutdown` leaks.
  Raw owning pointers, no destructor; nothing prevents a double `init`.
- **Fallback defaults disagree with `game_init`.** `nebula_layer.cpp` and `starfield_layer.cpp`
  each carry a `gs ? … : default` for every tunable, and several defaults differ from what
  `game_init` actually sets (`swirl_strength` 1.0 vs 0.8, `falloff_radius` 2.0 vs 0.7,
  `density` 0.06 vs 0.5, `target_px` 40 vs 12, `lod_levels` 4 vs 6, `parallax_near` 0.5 vs
  0.009). Dead in practice, but a materially different field if ever taken.
- **Layer seeds are hardcoded magic constants** (`0xDEADBEEF`, `0xBADDCAFE`), so procedural
  content is fixed across runs regardless of the galaxy seed the player chooses.
- `starfield_layer.cpp` hardcodes `base_cell = 64.0` with a stated invariant that
  `base_cell * 256 == BS_HIERPOS_CELL_SIZE` (16384) — an arithmetic relationship to an engine
  constant that no assertion checks.
- `nebula_layer.h`'s comment says the nebula "sits between the far and mid starfields", but
  those two layers were collapsed into one — the stated position no longer describes the stack.
- The two shader layers use different wiring styles for the same job: `MappedSystemLayer` takes
  `game_state*` in its constructor, `NebulaLayer` and `StarfieldLayer` use a separate
  `set_game_state`.
- `mapped_system_layer.cpp` stacks four independent size-scaling rules (min screen radius, a
  hero-star floor ramped by map weight, edge-aberration `dist_scale`, and per-planet min/max
  pixel clamps), each existing to match GalaxyMapRendering at the boundary.
- `global_background.cpp` includes `render/starfield_generator.h` and uses nothing from it —
  the include that keeps that dead module compiling.

**Source paths:** `sandbox/source/render/global_background.{cpp,h}`,
`sandbox/source/render/starfield_layer.{cpp,h}`, `sandbox/source/render/nebula_layer.{cpp,h}`,
`sandbox/source/render/mapped_system_layer.{cpp,h}`,
`sandbox/source/render/parallax_background.{cpp,h}`

**Last verified:** 2026-08-15, working tree on `game` (adds the god-ray submission in
`mapped_system_layer.cpp` beside `renderer_set_streak_source`; live-verified — the flagship
parked at the sun casts a hull-shaped shadow wedge away from it while light streams past the
bow and stern). Previously 2026-08-07, commit `812680c`
