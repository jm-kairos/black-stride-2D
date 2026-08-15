# CelestialFx

**Responsibility:** Owns how stars and planets *look* — the procedural falloff textures, the two
star render paths (classic sunburst and 3D procedural sphere), planet impostor spheres, the
anamorphic streak post-process state, the per-planet-type appearance parameters, the Planet
Editor window, and persistence of those parameters to disk. It explicitly does not own where
celestial bodies are (GalaxyGeneration and GalaxyRuntime), does not own when they are drawn
(Backdrop and GalaxyMapRendering call into it), and does not own the star *light* — that is
assembled in SceneOrchestration, though `make_star_light` lives in this header.

**Public interface:** `sandbox/source/render/star_fx.h` — `struct PlanetTypeParams`;
`struct StarFxSystem` with `init`, `shutdown`, `draw_star`, `draw_star_classic`, `draw_star_3d`,
`draw_planet_3d`, `apply_streak_state`, `build_ui`, `build_planet_editor`,
`planet_params_reset_defaults`, `planet_params_save`, `planet_params_load`; and the
`static inline make_star_light(...)` factory. The struct is a `game_state` member.
Used from outside by 3 subsystems.

**Depends on:** GameStateModel; engine `renderer/renderer.h`, `renderer/bs_ui.h`,
`renderer/renderer_types.h`, `math/math_utils.h`, `core/logger.h`, `defines.h`.
**Depended on by:** Backdrop, SceneOrchestration, GalaxyMapRendering, GameStateModel.

**Key invariants:**
- **`PLANET_EDITOR_TYPE_COUNT` must equal `PLANET_TYPE_COUNT`** — enforced by a `static_assert`
  in `render/star_fx.cpp`. This is one of very few real compile-time checks in the sandbox and
  the only cross-header one; adding a planet type without a params row is a build error rather
  than a silent mismatch.
- **`apply_streak_state` mutates global renderer state and the last caller wins.** The header
  says so explicitly: the streak is a single global post-process, so it must be called last for
  whichever star should own it (in practice the current system's hero star). Ordering between
  star draws therefore has a visible consequence.
- **`make_star_light` hardcodes the star light's physical model** — radius is `max_orbit * 4`
  and intensity is `5.0 * vis`, both multiplied by editor factors. Those magic numbers define
  how bright a system is, and they live in a `static inline` in a header, so every includer gets
  its own copy.
- **Both star paths clamp aggressively for GPU safety.** The classic path caps the glow quad at
  250 px with the comment "prevent GPU timeout from huge quad"; the streak length is capped at
  50.
- **`draw_star` is a dispatcher on `star_3d_mode`**, and the two paths take different parameters
  and produce different geometry — so the toggle changes far more than appearance (it also
  changes how Backdrop computes the min-screen-radius floor and the cull extent). **The 3D
  sphere is the DEFAULT as of 2026-08-15** (`init()` sets it TRUE); unlike the classic path it
  has no glow-quad cap and no hero-minimum pinning, so at fleet-spawn zoom the photosphere can
  fill the whole backdrop — `star_body_scale` (the "Sphere radius" slider) is the size knob.
  The toggle itself is NOT persisted; the default is the effective value every launch.
- All four draw methods are `const`, so the object is logically immutable during rendering
  despite driving all of it.

**Extension points:** **A new planet type's appearance** is a `PlanetTypeParams` row (guarded by
the `static_assert`) plus, if it needs new shader behaviour, a branch keyed on `planet_type` in
the planet surface shader — `bs_planetsurface_params` already carries a full genome (4-stop
palette, feature genes, anomaly id) so most variation is data. **A new star surface tunable** is
a `surf_*` field, a slider in `build_planet_editor` or `build_ui`, and a member of the uniform
block packed in `draw_star_3d`. Because parameters persist, adding a field means it is written
to `bin/planet_editor.cfg` on shutdown and read back on init — the load path must tolerate an
older file.

**Known limitations / tech debt:**
- **`StarFxSystem::shutdown()` has no caller** (verified 2026-08-15: `init()` is called from
  `game.cpp`, `shutdown()` from nowhere — the engine boundary has no game-shutdown callback,
  only init/update/render/on_resize). Consequences: the shutdown-time `planet_params_save()`
  never runs (planet params persist only via the Planet Editor's explicit "Save to disk"
  button), and the three radial textures leak at exit (harmless — process teardown). The
  god-ray/backlight tunables sidestep this with save-on-change in `build_ui`: any slider
  movement rewrites the cfg immediately.
- **It reads and writes a config file at runtime.** `planet_params_load()` runs during `init`,
  persisting to `planet_editor.cfg` in the working directory. This is the only sandbox module
  with editor state that survives a restart, and it is why `#define _CRT_SECURE_NO_WARNINGS`
  sits at the top of the file. The god-ray line ("g" prefix) tolerates short/missing lines
  (defaults hold) and is skipped by older loaders.
- **A render module owns a substantial editor UI.** `build_ui` and `build_planet_editor` build
  ImGui panels directly through `bs_ui.h` — the same concern-mixing as `sim/galaxy_history.cpp`
  and `core/profiler.cpp`.
- **The struct mixes four unrelated concerns**: streak post-process tuning, star surface shader
  tuning, per-type planet appearance, and editor window state (`show_planet_editor`,
  `planet_editor_sel_type`). All are persisted or edited together.
- Three 256×256 radial textures are baked into a shared function-local
  `static u8 pixels[256*256*4]` (256 KB) explicitly to avoid a stack overflow — the same pattern
  `render/text.cpp` uses, and the opposite of what `sim/projectile.cpp` does.
- `streak_length_mul` and `streak_intensity_mul` are described as "gameplay-derived" and are
  written by `render/galaxy_map_render.cpp` from the current star's radius and luminance — so
  one render pass feeds values into another module's persistent state.
- The classic and 3D star paths duplicate a fair amount of parameter packing, and the mode
  toggle is checked in three other files (`mapped_system_layer.cpp`, `galaxy_map_render.cpp`)
  to adjust culling and scaling.

**Source paths:** `sandbox/source/render/star_fx.{cpp,h}`

**Last verified:** 2026-08-15, working tree on `game` (same day, later: `star_3d_mode` defaults
TRUE — the 3D procedural sphere is the default star geometry, live-verified from spawn; the
god-ray sun disc tracks the sphere's `star_body_scale` sizing. Earlier: adds the god-ray +
hull-backlight tunables: eight `godray_*`/`backlit_*` fields with "Sun Shafts (God Rays)" / "Hull Backlight"
sliders in `build_ui`, persisted save-on-change to the cfg's "g" line — consumed by Backdrop's
god-ray submission and ShipRendering's mapped-sprite fill; live-verified, a slider drag
rewrites the cfg the same frame and the values load back on restart). Previously 2026-08-07,
commit `812680c`
