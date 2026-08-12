# Sandbox file summaries (raw)

Per-file structural notes for the `sandbox` side (`sandbox/source`, 128 files).
Files are described in isolation — no subsystem clustering or naming at this stage.
Companion to `dependency-graph.json` and `file-summaries-engine.md` in this directory.

---

## sandbox/source/core/coord_diag.cpp

- **Path:** `sandbox/source/core/coord_diag.cpp`
- **Purpose:** Debug-only instrumentation that periodically dumps every gameplay object's hierarchical coordinates to a file and checks runtime invariants proving the simulation runs in cell space rather than lossy absolute `f32`.
- **Key types/functions:**
  - `coord_diag_update(const game_state*, f32 dt)` — throttled per-frame dump and check.
  - `coord_diag_set_enabled` / `_is_enabled` / `_last_violations`.
  - `static diag_open()` and `static diag_check(...)` — file handle and per-entity invariant test.
  - Thresholds `DIAG_INTERVAL` (0.25 s), `INV_LOCAL_BOUND` (half cell + 1), `INV_RENDER_BOUND` (5.0e6).
- **Notable non-obvious dependencies:**
  - **Writes to disk from inside the frame loop** — appends to `coord_diag.txt` in the working directory, opening and closing the file on every dump (~4 Hz). Append mode means a long run grows the file without bound.
  - **Four file-static globals** (`g_enabled`, `g_accum`, `g_frame`, `g_last_viol`) hold all its state; nothing is passed in but the game state.
  - **Reaches deep into `game_state`** — camera hierpos, free-camera flag, view mode, the fleet array, the enemy ship, the combat-entity array, and the projectile pool. It is a read-only consumer that nonetheless knows the layout of five unrelated subsystems.
  - **Two named invariants encode the coordinate refactor's contract**: INV1 (canonical `local` folds within ±half cell) and INV3 (camera-relative render offset stays small, proving floating-origin rebasing works). INV2 is absent — the numbering implies a check that was dropped or never written.
  - **INV3 is conditionally suppressed** when the free camera is detached or the view is not `MODE_SYSTEM`, because a decoupled camera legitimately sits far from entities; the comment is explicit that enforcing it there would be a false positive.
  - **Entirely compiled out under `#else`** when `BS_DEBUG` is undefined, with empty stubs preserving the link surface. Since both build scripts always pass `-DBS_DEBUG`, the stub branch is currently dead.
  - Uses `fopen_s` (MSVC-specific) rather than `fopen`, and caps projectile logging at the first 8 active entries to bound output.
- **Interface vs internal:** Internal diagnostic tool. Its four functions are called from the game's update and an ImGui readout, but it participates in no gameplay logic and produces no state anything else consumes.

---

## sandbox/source/core/coord_diag.h

- **Path:** `sandbox/source/core/coord_diag.h`
- **Purpose:** Declares the four-function coordinate-diagnostics API and documents what the dump is for.
- **Key types/functions:** `coord_diag_update`, `coord_diag_set_enabled`, `coord_diag_is_enabled`, `coord_diag_last_violations`; a forward declaration of `struct game_state`.
- **Notable non-obvious dependencies:**
  - **Names the refactor it exists to verify** — the header calls itself "big_space / HierPos2 refactor verification", so its presence marks a migration in progress rather than a permanent feature.
  - **Documents the output path as `bin/coord_diag.txt`** while the implementation opens a bare relative `coord_diag.txt`; the two agree only because the game's working directory is `bin/`.
  - Declares the functions unconditionally even though the implementation is `#ifdef`-split, so callers need no guards — the no-op branch keeps every call site compiling.
  - `coord_diag_last_violations` exists specifically to feed an ImGui readout, coupling this header to the editor UI without including it.
  - Forward-declares `game_state` rather than including it, so the header stays cheap despite the implementation touching most of that struct.
- **Interface vs internal:** Internal interface — a debug tool's control surface, consumed by the game loop and the editor panel only.

---

## sandbox/source/core/cursor_world.cpp

- **Path:** `sandbox/source/core/cursor_world.cpp`
- **Purpose:** Converts the current mouse position into world-space coordinates, in three variants covering the legacy render-space path and the precision-safe true-world path.
- **Key types/functions:** `mouse_world(const game_state*)`, `mouse_true_world(const game_state*)`, `mouse_true_hierpos(const game_state*)`.
- **Notable non-obvious dependencies:**
  - **Every function calls the engine's global input singleton** (`input_get_mouse_position`) rather than taking a cursor position — so these are implicit reads of live global state, and calling them twice in a frame can yield different answers if the pump ran between.
  - **Reads framebuffer dimensions and the camera out of `game_state`** (`s->camera_state.camera`, `s->fb_width`, `s->fb_height`), so correctness depends on those being synchronised with the actual swapchain that frame.
  - **The three functions encode a migration boundary**: `mouse_world` goes straight through `camera2d_screen_to_world` (render space), while the other two route through `game_screen_to_true_world` / `_true_hierpos` to invert the camera's hierarchical transform. The comments state these coincide "in the legacy global path" — so which one a caller picks silently determines whether it is correct under the unified coordinate system.
  - No null check on `s` in any of the three despite all dereferencing it.
- **Interface vs internal:** Public interface within the sandbox — a small shared utility that picking, aiming, and editor code call. It holds no state of its own.

---

## sandbox/source/core/cursor_world.h

- **Path:** `sandbox/source/core/cursor_world.h`
- **Purpose:** Declares the three cursor-to-world helpers and documents which one a caller should choose.
- **Key types/functions:** `mouse_world`, `mouse_true_world`, `mouse_true_hierpos`; forward-declares `struct game_state`.
- **Notable non-obvious dependencies:**
  - **The header's real content is the selection rule** — it names the intended callers of the true-world variants ("edit picking, piloting/aiming") because choosing `mouse_world` there would be subtly wrong once render space diverges from true world. That guidance exists nowhere else.
  - **Includes `math/bs_hierpos.h` for the return type**, making it one of the sandbox's direct engine-boundary headers despite being three lines of declarations.
  - Neither the header nor the implementation exposes the framebuffer or camera the functions depend on, so the `game_state` parameter is doing double duty as an implicit context.
- **Interface vs internal:** Public interface within the sandbox — 21 files depend on the transform it fronts.

---

## sandbox/source/core/galaxy_coords.cpp

- **Path:** `sandbox/source/core/galaxy_coords.cpp`
- **Purpose:** Implements conversions between the galaxy-scale hierarchical frame and per-system local frames, plus nearest-system and orbital-zone lookups.
- **Key types/functions:**
  - `find_nearest_system(const HierPos2*, const StarSystem*, i32 count)` — brute-force nearest, `-1` when empty.
  - `galaxy_to_system_local(const HierPos2* ship, const HierPos2* center)` — difference as a plain `Vec2`.
  - `world_to_galaxy_pos(Vec2)` — canonicalises into the default cell size.
  - `get_system_zone(const game_state*, i32 system_idx)` — which concentric orbital ring the player occupies.
- **Notable non-obvious dependencies:**
  - **`get_system_zone` silently hardcodes the player's identity** as `s->galaxy.map_entities[0]` — index 0 is assumed to be the player fleet, an assumption stated nowhere and invisible at the call site.
  - **`find_nearest_system` recomputes the ship's world coordinates inside the loop**, converting `ship_pos` to `f64` once per candidate system rather than once overall.
  - **Zone numbering is outside-in** (0 = beyond the outermost orbit), the inverse of the usual convention, and is documented only in comments here and in the header.
  - **Sorts orbit radii with a bubble sort into a fixed `MAX_SYSTEM_PLANETS` stack array**, justified in-comment by "N <= 5"; a system exceeding that constant would overflow the array, since the loop bound is `planet_count` rather than the array size.
  - `galaxy_to_system_local` narrows an exact `f64` difference to `f32` — safe only because the result is a within-system offset, which is the whole point of the local frame.
  - Every function except `world_to_galaxy_pos` depends on `game_state`'s layout, either directly or through `StarSystem`.
- **Interface vs internal:** Public interface within the sandbox — the shared vocabulary for moving between galaxy and system coordinate frames.

---

## sandbox/source/core/galaxy_coords.h

- **Path:** `sandbox/source/core/galaxy_coords.h`
- **Purpose:** Declares the four galaxy/system coordinate helpers.
- **Key types/functions:** `find_nearest_system`, `galaxy_to_system_local`, `world_to_galaxy_pos`, `get_system_zone`; forward declarations of `game_state` and `StarSystem`.
- **Notable non-obvious dependencies:**
  - **Documents the outside-in zone numbering** — the one place the convention is stated as an interface contract rather than an implementation comment.
  - **`get_system_zone` takes `game_state*` while its three siblings take explicit coordinates**, an asymmetry that marks it as the only one bound to the live game rather than a pure conversion.
  - Forward-declares both structs, so the header itself pulls in only `defines.h` and `bs_hierpos.h` despite the implementation needing the full `game.h`.
  - Nothing here signals that `get_system_zone` reads the player from a hardcoded entity index, or that the planet-orbit scan is bounded by a fixed-size array.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/core/geom2d.cpp

- **Path:** `sandbox/source/core/geom2d.cpp`
- **Purpose:** Implements two stateless 2D geometry predicates — point-in-polygon and point-to-segment distance.
- **Key types/functions:** `point_in_polygon(Vec2 p, const Vec2* verts, i32 n)` (even-odd ray cast) and `point_to_segment(Vec2 p, Vec2 a, Vec2 b)` (projection clamped to the segment).
- **Notable non-obvious dependencies:**
  - **None** — no globals, no `game_state`, no allocation, no callbacks. Along with the engine's `math_utils.cpp` it is one of the very few files in the project with no hidden coupling at all.
  - `point_in_polygon` divides by `verts[j].y - verts[i].y` guarded only by the `(verts[i].y > p.y) != (verts[j].y > p.y)` test, which makes the denominator non-zero — correct but non-obvious, and it means the guard is load-bearing rather than merely an optimisation.
  - `point_to_segment` special-cases a degenerate segment (`ab2 < 0.0001f`) by returning the point-to-`a` distance.
  - Uses the engine's `clampf` and `vec2_*` free functions rather than open-coding, so it inherits `math_utils`'s conventions.
- **Interface vs internal:** Public interface within the sandbox — a shared leaf utility. The header names its two consumers explicitly: editor picking and combat hit-testing.

---

## sandbox/source/core/geom2d.h

- **Path:** `sandbox/source/core/geom2d.h`
- **Purpose:** Declares the two geometry predicates.
- **Key types/functions:** `point_in_polygon`, `point_to_segment`.
- **Notable non-obvious dependencies:**
  - **Advertises its own lack of coupling** — "Pure 2D geometry helpers (no `game_state`)" is the first line, marking the absence of the sandbox's dominant dependency as a deliberate property worth stating.
  - `point_in_polygon` takes a raw pointer and count with no ownership or lifetime note, and the polygon is documented as "closed" without saying whether the caller repeats the first vertex (the implementation wraps with `j = n - 1`, so it must not).
  - Names its consumers in a comment, which is the only record of who depends on it.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/core/profiler.cpp

- **Path:** `sandbox/source/core/profiler.cpp`
- **Purpose:** Implements the per-frame CPU zone profiler — zone naming, accumulation and rolling averages, ingestion of engine-supplied timings, and the ImGui panel body.
- **Key types/functions:**
  - `Profiler::init` — assigns the display name and group of all 15 zones.
  - `Profiler::begin_frame`, `::begin(id)`, `::end(id)` — accumulation and frame rollover.
  - `::set_wall_dt`, `::set_present_ms`, `::set_present_breakdown` — inputs from the engine.
  - `::build_ui` — emits the panel through `bs_ui_*`.
  - `static profiler_now_ns()` and three file-static group-label pointers `G_FRAME` / `G_UPDATE` / `G_RENDER`.
- **Notable non-obvious dependencies:**
  - **The three group labels are file-static pointers specifically so `build_ui` can compare them by pointer identity** — the comment notes string-literal pooling is not guaranteed across separate occurrences. Grouping in the UI therefore depends on every zone being assigned one of these three exact pointers in `init`.
  - **`build_ui` reaches into the engine and mutates renderer state** — the "GPU mode" button calls `renderer_set_present_mode` and only mirrors `present_immediate` when the call succeeds, because a driver may refuse IMMEDIATE. A profiler panel is thus a control surface for the swapchain.
  - **Uses `std::chrono::steady_clock` rather than the engine's clock**, and the header explains why: `platform_get_absolute_time` is engine-internal and not exported to the sandbox. So the game and the engine measure time from two different sources.
  - **Every ingested value is filtered through a plausibility window** (`> 1000 ms` discarded) to stop first-frame spikes and debugger breakpoints from poisoning the rolling averages — the same 0.9/0.1 EMA is applied to five separate quantities.
  - **`begin_frame` finalises the *previous* frame** rather than requiring an `end_frame`, which the header calls out as the reason there is no ordering dependency between the update and render passes.
  - Zones with no recorded time are skipped in the UI, so the panel's row set changes with what actually ran.
  - `Profiler::begin`/`end` do not bounds-check `id`, and mismatched pairs silently corrupt the accumulator rather than failing.
- **Interface vs internal:** Implementation of a type embedded directly in `game_state`. The profiler is a member, not a singleton, but there is exactly one instance and the whole game calls into it.

---

## sandbox/source/core/profiler.h

- **Path:** `sandbox/source/core/profiler.h`
- **Purpose:** Declares the zone enumeration, the profiler struct, and the RAII scope-timer macro.
- **Key types/functions:**
  - `enum ProfileZoneId` — two frame totals, six render zones, seven update zones, plus `PROF_ZONE_COUNT`.
  - `struct ProfileZone` — name, group, `last_ms`, `avg_ms`, `accum_ms`, `t0`.
  - `struct Profiler` — the fixed zone table plus `enabled`, `expanded`, `present_immediate`, and four engine-fed timings; nine member functions.
  - `struct ScopedProfile` and the `BS_PROFILE(prof, id)` macro (with `__LINE__`-based name mangling).
- **Notable non-obvious dependencies:**
  - **The enum is the coupling surface** — `PROF_ZONE_COUNT` sizes the table, and `init` populates names positionally, so inserting a zone mid-enum silently relabels every later one.
  - **The header documents a required call protocol**: `begin_frame()` once at the top of `game_update`, then either `BS_PROFILE` for scopes or explicit `begin`/`end` pairs. Nothing enforces it.
  - **Allocation-free and fixed-size by design**, stated up front — consistent with the codebase's general avoidance of dynamic allocation in the frame path.
  - **`ScopedProfile` does not null-check `p`**, so `BS_PROFILE(nullptr, ...)` crashes in the constructor; the macro's convenience hides the dereference.
  - The zone list itself is a map of the game's intended subsystem decomposition — fleet autopilot, fleet sim, ship collision, projectiles, travel, RTS controls, out-sensor FX, stars, background, ships, heat map, UI — recorded here before any of those subsystems is named as such anywhere else.
  - `present_immediate` is described as a "UI mirror", making explicit that the authoritative value lives in the engine.
- **Interface vs internal:** Public interface within the sandbox — the struct is embedded in `game_state` and the macro is used across update and render code.

---

## sandbox/source/core/render_layers.h

- **Path:** `sandbox/source/core/render_layers.h`
- **Purpose:** Defines the sandbox's render-layer numbering as a set of named constants shared by every draw site.
- **Key types/functions:** `LAYER_STARFIELD_FAR` (0), `LAYER_STARFIELD_MID` (2), `LAYER_MAPPED_SYSTEM` (3), `LAYER_SHIP` (10), `LAYER_CELESTIAL` (11), `LAYER_UI` (50), `LAYER_HUD_TEXT` (100), plus `LAYER_DEBUG` and `LAYER_GIZMO` derived from `BS_LAYER_BLOOM_THRESHOLD`.
- **Notable non-obvious dependencies:**
  - **Two of the nine constants are defined relative to an engine constant** — `LAYER_DEBUG = BS_LAYER_BLOOM_THRESHOLD` and `LAYER_GIZMO = threshold + 1` — so the game's debug overlays are pinned to the engine's bloom cutoff by construction rather than by a matching literal. Changing the engine's threshold silently moves them.
  - **`LAYER_UI` is the literal `50`, numerically equal to `BS_LAYER_BLOOM_THRESHOLD`** but written as a constant rather than derived — so `LAYER_UI` and `LAYER_DEBUG` currently collide, and would diverge if the engine constant changed. The two spellings express the same number by different means.
  - **Deliberately `static` rather than `extern`**, with a comment stating each TU gets its own copy to avoid ODR and link concerns — an unusual choice that also means these are not visible in a debugger by symbol name and produce unused-variable pressure in TUs that use only a few.
  - The numbering encodes rendering policy that the engine reads back: layers below the threshold go through bloom, at or above bypass it, and the backend separately treats layers relative to an unlit threshold.
- **Interface vs internal:** Public interface within the sandbox — the shared draw-order vocabulary, and the game side of a contract whose other half lives in `renderer_types.h`.

---

## sandbox/source/core/view_transform.cpp

- **Path:** `sandbox/source/core/view_transform.cpp`
- **Purpose:** Implements the paired screen ↔ true-world ↔ render-space conversions every galaxy-view system shares, plus the zoom-driven arena/map cross-fade weight.
- **Key types/functions:**
  - `view_arena_weight(f32 zoom)` — smoothstep across `[VIEW_MAP_ZOOM 0.05, VIEW_ARENA_ZOOM 0.14]`.
  - `game_screen_to_true_world`, `game_true_world_to_render`, `game_camera_center`.
  - `game_screen_to_true_hierpos`, `render_from_hierpos`, `game_camera_center_hierpos` — the precision-safe forms.
  - `f32 g_zoom_out_speed_gain` — a non-static global with external linkage.
- **Notable non-obvious dependencies:**
  - **Defines a mutable global tuning value, `g_zoom_out_speed_gain`**, declared `extern` in the header and driven by an editor slider — so an unrelated UI panel writes a variable this module owns and a third module (the zoom controller) reads.
  - **This file is the single definition of the floating-origin convention**: the renderer draws entities at `world - camera_hierpos`, and all six transforms exist to apply or invert exactly that. The header states the reason plainly — stars, ships, voronoi cells and hover FX all route through these so they cannot drift apart.
  - **The `Vec2` and `HierPos2` variants are not equivalent.** `game_true_world_to_render` collapses the camera hierpos to `Vec2` first and subtracts in `f32`; `render_from_hierpos` subtracts in integer cell space. Both are offered, and picking the wrong one reintroduces exactly the precision loss the refactor removed — with no compile-time distinction.
  - **The arena/map cross-fade replaced a discrete mode flip.** The comment notes the band deliberately straddles the old hard cutover point (`ZOOM_MIN = 0.08`), and that render sites should read `s->view_arena_w` rather than branch on `GameMode` — a stated migration away from mode-based branching that other files may or may not have followed.
  - **`game_camera_center` reconstructs the true centre as `camera_hierpos + camera.position`**, treating `camera.position` as a small render-space residual — an invariant about how the two camera representations are kept in sync that is documented only here.
  - All six `game_*` functions dereference `s` without a null check.
- **Interface vs internal:** Public interface within the sandbox, and one of its most depended-upon — 21 files include the header. It is the coordinate authority for everything drawn in the galaxy view.

---

## sandbox/source/core/view_transform.h

- **Path:** `sandbox/source/core/view_transform.h`
- **Purpose:** Declares the six view transforms, the arena-weight function, and the externally-tunable zoom gain.
- **Key types/functions:** `extern f32 g_zoom_out_speed_gain`; `view_arena_weight(f32)`; the three `Vec2` transforms and their three `HierPos2` counterparts; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Splits its own API into two documented tiers** — "pure functions of zoom only (no `game_state`; trivially unit-testable)" versus the `game_state`-dependent transforms. That is the only place in the sandbox where testability is called out as an organising principle.
  - **Exposes a mutable global as part of the interface**, complete with a note that it is driven by a named editor slider ("Zoom-out speed gain") — coupling this header to a specific UI control.
  - **States the core invariant in one line** — "the renderer draws entities at (world − camera_hierpos)" — which is the premise every transform here and every render site depends on.
  - The comment "Positions render linearly at every zoom" records a property that was presumably not always true, hinting at a prior non-linear scaling scheme.
  - Nothing distinguishes, at the type level, the `f32` transforms from their precision-safe twins; the choice is left to the caller's judgement and the comments.
- **Interface vs internal:** Public interface within the sandbox — declared for broad use and consumed by rendering, input, RTS control, and editor code alike.

---

## sandbox/source/entry.cpp

- **Path:** `sandbox/source/entry.cpp`
- **Purpose:** Supplies `game_create` — the single symbol the engine requires from the host — filling in the window config, the four lifecycle callbacks, and the game's state allocation.
- **Key types/functions:** `b8 game_create(Game* out_game)`. Nineteen lines, no types of its own.
- **Notable non-obvious dependencies:**
  - **This is the whole sandbox side of the engine/game inversion.** It is the only file that includes `<entry.h>`, and including that header is what pulls the engine's `main()` into this translation unit — so `sandbox.exe`'s entry point is defined in the engine tree and this file merely satisfies the `extern b8 game_create(Game*)` it declares.
  - **Allocates `game_state` through the engine's tagged allocator** (`bs_memory_allocator(sizeof(game_state), MEMORY_TAG_GAME)`) and stores it in `out_game->state`. It is never freed — the engine treats `state` as opaque and the game has no shutdown hook, so the allocation lives until process exit.
  - **The four function pointers are the only place `game_init` / `game_update` / `game_render` / `game_on_resize` are bound**; the engine validates all four are non-null before starting.
  - **Window configuration is hardcoded here** — title "Black Stride Engine Sandbox", position (100, 100), 1280×720 — with no config file or command-line override anywhere in the path.
  - Returns `TRUE` unconditionally: the allocation result is never checked, so an allocation failure produces a null `state` that every later call dereferences.
  - The allocated struct is zeroed by `bs_memory_allocator` but no constructor runs — `game_state` is treated as POD despite containing C++ members elsewhere.
- **Interface vs internal:** Public interface in the strictest sense — it exists solely to export one symbol the engine links against. Nothing in the sandbox calls it.

---

## sandbox/source/font8x8.h

- **Path:** `sandbox/source/font8x8.h`
- **Purpose:** Embeds a public-domain 8×8 monochrome bitmap font (printable ASCII) as a static lookup table.
- **Key types/functions:** `g_font8x8[FONT8X8_COUNT][FONT8X8_GLYPH_H]` — 95 glyphs × 8 row bytes; the macros `FONT8X8_FIRST` (0x20), `FONT8X8_LAST` (0x7E), `FONT8X8_COUNT`, `FONT8X8_GLYPH_W`, `FONT8X8_GLYPH_H`.
- **Notable non-obvious dependencies:**
  - **Third-party data vendored directly into the source tree** — attributed in the header to Daniel Hepper's font8x8, itself derived from IBM VGA ROM fonts, public domain. It sits in `sandbox/source` rather than a vendor directory, so it is compiled under the same `-Wall -Werror` as game code.
  - **The bit order is the non-obvious part and is documented at length**: bit 0 is the *leftmost* pixel, the opposite of the intuitive reading. Any consumer that gets this backwards renders mirrored glyphs.
  - **Names its single consumer** — `text.cpp`, which bakes the table into one RGBA8 atlas at startup. The table itself is never used at draw time.
  - **`static const` in a header**, so every including TU gets a private 760-byte copy; with one includer that is moot, but it follows the same convention as `render_layers.h`.
  - Indexing requires the caller to subtract `FONT8X8_FIRST`; nothing bounds-checks, so a character outside 0x20–0x7E reads out of the array.
- **Interface vs internal:** Internal data asset with a public-looking spelling. It declares no behaviour and has exactly one consumer.

---

## sandbox/source/game.h

- **Path:** `sandbox/source/game.h`
- **Purpose:** A seven-line forwarding shim that includes `state/game_state.h`, preserved so existing modules keep compiling after the state struct moved.
- **Key types/functions:** None of its own — a single `#include "state/game_state.h"`.
- **Notable non-obvious dependencies:**
  - **It documents its own obsolescence**: the comment states `game.h` "is now a thin shim", that every existing module still includes it unchanged, and that new code may include `state/game_state.h` directly. So the file exists purely to avoid touching ~48 call sites.
  - **Despite being empty of content it has the second-highest fan-in in the project (48 includers)** — a redirect that dominates the sandbox's include graph and makes almost every sandbox TU depend transitively on a 3652-line header.
  - Because it forwards rather than forward-declares, including it is not cheap: any change to `game_state.h` recompiles everything that touches `game.h`.
  - The migration it describes is incomplete — both spellings are in use, so the graph shows two distinct paths to the same definition.
- **Interface vs internal:** Public interface within the sandbox by usage, though it is really a compatibility artifact rather than a designed surface.

---

## sandbox/source/game_modules.h

- **Path:** `sandbox/source/game_modules.h`
- **Purpose:** An aggregate include pulling in every extracted gameplay, render, and UI module header, intended for exactly one consumer.
- **Key types/functions:** No declarations — 29 `#include` directives, each annotated with the functions the module provides.
- **Notable non-obvious dependencies:**
  - **It encodes an explicit architectural rule and the reasoning behind it.** The comment states this cascade used to live at the bottom of `game.h`, that it is now included *only* by `game.cpp`, and that modules "must NOT rely on this aggregate" — each module should include the specific peers it calls. Two justifications are given: avoiding a full rebuild when one module header changes, and forcing real dependencies to be explicit rather than satisfied by hidden transitive includes.
  - **The highest fan-out file in the project (29 project includes)** and, by design, a fan-in of essentially one. That asymmetry is the point.
  - **The annotated include list is the closest thing the codebase has to a module inventory** — it names the entry point of each subsystem (`galaxy_map_init`, `combat_arena_update_projectiles`, `ai_ships_update`, `render_scene`, `update_zoom_and_mode`, …) in one place.
  - **One comment references a header that is not included and does not exist in the tree** — the `action_log.h` entry says "The presentation panel is `ui/action_log_panel.h`", but no such file is present, so the note points at either a planned or a removed module.
  - Several comments carry phase markers ("Phase A") and doc-file references, tying modules to external design documents.
  - Nothing enforces the "modules must not include this" rule; it holds only by convention.
- **Interface vs internal:** Internal — a build-hygiene device for a single translation unit, not an API. Its real value is documentary.

---

## sandbox/source/game.cpp

- **Path:** `sandbox/source/game.cpp`
- **Purpose:** The frame orchestrator — implements the four engine callbacks, owns startup construction and asset loading, sequences every update and render subsystem, handles global keybindings, and marshals the entire RmlUi HUD snapshot. At 3403 lines it is the sandbox's second-largest file.
- **Key types/functions:**
  - `game_init(Game*)` — ~610 lines: placement-new of `game_state`, ~120 render/tuning defaults, registry loads, fleet and enemy construction, camera setup, debug self-tests, texture generation, and UI bring-up.
  - `game_update(Game*, f32 dt)` — ~1100 lines: profiling, the generation gate, the sim clock, ~25 keybindings, camera rebase, and subsystem dispatch.
  - `game_render(Game*, f32 dt)` — delegates world drawing to `render_scene`, then builds the ImGui editor windows.
  - `game_on_resize(Game*, u32, u32)` — stores framebuffer dimensions.
  - `static game_push_hud(game_state*, f32)` — ~960 lines building the `bs_rml_hud_state` snapshot and draining the UI action queue.
  - `static run_generation_stage(game_state*)` — the five-stage staged worldgen pipeline.
  - Loadout helpers: `ship_stash_append`, `ship_stash_remove_at`, `ship_rehome_weapons`, `ship_module_stash_append`, `ship_module_stash_remove_at`, `ship_evict_module`, `hardpoint_kind_label`, `hardpoint_short_id`, `arsenal_drop_on_slot`, `flagship_hardpoint_at_cursor`.
  - Exported tuning constants `SHIP_ACCEL`, `SHIP_DECEL`, `SHIP_MAX_SPEED`, `SHIP_TURN_ACCEL`, `SHIP_MAX_TURN` (non-static, consumed elsewhere).
- **Notable non-obvious dependencies:**
  - **It is the only includer of `game_modules.h`**, by explicit design, and therefore the one TU that sees every module header. Its 24 project includes make it the second-highest fan-out file in the project.
  - **`game_init` uses placement-new on engine-allocated memory** (`new (s) game_state()`), with a comment explaining why: `game_state` contains two `Ship` members at roughly 3 MB each, so value-initialising it as a stack temporary would blow the ~1 MB default Windows stack. `RtsControls` is separately placement-new'd with `s` as a constructor argument, making it the one member that needs the state pointer at construction.
  - **Reads four asset files by hardcoded relative path** — `assets/modules/modules.list`, `assets/weapons/weapons.list`, `assets/ships/ship/ship.ship`, `assets/enemy_ship.ship` — plus two emblem PNGs, the RmlUi font directory `assets/ui/fonts`, and the HUD document `assets/ui/hud.rml`. All resolve against the working directory, so the game depends on `build-all.bat` having staged `assets/` into `bin/`.
  - **Runs two engine self-tests at startup under `BS_DEBUG`** — `bs_hierpos_selftest()` (fatal on failure) and `system_evolution_selftest()`. This is the *only* caller of `bs_hierpos_selftest` in either tree, so the engine's dead-looking test function is in fact reachable from here.
  - **Generates a 64×64 exhaust texture procedurally on the stack** (16 KB of pixels) and uploads it via `renderer_create_texture`.
  - **~25 global keybindings are hardcoded inline** in `game_update` as `input_is_key_down(K) && !input_was_key_down(K)` edge tests — TAB, SHIFT, R, V, T, P, L, N, I, H, O, K, G, and F3–F12. There is no key-binding table, no remapping, and the mapping exists nowhere but these call sites.
  - **`game_push_hud` is the entire game→UI boundary**: it fills the ~145-field `bs_rml_hud_state` with pre-formatted strings and then drains `bs_rml_hud_poll_action` in a loop, dispatching on string comparison (`"close_discoveries"`, `"fleet_mode"`, `"station_inspect"`, `"defdrag"`, `"baydrop"`, `"uidrop"`, `"dragend"`, `"engage"`, `"avoid"`, `"hail"`, `"observe"`, plus prefixed forms). The action grammar is a stringly-typed protocol split between this function and `bs_rml.h`'s comments.
  - **The file is mostly a map of what it no longer contains.** Roughly 40 comment blocks record functions that moved out — "`update_zoom_and_mode` now lives in `sim/camera_controller.cpp`", "`draw_hierpos_cell_grid` now lives in `render/debug_overlay.cpp`", and so on. These breadcrumbs are the clearest available record of the extraction history.
  - **Startup order is load-bearing and undocumented as a whole**: `camera_hierpos` must be set before `galaxy_map_init` so the initial cache materialises around the home system; `text_init` must follow renderer readiness; `ship_visual_resolve_textures` must run after the renderer is live; `s->npc_combat_base` captures the combat-entity count immediately after `ai_ships_init`.
  - **A three-phase application state machine** (`APP_SETUP` → `APP_GENERATING` → `APP_PLAYING`) gates both update and render with early returns, and worldgen runs one heavy stage per frame so the progress bar can advance.
  - **Pushes engine timings into the profiler each frame** (`renderer_get_frame_timing`, `renderer_get_present_breakdown`) and clamps `dt` to 0.05 s twice — once raw and once after `time_scale` scaling.
  - Advances a shared in-game calendar at 1 real second = 1 in-game hour, which drives both local gameplay and galaxy history.
  - Per-frame it retags the enemy ship's faction from whichever civ owns the player's nearest node, defaulting to pirates in unclaimed space — so the "enemy" hull's identity is recomputed every frame rather than being fixed at spawn.
  - The formatting is unusual: nearly every statement is separated by a blank line, roughly doubling the file's length.
- **Interface vs internal:** Implementation, and the top of the sandbox's call graph. Its only exported surface is the four `game_*` callbacks (declared in `state/game_state.h`) plus the five ship-tuning constants; every helper is `static`.

---

## sandbox/source/jc_voronoi.h

- **Path:** `sandbox/source/jc_voronoi.h`
- **Purpose:** A vendored third-party single-header Voronoi diagram generator (Fortune's sweepline with clipping), used to partition the galaxy into territory cells.
- **Key types/functions:**
  - Types `jcv_point`, `jcv_rect`, `jcv_site`, `jcv_edge`, `jcv_graphedge`, `jcv_diagram`, `jcv_clipper`, `jcv_real`.
  - API `jcv_diagram_generate`, `jcv_diagram_generate_useralloc`, `jcv_diagram_free`, `jcv_diagram_get_sites`, `jcv_diagram_get_edges`, `jcv_diagram_get_next_edge`, and the box-clipper trio.
  - Configuration macros `JCV_REAL_TYPE` (defaults to `float`), `JCV_ATAN2`, `JCV_SQRT`, `JCV_PI`, `JCV_FLT_MAX`, `JCV_EDGE_INTERSECT_THRESHOLD`.
- **Notable non-obvious dependencies:**
  - **Third-party code living in the game's own source tree, not a vendor directory** — MIT, Mathias Westerdahl, 2015–2023. Unlike ImGui, RmlUi, and FreeType (isolated under `engine/vendor` and compiled with relaxed warnings), this compiles as part of `sandbox.exe` under the same `-Wall -Werror` as game code.
  - **Header-only with an implementation switch**: the body is fenced behind `#ifdef JC_VORONOI_IMPLEMENTATION`, and exactly one TU (`sim/voronoi_galaxy.cpp`) defines it. Same one-definition arrangement as the engine's `stb_image_impl.cpp`, but without a dedicated TU or warning suppressions.
  - **Allocates internally with `malloc`/`free` by default**, bypassing the engine's tagged allocator entirely — so Voronoi memory never appears in `bs_memory`'s accounting. A `_useralloc` variant exists but the code does not use it.
  - **`JCV_REAL_TYPE` is left at `float`**, which matters at galaxy scale: the same precision concern that motivated the whole `HierPos2` refactor applies to the diagram coordinates fed in here.
  - Uses `<assert.h>` directly, the only file in the sandbox that does, and its assertions are independent of the engine's own (unused) `BS_ASSERT` family.
  - Ships with its own usage docs, license text, and history at the bottom of the file, accounting for a large share of its 1494 lines.
  - Being a `.h`, it is not swept by any glob — it enters the build only through `voronoi_galaxy.cpp`'s include.
- **Interface vs internal:** Public interface of a third-party library, but consumed by exactly two sandbox files. Structurally it is the sandbox's only vendored dependency and the only place unmanaged `malloc`-backed memory enters the game.

---

## sandbox/source/render/debug_overlay.cpp

- **Path:** `sandbox/source/render/debug_overlay.cpp`
- **Purpose:** Draws a visual overlay of the hierarchical coordinate lattice — cell boundaries around the flagship, a parity checkerboard, and per-cell index labels.
- **Key types/functions:** `b8 g_debug_cell_grid` (global definition) and `draw_hierpos_cell_grid(const game_state*)`, plus two local lambdas `corner` and `cell_occupied`.
- **Notable non-obvious dependencies:**
  - **Defines a mutable global toggle, `g_debug_cell_grid`**, written by the editor panel and read by `game_render` — a three-way coupling between UI, orchestrator, and this module with no accessor.
  - **The overlay is deliberately a self-check, not decoration.** Every line is built from `HierPos2` corners routed through `render_from_hierpos`, "the exact path entities use", so a broken floating-origin rebase makes the grid visibly shear or jitter against the ships. Its diagnostic value depends entirely on it *not* taking a shortcut.
  - **Draws a fixed 7×7 cell window** (`R = 3`) centred on the flagship's cell, so cost is constant but coverage is tied to `s->player_ship()`.
  - **Highlights a cell when any fleet ship occupies it**, scanning the whole fleet per cell — 49 × fleet-size comparisons per frame.
  - **Emits up to ~245 debug primitives per frame** (196 lines, up to 24 quads, 49 text labels), all through the engine's sprite-batch-backed debug layer, so the overlay competes for the same 16384-sprite budget as the game.
  - **Uses two different layers with different bloom behaviour** — `LAYER_UI` for lines and `LAYER_HUD_TEXT` for labels — while the parity fill is drawn on the bare literal layer `1`, below the ships rather than through a named constant.
  - **Records a migration in its closing comment**: the screen-anchored numeric readout that used to live here is now an RmlUi document (`#debug` in `hud.rml`, filled by `game_push_hud`), leaving only world-anchored drawing.
  - Labels are positioned by projecting the cell centre through `camera2d_world_to_screen` and then centring on measured text width, mixing world-space and screen-space drawing in one function.
- **Interface vs internal:** Internal debug tool with one exported function and one exported global. It participates in no gameplay logic.

---

## sandbox/source/render/debug_overlay.h

- **Path:** `sandbox/source/render/debug_overlay.h`
- **Purpose:** Declares the cell-grid overlay function and its global toggle.
- **Key types/functions:** `extern b8 g_debug_cell_grid`; `draw_hierpos_cell_grid(const game_state*)`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Exposes a mutable global as part of a two-symbol interface**, and names both ends of the wiring in the comment: set from the "COORDINATE SPACE editor panel", read by `game_render`. The header is the only record of that contract.
  - **Restates the diagnostic rationale** — that routing through `render_from_hierpos` is what makes a broken rebase visible — so the invariant is documented at the interface, not just the implementation.
  - The comment claims the overlay includes "a HUD readout", which is no longer true; that part moved to the RmlUi document and only the implementation's closing comment records the change.
  - Forward-declares `game_state`, keeping the header to two lines of real content.
- **Interface vs internal:** Internal interface — a debug affordance wired between one UI panel and the frame orchestrator.

---

## sandbox/source/render/defense_laser_overlay.cpp

- **Path:** `sandbox/source/render/defense_laser_overlay.cpp`
- **Purpose:** Draws point-defense presentation — the flagship's engagement ring and the beams the PD simulation recorded this frame, each with a glow, a hot core, and an impact flash.
- **Key types/functions:** `defense_laser_overlay_draw(game_state*)`; local styling constants `GLOW_TH`, `CORE_TH`, `CORE_COL`, and a `GATE_FRAC[3]` table.
- **Notable non-obvious dependencies:**
  - **Consumes a per-frame buffer produced by another module** — `s->defense_beams` / `s->defense_beam_count`, written by `sim/point_defense.cpp`. The two communicate only through `game_state`, so the draw is a pure replay of the sim's decisions with no shared code.
  - **Duplicates the simulation's range formula rather than sharing it.** The comment says it "mirrors `sim/point_defense.cpp`", and the `GATE_FRAC` table `{0.6, 0.8, 1.0}` is a second copy of the gate tiers also documented in `bs_rml.h`. Three files now encode the same three numbers; nothing keeps them in step.
  - **Colour encodes simulation state** — amber for `PD_OVERDRIVE`, steel for `PD_STANDARD`, hidden entirely for `PD_HOLD` or an unmounted launcher — so stance changes get in-world feedback without any UI.
  - **The impact flash is divided by camera zoom** to hold a constant screen size, the same manual zoom-cancellation the engine's debug line thickness performs internally.
  - Falls back to `flag.sensors.layer0_radius` when the PD has no explicit range, coupling the overlay to the sensor model.
  - Every position goes through `render_from_hierpos`, so the overlay is correct under floating origin.
  - Clamps zoom to `1e-4` before dividing, guarding a degenerate camera.
- **Interface vs internal:** Internal render pass with a single exported entry point. It reads game state and emits draws; nothing consumes anything it produces.

---

## sandbox/source/render/defense_laser_overlay.h

- **Path:** `sandbox/source/render/defense_laser_overlay.h`
- **Purpose:** Declares the one-function point-defense overlay pass.
- **Key types/functions:** `defense_laser_overlay_draw(game_state*)`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Labels itself "presentation only"** and names the exact state it reads (`s->defense_beams` / `defense_beam_count`) and the module that fills it — making the sim→render handoff explicit at the interface, which the include graph does not show.
  - Takes a non-const `game_state*` despite being read-only, matching the convention of the other render passes rather than its own needs.
  - The comment describes the beams and impact flash but not the engagement ring the implementation also draws.
- **Interface vs internal:** Internal interface — one render pass among the ordered set the scene renderer drives.

---

## sandbox/source/render/frame_lighting.cpp

- **Path:** `sandbox/source/render/frame_lighting.cpp`
- **Purpose:** Assembles and submits the frame's lighting — speed-driven dynamic bloom, the volumetric star light and ambient cross-fade, the point-light array, and the glow and bloom parameters.
- **Key types/functions:** `submit_frame_lighting(game_state*)`; a local `bs_light2d frame_lights[16]` array.
- **Notable non-obvious dependencies:**
  - **This is the single place the renderer's lighting state is set each frame** — four engine calls (`renderer_set_lights`, `_set_glow_params`, `_set_bloom_enabled`, `_set_bloom_params`) live here and nowhere else, so lighting is centralised even though its inputs are scattered.
  - **Everything is driven by `s->view_arena_w`, the continuous zoom-derived blend weight**, not by discrete modes. Star light and ambient fade in by map weight (`1 - arena_w`) while dynamic bloom fades in by arena weight — two effects cross-fading in opposite directions across the same band. The comments state the endpoints explicitly: at `map_w == 0` the arena is fullbright, at `map_w == 1` the map is exactly the original volumetric-lit look.
  - **Hardcodes `16` as the light array bound**, matching the backend's `BS_BACKEND_MAX_LIGHTS` by coincidence of literal rather than by a shared constant — the engine's cap is not exported.
  - **Passes `LAYER_UI` as the renderer's `unlit_layer` threshold**, so the sandbox's layer numbering decides which sprites render fullbright. A change to `LAYER_UI` silently changes lighting.
  - **Depends on ordering it cannot enforce**: the header states it must run after the star position is established and before the lit sprite passes, and it reads `s->celestial_anchor`, which `game_render` computes earlier in the same frame.
  - **Reparallaxes the star light to match the drawn star** via `celestial_center_render` with `depth_star`, so lighting and visuals agree — a correction that would be invisible if omitted except as a subtle misalignment.
  - Star light intensity is scaled by sensor visibility (`get_sensor_visibility`), so an unscanned system lights the scene less.
  - The star light always occupies slot 0 when present, silently displacing one editor light if the player has 16.
  - The comment "Extracted verbatim from `game_render`" marks this as a lift-and-shift refactor rather than a designed module.
- **Interface vs internal:** Internal render pass. One exported function, invoked from the scene renderer's ordered pass list.

---

## sandbox/source/render/frame_lighting.h

- **Path:** `sandbox/source/render/frame_lighting.h`
- **Purpose:** Declares the lighting submission pass.
- **Key types/functions:** `submit_frame_lighting(game_state*)`; forward-declares `game_state`. Nine lines, no includes at all.
- **Notable non-obvious dependencies:**
  - **States a frame-ordering precondition in prose** — "must run after the star position is established and before the lit sprite passes" — which is the only record of where this pass belongs in the sequence.
  - **The only sandbox header with no `#include` whatsoever**, relying on the forward declaration alone.
  - Enumerates the four things it submits, which doubles as the inventory of what the game controls about lighting.
  - Notes it was "extracted verbatim from `game_render`", flagging that the grouping is historical rather than principled.
- **Interface vs internal:** Internal interface — one entry in the scene renderer's pass order.

---

## sandbox/source/render/gameplay_overlays.cpp

- **Path:** `sandbox/source/render/gameplay_overlays.cpp`
- **Purpose:** Draws the in-world gameplay overlay set — projectiles, RTS selection, enemy and NPC markers, sensor and point-defense overlays, edit-mode highlights and gizmos, travel debug, range rings, and the heat map.
- **Key types/functions:**
  - `draw_gameplay_overlays(game_state*)` — the single exported pass.
  - `static draw_unidentified_marker(...)` — the circle-and-cross glyph for unscanned contacts.
  - `static draw_enemy_marker(game_state*)` — reticle, corner brackets, and detector ring.
  - `static draw_npc_ship_markers(game_state*)` — role-shaped, civ-coloured NPC markers.
  - `static unid_marker_fade(...)` plus `UNID_MARKER_FULL_PX` / `UNID_MARKER_GONE_PX`.
- **Notable non-obvious dependencies:**
  - **The highest-coupling render file: 17 project includes**, reaching into editor tools, heat map, combat arena, ship render, sensor overlay, defense laser overlay, galaxy history, AI ship archetypes, text, and weapon defs. It is a hub that calls four other render passes plus several sim modules.
  - **Calls into three other modules' draw entry points** (`sensor_overlay_draw`, `defense_laser_overlay_draw`, `draw_ship_metaballs`) and two object methods (`s->projectiles.render(...)`, `s->rts_controls.draw()`), so the "pass" is really a small dispatcher.
  - **Mutates state while drawing** — it assigns `s->projectiles.glow_override = &s->render.bullet_glow` before rendering, so a render pass is writing a pointer into the simulation's projectile pool that the engine will later compare by identity for batch breaking.
  - **Opens and closes two profiler zones inline** (`PROF_PROJECTILES_DRAW`, `PROF_HEAT_MAP`) with explicit `begin`/`end` rather than the RAII macro.
  - **Marker visibility is driven by apparent system size, not zoom.** `unid_marker_fade` measures the current system's outermost orbit in screen pixels and smoothsteps between 120 px and 600 px, with the comment explaining that an absolute zoom threshold would not work because system sizes vary across decades of scale. A planetless system falls back to a hardcoded 250000-unit extent.
  - **The gizmo drawing must agree with `edit_pick_gizmo`'s hit test** — it calls that same function purely to decide hover colouring, so visual feedback and pick logic share one source of truth by calling it rather than duplicating it.
  - **The edit-mode selection path assumes only two selectable ships**, mapping index 0 to the player and anything else to the enemy — a hardcoded two-entity model in code that otherwise handles a fleet.
  - **The weapon-group reach ring uses the shortest reach in the active fire group**, with the reasoning spelled out: firing from outside it means the shortest-legged weapon's shells expire before arriving. Reach is derived from `proj_speed * proj_life` via `weapon_effective_reach`.
  - **Screen-constant sizing is done by hand throughout** — radii divided by zoom, thicknesses passed in screen pixels because the engine already divides those internally. The two conventions sit side by side in the same calls.
  - **Draws across three layers with different bloom behaviour** (`LAYER_UI`, `LAYER_GIZMO`, `LAYER_DEBUG` imported though only two are used), and iterates `NPC_SHIP_MAX` every frame regardless of how many NPCs are active.
  - The comment records a deduplication: `rts_controls.draw()` "runs exactly once here (previously duplicated per mode)".
- **Interface vs internal:** Internal render pass with one exported function, though in practice it is an aggregation point for most of the game's non-sprite visual feedback.

---

## sandbox/source/render/gameplay_overlays.h

- **Path:** `sandbox/source/render/gameplay_overlays.h`
- **Purpose:** Declares the gameplay overlay pass.
- **Key types/functions:** `draw_gameplay_overlays(game_state*)`; forward-declares `game_state`. Ten lines.
- **Notable non-obvious dependencies:**
  - **Enumerates the pass's contents in a comment** — projectiles, RTS, gizmos, travel, sensor range, heat map — which is the only inventory of what the pass covers.
  - **States the design constraint**: drawn in *all* looks so gameplay stays continuous across the arena↔map blend, i.e. these overlays must not pop at a mode boundary. That is why the pass is unconditional rather than mode-gated.
  - Tags itself "extracted verbatim from `game.cpp` (R1 scene_renderer decomposition)", naming the refactor that produced it.
  - Declares no includes at all beyond the forward declaration.
- **Interface vs internal:** Internal interface — one entry in the scene renderer's ordered pass list.

---

## sandbox/source/render/global_background.cpp

- **Path:** `sandbox/source/render/global_background.cpp`
- **Purpose:** Owns the parallax background layer stack (starfield, nebula, mapped system) and draws them back-to-front with per-layer virtual cameras and motion blur.
- **Key types/functions:** `GlobalBackground::init`, `::shutdown`, `::notify_system_changed`, `::draw`; the file-local template `draw_bg_layer<LayerT>`.
- **Notable non-obvious dependencies:**
  - **The only place in the sandbox that uses raw `new`/`delete`.** All three layers are heap-allocated in `init` and freed in `shutdown`, bypassing the engine's tagged allocator entirely — so background layer memory is invisible to `bs_memory`'s accounting, and a missed `shutdown` leaks.
  - **`draw_bg_layer` is a template, not virtual dispatch** — the three layer types share no base class and are unified structurally by having `parallax`, `zoom_scale`, `is_custom_gpu`, and `draw(...)`. That duck-typing contract is enforced only at instantiation.
  - **Each layer gets a synthesised virtual camera**: position scaled by the layer's parallax, zoom scaled by `zoom_scale`. Layers flagged `is_custom_gpu` skip `renderer_set_camera` because they take camera parameters through their own uniform path instead.
  - **Motion blur is derived from the player's velocity** (`gs->player_flight().velocity * dt`) and scaled by `1 - parallax`, so distant layers streak more. The background therefore depends on the flagship's flight state.
  - **Layer seeds are hardcoded magic constants** — `0xDEADBEEF` for the starfield and `0xBADDCAFE` for the nebula — so the procedural content is fixed across runs regardless of the galaxy seed the player chooses.
  - **Restores the real camera after drawing** (`renderer_set_camera(cam)` at the end), a required cleanup because the per-layer cameras leak into subsequent passes otherwise.
  - **Each layer is gated by an independent editor toggle** (`bg_layer0_enabled`, `bg_nebula_enabled`, `bg_layer2_enabled`), with the odd detail that a null `gs` is treated as "draw everything".
  - The starfield comment records a consolidation: the old far/mid parallax pair collapsed into one world-locked multi-LOD field covering both arena and map zoom ranges — which is why `LAYER_STARFIELD_MID` still exists as a constant but only one starfield object is created.
  - `notify_system_changed` exists specifically so sim code can trigger the render-side handoff without depending on the concrete `MappedSystemLayer` type.
- **Interface vs internal:** Implementation of a type embedded in `game_state`. Internal to rendering, but with one method (`notify_system_changed`) deliberately shaped as a sim-facing hook.

---

## sandbox/source/render/global_background.h

- **Path:** `sandbox/source/render/global_background.h`
- **Purpose:** Declares the `GlobalBackground` container that owns and sequences the parallax layers.
- **Key types/functions:** `struct GlobalBackground` — three layer pointers, a retained `game_state*`, and the methods `init`, `shutdown`, `draw`, `notify_system_changed`.
- **Notable non-obvious dependencies:**
  - **Forward-declares all four collaborator types** (`StarfieldLayer`, `NebulaLayer`, `MappedSystemLayer`, `StarFxSystem`) so the header stays light despite owning them by pointer — the concrete types are only needed in the `.cpp`.
  - **Retains a `game_state*` as a member**, documented as being for velocity-based motion blur. That makes the background a long-lived holder of a back-pointer into the god struct, not merely a per-frame consumer.
  - **Raw owning pointers with a manual `shutdown`** and no destructor, so lifetime is the caller's responsibility; nothing prevents a double `init` from leaking.
  - `notify_system_changed`'s comment explains the indirection's purpose — letting sim callers trigger a render-side handoff without naming the concrete layer type — which is the only stated reason the method exists.
  - The struct's field order fixes the draw order implicitly, though the actual sequence is hardcoded in `draw`.
- **Interface vs internal:** Internal interface, though the struct is a `game_state` member so its shape is visible to everything that includes the state header.

---

## sandbox/source/render/mapped_system_layer.cpp

- **Path:** `sandbox/source/render/mapped_system_layer.cpp`
- **Purpose:** Draws the camera's current star system — star, planets, and moons — as a world-locked background layer in the arena look.
- **Key types/functions:** `MappedSystemLayer::MappedSystemLayer`, `::on_system_changed`, `::draw`; file-local `is_on_screen(...)` and `get_sensor_visibility_global(...)`.
- **Notable non-obvious dependencies:**
  - **Tracks the camera's system, not the ship's**, and a long comment records the bug that forced this: using the ship's system drew the star ~1e8 units off-screen while the map renderer faded the viewed system's star, so stars appeared to "fade to nothing" when zooming into a remote system. It reads `gs->galaxy.current_system`, described as the camera's hot-cache slot set each frame by `galaxy_materialize_update`.
  - **Re-derives the render-space residual camera by hand** (`dcam.position = gs->camera_state.camera.position`) rather than using the parallax layer camera, with a comment quantifying why: at 2e9 units `f32` ULP is ~256 units, so subtracting two absolute floats loses all precision and the star jitters and gets culled. Integer cell differencing via `hierpos_diff` is the fix.
  - **This layer and the galaxy-map renderer draw the same star simultaneously**, each faded by the complementary blend weight, relying on additive blending to sum to a continuous full-intensity star across the band. The opaque dark pocket uses a separate saturating weight (`view_arena_w / 0.45` clamped) so at least one of the two passes fully occludes at every zoom — a cross-module numerical agreement with no shared constant.
  - **Calls `renderer_set_draw_alpha` and must restore it**, doing so at the end because it is the last background layer; the fade is deliberately set before the star cull so the planet loop inherits it even when the star is skipped.
  - **Toggles aux-bloom mode around the star draw** and pushes `renderer_set_streak_source`, computing a proxy world position (`star_world + main_cam_pos * (1 - parallax)`) so the aux pass, which uses the main camera, projects the proxy to the same screen point.
  - **Culls on the true draw extent, not the body radius** — in 3D sphere mode the dark-surround pocket extends well past the star, and culling on the body alone let the nebula bleed in at screen edges.
  - **Four separate size-scaling rules stack**: a minimum screen radius floor, a "hero star" floor that ramps by map weight, an edge-aberration `dist_scale` that also fades by weight, and per-planet min/max pixel clamps. Each exists to match the galaxy-map renderer's behaviour at the boundary.
  - Depends on `gs->celestial_anchor` being computed earlier in the frame, and reparallaxes star and planets with different depths (`depth_star` vs `depth_planet`).
  - Planets and moons are gated by one editor toggle (`celestial_draw_planets`) evaluated inside the loop condition rather than around it.
- **Interface vs internal:** Internal render layer, instantiated and owned solely by `GlobalBackground`.

---

## sandbox/source/render/mapped_system_layer.h

- **Path:** `sandbox/source/render/mapped_system_layer.h`
- **Purpose:** Declares the `MappedSystemLayer` background-layer type.
- **Key types/functions:** `struct MappedSystemLayer` — the duck-typed layer fields (`id`, `parallax`, `zoom_scale`, `is_custom_gpu`), back-pointers `gs` and `star_fx`, `current_system`, and the `on_system_changed` / `draw` pair.
- **Notable non-obvious dependencies:**
  - **Satisfies `GlobalBackground::draw_bg_layer`'s implicit template contract** — the four leading fields and the `draw` signature exist to match that template, not because this type needs them. There is no base class enforcing it.
  - **Documented as reading live `StarSystem` data with no copy**, so it is always synchronised with system mode — meaning the layer holds no snapshot and any mid-frame mutation of the galaxy is visible to it.
  - Holds two raw back-pointers (`game_state*`, `StarFxSystem*`) supplied at construction, making it a long-lived observer of both.
  - `parallax` is fixed at 1.0 in the constructor with the reasoning in the `.cpp`: the star is a real object the fleet operates inside, so it must track the camera 1:1 and match how system mode draws it.
  - The `blur` parameter is declared with a default and ignored by the implementation.
- **Interface vs internal:** Internal interface — one of three layer types known only to `GlobalBackground`.

---

## sandbox/source/render/nebula_layer.cpp

- **Path:** `sandbox/source/render/nebula_layer.cpp`
- **Purpose:** Packs the editor-tunable nebula parameters into a `bs_nebula_params` block and issues the single fullscreen procedural draw.
- **Key types/functions:** `NebulaLayer::NebulaLayer`, `NebulaLayer::draw`.
- **Notable non-obvious dependencies:**
  - **Almost the entire body is a field-by-field copy from `gs->render.*` into `bs_nebula_params`**, each with a hardcoded fallback for a null `game_state`. The defaults are duplicated here and in `game_init`, and the two sets do not fully agree — `swirl_strength` defaults to 1.0 here but 0.8 in `game_init`, and `falloff_radius` to 2.0 here but 0.7 there.
  - **Passes the camera centre as a `HierPos2` split** (`cam_cell` / `cam_local` from `game_camera_center_hierpos`) so the shader can reduce sample coordinates without `f32` snapping far from the origin — the same precision technique the starfield uses.
  - **Marked `is_custom_gpu = TRUE`**, which tells `GlobalBackground` to skip `renderer_set_camera` for this layer because the parameters travel through the uniform block instead.
  - The comment records a removed gate: the old `*= view_arena_w` kill-switch was dropped because LOD-driven base frequency keeps the clouds resolved at every scale, so the nebula is now continuous across arena and map.
  - Ignores `dt`, `elapsed_time`, and `blur` entirely — the effect is time-independent.
  - The layer's `seed` is a construction-time constant, so nebula appearance is unrelated to the player's chosen galaxy seed.
- **Interface vs internal:** Internal render layer owned by `GlobalBackground`.

---

## sandbox/source/render/nebula_layer.h

- **Path:** `sandbox/source/render/nebula_layer.h`
- **Purpose:** Declares the `NebulaLayer` background-layer type.
- **Key types/functions:** `struct NebulaLayer` — the four duck-typed layer fields, `seed`, a defaulted `gs` back-pointer, `set_game_state`, and `draw`.
- **Notable non-obvious dependencies:**
  - **Uses a two-phase setup** — constructor takes id/parallax/zoom/seed, then `set_game_state` injects the back-pointer — unlike `MappedSystemLayer`, which takes it in the constructor. The two sibling layers differ in wiring style for no stated reason.
  - Its `gs` member is documented as being "for editor panel tunables", making the dependency on the god struct purely a settings channel.
  - The header comment says the nebula "sits between the far and mid starfields", but those two layers were since collapsed into one — so the stated position no longer describes the actual stack.
  - Forward-declares `Camera2D` and `game_state`, keeping the header free of both real definitions.
- **Interface vs internal:** Internal interface — known only to `GlobalBackground`.

---

## sandbox/source/render/out_sensor_detection_fx.cpp

- **Path:** `sandbox/source/render/out_sensor_detection_fx.cpp`
- **Purpose:** Draws a radar-style contact signature — ring, expanding ping, rotating sweep wedge, crosshair, and blip — in place of the hull sprite for an enemy outside sensor range.
- **Key types/functions:** `OutSensorDetectionFX::init`, `::update(dt)`, `::render(...)`; file-local `sensor_visibility_from_dist`, `draw_glow_line`, `draw_glow_circle`, `reset_overlay_glow`, and `LAYER_FX` (15).
- **Notable non-obvious dependencies:**
  - **Builds its own glowing-line primitive rather than using the engine's debug layer** — `draw_glow_line` hand-constructs a `bs_sprite` (rotated quad, additive, white texture) and sets `custom.w = 1` to enable overlay glow in the fragment shader. The choice of `custom.w` over `custom.x` is deliberate and commented: `custom.x` drives heat/distortion, which would dirty the overlay. That channel meaning exists only in HLSL.
  - **Attaches `&overlay_glow` as each sprite's `glow_override`**, so every one of these draws shares one pointer — which is exactly what keeps the backend from breaking the batch between them, since runs split on glow-pointer identity.
  - **Duplicates `sensor_visibility_from_dist`**, which also exists in `sim/galaxy_map.cpp` as a shared function; this file defines a private copy with the same cubic falloff.
  - **Reads `enemy->render_pos`, a transient field written by the ship render pass**, so this effect depends on having been called after that pass in the same frame — an ordering contract carried only in a trailing comment.
  - **Emits roughly 110 sprites per frame** when active (two 48-segment glow circles, a 48-segment ping, 11 wedge lines, 2 crosshair lines, 1 blip), all through the batch, and uses five consecutive layers (`LAYER_FX` through `+4`) hardcoded as a private constant rather than drawn from `render_layers.h`.
  - **`reset_overlay_glow` zeroes almost every glow field** to neutralise the bullet-oriented defaults — the struct is being reused for a purpose its fields were not named for.
  - Screen-constant sizing is done twice over: an outer `screen_scale = clamp(1/zoom, 1, 8)` multiplier plus `draw_glow_line`'s own `thickness / zoom`.
  - `elapsed_time` is accepted and ignored; animation is driven by internally accumulated `sweep_angle` and `ping_phase`.
- **Interface vs internal:** Implementation of a type embedded in `game_state`. It is a self-contained visual effect with no consumers of its output.

---

## sandbox/source/render/out_sensor_detection_fx.h

- **Path:** `sandbox/source/render/out_sensor_detection_fx.h`
- **Purpose:** Declares the out-of-sensor-range contact effect struct with its editor-tunable appearance fields.
- **Key types/functions:** `struct OutSensorDetectionFX` — `color`, `intensity`, `radius_scale`, `overlay_glow`, `sweep_speed`, `sweep_angle`, `ping_phase`; `init`, `update`, `render`.
- **Notable non-obvious dependencies:**
  - **Includes `sim/ship.h` for the `Ship*` parameters** rather than forward-declaring, making a render header depend on a 558-line simulation header.
  - **Default member initialisers disagree with `init()`** — the header defaults `color` to `{1.0, 0.25, 0.25, 1}` while `init()` sets `{1.0, 0.45, 0.45, 1}`, so the value depends on whether `init` was called.
  - **Carries both animation state and appearance settings in the same struct**, so what is editor-tunable and what is per-frame simulation state is not distinguished.
  - `render` takes `elapsed_time` although the implementation ignores it, and takes `sensor_range` and `zoom` as explicit parameters rather than reading `game_state` — the one render module here that avoids the god struct entirely.
- **Interface vs internal:** Internal interface; the struct is a `game_state` member, so its shape is visible tree-wide.

---

## sandbox/source/render/parallax_background.cpp

- **Path:** `sandbox/source/render/parallax_background.cpp`
- **Purpose:** The background render pass — updates the star's render-space position for the dazzle effect, draws the layer stack with a parallax-appropriate camera, and restores the render camera.
- **Key types/functions:** `draw_parallax_background(game_state*, f32 dt)`; file-local `bg_cam_for_parallax(const game_state*)`.
- **Notable non-obvious dependencies:**
  - **Exists mainly to reconcile two camera conventions.** Background layers scroll by `cam.position * parallax`, but under floating origin `camera.position` is only a render-space residual — so `bg_cam_for_parallax` substitutes the absolute `game_camera_center`. Without it the background would sit still while the ship moved.
  - **Writes `s->star_pos`, which two later passes read** (frame lighting and the ship pass), making this a producer in the pass chain rather than a pure draw. The scene renderer's comment names that dependency as the reason for the pass order.
  - **Must restore `renderer_set_camera(s->camera_state.camera)` afterwards** because `GlobalBackground::draw` restores only the camera it was handed — the effective-centre one, not the render-space one. Two layers of camera restoration, each correct only in combination.
  - Reparallaxes the star through `celestial_center_render` with `depth_star`, matching what the mapped-system layer and the lighting pass do, so all three agree on where the star appears.
  - Wraps the work in the `PROF_BACKGROUND` profiler zone with explicit begin/end.
  - Tagged "extracted verbatim from `game_render`".
- **Interface vs internal:** Internal render pass with one exported function.

---

## sandbox/source/render/parallax_background.h

- **Path:** `sandbox/source/render/parallax_background.h`
- **Purpose:** Declares the parallax background pass.
- **Key types/functions:** `draw_parallax_background(game_state*, f32)`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Documents the pass's side effect** — updating the current star's render-space position for the dazzle effect — which is the reason later passes depend on this one running first.
  - Notes the background now spans "the full zoom range", recording the consolidation of the old per-mode backgrounds into one virtual-quadtree LOD field.
  - Says nothing about the camera save/restore dance the implementation performs, which is its most fragile behaviour.
- **Interface vs internal:** Internal interface — one entry in the scene renderer's pass order.

---

## sandbox/source/render/scene_renderer.cpp

- **Path:** `sandbox/source/render/scene_renderer.cpp`
- **Purpose:** Orchestrates the frame's world-drawing passes in a fixed order.
- **Key types/functions:** `render_scene(game_state*, f32 dt)` — 32 lines calling eight passes.
- **Notable non-obvious dependencies:**
  - **The file is pure sequencing — its entire content is the pass order**: set camera → galaxy-map look → parallax background → frame lighting → system asteroids → resources → decorations → stations → ships → gameplay overlays.
  - **The order is a hard dependency, not a preference.** `draw_parallax_background` writes `s->star_pos`; `submit_frame_lighting` and `draw_ship_scene` read it. Reordering silently produces one-frame-stale lighting rather than an error.
  - **Deliberately includes each peer header explicitly** rather than leaning on the `game.h` cascade, with a comment saying so — following the rule `game_modules.h` states.
  - Opens the `PROF_STARS` zone around only the galaxy-map pass; the other passes manage their own zones or none.
  - It sets the camera once at the top, but two later passes (`parallax_background` and, transitively, `global_background`) change and restore it, so the camera is not stable across the sequence.
- **Interface vs internal:** Internal — one exported function called from `game_render`.

---

## sandbox/source/render/scene_renderer.h

- **Path:** `sandbox/source/render/scene_renderer.h`
- **Purpose:** Declares the scene orchestrator and records why it exists.
- **Key types/functions:** `render_scene(game_state*, f32)`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **States the design rationale plainly**: the pass order is a hard dependency (parallax sets `star_pos`; lighting and ships read it), so it is expressed in one place instead of being implied by call order inside `game_render`. That is the clearest statement of intent in the render tree.
  - **Draws the boundary of the module**: world passes here, UI panels and frame timing left in `game_render`.
  - The listed order in the comment omits the four `system_content_render` passes the implementation also calls, so the header's summary is already out of date with its own `.cpp`.
- **Interface vs internal:** Internal interface — the single entry point `game_render` uses for all world drawing.

---

## sandbox/source/render/sensor_overlay.cpp

- **Path:** `sandbox/source/render/sensor_overlay.cpp`
- **Purpose:** Draws all sensor visuals — the flagship's layer rings, world-space radar blips for detected hostile projectiles, and screen-edge chevrons pointing at off-screen contacts.
- **Key types/functions:**
  - `sensor_overlay_draw(game_state*)` — the entry point.
  - `f32 g_sensor_fade_distance` — editor-tunable global (4,000,000 world units).
  - `static draw_sensor_rings`, `draw_sensor_contacts`, `draw_sensor_hud_indicators`, `draw_screen_line`, `snapshot_contacts_to_last_sweep`.
  - Helpers `sweep_frac`, `sensor_contact_color`, `safe_zoom`, `far_fade`; constant `SENSOR_SWEEP_PERIOD` (0.5 s).
- **Notable non-obvious dependencies:**
  - **Uses a function-local `static SensorContact contacts[MAX_PROJECTILES]` buffer** — a hidden global sized by the projectile pool, reused every frame. It avoids per-frame allocation but makes the function non-reentrant and keeps the array resident for the process lifetime.
  - **Rewrites contact positions before drawing them.** `snapshot_contacts_to_last_sweep` back-projects each unidentified contact along its velocity to where it was at the last sweep tick, so blips step rather than slide. The comment explains this works without history because ballistic contacts move at constant velocity — an analytic reconstruction that would silently break for anything accelerating (e.g. the guided missiles the game also fires).
  - **Contains a fix documented at length**: the blink phase must not be scaled by the distance-varying confidence, because dividing an ever-growing `time` by a shrinking period made the phase sweep many cycles per second and read as a strobe. A fixed period is the fix.
  - **Layer-1-and-closer contacts are exempted from the snapshot** (`confidence >= 0.999f`), so identified contacts track live while distant ones step — two visual behaviours from one code path, keyed on a float comparison against a magic threshold.
  - **`draw_screen_line` inverts the camera to draw in screen space** — it projects screen-pixel endpoints back through `camera2d_screen_to_world` because the renderer's primitives live in render space. The edge chevrons are therefore screen-space UI drawn through a world-space API.
  - **Detection is delegated entirely to `sim/sensor_system`** (`sensor_gather_hostile_contacts`), so this module is presentation-only, as the header states.
  - Exposes a mutable tuning global rather than a `game_state` field, unlike most editor-tunable values.
  - Comments correct two engine-convention traps: `renderer_draw_circle` thickness is already in screen pixels, while blip radii must be divided by zoom manually.
- **Interface vs internal:** Internal render pass with one exported function plus one exported tuning global, called from `draw_gameplay_overlays`.

---

## sandbox/source/render/sensor_overlay.h

- **Path:** `sandbox/source/render/sensor_overlay.h`
- **Purpose:** Declares the sensor overlay entry point and its editor-tunable fade distance.
- **Key types/functions:** `sensor_overlay_draw(game_state*)`; `extern f32 g_sensor_fade_distance`.
- **Notable non-obvious dependencies:**
  - **Enumerates the three visualisations and their gating** — rings gated on `s->show_sensor_layers`, contacts always on, edge indicators for off-screen contacts — which is the only place that policy is written down.
  - **States the module boundary explicitly**: "Detection is delegated to `sim/sensor_system` (fleet-wide union). This module only draws."
  - **Documents that Layer 2 is unbounded**, so contacts never drop out of detection and the fade distance controls only how quickly distant returns dim — a gameplay rule expressed in a render header.
  - Exposes a mutable global as part of the interface, matching the pattern in `view_transform.h` and `debug_overlay.h`.
- **Interface vs internal:** Internal interface, called from one place.

---

## sandbox/source/render/ship_visual.cpp

- **Path:** `sandbox/source/render/ship_visual.cpp`
- **Purpose:** Parses `.svis` visual-layer definitions from disk and resolves their texture paths into live renderer handles.
- **Key types/functions:** `ship_visual_clear(ShipVisual*)`, `ship_visual_load(ShipVisual*, const char* path)`, `ship_visual_resolve_textures(ShipVisual*)`; file-local `rstrip_vis`.
- **Notable non-obvious dependencies:**
  - **Reads files directly with `fopen`/`fgets`**, not through any engine facility, and defines `_CRT_SECURE_NO_WARNINGS` at the top to silence the resulting MSVC warnings — the only sandbox file that does so.
  - **A two-phase load is mandatory**: parsing records only path strings, and `ship_visual_resolve_textures` must run later, after the renderer is live. `game_init` calls it separately for every fleet ship and the enemy. Skipping phase two leaves every `texture.id` at 0, which the engine silently renders as the 1×1 white texture rather than failing.
  - **The file format is parsed with `sscanf` line matching** and is whitespace-tolerant with `#` comments; a malformed `layer` line is skipped silently without incrementing `layer_count`, so a typo drops art with no diagnostic.
  - **Silently truncates** at `VIS_MAX_LAYERS` (8) via `continue`, and truncates any path longer than 127 characters through `strncpy`.
  - **Texture load failures warn but do not fail** — `ship_visual_resolve_textures` logs and continues, so a missing normal or depth map yields an invalid handle the backend replaces with white, changing the lighting rather than erroring.
  - The `mapped` layer kind requires four texture paths on one line (diffuse, normal, depth, position), tying the format directly to the engine's four-sampler mapped-sprite pipeline.
  - Includes `sim/ship.h` although it only manipulates `ShipVisual`.
- **Interface vs internal:** Public interface within the sandbox — an asset-loading utility called from ship loading and from `game_init`.

---

## sandbox/source/render/ship_visual.h

- **Path:** `sandbox/source/render/ship_visual.h`
- **Purpose:** Defines the ship appearance model — an ordered list of textured layers in ship-local space — and the three functions that manage it.
- **Key types/functions:** `VIS_MAX_LAYERS` (8); `enum VisualLayerKind` (`SPRITE`, `MAPPED`); `struct VisualLayer` (four path strings, four texture handles, `offset_local`, `px_per_unit`, `z`, `roof_only`); `struct ShipVisual` (layer array, count, `size_local`, `has_sprite`); `ship_visual_clear` / `_load` / `_resolve_textures`.
- **Notable non-obvious dependencies:**
  - **States the decoupling as its purpose**: "what the player SEES — independent of nav/systems", so a ship's appearance is deliberately separable from its simulation model.
  - **Each `VisualLayer` carries 512 bytes of path strings** alongside its handles, and `ShipVisual` embeds eight of them — roughly 4.5 KB per ship, retained after load even though the paths are only needed during resolution. With two `Ship` members at ~3 MB each in `game_state`, this contributes to the size that forced placement-new in `game_init`.
  - **`roof_only` encodes a rendering mode in the data model** — layers drawn only as the global-mode roof silhouette — coupling the format to a specific view.
  - **Documents the two-phase contract** (resolve after renderer init, safe to call once at game init) in the declaration comment; nothing enforces it.
  - The header mentions authoring "directly in a .ship file or via a standalone .svis file", so the same layer syntax is embedded in two file formats parsed by two different modules.
  - `z` orders layers within a ship, a third layering concept alongside the engine's sprite `layer` and the sandbox's `render_layers.h` constants.
- **Interface vs internal:** Public interface within the sandbox — the shared ship-appearance type, consumed by ship loading and the ship render passes.

---

## sandbox/source/render/ship_render.cpp

- **Path:** `sandbox/source/render/ship_render.cpp`
- **Purpose:** Draws ship annotations and procedural hardware — clustered fleet emblems, collider and hardpoint debug outlines, drag-and-drop slot feedback, and flat-shaded turret and radar-dish mount art.
- **Key types/functions:**
  - `draw_fleet_emblems(game_state*)` — union-find clustering plus per-cluster emblem.
  - `draw_collider_outline`, `draw_hardpoint_overlay`, `draw_hardpoint_highlight`, `draw_hardpoint_drag_feedback`, `draw_ship_mounts`.
  - Statics: `EMBLEM_NO_GLOW`, `draw_ring_arc`, `draw_one_ship_emblem`, `emblem_find` / `emblem_union`, `hardpoint_color`, `draw_solid_rect`, `draw_turret`, `draw_radar_dish`, and `LAYER_MOUNT_ART` (`LAYER_SHIP + 1`).
- **Notable non-obvious dependencies:**
  - **Implements union-find (with path halving) purely for visual clustering** — same-type fleet ships whose screen-space rings overlap fuse into one emblem at the centroid with averaged velocity and a count badge. The clustering is O(n²) over `FLEET_MAX_SHIPS` per frame and is recomputed from scratch every frame.
  - **Two `bs_sprite.custom` channels are used as shader flags with meanings defined only in HLSL** — `custom.x = 1` on emblems and `custom.z = 1` on mount art, the latter commented as marking the sprite self-emissive so galaxy-map star light cannot wash dark steel to white ("same fix as the hull").
  - **`EMBLEM_NO_GLOW` is a file-static glow struct attached as `glow_override` to every emblem**, which both disables glow and keeps all emblems in one draw run, since the backend breaks runs on glow-pointer identity.
  - **Reads `ship->render_pos`, a transient field the ship render pass writes**, in five of its six exported functions — so all of them depend on having run after that pass, a contract stated nowhere in the header.
  - **Fades by map weight and brackets its work in `renderer_set_draw_alpha(map_w)` / `(1.0f)`**, returning early when `map_w <= 0`, so emblems are a galaxy-map affordance that never appears in the deep arena view.
  - **Mixes two sizing conventions deliberately and says so**: emblem geometry converts screen pixels to world units by dividing by zoom, while `renderer_draw_line` thickness is passed as a plain screen-pixel constant. The mount art comment notes the opposite choice — all extents in world units so turrets scale with the hull.
  - **Turret aim comes from `ship->mount_aim[]`, live simulation state**, so the art is driven by the weapon system rather than being decorative; the radar dish spins on wall-clock `time` instead.
  - **Only two ship types have emblems** (`SHIP_TYPE_DRONE`, `SHIP_TYPE_EXTRACTOR`); a cluster of any other type is silently skipped after the clustering work is already done.
  - `draw_one_ship_emblem` takes `s` only to read camera rotation and project the badge, and `draw_ring_arc` takes `s` without using it at all.
  - Mount art exists because no turret PNGs do — the comment states the rectangles are a stand-in for missing assets.
- **Interface vs internal:** Public interface within the sandbox — six exported draw helpers consumed by the ship scene pass, the gameplay overlays, and the editor.

---

## sandbox/source/render/ship_render.h

- **Path:** `sandbox/source/render/ship_render.h`
- **Purpose:** Declares the six ship annotation and mount-art draw helpers.
- **Key types/functions:** `draw_fleet_emblems`, `draw_collider_outline`, `draw_hardpoint_overlay`, `draw_hardpoint_highlight`, `draw_hardpoint_drag_feedback`, `draw_ship_mounts`; forward-declares `game_state`, `Ship`, `bs_color`.
- **Notable non-obvious dependencies:**
  - **Documents the target layer for every function** (`LAYER_DEBUG`, `LAYER_GIZMO`, `LAYER_SHIP + 1`), which is how a reader learns each one's bloom behaviour and draw order without opening the `.cpp`.
  - **Forward-declares `bs_color`, which is a `typedef struct`** — legal here but it means the header cannot be used to construct one.
  - **Describes the drag-feedback semantics as a gameplay rule** — green when the dragged item fits, dim red when the slot rejects it for wrong kind or insufficient size — encoding loadout validation in a render header.
  - Explains that emblems fade in by map weight "as the view crosses from arena to galaxy map", the same blend convention the other passes follow.
  - Nothing here states the shared precondition that `ship->render_pos` must already be set for the frame.
- **Interface vs internal:** Public interface within the sandbox, with an unusually wide set of consumers for a render header.

---

## sandbox/source/render/ship_scene.cpp

- **Path:** `sandbox/source/render/ship_scene.cpp`
- **Purpose:** The ship rendering pass — computes every entity's render-space position, derives per-ship star lighting, and draws hulls, mounts, exhaust, debug overlays, the sensor-gated enemy, NPC ships, combat quads, and fleet emblems.
- **Key types/functions:**
  - `draw_ship_scene(game_state*)` — the exported pass.
  - Statics: `arsenal_drag_fits`, `draw_weapon_group_digits`, `draw_ship_visual_ex`, `draw_ship_visual`, `draw_enemy_ship_sensor`, `draw_engine_exhaust`, plus the `EXHAUST_*` tuning constants and `COLLIDER_COLOR`.
- **Notable non-obvious dependencies:**
  - **This pass writes the `render_pos` field that five other modules read.** It populates `render_pos` for every fleet ship, the enemy, every NPC, and every non-ship combat entity — so `ship_render.cpp`, `out_sensor_detection_fx.cpp`, and the overlays all depend on running after it. The comment stresses the persistent `HierPos2` state is never mutated by rendering; `render_pos` is the transient shadow.
  - **It conditionally recomputes `s->star_pos`** when `view_arena_w <= 0`, covering the deep galaxy-map side where the parallax pass is skipped — so two different passes write the same field depending on zoom, keeping it defined across the range without a mode branch.
  - **`arsenal_drag_fits` is an explicitly acknowledged mirror of `arsenal_drop_on_slot` in `game.cpp`.** A render module duplicates the loadout validation rules so the world-space green/red feedback matches what a drop will actually do; the two switch statements over six drag kinds must be kept in step by hand.
  - **`bs_sprite.custom` is used as a shader flag field in three different ways** in this one file, each documented against `sprite.frag.hlsl`: hull art zeroes `custom.x`/`custom.w` so ship-glow parameters cannot warp the hull, sets `custom.z = 1` to mark it self-emissive against map-look star light, and exhaust sets `custom.x = speed_ratio * glow_mul` to drive heat distortion and the temperature gradient.
  - **Per-ship light direction is recomputed for every hull** from that ship toward `s->star_pos`, with a fleet-wide fallback — correct but redundant for a distant star, as the comment concedes.
  - **Exhaust length is expressed as a fraction of hull length** so a drone and a cruiser both read correctly, with the fractions calibrated against a legacy 16+48-unit jet on a ~41-unit hull. Turbulence comes from two out-of-phase sines at 30 Hz and 47.3 Hz.
  - **Iterates `NPC_SHIP_MAX` unconditionally** and sets `render_pos` even for undiscovered NPCs before skipping their draw — so the marker pass in `gameplay_overlays` can still position them.
  - **The flagship inspector gates a whole sub-pass**: hardpoint skeleton, weapon-group digits, per-slot drag feedback, and a cursor tether line drawn from the source hardpoint to `mouse_true_hierpos` — a render pass reading live mouse position.
  - Mapped visual layers are skipped entirely unless all four textures resolved, so one missing map silently drops the layer.
  - Enemy visibility uses `s->ship_sensor_range` with `sensor_visibility_from_dist`, a different range field from the one `sensor_overlay` and `galaxy_map` use.
- **Interface vs internal:** Internal render pass with one exported function, but its `render_pos` side effect makes it a de-facto producer for several later passes.

---

## sandbox/source/render/ship_scene.h

- **Path:** `sandbox/source/render/ship_scene.h`
- **Purpose:** Declares the ship rendering pass.
- **Key types/functions:** `draw_ship_scene(game_state*)`; forward-declares `game_state`. Nine lines, no includes.
- **Notable non-obvious dependencies:**
  - **Lists the pass's responsibilities in one sentence** — render-space positions, debug grid, per-ship lighting, sprites, exhaust, colliders, sensor-gated enemy, combat quads, emblems — which is the only inventory of what is a single function's job here.
  - **States the ordering precondition**: must run after the star position and lighting are established. Combined with `scene_renderer.h`'s note, the two headers together document the whole chain.
  - Does not mention that the pass *writes* `render_pos`, which is the dependency later passes actually rely on.
  - Tagged "extracted verbatim from `game_render`", like its sibling passes.
- **Interface vs internal:** Internal interface — one entry in the scene renderer's pass order.

---

## sandbox/source/render/starfield_generator.cpp

- **Path:** `sandbox/source/render/starfield_generator.cpp`
- **Purpose:** CPU-side starfield generation — a random-walk star distribution, a tile index for GPU culling, and interleaved vertex packing.
- **Key types/functions:** `StarfieldGenerator::generate`, `::build_tile_index`, `::pack_vertices`; a local LCG lambda and a precomputed annulus offset table.
- **Notable non-obvious dependencies:**
  - **Dead code.** No file calls any of its three functions. `global_background.cpp` includes the header and never uses it; the live starfield is the procedural shader path. Together with `engine/source/renderer/starfield_gpu_resources.*` — which consumed exactly this data — it forms the abandoned VBO starfield pipeline, one half in each tree.
  - **Its 7-float vertex format is the one `StarfieldGpuResources::init` describes** (offset_xy, size, corner, r, g, b), so the two dead files are the matched producer and consumer of a format nothing else uses.
  - **Uses `std::vector` throughout**, including a `~72000`-entry offset table built on every `generate` call, and allocates through the default allocator rather than the engine's.
  - **The LCG is written inline for reproducibility** (`s * 1103515245 + 12345`), a third independent RNG in the project alongside the galaxy generator's and the engine's.
  - **Tile indexing assumes `fieldSize` is a power of two** — `widthMod = fieldSize - 1` is used as a wrap mask by the consumer — and assumes a square field, since `tileCols` is reused for both axes.
  - Brightness follows a `pow(u, 2.5)` power law with size and colour temperature derived from it, and the comments attribute the whole approach to Endless Sky.
- **Interface vs internal:** Nominally a public static-method utility; in practice unreachable.

---

## sandbox/source/render/starfield_generator.h

- **Path:** `sandbox/source/render/starfield_generator.h`
- **Purpose:** Declares the `Star` record and the `StarfieldGenerator` static-method class.
- **Key types/functions:** `struct Star` (x, y, size, brightness, r, g, b); `class StarfieldGenerator` with `FIELD_SIZE` (4096), `TILE_SIZE` (256), `STARS_PER_LAYER` (3000), and the three static methods.
- **Notable non-obvious dependencies:**
  - **A `class` with only static methods** — one of two C++-class-shaped types in the sandbox, alongside `RtsControls`, in a codebase otherwise built from free functions and PODs.
  - **`FIELD_SIZE` 4096 and `TILE_SIZE` 256 are the constants the dead engine-side consumer relies on** for its power-of-two wrap mask and 256-unit tile stride; neither side validates the relationship.
  - The `Star` comment claims sizes of 1.25–2.5 world units while the implementation computes `2.0 + brightness * 4.0`, i.e. 2.0–6.0 — the documentation predates the current formula.
  - Includes `<vector>` in a header, unlike the sandbox's usual fixed-array style.
- **Interface vs internal:** Public-looking interface with no consumers.

---

## sandbox/source/render/starfield_layer.cpp

- **Path:** `sandbox/source/render/starfield_layer.cpp`
- **Purpose:** Packs the virtual-quadtree starfield parameters and issues the single procedural fullscreen draw.
- **Key types/functions:** `StarfieldLayer::StarfieldLayer`, `StarfieldLayer::draw`.
- **Notable non-obvious dependencies:**
  - **Explicitly sets `params.layer_data = nullptr`** and comments it "procedural path" — the one line that makes the entire VBO starfield pipeline (generator plus GPU resources) unreachable.
  - **Passes the camera centre as a `HierPos2` split** so the shader can reduce sample coordinates per LOD level without `f32` snapping far from the origin — the same technique the nebula layer uses.
  - **`base_cell` is hardcoded to 64.0 with a stated invariant**: `base_cell * 256 == BS_HIERPOS_CELL_SIZE` (16384). The starfield's finest LOD is therefore locked to the engine's hierarchical cell size by an arithmetic relationship no assertion checks.
  - **Every other parameter has a `gs ? … : default` fallback whose defaults disagree with `game_init`** — `density` 0.06 here versus 0.5 there, `target_px` 40 versus 12, `lod_levels` 4 versus 6, `parallax_near` 0.5 versus 0.009. The fallbacks are dead in practice but would produce a visibly different field.
  - **Feeds `gs->star_pos` as `star_rel` for the dazzle effect**, with a comment noting it is already in the shader's per-pixel offset frame — so this layer depends on whichever earlier pass set that field this frame.
  - Records the removal of a `brightness_mul *= view_arena_w` kill-gate, now replaced by LOD weighting so the field is continuous across arena and map.
  - Ignores `dt`, `elapsed_time`, and `blur`.
- **Interface vs internal:** Internal render layer owned by `GlobalBackground`.

---

## sandbox/source/render/starfield_layer.h

- **Path:** `sandbox/source/render/starfield_layer.h`
- **Purpose:** Declares the procedural starfield layer type.
- **Key types/functions:** `struct StarfieldLayer` — the four duck-typed layer fields, `seed`, `gs`, `set_game_state`, `draw`.
- **Notable non-obvious dependencies:**
  - **Structurally identical to `NebulaLayer`** — same fields, same two-phase `set_game_state` wiring, same defaulted `blur` parameter — but the two share no base class or common header; the similarity is convention only.
  - **Advertises what it no longer does**: "No CPU star generation, no VBO upload, no tile culling" — a header comment that doubles as the epitaph for `starfield_generator` and the engine's `starfield_gpu_resources`.
  - Marked `is_custom_gpu` in the constructor, which tells `GlobalBackground` to skip `renderer_set_camera` for it.
- **Interface vs internal:** Internal interface — known only to `GlobalBackground`.

---

## sandbox/source/render/text.cpp

- **Path:** `sandbox/source/render/text.cpp`
- **Purpose:** Bakes the embedded 8×8 font into a GPU atlas and draws screen-pinned bitmap strings as one textured quad per glyph.
- **Key types/functions:** `text_init`, `text_shutdown`, `text_width`, `text_height`, `text_draw`; file-local atlas geometry constants and `longest_line_cells`.
- **Notable non-obvious dependencies:**
  - **One file-static module state struct** (`g_text`) holding the atlas handle and a ready flag; `text_draw` silently no-ops until `text_init` succeeds, so a failed init degrades to invisible text rather than a crash.
  - **The atlas is baked into a function-local `static u8 pixels[]` (~38 KB)**, deliberately not on the stack, and retained for the process lifetime even though it is only needed once.
  - **Glyphs are padded into 10×10 cells with a 1 px transparent guard band**, and the comment explains exactly why: the backend's sampler is NEAREST + CLAMP_TO_EDGE, so at non-integer scales sampling can land outside the 8×8 footprint and would otherwise pick up the neighbouring glyph. A rendering detail from the engine's sampler configuration dictates the atlas layout.
  - **RGB is white everywhere and only alpha carries the glyph**, so tint controls colour and there is no dark fringe under interpolation.
  - **`text_draw` implements a camera-cancel trick**: it projects the requested screen pixel into world space, sets `sprite.rotation = cam->rotation` to null the view rotation, and sizes the quad as `screen_px / zoom`. The live view-projection then maps the quad back to an axis-aligned pixel-sized rectangle. The header calls this the same trick "the upcoming UI panels use, proven here on plain text first".
  - **`text_shutdown` has no callers**, so the atlas texture is never released — harmless at process exit, but the lifecycle the header describes is only half-wired.
  - Emits one sprite per non-blank glyph into the shared batch, so a long HUD string competes with game sprites for the 16384 budget.
  - `text_width` returns the longest line rather than a single-line width, which is correct for layout but surprising for a function of that name.
- **Interface vs internal:** Public interface within the sandbox — the HUD/debug text primitive used by overlays, emblems, and the cell grid.

---

## sandbox/source/render/text.h

- **Path:** `sandbox/source/render/text.h`
- **Purpose:** Declares the bitmap-text API and documents its coordinate model and lifecycle.
- **Key types/functions:** `text_init`, `text_shutdown`, `text_width`, `text_height`, `text_draw`.
- **Notable non-obvious dependencies:**
  - **Defines the screen-space contract precisely** — origin top-left, +y down, pixels — which is the opposite of the world-space y-up convention every other draw call in the sandbox uses. This is the one API where the two conventions meet.
  - **States the lifecycle ordering**: `text_init()` after `renderer_initialize()`, `text_shutdown()` at teardown. The second half never happens.
  - **Explains that `cam` and the framebuffer size must be the LIVE values for this frame**, because the function inverts them — passing a stale camera silently misplaces text rather than failing.
  - Documents the monospace advance (8 × scale) and that unencoded bytes advance a blank cell, so callers can lay out without measuring.
  - Describes the technique as a precursor for UI panels, marking it as intentionally general rather than a one-off.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/render/star_fx.cpp

- **Path:** `sandbox/source/render/star_fx.cpp`
- **Purpose:** Implements the star and planet visual system — procedural falloff textures, the classic-sunburst and 3D-sphere star paths, planet impostor spheres, the anamorphic streak state, and the Planet Editor UI with disk persistence.
- **Key types/functions:** `StarFxSystem::init` / `shutdown` / `draw_star` / `draw_star_classic` / `draw_star_3d` / `draw_planet_3d` / `apply_streak_state` / `build_ui` / `build_planet_editor` / `planet_params_reset_defaults` / `planet_params_save` / `planet_params_load`; statics `make_radial_texture`, `falloff_core`, `falloff_corona`, `falloff_halo`.
- **Notable non-obvious dependencies:**
  - **Reads and writes a config file at runtime** — `planet_params_load()` runs during `init` and `planet_params_save()` during `shutdown`, persisting per-planet-type appearance to `bin/planet_editor.cfg`. This is the only sandbox module with editor state that survives a restart, and `#define _CRT_SECURE_NO_WARNINGS` at the top exists specifically for that file IO.
  - **Bakes three 256×256 radial textures into a shared function-local `static u8 pixels[256*256*4]` buffer** (256 KB) explicitly to avoid a stack overflow, then uploads each — the same pattern `text.cpp` uses.
  - **`apply_streak_state` mutates global renderer state and the last caller wins**, which the header documents as a real constraint: the streak is a single global post-process, so it must be called last for whichever star should own it. Ordering between star draws therefore has a visible consequence.
  - **Both star paths clamp aggressively for GPU safety** — the classic path caps the glow quad at 250 px with the comment "prevent GPU timeout from huge quad", and the streak length is capped at 50.
  - **Contains both an editor UI and a renderer**, so a render module directly builds ImGui panels through `bs_ui_*` — the same blending of concerns as `profiler.cpp`.
  - `draw_star` is a dispatcher on `star_3d_mode`, and the two paths take different parameters and produce different geometry, so the toggle changes far more than appearance.
  - Streak intensity and length carry separate `_mul` fields described as "gameplay-derived", so simulation code can scale a purely visual effect.
- **Interface vs internal:** Implementation of a type embedded in `game_state`. Public within the sandbox by virtue of being a state member; its draw methods are called from two background layers and the galaxy-map renderer.

---

## sandbox/source/render/star_fx.h

- **Path:** `sandbox/source/render/star_fx.h`
- **Purpose:** Declares the per-planet-type appearance parameters, the `StarFxSystem` struct, and the inline star-light factory.
- **Key types/functions:** `struct PlanetTypeParams` (size_mul, min_px, rotation_speed, halo_scale, cloud_amount, surface_color); `struct StarFxSystem` (~30 tunables, three owned textures, twelve methods); `static inline make_star_light(...)`.
- **Notable non-obvious dependencies:**
  - **`make_star_light` is a `static inline` in a header**, so every includer gets its own copy — and it hardcodes the star light's physical model: radius is `max_orbit * 4` and intensity is `5.0 * vis`. Those magic factors define how bright a system is and live in a header rather than a tuning struct.
  - **`PLANET_EDITOR_TYPE_COUNT` is 8 and must match `PLANET_TYPE_COUNT`**, enforced by a `static_assert` in the `.cpp` — a cross-header invariant with an actual compile-time check, which is rare here.
  - **The struct mixes four unrelated concerns**: streak post-process tuning, star surface shader tuning, per-type planet appearance, and editor window state (`show_planet_editor`, `planet_editor_sel_type`). All are persisted or edited together.
  - **`apply_streak_state`'s comment states the global-state hazard** — single global post-process, last caller wins — making an ordering dependency explicit at the declaration.
  - `planet_params_save` / `_load` are declared here, naming `bin/planet_editor.cfg` as the persistence target.
  - `draw_star`, `draw_star_classic`, `draw_star_3d`, and `draw_planet_3d` are all `const` methods that submit draws, so the object is logically immutable during rendering despite driving all of it.
- **Interface vs internal:** Public interface within the sandbox — the struct is a `game_state` member and its methods are called from three render modules.

---

## sandbox/source/render/system_content_render.cpp

- **Path:** `sandbox/source/render/system_content_render.cpp`
- **Purpose:** Draws per-system ambient content — asteroids, resource nodes, dust decorations, and civilian stations — with sensor-gated visibility and aggressive level-of-detail.
- **Key types/functions:** `draw_system_stations`, `draw_system_asteroids`, `draw_system_resources`, `draw_system_decorations`; statics `system_sensor_vis`, `on_screen`, `draw_asteroid_poly`, `system_belt_screen_px`, `adaptive_segments`, `draw_station_unknown_marker`; constants `LOD_DOT_PX` (3.0) and `SYSTEM_DETAIL_MIN_SCREEN_PX` (48.0).
- **Notable non-obvious dependencies:**
  - **Its LOD scheme exists explicitly to respect the engine's 16384-sprite batch cap**, and the comments say so twice with reasoning: per-system detail "numbers in the thousands per system", so drawing every materialised system's belt at galaxy zoom would blow the batch. Three mechanisms stack — whole-system skip below 48 px of belt, per-entity collapse to a single quad below 3 px, and `adaptive_segments` scaling ring tessellation with on-screen radius. This is the clearest case of an engine limit shaping game-side rendering.
  - **Duplicates helpers that exist elsewhere** — `on_screen` is a near-copy of `mapped_system_layer.cpp`'s `is_on_screen`, and `system_sensor_vis` mirrors that file's `get_sensor_visibility_global`, with the comment acknowledging the parallel.
  - **Station appearance encodes discovery state**: a pulsing antenna marker until scanned inside the flagship's Layer 1 radius, then a filled circle in the owner civ's colour — so a render module reads both the sensor model and the galaxy's ownership data.
  - **Asteroid silhouettes use per-vertex jitter generated with the object**, so shapes are deterministic and stable across frames without being stored per-frame.
  - Everything is drawn on `LAYER_CELESTIAL`, below the bloom threshold, so all of it participates in bloom.
  - All four entry points are gated on sensor visibility, meaning ambient scenery disappears outside sensor range rather than being merely dimmed.
- **Interface vs internal:** Internal render passes — four exported functions, all called from `render_scene`.

---

## sandbox/source/render/system_content_render.h

- **Path:** `sandbox/source/render/system_content_render.h`
- **Purpose:** Declares the four per-system ambient content passes.
- **Key types/functions:** `draw_system_stations`, `draw_system_asteroids`, `draw_system_resources`, `draw_system_decorations`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Each declaration documents its own LOD and gating behaviour** — "faded by sensor visibility and collapsed to a quad at wide zoom" appears on three of the four — so the batch-budget strategy is visible at the interface.
  - **Distinguishes scope per pass**: stations only in owned systems, asteroids and dust in every system, resources concentrated in belt and mid zones. That is the only statement of where each content type appears.
  - Names the station discovery rule (pulsing marker until scanned within the Layer 1 radius) as part of the interface contract.
  - All four take a non-const `game_state*` despite being read-only.
- **Interface vs internal:** Internal interface — four entries in the scene renderer's pass order.

---

## sandbox/source/render/voronoi_cell_hover_effect.cpp

- **Path:** `sandbox/source/render/voronoi_cell_hover_effect.cpp`
- **Purpose:** Tracks which Voronoi territory cell the cursor is over and draws its outline with a pulsing highlight.
- **Key types/functions:** `update_cell_hover_effect(...)`, `draw_cell_hover_effect(...)`; static `pick_voronoi_cell(...)`.
- **Notable non-obvious dependencies:**
  - **Picks the cell by nearest *site*, not by polygon containment** — a brute-force scan over all systems comparing `f64` distances. That is correct for a Voronoi diagram by definition, but it means the pick never touches the computed cell geometry and costs O(system_count) per frame.
  - **Mutates the `GalaxyVoronoi` it is handed** — `hovered_cell` and `hover_head_dist` are written by the update function, so a "hover effect" module owns two fields of the galaxy's territory structure.
  - **The pulse accumulator wraps at `100 * PI`** rather than `2 * PI`, despite the comment saying "wrap at 2π to avoid fp drift" — the code and its comment disagree, though both bound the drift.
  - **Round-trips cell vertices through `HierPos2`** (`hierpos_from_vec2` then `hierpos_diff`) to reach render space, converting stored `Vec2` verts into the hierarchical frame every frame for every edge.
  - Uses `VORONOI_LAYER_CELESTIAL`, a layer constant from `sim/voronoi_galaxy.h` rather than `core/render_layers.h` — a second, separate layer vocabulary.
  - The `zoom` parameter is accepted and explicitly discarded.
- **Interface vs internal:** Internal — an update/draw pair called from the galaxy-map path.

---

## sandbox/source/render/voronoi_cell_hover_effect.h

- **Path:** `sandbox/source/render/voronoi_cell_hover_effect.h`
- **Purpose:** Declares the Voronoi cell hover update and draw functions.
- **Key types/functions:** `update_cell_hover_effect`, `draw_cell_hover_effect`; forward declarations of `GalaxyVoronoi`, `StarSystem`, `game_state`, and the two `bs_math` types.
- **Notable non-obvious dependencies:**
  - **Documents that `zoom` is unused and "retained for the call signature"** — an explicitly preserved dead parameter.
  - **Splits its inputs oddly**: `update` takes the camera hierpos and system array explicitly, while `draw` takes the whole `game_state`. The two halves of one feature use different dependency styles.
  - **Forward-declares `bs_math::Vec2` and `HierPos2` as structs** to avoid including the math header, then takes them by reference and pointer.
  - The comment describes a "rotating neon trail" that the implementation does not draw — it renders a uniform pulsing outline, so the header describes a superseded effect.
- **Interface vs internal:** Internal interface.

---

## sandbox/source/render/galaxy_map_render.cpp

- **Path:** `sandbox/source/render/galaxy_map_render.cpp`
- **Purpose:** The galaxy-map render pass — Voronoi territory, travel lanes, star sunbursts, planets and orbit rings, map entities and range rings — plus the two hit-tests that answer "what is under the cursor" on the map. At 1734 lines it is the largest render file in the sandbox.
- **Key types/functions:**
  - `draw_galaxy_map_look(game_state*, f32 dt)` — the pass (lines 1033 onward), cross-faded by map weight.
  - `galaxy_pick_planet(...)` — planet hit-test returning cache slot and planet index.
  - `galaxy_map_hover_tooltip(...)` — builds the tooltip string and anchor position.
  - `static draw_galaxy_overview(game_state*)` — far-system dots and lanes (~550 lines).
  - `static draw_rotated_rect_outline(...)`.
  - The externally-visible star constants `STAR_MIN_SCREEN_RADIUS` (3.0), `STAR_DIST_SCALE_FACTOR` (0.0003), `STAR_MAX_DIST_SCALE` (4.0), `STAR_HERO_MAP_MIN_RADIUS` (42.0).
  - A local `SunburstCandidate` struct for prominence-ranked star selection.
- **Notable non-obvious dependencies:**
  - **Defines four `const f32` globals that `mapped_system_layer.cpp` also reads** via `extern` declarations in `game.h` — the numeric agreement that lets the arena and map star draws blend seamlessly. The constants live in the map renderer but govern both.
  - **The second-highest fan-out file in the sandbox (20 project includes)**, reaching into galaxy generation, history, missions, Voronoi, star system generation, celestial parallax, cursor, view transforms, input, memory, and three engine UI facades.
  - **`MAX_SUNBURST_STARS` is declared locally as 4 with a comment to "keep in sync with `BS_MAX_SUNBURST_STARS` in the backend"** — a duplicated capacity constant across the engine/game boundary with no shared definition, because the engine does not export it.
  - **Ranks candidate stars by a computed "prominence" and keeps only the top few**, because the backend silently drops queue overflow. The game implements the selection policy the engine's fixed queue would otherwise apply arbitrarily.
  - **Derives streak length and intensity multipliers from the current star's radius and luminance**, writing them into `star_fx.streak_length_mul` / `_intensity_mul` — a render pass feeding gameplay-derived values into another module's persistent state.
  - **`galaxy_pick_planet` deliberately mirrors the Pass-2 draw math** — same parallax anchor, same per-type size, same ≥6 px orbit LOD gate — so hit-testing matches what is visible. Any change to the draw path must be mirrored here, and the comment says so.
  - **`galaxy_map_hover_tooltip` computes but does not draw**, and is called from `game_push_hud` in the *update* path specifically so the RmlUi HUD consumes it the same frame with no cursor lag. A render-module function deliberately invoked outside the render pass.
  - **The returned cache slot is documented as valid only for the current frame**, with callers told to stash the system's `galaxy_center` to track a planet across frames — a lifetime rule for an index into a hot cache that is re-materialised as the camera moves.
  - **Brackets the whole pass in `renderer_set_draw_alpha(map_w)` and early-outs when `view_arena_w >= 1`**, making it the mirror of `mapped_system_layer`'s arena-weighted draw; the two together produce one continuous star.
  - Toggles `renderer_set_aux_bloom_mode` around the star pass, and gates on `star_fx.streak_enabled`.
  - Uses `bs_imgui_wants_mouse` and `bs_rml_wants_mouse` to suppress map picking while a UI panel owns the cursor.
  - A duplicated comment line at the top of `galaxy_pick_planet` shows a copy-paste artifact in the documentation.
- **Interface vs internal:** Mixed. `draw_galaxy_map_look` is an internal pass, but `galaxy_pick_planet` and `galaxy_map_hover_tooltip` are a public query interface consumed by `game.cpp`'s update path — this is the one render module the simulation side calls into for answers rather than drawing.

---

## sandbox/source/render/galaxy_map_render.h

- **Path:** `sandbox/source/render/galaxy_map_render.h`
- **Purpose:** Declares the galaxy-map pass and its two cursor hit-test queries.
- **Key types/functions:** `draw_galaxy_map_look`, `galaxy_pick_planet`, `galaxy_map_hover_tooltip`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Documents the frame-lifetime hazard explicitly** — the returned slot "is only valid this frame; callers that track the planet across frames should stash the system's `galaxy_center`" — which is the clearest statement anywhere of how the galaxy hot cache invalidates indices.
  - **States that the hit-test matches the draw exactly** (per-type render size and the same visibility gate), making the mirroring a documented contract rather than an implementation coincidence.
  - **Explains why the tooltip is computed in the update path** — so the RmlUi HUD picks it up the same frame with no cursor lag — and stresses "No drawing — the HUD renders the string".
  - Records that the pass cross-fades by map weight and no-ops in the arena look, so callers need not gate it.
  - Tagged "extracted verbatim from `game_render` (R1 scene_renderer decomposition)".
- **Interface vs internal:** Both — one internal pass declaration alongside two genuinely public query functions.

---

## sandbox/source/sim/action_log.cpp

- **Path:** `sandbox/source/sim/action_log.cpp`
- **Purpose:** Appends printf-formatted messages to the rolling HUD action-log buffer, evicting the oldest at capacity.
- **Key types/functions:** `action_log_push(game_state*, const char* fmt, ...)`; `ACTION_LOG_MAX` (30).
- **Notable non-obvious dependencies:**
  - **Owns no storage** — it writes into `s->action_log`, a buffer defined in `game_state.h`, so the module is a mutator of state it does not declare. `ACTION_LOG_MAX` is defined here while the array it indexes is sized elsewhere; the two must agree.
  - **Eviction is an O(n) `memcpy` shuffle of 30 × 128-byte entries** on every push once full, rather than a ring buffer — simple but linear per message.
  - **The 128-byte entry width is a magic number repeated three times** in the function (memcpy size, vsnprintf bound, NUL index) rather than derived from the array type.
  - **Resets `inactivity_timer`**, which drives the HUD panel's idle fade — so logging a message has a visual side effect beyond adding a line.
  - **Called from across the whole sandbox** (keybindings, combat, AI, missions, trade), making it one of the most widely invoked sandbox functions and an implicit dependency of nearly every gameplay module.
  - The closing comment records that the old `bs_ui` presentation panel was retired in favour of the RmlUi HUD, so this file is now data-only.
- **Interface vs internal:** Public interface within the sandbox — a one-function logging facility used everywhere.

---

## sandbox/source/sim/action_log.h

- **Path:** `sandbox/source/sim/action_log.h`
- **Purpose:** Declares the variadic action-log push function.
- **Key types/functions:** `action_log_push(game_state*, const char*, ...)`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Documents the capacity and the fade behaviour** (30 entries, oldest evicted, timer reset) even though neither is visible in the signature.
  - **Names the renderer of the buffer** — `game_push_hud` via RmlUi — so the data/presentation split is recorded at the interface.
  - No format-string attribute is applied, so mismatched varargs are not diagnosed despite the function being printf-style.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/camera_controller.cpp

- **Path:** `sandbox/source/sim/camera_controller.cpp`
- **Purpose:** Drives camera zoom from the mouse wheel, eases it in log space, flips the arena/galaxy-map label at the threshold with a control hand-off, and pins the point under the cursor while zooming.
- **Key types/functions:** `update_zoom_and_mode(game_state*, f32 dt)`; constants `ZOOM_MIN` (0.08), `ZOOM_MAX` (12.0), `ZOOM_STEP` (1.12), `ZOOM_GLOBAL_MIN` (7.5e-10), `ZOOM_SPEED_RAMP` (0.02).
- **Notable non-obvious dependencies:**
  - **Reads the engine's input singleton directly** (`input_get_mouse_wheel`, `input_get_mouse_position`), and the wheel accumulator is frame-scoped — so this function must run inside the game's update or the notch is lost.
  - **`ZOOM_GLOBAL_MIN` is 7.5e-10**, justified in a comment: positions render linearly with no cosmetic compression, so framing a galaxy disc of radius ~3.2e11 requires the zoom to span ten decades. That single constant is why the whole codebase needed hierarchical coordinates.
  - **All easing and stepping happens in log space** because zoom is multiplicative across those decades, giving constant *perceived* speed — and `g_zoom_out_speed_gain` (owned by `view_transform.cpp`, driven by an editor slider) boosts the per-notch step below the ramp threshold.
  - **The mode flip is documented as a pure label change.** Crossing `ZOOM_MIN` sets `view.mode`, but the comment stresses both looks share one coordinate space so there is no re-anchor and no jump — the remaining work is a free-camera hand-off.
  - **Control intent is remembered across the round trip.** `global_free_camera_saved` is deliberately *not* snapshotted on the outbound crossing, with a comment explaining that doing so would let a temporary off-screen fallback overwrite a genuine piloting intent. Re-entering restores piloting, glides onto the ship if it is on-screen, and falls back to free camera otherwise.
  - **Calls `s->render.global_background.notify_system_changed(...)`** when re-entering the arena — a camera controller poking a render subsystem.
  - **Cursor-pin zoom is suppressed while a planet approach is engaged or a candidate**, because the approach logic re-centres on the planet and the two would fight.
  - Edit mode takes a different branch that writes `camera_hierpos` directly and zeroes the render residual, keeping the floating-origin anchor tight.
  - Early-returns when the eased zoom did not change, explicitly to avoid fighting pan and follow logic on idle frames.
- **Interface vs internal:** Internal — one exported function called from the update path, but it mutates camera, view mode, planet approach, and background state.

---

## sandbox/source/sim/camera_controller.h

- **Path:** `sandbox/source/sim/camera_controller.h`
- **Purpose:** Declares the zoom and view-mode controller.
- **Key types/functions:** `update_zoom_and_mode(game_state*, f32)`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Summarises four distinct responsibilities in one comment** — wheel input, log-space easing, mode-label flip with hand-off, and cursor pinning — which is the only warning that this "zoom" function also changes control mode.
  - Ten lines total; gives no hint that it reads global input state or writes to the background subsystem.
- **Interface vs internal:** Internal interface.

---

## sandbox/source/sim/celestial_parallax.cpp

- **Path:** `sandbox/source/sim/celestial_parallax.cpp`
- **Purpose:** Computes the depth-parallax offset applied to celestial bodies, from a single shared anchor, with a zoom-driven fade.
- **Key types/functions:** `celestial_shared_anchor(const game_state*)`, `celestial_center_render(...)`, `celestial_parallax_fade(const game_state*)`; static `parallax_fade_weight(...)`.
- **Notable non-obvious dependencies:**
  - **The shared anchor is the whole design.** Parallaxing each system against its own centre would collapse every system onto the screen centre as depth approaches 1 and fuse them; using one anchor keeps the depth term common so inter-system layout stays rigid. That reasoning appears in both the header and the `.cpp`.
  - **A special case exists purely for the planet-approach camera**: while a planet is captured, the camera is deliberately offset from its star, so "nearest node to the camera" would snap the anchor onto a *neighbour* star and break the correction. The anchor is pinned to the captured system instead.
  - **Parallax fades out at low zoom by design** — on the galaxy map it would tear ships away from their own systems — and the band is `[bg_parallax_fade_zoom, ×2.8]`, chosen to match the arena/map view fade defaults. Two independent fade ramps are deliberately kept separate so the disappear point can be tuned alone.
  - **Both the map and arena renderers must call this with identical arguments** or a seam appears across the cross-fade; the requirement is stated but unenforceable.
  - **`celestial_parallax_fade` is exposed specifically so camera code can cancel a followed body's parallax shift** — a getter that exists for one caller's correction.
  - Uses `hierpos_diff` for both differences, keeping the computation precision-safe far from the origin.
  - Lives under `sim/` despite being purely a render-space transform.
- **Interface vs internal:** Public interface within the sandbox — called by four render modules and the camera code.

---

## sandbox/source/sim/celestial_parallax.h

- **Path:** `sandbox/source/sim/celestial_parallax.h`
- **Purpose:** Declares the three parallax functions and documents the shared-anchor rule.
- **Key types/functions:** `celestial_shared_anchor`, `celestial_center_render`, `celestial_parallax_fade`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Draws an explicit line between backdrop and gameplay**: stars, planets, orbits and test sprites are parallaxed; ships, enemies, projectiles and FX are excluded and stay at parallax 1.0 via `render_from_hierpos`. That split is the reason two different transforms exist for "world to render".
  - **States the failure mode the shared anchor prevents** (all systems collapsing and fusing as depth→1), making the design constraint legible without reading the math.
  - **Gives the formula** `(system_center − cam) − (shared_anchor − cam) × (depth × zoom_fade)` so callers can reason about what to add on top.
  - Notes the anchor is computed once per frame and cached in `game_state::celestial_anchor`, which is what makes the per-frame consistency guarantee hold.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/sensor_system.cpp

- **Path:** `sandbox/source/sim/sensor_system.cpp`
- **Purpose:** Detection logic — converts distance to a per-ship confidence reading and gathers all hostile projectiles the fleet can sense.
- **Key types/functions:** `sensor_reading(const SensorSuite*, f32 dist)`, `sensor_gather_hostile_contacts(const game_state*, SensorContact*, i32 max_out)`.
- **Notable non-obvious dependencies:**
  - **Detection is a fleet-wide union computed by max**, so overlapping sensor coverage widens the picture and raises confidence without double-counting — the rule is stated in the header and implemented as a nested scan.
  - **The implementation contradicts its own header.** The header says "Only contacts with confidence > 0 are written", but the code writes *every* active non-player projectile regardless, with a comment explaining that Layer 2 is unbounded so distant returns are still tracked at confidence 0 and merely dimmed by the overlay. The code is the newer behaviour; the header text was not updated.
  - **Cost is O(active projectiles × fleet size) every frame**, scanning the full `MAX_PROJECTILES` pool rather than an active list.
  - **Faction filtering is a single hardcoded comparison** against `FACTION_PLAYER`, so "hostile" means "not the player" rather than consulting the diplomacy model the rest of the game uses.
  - Guards against a misconfigured suite where Layer 2 ≤ Layer 1 by flooring the denominator.
  - Produces data consumed solely by `render/sensor_overlay.cpp`, keeping detection and presentation genuinely separate.
- **Interface vs internal:** Public interface within the sandbox — two pure functions with one consumer.

---

## sandbox/source/sim/sensor_system.h

- **Path:** `sandbox/source/sim/sensor_system.h`
- **Purpose:** Declares the sensor contact record and the two detection functions.
- **Key types/functions:** `struct SensorContact` (position, velocity, confidence, owner); `sensor_reading`; `sensor_gather_hostile_contacts`.
- **Notable non-obvious dependencies:**
  - **Includes `sim/ship.h` for `SensorSuite` and `VesselFaction`**, pulling a 558-line header into every consumer of a 36-line interface.
  - **Gives the confidence formula explicitly** — `clamp((L2 − dist) / (L2 − L1), 0, 1)` — so the meaning of the 0..1 value is pinned at the interface.
  - **States the union-by-max rule** as the fleet's detection model, which is the one piece of design here that is not obvious from the signatures.
  - Its claim that only confidence > 0 contacts are written no longer matches the implementation.
  - `SensorContact` carries `owner` as a `VesselFaction`, though the gather function filters on the numeric `faction_id` instead — two faction representations in one path.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/ship_control.cpp

- **Path:** `sandbox/source/sim/ship_control.cpp`
- **Purpose:** Turns pilot input into thrust and turn commands, resolves fleet-vs-enemy hull collisions, and reports which ship the player is flying.
- **Key types/functions:** `control_ship_global(game_state*, FleetShip*, f32 dt)`, `resolve_ship_collision(game_state*)`, `piloted_ship_origin(game_state*)`.
- **Notable non-obvious dependencies:**
  - **Reads the engine input singleton directly** for W/S/Q/E/C/A/D, so the control scheme is hardcoded at these call sites with no binding indirection.
  - **A deliberate split of responsibility is documented at length**: this function only *adds* commanded turn and returns whether one was issued; the complementary auto-stabilise lives in `simulate_ship`. The reason given is that the integrator runs in both modes while this control function runs only while piloting, so a stabiliser here would let spin accumulated in one mode rotate the ship forever in the other.
  - **Mutates velocities only, never the pose** — integration belongs to the simulator, which is what lets a ship keep coasting when nobody is piloting it.
  - **Mouse-follow mode uses a hand-written PD controller** (`diff * 3.0 - angular_velocity * 1.5`, clamped), with the derivative term damping overshoot, and deliberately issues no turn inside a 0.01 rad deadband so the simulator's stabiliser bleeds the residual.
  - **Collision treats the enemy hull as an immovable derelict** — fleet ships are pushed out by the full SAT minimum-translation vector and only the inward velocity component is cancelled, so ships slide along rather than sticking. It must run after pose integration, a requirement stated only in a comment.
  - **`piloted_ship_origin` asks `s->rts_controls.piloted_index()`** and silently falls back to the flagship on an out-of-range index — so a `sim` module depends on the RTS control object to answer "who is the player".
  - Per-ship motion tuning comes from `ship->motion`, resolved from the hull's size class at load, so a drone and a cruiser share this code with different constants.
  - Free-camera mode short-circuits the whole function, which is how "autopilot" is expressed: no input, but the ship keeps coasting.
- **Interface vs internal:** Public interface within the sandbox — three functions called from the update path.

---

## sandbox/source/sim/ship_control.h

- **Path:** `sandbox/source/sim/ship_control.h`
- **Purpose:** Declares the pilot-control, collision-resolution, and piloted-ship-lookup functions.
- **Key types/functions:** `control_ship_global`, `resolve_ship_collision`, `piloted_ship_origin`; forward-declares `game_state` and `FleetShip`.
- **Notable non-obvious dependencies:**
  - **Documents the mutate-velocities-not-pose contract** and the meaning of the `b8` return (a turn was commanded, so skip auto-stabilising) — both are invisible from the signature and both matter to the caller.
  - **Names the full control scheme in the comment** (heading-relative WASD, Q/E strafe, C brake, A/D or mouse-follow turn), which is the closest thing to a keybinding reference for flight.
  - Describes the collision response as SAT-MTV with inward-velocity cancellation, but does not state the ordering requirement that it run after integration.
  - `piloted_ship_origin` takes a non-const `game_state*` although it only reads.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/travel.cpp

- **Path:** `sandbox/source/sim/travel.cpp`
- **Purpose:** Implements a simple parameterised point-to-point travel interpolator over hierarchical positions.
- **Key types/functions:** `travel_init`, `travel_update`, `travel_reset`, `travel_set_destination`; static `travel_ease(f32, TravelEaseMode)`.
- **Notable non-obvious dependencies:**
  - **The only sandbox `sim` module that touches no `game_state`** — it operates purely on the `TravelState` it is handed, making it independently testable.
  - **Interpolates with `hierpos_lerp`**, the precision-safe path, which is exactly the ~50,000-unit scenario the engine's `bs_hierpos_selftest` hardcodes a case for. The engine test and this module are the two halves of that concern.
  - **Progress is a fraction per second, not a speed** — `speed = 0.15` means ~6.7 seconds for any journey regardless of distance, so travel time is independent of how far apart the endpoints are.
  - **Caches an `f64` world position on every update** purely "for diagnostics", so the update path pays a conversion for the debug overlay's benefit.
  - Three ease modes are defined and `travel_init` always selects `TRAVEL_EASE_LINEAR`; nothing else sets `ease_mode`, so the other two are unreachable in practice.
  - `travel_reset` reactivates the journey but leaves `paused` untouched, unlike `travel_init`.
- **Interface vs internal:** Public interface within the sandbox — a self-contained utility driven by the editor-gated travel debug feature.

---

## sandbox/source/sim/travel.h

- **Path:** `sandbox/source/sim/travel.h`
- **Purpose:** Defines the travel state record and its four operations.
- **Key types/functions:** `enum TravelEaseMode` (linear, smoothstep, quad-in-out, count); `struct TravelState` (origin, destination, current, progress, speed, active, paused, ease_mode, cached `world_x`/`world_y`); the four functions.
- **Notable non-obvious dependencies:**
  - **Stores three `HierPos2` values plus a redundant `f64` pair**, with the latter annotated as diagnostics-only — a debug affordance baked into the data model rather than computed on demand.
  - **`speed` is documented as "fraction per second"**, which is the one field whose units are surprising and the one place that is written down.
  - The struct is embedded in `game_state` as `s->travel`, alongside separate `travel_enabled` / `travel_paused` flags that duplicate the struct's own `active` / `paused` — two layers of the same state.
  - Forward-declares nothing and depends only on `bs_hierpos.h`, keeping it independent of the rest of the sandbox.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/heat_map.cpp

- **Path:** `sandbox/source/sim/heat_map.cpp`
- **Purpose:** Gathers radiation detector and emitter sources (with velocity-extrapolated trails) and submits one GPU heat-map draw.
- **Key types/functions:** `draw_ship_metaballs(game_state*)`; static `heat_map_fade_weight(f32 zoom)`; local `MBSource` struct; constants `HEAT_FADE_FULL_ZOOM` (0.005) and `HEAT_FADE_ZERO_ZOOM` (0.000004).
- **Notable non-obvious dependencies:**
  - **`HEAT_FADE_ZERO_ZOOM` is annotated "== ZOOM_GLOBAL_MIN"** but the two do not match — the camera controller's floor is `7.5e-10`, this is `4e-6`. A cross-module constant that drifted apart from the value its comment claims to track.
  - **Works in a camera-relative true-world frame** by translating every source by `-camera_hierpos`, with the comment explaining that this keeps the heat map precision-safe far from the origin while leaving the on-screen result identical because the shader only uses relative offsets.
  - **Synthesises trail sources by velocity extrapolation** — up to 8 points per combat entity and 16 per projectile, each with age-faded emission — so one moving emitter can consume 17 of the 256 shader source slots. The `BS_MAX_HEAT_SOURCES` cap is checked in five separate places, and overflow silently truncates whatever is gathered last.
  - **Player fleet ships are submitted as *detector* sources** with zero emission, so the same array carries two semantically different entity kinds distinguished by an `is_detector` flag the shader interprets.
  - **Times itself with `std::chrono` and writes a rolling average into `s->heat_map_cpu_ms`**, feeding the profiler panel from inside a draw function.
  - **Culls by an inflated viewport** (15% margin plus three times the larger radius) before adding sources, which is what keeps the source count bounded at normal zoom.
  - Lives under `sim/` but is purely a render submission — the file name (`metaballs`) also predates the current "heat map" naming used everywhere else.
  - Reads eleven separate tuning fields off `game_state` (threshold, palette, colours, falloff, warp, venn sharpness, intensity, tail length and fade, radii), all editor-driven.
- **Interface vs internal:** Internal — one exported function called from `draw_gameplay_overlays`.

---

## sandbox/source/sim/heat_map.h

- **Path:** `sandbox/source/sim/heat_map.h`
- **Purpose:** Declares the heat-map submission function.
- **Key types/functions:** `draw_ship_metaballs(game_state*)`; forward-declares `game_state`. Nine lines.
- **Notable non-obvious dependencies:**
  - **Names both halves of the source model** — detectors are the player fleet, emitters are enemy combat entities and projectiles with trails — which is the only place that distinction is documented.
  - States the two no-op conditions (UI toggle off, or fully faded by zoom), so callers need not gate it.
  - The declared function name still says "metaballs" while the header, the comments, and the rest of the codebase say "heat map".
- **Interface vs internal:** Internal interface.

---

## sandbox/source/sim/steering.cpp

- **Path:** `sandbox/source/sim/steering.cpp`
- **Purpose:** Implements shared steering primitives and the locomotion layer that converts a desired velocity into thrust, turn, and pose integration.
- **Key types/functions:** `steering::arrive`, `::seek`, `::flee`, `::standoff`, `::apply`, `::apply_face`; static `apply_impl`.
- **Notable non-obvious dependencies:**
  - **One of very few sandbox files using a namespace**, and the only shared locomotion layer — the header states it is "reused by any Ship-backed agent", so NPC AI and fleet autopilot share exactly this integration path while the *player* goes through `ship_control.cpp` instead.
  - **`apply_impl` integrates the pose with `hierpos_add_vec2`**, making this one of the few places outside the fleet simulator that advances a ship's authoritative position.
  - **The four desired-velocity functions are pure and operate on local vectors** produced by `hierpos_diff`, which the header calls out as what keeps them precision-safe anywhere in the galaxy.
  - **Heading convention (angle 0 → nose +Y) is applied via `atan2f(-fd.x, fd.y)`**, the same non-obvious argument order used in `ship_control.cpp` and `ship_render.cpp`; three files independently encode it.
  - **Facing defaults to the travel direction but falls back to the desired velocity below 1 unit/s**, so a nearly-stopped ship still points where it intends to go rather than spinning freely.
  - `arrive` has a hardcoded 1.0-unit arrival deadband, and `standoff`'s band width is `standoff_dist * 0.25 + 1`, both magic values with no tuning hook.
  - Angle wrapping uses `while` loops rather than `fmod`, correct but unbounded for a wildly out-of-range input.
- **Interface vs internal:** Public interface within the sandbox — the shared movement vocabulary for all non-player agents.

---

## sandbox/source/sim/steering.h

- **Path:** `sandbox/source/sim/steering.h`
- **Purpose:** Declares the steering primitives and the two `apply` variants inside `namespace steering`.
- **Key types/functions:** `arrive`, `seek`, `flee`, `standoff`, `apply`, `apply_face`; forward-declares `Ship` and `ShipFlight`.
- **Notable non-obvious dependencies:**
  - **States the precision contract at the interface** — inputs are local vectors from `hierpos_diff` — which is what makes these usable at galaxy scale despite taking plain `Vec2`.
  - **Calls itself "one locomotion layer, reused by any Ship-backed agent"**, naming the two current consumers (General Ship AI and fleet autopilot) and implicitly excluding the player.
  - **`apply` and `apply_face` differ only in whether the nose tracks travel or a supplied direction**, the distinction that separates "flying somewhere" from "keeping guns on target".
  - Forward-declares both structs, so the header depends only on the math library.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/discovery.cpp

- **Path:** `sandbox/source/sim/discovery.cpp`
- **Purpose:** Detects when the player's ship comes within identification range of NPC ships or stations, marks them discovered, remembers them, and logs the find.
- **Key types/functions:** `discovery_npc_is_known`, `discovery_update`, `discovery_log_push`; statics `discovery_npc_remember`, `role_word`, `role_kind`, `civ_name_of`, `node_name_of`.
- **Notable non-obvious dependencies:**
  - **Two different persistence mechanisms for two object kinds** — NPCs are remembered in a global `npc_discovered[]` registry keyed by `(home_node, spawn_seed)` because the agents themselves are transient, while stations just set a `discovered` flag that rides along with the cached `StarSystem`.
  - **Both registries evict the oldest by O(n) array shift** at capacity, and the NPC one carries a comment acknowledging the bound is provisional ("save/load can widen this later") — so discoveries can be silently forgotten.
  - **Discovery range is the ship's `sensors.layer1_radius`**, deliberately reusing the sensor model rather than a dedicated constant; both the header and an in-file comment record that this replaced a separate field.
  - **Writes to two log surfaces at once** — `discovery_log_push` appends to the Discoveries browser *and* calls `action_log_push` for immediate HUD feedback, so one event produces two user-visible records.
  - **Scans every cached system's every station every frame** (`system_count × station_count`) plus all `NPC_SHIP_MAX` slots, with no spatial index and no throttling.
  - **Maps AI archetypes to display labels and browser categories** through two parallel switch statements, collapsing warship/interceptor/pirate into one kind — a presentation taxonomy living in a sim module.
  - Names are composed from the owning civ plus a role word, so a discovery's label depends on the galaxy history subsystem.
- **Interface vs internal:** Public interface within the sandbox — three exported functions driven from the update path.

---

## sandbox/source/sim/discovery.h

- **Path:** `sandbox/source/sim/discovery.h`
- **Purpose:** Declares the discovery query, the per-frame scan, and the browser-feed push.
- **Key types/functions:** `discovery_npc_is_known`, `discovery_update`, `discovery_log_push`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Documents the full feature across four other files** — that undiscovered objects render as generic "unidentified" markers in `ship_scene.cpp`, `gameplay_overlays.cpp`, and `system_content_render.cpp` — so the header is the map of a feature whose visible half lives entirely in the render tree.
  - **Spells out both persistence keys** (NPCs by `(home_node, spawn_seed)`; stations by a flag on the cached system), which is the only place the asymmetry is explained.
  - **Records a design change in a standalone comment**: discovery range "is no longer a separate constant or a dedicated field", now reusing `Ship::sensors.layer1_radius`.
  - Notes that `discovery_log_push` also emits an Action Log line, making the double-logging explicit.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/module.cpp

- **Path:** `sandbox/source/sim/module.cpp`
- **Purpose:** Loads the ship-module registry — parses a manifest of `.module` files into an immutable fixed pool and looks defs up by id.
- **Key types/functions:** `module_registry_load(ModuleRegistry*, const char* manifest_path)`, `module_registry_find(const ModuleRegistry*, const char* id)`; statics `module_def_load`, `module_type_from_token`, `module_size_from_token`, `module_rstrip`.
- **Notable non-obvious dependencies:**
  - **Reads a two-level file structure from disk** — a manifest (`assets/modules/modules.list`) naming one `.module` path per line, each parsed with `sscanf` line matching. Paths are relative to the working directory, so this depends on the asset-staging step.
  - **`#define _CRT_SECURE_NO_WARNINGS` at the top**, with a comment noting the sandbox build does not define it globally the way the engine build does — the same workaround as `ship_visual.cpp` and `star_fx.cpp`.
  - **Failure is graded**: a missing manifest returns `FALSE`, but an individual bad module file only logs a warning and is skipped, and an unknown `type` or `size` token warns while leaving a default. A typo therefore silently changes a module's behaviour rather than failing the load.
  - **Applies three defaulting rules after parsing** — name falls back to id, glyph to the name's first character, and icon to `"ic-sensor"` for sensor modules — so the data files can be terse. The icon default is explicitly noted as provisional ("only sensors exist today").
  - **Quoted-string fields (`name`, `desc`) are parsed by hand** rather than with `sscanf`, scanning for the closing quote and truncating in place; an unterminated quote silently consumes the rest of the line.
  - `module_registry_find` is a linear scan by string compare over up to 32 entries, called during setup rather than per frame.
  - Registry capacity overflow warns and skips, so a manifest longer than 32 entries silently loses the tail.
- **Interface vs internal:** Public interface within the sandbox — two exported functions called from `game_init` and the loadout UI.

---

## sandbox/source/sim/module.h

- **Path:** `sandbox/source/sim/module.h`
- **Purpose:** Defines the immutable module definition, the fixed registry, and the two registry functions; documents the file format and the stat-composition rule.
- **Key types/functions:** `MODULE_REGISTRY_MAX` (32); `struct ModuleDef` (id, name, glyph, icon, desc, type bit, size, `sensor_mult[3]`); `struct ModuleRegistry`; `module_registry_load`, `module_registry_find`.
- **Notable non-obvious dependencies:**
  - **Documents the whole `.module` file format inline**, field by field with an example — the only specification of that format anywhere.
  - **States the pointer-sharing safety argument explicitly**: ships mount defs *by pointer* (`Ship::module_mounts[]`, `module_stash[]`) and that is safe because defs carry no per-instance runtime state. The registry must therefore outlive every ship, which it does by living in `game_state`.
  - **Spells out the stat-composition contract** — modules never write ship stats; `Ship::sensors_base` is the authored baseline and `ship_recompute_stats()` re-derives the effective suite as baseline × the product of mounted multipliers. It then names the downstream consumers that pick it up automatically (discovery radius, asteroid reveal, point-defense range), which is how a sensor module silently changes three unrelated systems.
  - **Distinguishes single-kind modules from multi-kind hardpoints** — a module *is* one `MODULE_TYPE_*` bit, while a hardpoint's accepts-mask may OR several — a subtlety that explains why `type` is a `u32` bit rather than an enum.
  - Includes `sim/ship.h` for `HardpointSize`, so this small header pulls in the 558-line ship definition.
  - The size rule ("fits hardpoints of this size OR LARGER") is documented here and re-implemented independently in two drag-validation functions.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/point_defense.cpp

- **Path:** `sandbox/source/sim/point_defense.cpp`
- **Purpose:** Runs the point-defense laser for every fleet ship that carries the fleet's pooled PD device (gate: `enabled && point_defense_mount >= 0`) — doctrine gating, target acquisition and validation, capacitor drain, damage application, and beam recording.
- **Key types/functions:** `point_defense_update(game_state*, f32 dt)`; local `GATE_FRAC[3]` table.
- **Notable non-obvious dependencies:**
  - **Has strict ordering requirements stated only in the header** — after `combat_arena_sync_entities()` so positions are current, and *before* `combat_arena_update_projectiles()` so destroyed threats never advance or collide that frame. Getting this wrong would let intercepted missiles still hit.
  - **Frees projectile slots directly** (`p.active = FALSE; --s->projectiles.count`), reaching into another subsystem's pool rather than going through an API.
  - **Writes `s->defense_beams` / `defense_beam_count`**, the buffer `render/defense_laser_overlay.cpp` replays — the sim/render handoff for this feature.
  - **`GATE_FRAC[3] = {0.6, 0.8, 1.0}` is the third copy of these numbers**, alongside the overlay's identical table and `bs_rml.h`'s documented tiers.
  - **Range is live-coupled to `sensors.layer0_radius` when no override is set**, so installing a sensor module changes point-defense reach — the composition effect `module.h` describes. The header's comment says Layer 1 while the code uses Layer 0, and an in-code comment explains the Layer 0 choice deliberately ("last-ditch screen, must not reach identification range"), so the header is stale.
  - **A capacitor reserve floor blocks *new* locks but lets existing ones burn out**, so PD throttles itself before bottoming the bank; running dry mid-dwell drops the lock and is described as the intended saturation failure mode.
  - **Three doctrine axes multiply into behaviour** — stance (HOLD/STANDARD/OVERDRIVE scaling damage ×2, drain ×3, retarget ×0.5), priority (impact time / missiles-first / nearest), and gate tier — producing 3 × 3 × 3 configurations from one code path.
  - **Target scoring uses closing speed, not distance**, for the default priority: receding or tangential threats score `1e6 + d` so they are engaged only when nothing is closing, and missiles get a `1e7` penalty offset to strictly outrank shells.
  - **Logs only missile intercepts** to the action log, deliberately staying silent on shell kills to avoid spam.
  - Also drives presentation directly by calling `ship_turret_aim_at` to swing the PD turret art onto the locked target.
- **Interface vs internal:** Internal — one exported function in the update chain, but it mutates ships, projectiles, and the render beam buffer.

---

## sandbox/source/sim/point_defense.h

- **Path:** `sandbox/source/sim/point_defense.h`
- **Purpose:** Declares the point-defense tick and documents its behaviour and call-ordering contract.
- **Key types/functions:** `point_defense_update(game_state*, f32)`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **The ordering constraint is the header's most important content** — call after `combat_arena_sync_entities()` and before `combat_arena_update_projectiles()`, with the reason for each given. Nothing enforces it.
  - **Describes the full engagement cycle** (acquire → dwell → damage → destroy or release → cooldown) so a reader need not infer the state machine from the code.
  - **Says the engagement range is the Layer 1 sensor radius**, which the implementation contradicts by using Layer 0 with an explicit justification — a documented-versus-actual mismatch in a gameplay-relevant number.
  - Notes that beams are recorded into `s->defense_beams` "for the overlay to draw", making the producer/consumer split explicit.
  - Labels itself "targeting + damage only — no rendering", the same separation the sensor system declares.
- **Interface vs internal:** Internal interface.

---

## sandbox/source/sim/projectile.cpp

- **Path:** `sandbox/source/sim/projectile.cpp`
- **Purpose:** Implements the fixed projectile pool — procedural streak and flash textures, shell and missile spawning, motion integration, and rendering.
- **Key types/functions:** `ProjectileSystem::init`, `::spawn`, `::spawn_missile`, `::update`, `::render`; static `smoothstep`.
- **Notable non-obvious dependencies:**
  - **`init` bakes two textures on the stack** — a 128×512 streak (256 KB) and a 128×128 flash (64 KB) — unlike `text.cpp` and `star_fx.cpp`, which deliberately use `static` buffers to avoid exactly this. 256 KB is a quarter of the default Windows stack.
  - **`spawn_missile` duplicates `spawn`'s entire free-slot loop**, with a comment explaining why: `spawn` does not report which slot it filled. The fix would be returning an index; instead the body is copied.
  - **`update` recounts active projectiles from scratch** each tick, which quietly repairs `count` after `point_defense.cpp` decrements it directly — two subsystems maintaining the same counter by different means.
  - **`render` calls `hierpos_diff(&p.position, camera)` with the two-argument overload**, taking the default cell size, while nearly every other call site passes `BS_HIERPOS_CELL_SIZE` explicitly.
  - **`custom.x` and `custom.y` carry glow strength and age** into the fragment shader for the shimmer effect — the same untyped channel convention used across the render tree.
  - **Steering is deliberately not here.** Missiles are spawned tagged and guided by `combat_arena_steer_missiles`, so the pool is pure ballistics plus a kind tag.
  - Trail length is derived from speed (`speed * 0.04`) with a radius-based floor, and the muzzle flash is a 50 ms burst — both hardcoded.
  - Linear free-slot scans make spawning O(MAX_PROJECTILES) worst case, and a full pool fails silently by returning `FALSE`.
- **Interface vs internal:** Public interface within the sandbox — the struct is a `game_state` member and its methods are called by weapons, combat, point defense, and the overlay pass.

---

## sandbox/source/sim/projectile.h

- **Path:** `sandbox/source/sim/projectile.h`
- **Purpose:** Defines the projectile record, the three projectile kinds, and the pooled projectile system.
- **Key types/functions:** `MAX_PROJECTILES` (512); `enum ProjectileKind` (`SHELL`, `MISSILE`, `FLAK`); `struct Projectile` (16 fields); `struct ProjectileSystem` (pool, count, two textures, `glow_override`, five methods).
- **Notable non-obvious dependencies:**
  - **Carries two parallel faction representations** — `owner` (legacy binary `VesselFaction`, "kept for visuals/back-compat") and `faction_id` (unified civ index) — with the header stating which is authoritative for hit filtering and kill attribution. Every spawn site must set both correctly.
  - **`max_speed` is documented at length as a per-missile clamp** stamped from the launcher's `proj_speed`, explicitly so a `.weapon` file's value governs flight and therefore reach, rather than a global tuning value. `0` means fall back to the global clamp.
  - **`glow_override` is a mutable pointer on the system**, set by `gameplay_overlays.cpp` before rendering — so a render pass configures the simulation object, and the pointer's *identity* determines batch merging in the engine.
  - **The enum comment specifies each kind's behaviour and its owning module** — shells fly straight, missiles are steered by `combat_arena_steer_missiles` under `missile_tuning`, flak proximity-detonates under `flak_tuning` and never damages ships — a three-way behavioural contract split across three files.
  - `MAX_PROJECTILES` (512) sizes the pool here, `sensor_overlay.cpp`'s static contact buffer, and the point-defense scan loop.
  - Includes `sim/ship.h` for `VesselFaction`, pulling the large ship header into everything that touches projectiles.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/weapon.h

- **Path:** `sandbox/source/sim/weapon.h`
- **Purpose:** Defines the weapon class hierarchy — an abstract `Weapon` base with ballistic and missile subclasses — plus the shared effective-reach helper.
- **Key types/functions:** `enum WeaponKind`, `enum FireMode` (`MODE_AP`, `MODE_FLAK`); `struct Weapon` (abstract: `fire`, `update`, `ready`, `cooldown_progress`, `projectile_speed`, `cap_cost`); `struct BallisticWeapon`; `struct MissileLauncher`; `weapon_effective_reach(const Weapon*)`.
- **Notable non-obvious dependencies:**
  - **The only virtual-dispatch class hierarchy in the project.** Everything else is PODs and free functions; weapons use `virtual` methods and a base-class pointer (`Ship::mounts[]` holds `Weapon*`). It also carries a `wkind` tag explicitly "no RTTI in the hot path", so the code both uses virtual dispatch and works around it.
  - **`weapon_effective_reach` is declared the "SINGLE SOURCE OF TRUTH"** for whether a weapon can hit at a distance, with the two consumers named — the RTS attack order gates firing on it and the HUD range ring draws it, so what the player sees matches what the ship does. That guarantee depends on both call sites using it, which they do.
  - **Instances are heap-allocated and owned by ships** via raw `Weapon*` in `mounts[]` and `weapon_stash[]`, with a virtual destructor but no smart pointers — so mounting, unmounting, and stashing must hand ownership around manually.
  - **The base class carries `owner_faction` and `owner_faction_id`**, the same dual-faction scheme as projectiles, with a note that the unified id is stamped "at each fire site" rather than at equip time.
  - **`cap_cost` ties weapons to the capacitor model** introduced for point defense, referencing `docs/POINT_DEFENSE_AND_MISSILES.md` — the header points at an external design doc for the rule.
  - **`FireMode` is per-weapon state toggled per fire group via the T key**, so a keybinding in `game.cpp` mutates a field declared here.
  - The closing comment records that "no hardcoded factories remain" — instances are built from `.weapon` defs by `weapon_instantiate`, so this header describes a runtime shape whose values all come from data.
  - `size` is a `u8` holding a `HardpointSize`, compared against hardpoint sizes in three separate validation functions.
- **Interface vs internal:** Public interface within the sandbox — the weapon abstraction consumed by ships, combat, AI, and the loadout UI.

---

## sandbox/source/sim/weapon.cpp

- **Path:** `sandbox/source/sim/weapon.cpp`
- **Purpose:** Implements the two concrete weapon classes — ballistic cannon and missile launcher — and the shared effective-reach calculation.
- **Key types/functions:** `BallisticWeapon` constructor, `::fire`, `::update`, `::ready`, `::cooldown_progress`; the same four for `MissileLauncher`; `weapon_effective_reach(const Weapon*)`.
- **Notable non-obvious dependencies:**
  - **`weapon_effective_reach` prefers the authored `.weapon` def over the live instance**, with the reasoning stated: editing a data file must move *both* the drawn range ring and the distance at which the ship opens fire. The instance fallback exists only for legacy weapons built without a def.
  - **That fallback downcasts on the `wkind` tag** rather than adding a virtual method — a deliberate choice given the header's "no RTTI in the hot path" note, but it means a new weapon subclass silently falls into the ballistic branch.
  - **Flak is a fire *mode* on the ballistic weapon, not a subclass** — `MODE_FLAK` scales speed to 0.6× and hardcodes a 1.4 s lifetime, producing a short proximity envelope, and tags the spawn `PROJ_FLAK`. So one weapon object switches projectile kind at fire time.
  - **The faction colour switch assigns the identical colour to all five cases**, leaving the per-faction tinting effectively dead while looking intentional.
  - **Both `fire` methods add the ship's velocity to the muzzle velocity**, so shots inherit the firing platform's motion — a physics detail the callers must supply correctly.
  - **Capacitor costs are hardcoded in the constructors** (4.0 ballistic, 25.0 missile) and then overwritten from the def by `weapon_instantiate`, so the constructor defaults are only visible for legacy instances.
  - `MissileLauncher`'s constructor overrides `icon` to `"ic-missile"` and sets `wkind`, the only two fields the base constructor cannot know.
  - The missile tint is explicitly a placeholder "until bespoke art lands".
- **Interface vs internal:** Implementation of a public sandbox interface; `weapon_effective_reach` is the one free function and it is consumed by both the RTS layer and the HUD.

---

## sandbox/source/sim/weapon_def.h

- **Path:** `sandbox/source/sim/weapon_def.h`
- **Purpose:** Defines the immutable weapon stat block, its fixed registry, and the instantiation entry point; documents the `.weapon` file format.
- **Key types/functions:** `WEAPON_REGISTRY_MAX` (32); `struct WeaponDef` (17 fields); `struct WeaponRegistry`; `weapon_registry_load`, `weapon_registry_find`, `weapon_instantiate(const WeaponDef*, VesselFaction)`.
- **Notable non-obvious dependencies:**
  - **Deliberately mirrors the module registry** — the header calls it "the module registry twin" — with the same manifest-plus-per-file layout, the same fixed pool, and the same size rule. Two parallel data systems built to one pattern.
  - **Instances point their `name` and `icon` *into* the def's pool storage**, which the header justifies as safe because the pool is fixed and never reallocates. That makes the registry's lifetime a hard requirement for every live weapon — it lives in `game_state`, so it outlasts everything.
  - **Documents the whole `.weapon` format inline** with per-field annotations, including which fields are levers for which system: `cap_cost` is "the economy lever", `emission` is "the stealth lever", `proj_hp` governs how hard point defense kills the shot.
  - **`reach = speed * proj_life` is stated in the format documentation**, tying the data file directly to the single-source-of-truth reach function.
  - **`price` and `tier` are declared and marked "market-forward: unused v1"** — schema reserved for a market system that does not yet trade weapons.
  - Includes both `sim/ship.h` and `sim/weapon.h`, so anything touching weapon defs pulls in the whole ship and weapon hierarchy.
  - `weapon_instantiate` returns a raw heap `Weapon*` with no matching destroy function declared, leaving ownership entirely to the caller.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/weapon_def.cpp

- **Path:** `sandbox/source/sim/weapon_def.cpp`
- **Purpose:** Parses `.weapon` files into the registry and builds concrete weapon instances from defs.
- **Key types/functions:** `weapon_registry_load`, `weapon_registry_find`, `weapon_instantiate`; statics `weapon_def_load`, `weapon_kind_from_token`, `weapon_size_from_token`, `weapon_rstrip`.
- **Notable non-obvious dependencies:**
  - **Structurally near-identical to `module.cpp`** — same `_CRT_SECURE_NO_WARNINGS` preamble, same manifest loop, same `rstrip`, same quoted-string hand-parsing, same graded failure policy. Four helper functions are duplicated between the two files with only the token tables differing.
  - **Every stat has an "archetype-neutral" default** applied before parsing, so a `.weapon` file naming only an `id` still produces a working baseline gauss cannon. The defaults are documented as matching `docs/POINT_DEFENSE_AND_MISSILES.md` tuning — a numeric agreement with an external document.
  - **Those defaults duplicate values that also appear elsewhere**: `proj_speed` 12000 matches the missile-tuning comment in `game.cpp` ("< cannon shell speed (12000)"), and `cap_cost` 4.0 matches `BallisticWeapon`'s constructor.
  - **`weapon_instantiate` is where data meets the class hierarchy** — it selects `BallisticWeapon` or `MissileLauncher` on `def->kind` and copies stats in, which is why "no hardcoded factories remain".
  - Missing or invalid `id` is the only fatal per-file condition; unknown kind or size tokens warn and keep the default, so a typo silently produces a ballistic medium weapon.
  - Registry overflow past 32 warns and drops the remainder.
- **Interface vs internal:** Implementation of a public sandbox interface, called from `game_init` and the loadout paths.

---

## sandbox/source/sim/editor_tools.cpp

- **Path:** `sandbox/source/sim/editor_tools.cpp`
- **Purpose:** Implements the in-world entity editor — picking lights and ships, dragging via transform gizmos, and the gizmo geometry the renderer draws.
- **Key types/functions:** `update_edit_mode`, `edit_entity_position`, `edit_pick_gizmo`, `gizmo_axis_len`, `gizmo_ring_radius_ship`, `gizmo_ring_radius_light`; statics `edit_entity_angle`, `edit_entity_set_position`, `edit_entity_set_angle`, `edit_pick`.
- **Notable non-obvious dependencies:**
  - **Only two entities are editable, hardcoded** — selection index 0 maps to the player ship and anything else to the enemy, in four separate switch statements. The fleet's other members cannot be selected despite `EDIT_SHIP` looking general.
  - **Lights are stored as `Vec2` and round-tripped through `HierPos2`** on every pick and every write (`hierpos_from_vec2` / `hierpos_to_vec2`), so editor lights are the one entity class not natively in the hierarchical frame.
  - **Gates on both UI facades** (`bs_imgui_wants_mouse` and `bs_rml_wants_mouse`) so dragging does not fire while the cursor is over a panel — the arbitration contract those headers describe.
  - **`edit_pick_gizmo` is called by the *renderer* for hover feedback**, which is why it is exported: the header states it exists so the highlight matches `update_edit_mode`'s hit-test rather than duplicating it.
  - **All gizmo sizes take `zoom_inv`** and are documented as target screen pixels × inverse zoom, keeping the gizmo screen-constant — the same manual convention used across the render tree.
  - Lights are picked by centre with a 20-pixel tolerance rather than by radius, a deliberate choice noted in a comment.
  - Reads the engine input singleton directly for the left button.
- **Interface vs internal:** Public interface within the sandbox — six exported functions split between the update path and the gizmo renderer.

---

## sandbox/source/sim/editor_tools.h

- **Path:** `sandbox/source/sim/editor_tools.h`
- **Purpose:** Declares the edit-mode update, the selection-position query, the gizmo hit-test, and four gizmo geometry helpers.
- **Key types/functions:** `update_edit_mode`, `edit_entity_position`, `edit_pick_gizmo`, `gizmo_axis_len`, `gizmo_ring_radius_ship`, `gizmo_ring_radius_light`, `gizmo_arrow_size`; forward-declares `game_state`, `EditSelection`, `Ship`, and `enum EditDragMode : int`.
- **Notable non-obvious dependencies:**
  - **Forward-declares a scoped enum with its underlying type** (`enum EditDragMode : int`), a C++11 opaque declaration — the only place in the sandbox that technique appears, used to avoid including `game_state.h`.
  - **Splits the API by consumer**: `update_edit_mode` for the update path, the rest for the gizmo renderer, with the comment explaining that `edit_pick_gizmo` exists so hover feedback matches the real hit-test.
  - **States the input gating** (`bs_imgui_wants_mouse`) as part of the contract, though the implementation also checks the RmlUi facade.
  - Documents the screen-constant sizing convention for all four geometry helpers in one line.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/fleet.cpp

- **Path:** `sandbox/source/sim/fleet.cpp`
- **Purpose:** Implements fleet membership, selection, orders, FTL jumps, per-ship autopilot, and pose integration.
- **Key types/functions:** `FleetShip::simulate` / `update_move` / `update_attack` / `clear_move_target` / `clear_attack_target`; the `Fleet` class methods (membership, piloting, selection, orders, jump, `update_autopilot`, `simulate_all`); statics `normalize_angle`, `find_combat_entity_for_ship`; ten autopilot tuning constants.
- **Notable non-obvious dependencies:**
  - **`FleetShip::simulate` owns the auto-stabilise half of the turn model** that `ship_control.cpp` deliberately excludes — the two files implement complementary halves of one behaviour, and the reasoning lives in `ship_control.cpp`'s comments.
  - **Autopilot uses its own strafing controller rather than `steering::apply`** — desired velocity, velocity error, gain, then projection onto local thrusters — so the fleet and the NPC AI move through two different locomotion implementations despite `steering.h` claiming to be the shared layer.
  - **Braking distance is computed physically** (`sqrt(2 * decel * dist)`), so ships decelerate correctly into a move target rather than overshooting.
  - **Attack range is derived from `weapon_effective_reach` × 0.85**, with a 10000-unit fallback for an unarmed hull — the RTS side of the single-source-of-truth reach guarantee.
  - **`find_combat_entity_for_ship` linear-scans the combat entity array** to validate an attack target each frame, coupling the fleet to the combat arena's mirror structures.
  - **Formation spacing is derived from the largest bounding radius** (× 2.5, floored at 400 units), so formations widen automatically for bigger hulls.
  - Fires weapons directly from the autopilot when aligned within 0.25 rad and outside a 150-unit minimum range.
- **Interface vs internal:** Public interface within the sandbox — the `Fleet` object is a `game_state` member driven by the update path and the RTS layer.

---

## sandbox/source/sim/fleet.h

- **Path:** `sandbox/source/sim/fleet.h`
- **Purpose:** Defines the fleet data model — per-ship flight state, order state, and the `Fleet` container with selection, orders, jumps, and simulation.
- **Key types/functions:** `FLEET_MAX_SHIPS` (8); `JUMP_RADIUS_DEFAULT` (2e9); `enum ShipType`; `struct ShipFlight`; `struct FleetShip`; `class Fleet` (~25 methods, four private members).
- **Notable non-obvious dependencies:**
  - **The fixed array is a stability guarantee, not just a size cap** — the header states ships live in a fixed array "so `Ship*` pointers handed to the combat / render systems stay stable for the lifetime of the fleet". Combat entities hold raw `Ship*`, so this is load-bearing.
  - **`JUMP_RADIUS_DEFAULT` is calibrated to `GALAXY_GRID_CELL` (2e9)** so one jump reaches a neighbouring system — a gameplay constant tied numerically to the galaxy generator's spacing, in a different header.
  - **`set_count` truncates the active fleet without destroying escort data**, keeping a `m_spawned` high-water mark so the editor's "multiple ship command" toggle can hide and restore escorts instantly. That is why `game_init` spawns four escorts then immediately sets the count to 1.
  - **Move and attack targets are independent by design** — a ship can strafe toward a destination while its nose tracks and fires at a different target, stated explicitly.
  - **`Fleet` is one of only two real C++ classes in the sandbox** (with `RtsControls`), using private members and accessors where everything else is a POD struct.
  - `flight_for_ship(const Ship*)` exists to map a raw `Ship*` back to its flight state, the reverse lookup combat entities need.
  - `ShipType::SHIP_TYPE_EXTRACTOR` is declared but documented as unpopulated — reserved.
- **Interface vs internal:** Public interface within the sandbox — the fleet is central to control, combat, rendering, and the HUD.

---

## sandbox/source/sim/combat_arena.h

- **Path:** `sandbox/source/sim/combat_arena.h`
- **Purpose:** Declares the combat-arena subsystem — the combat-entity mirror of the fleet and enemy, encounter detection, enemy AI, entity sync, and projectile advancement.
- **Key types/functions:** `ENEMY_DETECTOR_RADIUS` (120000); `combat_arena_init`, `_rebuild_player_entities`, `_update_encounter`, `_update_enemy_orbit`, `_update_enemy_ai`, `_sync_entities`, `_update_projectiles`.
- **Notable non-obvious dependencies:**
  - **The header is largely a specification of call order** — `sync_entities` after fleet integration, `update_projectiles` after `sync_entities` — which, combined with `point_defense.h`'s constraints, defines a four-step chain across three files that nothing enforces.
  - **`ENEMY_DETECTOR_RADIUS` is documented as deliberately larger than the flagship's sensor layers** so enemy projectiles sweep inward through Layer 2 → 1 → 0, i.e. a gameplay constant chosen to exercise the sensor system for testing.
  - **Draws an explicit simulation/rendering boundary** and names where the render half lives (`ship_scene.cpp`, `heat_map.cpp`, and a `render/game_hud.cpp` that does not exist in the tree — a stale reference).
  - **Two enemy behaviours are declared side by side** — `update_enemy_orbit` ("hardcoded demo motion") and `update_enemy_ai` ("STATIC enemy AI… Replaces the orbit demo") — so the superseded function is still exported.
  - **`rebuild_player_entities` exists because the NPC entity window is packed after the player window**, so changing the active fleet count must recompute `npc_combat_base` — an index-layout dependency between the fleet and the AI subsystem.
  - Notes that weapon firing deliberately stays in `game_update`'s input path rather than here.
- **Interface vs internal:** Public interface within the sandbox — seven entry points driven from `game_init` and the update path.

---

## sandbox/source/sim/rts_controls.h

- **Path:** `sandbox/source/sim/rts_controls.h`
- **Purpose:** Declares the RTS control layer — selection box state, the `RtsControls` class, and an inline world-box containment test.
- **Key types/functions:** `inline ship_inside_world_box(...)`; `struct RtsSelectionBox`; `class RtsControls` (constructors, `update`, `draw`, `piloted_index`, `jump_mode_active`, `hud_toggle_pilot_mode`, plus seven private draw helpers).
- **Notable non-obvious dependencies:**
  - **`ship_inside_world_box` is inline in the header and works in a min-relative frame**, explicitly "so the test stays precise far from the galaxy origin" — box selection is one of the places naive `f32` comparison would fail at galaxy scale.
  - **Holds a `game_state*` member and is constructed with it**, which is why `game_init` placement-news it separately from the rest of the state — the only member needing a constructor argument.
  - **Three accessors exist purely to feed the RmlUi HUD snapshot** (`piloted_index`, `jump_mode_active`, `hud_toggle_pilot_mode`), documented as such — the HUD reads private state through a narrow window rather than the class exposing it.
  - **`hud_toggle_pilot_mode` is a no-op during a recenter glide**, with the button rendered dimmed — UI state coupling declared in a sim header.
  - **The class both owns input state and draws**, with seven private rendering helpers, so it spans simulation and presentation in one object.
  - The comment records a refactor: orders are now executed by `Fleet`, leaving this module owning only input state and transition logic.
- **Interface vs internal:** Public interface within the sandbox — the object is a `game_state` member driven from the update and overlay paths.

---

## sandbox/source/sim/combat_arena.cpp

- **Path:** `sandbox/source/sim/combat_arena.cpp`
- **Purpose:** Implements the combat simulation — combat-entity registration and sync, encounter detection, the static enemy AI, missile steering, and projectile/entity collision resolution.
- **Key types/functions:** `combat_arena_init`, `_rebuild_player_entities`, `_update_encounter`, `_update_enemy_orbit`, `_update_enemy_ai`, `_sync_entities`, `_update_projectiles` (plus the missile steering pass the projectile header names).
- **Notable non-obvious dependencies:**
  - **`combat_arena_init` sets ~20 tuning fields that belong to other subsystems** — sensor range, heat-map palette and colours, metaball radius and threshold, tail length and fade, warp strength, venn sharpness, and the encounter flags. It is the de-facto initialiser for the heat map and sensor FX as well as combat.
  - **The combat-entity array is a manually partitioned window**: slot 0 is the enemy, slots 1..N mirror the active fleet, and `npc_combat_base` marks where AI agents append. `rebuild_player_entities` must run whenever the active fleet count changes or the two windows overlap.
  - **Entities hold raw `Ship*` back-pointers**, which is why `Fleet`'s fixed array is documented as a stability guarantee.
  - **`ship_sensor_range` is set to `SENSOR_LAYER1_RADIUS`** with the reasoning that an enemy hull should resolve into a real sprite exactly when the sensor suite says it is identified — a deliberate numeric coupling between rendering and the sensor model.
  - **Consults the galaxy history for per-entity hostility** (`galaxy_history_faction_is_hostile`), so whether a shot can hit depends on the deep-time diplomacy simulation.
  - **Calls into the NPC AI to apply damage** (`ai_ship_damage`), making combat the bridge between the projectile pool and the agent system.
  - Uses `point_in_polygon` from `geom2d` for hull hit-testing, one of that module's two named consumers.
  - Both the superseded orbit demo and the current static AI are implemented here side by side.
- **Interface vs internal:** Public interface within the sandbox — the combat subsystem's implementation, driven from `game_init` and the update chain.

---

## sandbox/source/sim/rts_controls.cpp

- **Path:** `sandbox/source/sim/rts_controls.cpp`
- **Purpose:** Implements the RTS control layer — hover detection, box and click selection, move/attack orders, FTL jump mode, free-camera movement, and all the selection/order overlay drawing.
- **Key types/functions:** `RtsControls` constructors, `::update`, `::draw`, `::hud_toggle_pilot_mode`, `::clear_fleet_orders`, and the six private draw helpers; static `read_wasd_dir`; ~16 appearance and behaviour constants.
- **Notable non-obvious dependencies:**
  - **`HOVER_CIRCLE_LAYER` is the literal `50` with the comment "same as `LAYER_UI` in game.cpp"** — a hand-copied layer constant rather than an include of `render_layers.h`, so the two can drift.
  - **Gates on both UI facades** (`bs_imgui_wants_mouse`, `bs_rml_wants_mouse`) before acting on clicks, the same arbitration `editor_tools` performs.
  - **Calls `galaxy_pick_planet` from the render tree** — a sim module reaching into `galaxy_map_render.h` for a hit-test, one of the two places that inverted dependency occurs.
  - **Free-camera movement is a separate locomotion model** with its own constants (1600 units/s, ×3 on shift, 24-pixel edge pan margin), independent of both the ship flight model and `steering`.
  - **A 4-pixel drag threshold distinguishes a click from a box selection**, the kind of UX constant that only exists at one call site.
  - **Both a default and a `game_state*` constructor exist**, with the default leaving `m_state` null — the object is a `game_state` member so the default runs first during placement-new, then `game_init` re-constructs it with the pointer.
  - Two `constexpr bs_color` initialisers rely on aggregate initialisation of an engine POD in a `constexpr` context.
  - The class draws its own overlays rather than exposing state for a render pass, unlike every other sim module.
- **Interface vs internal:** Implementation of a public sandbox class embedded in `game_state`.

---

## sandbox/source/sim/galaxy_rng.h

- **Path:** `sandbox/source/sim/galaxy_rng.h`
- **Purpose:** A header-only deterministic PRNG (splitmix64) shared by the galaxy-scale generators.
- **Key types/functions:** `galaxy_splitmix64(u64)`, `galaxy_seed_for(u64 master, i32 index)`, `struct GalaxyRng`, `galaxy_rng_seed`, `galaxy_rng_next`, `galaxy_rng_f32`, `galaxy_rng_range`, `galaxy_rng_int`.
- **Notable non-obvious dependencies:**
  - **Defines the seed hierarchy the whole galaxy depends on** — master seed → per-node seed → per-body seed — so worldgen is reproducible from the single seed the player picks on the New Game screen.
  - **`galaxy_seed_for` is deliberately index-invariant**, hashing `master ^ (golden_ratio * (index+1))` so adding or removing systems never disturbs an unrelated index's seed. That is what lets the generator stream systems in and out of a hot cache and get identical content each time.
  - **`ss_generation.cpp` keeps its *own* equivalent splitmix64** seeded from the node seed produced here, which the comment states explicitly — two copies of the same algorithm, deliberately, so a node's contents are fully determined by its seed.
  - **All functions are `static inline` in a header**, so every including TU gets private copies; combined with the third copy in `starfield_generator.cpp` (a different LCG), the project has three independent RNGs.
  - `galaxy_rng_f32` uses only the low 24 bits, and `galaxy_rng_int` uses modulo, so both carry mild distribution bias — acceptable for content generation, worth knowing.
- **Interface vs internal:** Public interface within the sandbox — the shared determinism primitive for worldgen.

---

## sandbox/source/sim/galaxy_params.h

- **Path:** `sandbox/source/sim/galaxy_params.h`
- **Purpose:** Defines the galaxy morphology enum and the concrete generation-parameter struct, as pure data with no logic.
- **Key types/functions:** `enum GalaxyShape` (spiral, barred, elliptical, ring, irregular, flocculent, + count); `struct GalaxyGenParams` (~20 fields covering disc extent, arms, bulge, bar, ring, clumps, ellipticity, orientation, and colour biases).
- **Notable non-obvious dependencies:**
  - **The header exists specifically to break an include cycle**, and says so: `game_state.h` embeds the params in `GalaxyState` and the shape in `GalaxySetupParams`, while `galaxy_gen.{h,cpp}` needs both — so the shared POD was factored out.
  - **States the determinism contract**: "A `GalaxyGenParams` is a pure function of (shape, master_seed)", with some fields randomised from the seed. That is what makes the galaxy reproducible from the setup screen.
  - **Documents the three-stage pipeline** the struct feeds — `galaxy_params_for_shape()` builds it, `place_nodes()` samples positions from it, `galaxy_env_at()` derives each star's population and colour — so the struct's consumers are named at its definition.
  - **Uses `f64` for every length** (disc scale, radii, bar extent) while colours and fractions stay `f32` — the precision split that galaxy-scale coordinates require.
  - Fields are explicitly shape-conditional ("unused fields for a given shape are simply ignored"), so the struct is a union of six generators' inputs rather than a minimal set.
  - The two colour biases are annotated with which shapes push them high, tying morphology to stellar population.
- **Interface vs internal:** Public interface within the sandbox — a shared data contract between the state definition and the generator.

---

## sandbox/source/sim/galaxy_gen.h

- **Path:** `sandbox/source/sim/galaxy_gen.h`
- **Purpose:** Declares the galaxy generator — node placement tunables, the structural-environment query, the shape preset builder, and generate/free.
- **Key types/functions:** `GALAXY_TARGET_SYSTEMS` (10000), `GALAXY_DISC_RMAX` (3.2e11), `GALAXY_MIN_SEPARATION` (1.6e9), `GALAXY_GRID_CELL` (2.0e9), `GALAXY_KNN_K` (8), `GALAXY_LANE_ADDBACK` (0.20), plus nine spiral-structure constants; `struct SSGenEnv`; `galaxy_params_for_shape`, `galaxy_arm_angle`, `galaxy_env_at`, `galaxy_generate`, `galaxy_free`.
- **Notable non-obvious dependencies:**
  - **The separation constants are derived from a physical constraint, and the header shows the arithmetic**: typical neighbour spacing must exceed a star system's outer-orbit *diameter* (~4.0e8) or neighbouring systems' orbits would intersect, so `GALAXY_MIN_SEPARATION` is set to ~4× that "so interstellar space reads as EMPTY relative to system size". Blue-noise rejection during placement enforces it.
  - **`SSGenEnv` exists to guarantee a specific consistency property**, spelled out at length: a system's summarised map dot (generated from its true world position) and the system the player later flies into (materialised from `{0,0}` then re-anchored for precision) must derive the *same* star population and colour. Making the environment a pure function of position is what makes that hold.
  - **`GALAXY_GRID_CELL` (2e9) is simultaneously the spatial-grid bucket size, the typical neighbour spacing, and the value `fleet.h`'s `JUMP_RADIUS_DEFAULT` is calibrated against** — one number tying generation, spatial indexing, and FTL range together across three headers.
  - **Node contents are explicitly *not* stored** — planets and orbits materialise lazily from `node.seed` near the camera — so this header defines the summary layer and `galaxy_map.cpp` owns the hot cache.
  - **Node 0 is forced to the origin ("Sol")** so the player start stays valid, which is why `game_init` sets `camera_hierpos` to `{0,0}` before generation.
  - **Allocates from `MEMORY_TAG_GAME`**, so the node, grid, and lane arrays do appear in the engine's memory accounting.
  - The lane graph is documented as kNN → MST → add-back, with the MST guaranteeing connectivity that `galaxy_route_find` later relies on.
- **Interface vs internal:** Public interface within the sandbox — the worldgen entry point plus a pure query used by both generation and rendering.

---

## sandbox/source/sim/galaxy_spatial.h

- **Path:** `sandbox/source/sim/galaxy_spatial.h`
- **Purpose:** Declares the uniform bucket grid providing nearest-node and radius queries over the ~10,000 galaxy nodes.
- **Key types/functions:** `struct GalaxySpatialGrid` (origin, cell size, dimensions, CSR `cell_start` / `node_order`, count); `galaxy_grid_build`, `galaxy_grid_free`, `galaxy_grid_nearest`, `galaxy_grid_query_radius`.
- **Notable non-obvious dependencies:**
  - **States an immutability guarantee with a threading consequence**: built once at generation and never mutated, so all queries are pure reads "safe to call lock-free from any thread". That is one of very few concurrency statements anywhere in the project.
  - **Records what it replaced** — an O(N) Voronoi nearest-site scan with a fixed 64-site cap — so the file documents both the current design and the limitation that motivated it.
  - **Uses counting-sorted CSR storage** (`cell_start[c]..cell_start[c+1]` indexing `node_order`), the same layout the lane graph uses, allocated from `MEMORY_TAG_GAME`.
  - **Forward-declares `GalaxyNode` whose definition lives in `state/game_state.h`** — so a `sim` module's spatial index depends on a type declared in the god-struct header.
  - `galaxy_grid_query_radius` silently drops candidates past `max` rather than reporting truncation.
- **Interface vs internal:** Public interface within the sandbox — the spatial backbone under `galaxy_nearest_node` and the materialisation logic.

---

## sandbox/source/sim/galaxy_map.h

- **Path:** `sandbox/source/sim/galaxy_map.h`
- **Purpose:** Declares the galaxy-map subsystem — generation staging, per-frame upkeep, the hot-cache materialisation, lane routing, and deterministic station identity.
- **Key types/functions:** `GALAXY_MATERIALIZE_RADIUS` (2.4e9); `galaxy_map_init`, `_worldgen`, `_finalize`, `_sync_entities`, `_update_orbits`, `galaxy_materialize_update`, `galaxy_nearest_node`, `galaxy_ensure_materialized`, `galaxy_route_find`, `galaxy_lane_length`; `station_id_make` / `_node` / `_index`; `struct StationLayoutEntry`; `galaxy_node_station_layout`, `galaxy_station_pos_by_id`.
- **Notable non-obvious dependencies:**
  - **`GALAXY_MATERIALIZE_RADIUS` is deliberately smaller than the ~2e9 inter-system spacing**, so only a few non-overlapping neighbours ever draw full orbit detail at once — a rendering budget expressed as a simulation constant.
  - **The hot cache is the central mechanism**: nodes near the camera materialise into full `StarSystem`s, distant ones stay summary dots, and `s->galaxy.current_system` is a *cache slot*, not a node index. That is why `galaxy_map_render.h` warns its returned slot is valid only for the current frame.
  - **Far systems are inert** — `galaxy_map_update_orbits` advances only materialised systems, so orbital motion literally does not run outside the camera's neighbourhood.
  - **Station identity is a packed `(node << 8) | index` id** with a deterministic layout reproducible from the *lightweight node summary* alone, explicitly so the macro mission layer can locate stations without materialising the system. Two code paths must produce identical positions, and the header names the function that must agree (`generate_system_stations`).
  - **The staged generation split exists for the progress bar** — `worldgen` then `finalize`, with the history stages running in between — matching `run_generation_stage`'s five-step pipeline in `game.cpp`.
  - **`galaxy_route_find` relies on the MST spanning tree for its connectivity guarantee**, a property established in `galaxy_gen.h` and consumed here.
  - Notes that `sensor_visibility_from_dist` / `get_sensor_visibility` are declared in `state/game_state.h` but *defined* in this module — a split that explains why three render files can call them without including this header.
- **Interface vs internal:** Public interface within the sandbox — one of the widest, spanning generation, per-frame upkeep, routing, and station queries.

---

## sandbox/source/sim/galaxy_gen.cpp

- **Path:** `sandbox/source/sim/galaxy_gen.cpp`
- **Purpose:** Generates the galaxy — places ~10,000 nodes on a structured disc, derives each node's deterministic summary, builds the spatial grid, and constructs the travel-lane graph.
- **Key types/functions:** `galaxy_generate`, `galaxy_free`, `galaxy_params_for_shape`, `galaxy_arm_angle`, `galaxy_env_at`; statics `galaxy_system_name`, `fill_node_summary`, plus the placement and lane-graph passes.
- **Notable non-obvious dependencies:**
  - **System names come from an LCG permutation, not a hash**, and the header comment justifies it: `(i*48271 + 12345) mod 676000` is a bijection because 48271 is coprime to the modulus, so distinct indices can never collide — where a plain hash would collide birthday-paradox often. Naming correctness rests on a number-theory property stated in the comment.
  - **`fill_node_summary` generates a full `StarSystem` and throws it away**, keeping only colour, radius, sorted orbit radii, and habitability. The comment explains why this is safe rather than wasteful: the same seed plus environment re-derives the identical system during lazy materialisation, so the summary always matches what the player later flies into.
  - **Habitability is harvested "FREE" from that discarded system** and becomes the substrate the galaxy history uses to seed civilization cradles — so worldgen and deep-time history are coupled through a byte per node.
  - **`using GalaxyState = game_state::GalaxyState`** — the galaxy state is a *nested* struct of the god struct, so generator internals must alias it to name it at all.
  - Orbit radii are insertion-sorted into the summary so downstream consumers (zone lookup, station layout) can assume ascending order.
  - Depends on `ss_generation` for the actual star-system content and on `galaxy_rng` for the seed hierarchy; allocates node, grid, and lane arrays from `MEMORY_TAG_GAME`.
- **Interface vs internal:** Implementation of a public sandbox interface, invoked once per new game through the staged generation pipeline.

---

## sandbox/source/sim/galaxy_spatial.cpp

- **Path:** `sandbox/source/sim/galaxy_spatial.cpp`
- **Purpose:** Builds and queries the uniform bucket grid over galaxy nodes.
- **Key types/functions:** `galaxy_grid_build`, `galaxy_grid_free`, `galaxy_grid_nearest`, `galaxy_grid_query_radius`; statics `node_world`, `clampi`.
- **Notable non-obvious dependencies:**
  - **Builds by counting sort into CSR** — count per cell, prefix-sum, scatter — a three-pass construction that allocates exactly two arrays and never reallocates, which is what underwrites the header's lock-free-read guarantee.
  - **Converts every node position through `hierpos_to_f64`** on both build passes, so the grid works in absolute `f64` world space rather than the hierarchical frame — acceptable because `f64` has ample precision at 3.2e11 and the grid is only an index.
  - **Adds a one-cell margin to the origin and two cells to each dimension** so edge nodes cannot fall outside the grid after the floor division.
  - **Clamps cell indices defensively** even after the margin, so a node exactly at the AABB maximum still lands in-bounds.
  - Allocates from `MEMORY_TAG_GAME`, making the grid visible in the engine's memory accounting, unlike the Voronoi structures.
  - Grid dimensions derive from the node AABB rather than the configured disc radius, so an unusually compact galaxy produces a correspondingly smaller grid.
- **Interface vs internal:** Implementation of a public sandbox interface.

---

## sandbox/source/sim/galaxy_map.cpp

- **Path:** `sandbox/source/sim/galaxy_map.cpp`
- **Purpose:** Owns the galaxy hot cache — staged generation, per-frame materialisation and eviction, orbital updates for cached systems, map-entity sync, lane routing, and deterministic station layout.
- **Key types/functions:** `galaxy_map_init`, `_worldgen`, `_finalize`, `_sync_entities`, `_update_orbits`, `galaxy_materialize_update`, `galaxy_nearest_node`, `galaxy_ensure_materialized`, `galaxy_route_find`, `galaxy_lane_length`, `galaxy_node_station_layout`, `galaxy_station_pos_by_id`, `generate_system_stations`, plus `sensor_visibility_from_dist` / `get_sensor_visibility`; statics `station_mix`, `station_near_habited_gen`.
- **Notable non-obvious dependencies:**
  - **Defines two functions declared in `state/game_state.h`** — `sensor_visibility_from_dist` and `get_sensor_visibility` — which is why three render modules can call them without including this header. A declaration/definition split across unrelated files.
  - **`GALAXY_MASTER_SEED` is a file-static constant** (`0x9E3779B9…`) that duplicates the value `game_init` writes into `s->setup.seed`, so the "player-chosen seed" and this hardcoded one currently coincide.
  - **Uses a third, independent splitmix64 (`station_mix`)** deliberately isolated so station layout rolls never perturb the star-system RNG stream — determinism preserved by keeping generators from sharing state. That is the third copy of splitmix64 in the sandbox.
  - **Reads a *frozen* ownership snapshot (`node_owner_gen`) rather than live borders** when deciding whether an uninhabited system gets stations, explicitly so the answer stays stable for the session regardless of how the history simulation shifts territory. Live and frozen ownership are two distinct arrays with different purposes.
  - **Station spawn policy is a documented rule set** — full set when habited now, otherwise a 10% deterministic roll (+10% within 1–2 lane hops of a system habited at generation), and half the count when it does spawn — with the two-hop search walking the lane graph's CSR adjacency directly.
  - **The station layout must match between two code paths** — this module's materialisation and `galaxy_node_station_layout`'s summary-only reproduction — since the mission layer locates stations without materialising systems.
  - Allocates route-finder scratch through `bs_memory_allocator` per call rather than keeping a persistent buffer.
  - Retains a `BS_LOG_INFO` marked "temp Phase 0 verification".
- **Interface vs internal:** Implementation of one of the sandbox's widest public interfaces, plus two functions belonging to another header.

---

## sandbox/source/sim/galaxy_history.cpp

- **Path:** `sandbox/source/sim/galaxy_history.cpp`
- **Purpose:** The deep-time civilization simulation — seeds civs onto habitable cradles, runs a resumable year-stepped history producing territory, dynasties, wars and a chronicle, keeps it living during play, and builds five ImGui browser windows. At 1455 lines it is the third-largest sandbox file.
- **Key types/functions:**
  - Simulation: `galaxy_history_sim_begin`, `_sim_step`, `_finalize_view`, `_sim_free`, `_sim_progress`, `_generate`, `_live_tick`, `_log_summary`.
  - Statics driving the model: `ethos_affinity`, `civ_aggression`, `civ_make_stem`, `civ_compose_name`, `house_shade`, `pick_free_cradle`, `house_register`, `sim_spawn_root` / `_house` / `_successor`, `sim_fragment_civ`, `evt_importance`, `live_feed_push`, `hist_add`, `rel_score_add`.
  - Garrison layer: `garrison_capacity`, `garrison_seed`, `garrison_step`, `galaxy_history_seed_garrison`, `_garrison_at`, `_garrison_add`.
  - Queries: `_is_hostile`, `_civ_at_war`, `_civ_allied`, `_faction_is_hostile`, `_factions_hostile`, `_faction_label`, `_civ_at_node`, `_owner_at_node`, `_player_rep`.
  - Player coupling: `_player_raid`, `_player_aid`.
  - UI: `_build_legends`, `_build_news`, `_build_houses`, `_build_inspector`, `_build_gov_interaction` and four per-government builders.
  - Debug: `_debug_war_frontier`.
- **Notable non-obvious dependencies:**
  - **Keeps a resident simulation working-set after generation** so the "living present" can keep stepping the same state during play — `sim_free` is idempotent and called at regeneration. The history is therefore not a one-shot precompute but a paused simulation.
  - **Chunked and resumable by design** (`sim_begin` / `sim_step(max_steps)` / `finalize_view`) purely so the New Game progress bar can advance by simulated year — a UI requirement shaping the simulation's control flow.
  - **Two ownership arrays with different semantics** — the live `node_owner` and the frozen `node_owner_gen` snapshot that `galaxy_map.cpp` reads for stable station spawning.
  - **Builds ImGui windows directly** (`bs_ui.h`) for five browsers, so a simulation module owns a large amount of presentation — the same concern-mixing as `star_fx.cpp` and `profiler.cpp`.
  - **Allocates territory and relation arrays from `MEMORY_TAG_GAME`** and maintains a relation matrix mutated through `rel_score_add`.
  - **The faction stance functions are the game's diplomacy authority**, consumed by combat hit filtering, NPC target acquisition, patrol labelling, and overlay colouring. `faction_is_hostile` explicitly folds transitive diplomacy (an ally's enemies read hostile), and negative ids encode static factions.
  - **The player perturbs the macro sim** through `player_raid` / `player_aid`, whose effects surface in the news feed and then propagate — coupling minute-to-minute play into deep-time state.
  - **A garrison layer runs player-independently** — seeded from ownership, evolved each living tick, read by NPC materialisation, written back by player kills — so NPC density is an emergent property of the history.
  - Uses the shared `galaxy_rng` seed hierarchy, keeping the whole history reproducible from the master seed.
  - `debug_war_frontier` can force a war between two civs to make a frontier grind, a testing hook wired to a keybinding.
- **Interface vs internal:** Public interface within the sandbox — the widest single API in the game (~30 exported functions), spanning simulation, queries, player actions, and UI.

---

## sandbox/source/sim/galaxy_history.h

- **Path:** `sandbox/source/sim/galaxy_history.h`
- **Purpose:** Declares the galaxy history API — deep-time simulation staging, the living present, diplomacy queries, the garrison layer, player coupling, and the browser windows.
- **Key types/functions:** ~30 free functions across seven commented groups; no types of its own.
- **Notable non-obvious dependencies:**
  - **Explicitly delegates its types to `state/game_state.h`** — `Civilization`, `CivGovernment`, `CivEthos`, `GALAXY_CIV_MAX` live there "so `GalaxyState` can embed the `civs[]` array, mirroring `GalaxyNode` / `GalaxyLaneGraph`". The pattern is consistent across the galaxy subsystems: data in the god struct, behaviour in `sim/`.
  - **States the core modelling decision**: a civilization is ONE aggregate agent, not a swarm, chosen so the whole history stays "tractable + reproducible from the master seed". That single choice is why deep time is simulable at all.
  - **The comments carry a phase history** (Phase 1 origins → Phase 2 territory → Phase 3 chronicle → Phase B deep time → Phase C living present → Phase C2 player coupling → Feature B diplomacy → Step B garrison), so the header doubles as a development log.
  - **Three overlapping hostility queries exist** — `is_hostile(civ)`, `faction_is_hostile(faction_id)`, and `factions_hostile(a, b)` — each documented with different fold rules for the player, pirates, and wild space. Callers must pick the right one.
  - **Declares that four per-government builders are "exposed for testing"**, the only place in the sandbox where a symbol's visibility is justified by testability.
  - `galaxy_history_generate` is documented as a one-shot wrapper around begin/step-all/end used by `galaxy_map_init`, parallel to the staged path used by the New Game flow — two entry points into the same simulation.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/voronoi_galaxy.cpp

- **Path:** `sandbox/source/sim/voronoi_galaxy.cpp`
- **Purpose:** Wraps the vendored Voronoi library — generates territory cells from star positions, orders their vertices, answers containment queries, and draws cell and lane edges.
- **Key types/functions:** `generate_galaxy_voronoi`, `find_system_by_cell`, `draw_voronoi_edges`, `draw_delaunay_lanes`; static `sort_verts_by_angle`.
- **Notable non-obvious dependencies:**
  - **This is the single TU that defines `JC_VORONOI_IMPLEMENTATION`**, so the whole third-party library compiles here — under the sandbox's `-Wall -Werror`, unlike the engine's vendored libraries.
  - **`sort_verts_by_angle` does more than sort**: after an O(n²) bubble sort by `atan2`, it finds the largest angular gap and rotates the array so the gap sits at the boundary. The comment explains why — without it the `atan2` wraparound leaves consecutive array entries non-adjacent on the polygon, so edges would cross.
  - **Mixes generation, query, and rendering in one module**, unlike the surrounding sim/render split — `draw_voronoi_edges` and `draw_delaunay_lanes` submit draws directly.
  - **Includes `<new>`**, suggesting placement-new use for the diagram, and passes galaxy positions through `f32` `Vec2` into a library configured with `JCV_REAL_TYPE float` — the precision concern `HierPos2` exists to avoid.
  - Depends on `core/geom2d.cpp` for `point_in_polygon`, noted in a comment where the local copy used to live.
- **Interface vs internal:** Public interface within the sandbox — four exported functions consumed by the galaxy-map renderer and the hover effect.

---

## sandbox/source/sim/voronoi_galaxy.h

- **Path:** `sandbox/source/sim/voronoi_galaxy.h`
- **Purpose:** Declares the Voronoi diagram data structures and the four generate/query/draw functions.
- **Key types/functions:** `VORONOI_MAX_SITES` (64); `VORONOI_LAYER_CELESTIAL` (2) and `VORONOI_LAYER_UI` (50); `struct GalaxyVEdge`, `GalaxyDEdge`, `GalaxyVCell`, `GalaxyVoronoi`.
- **Notable non-obvious dependencies:**
  - **Declares its own layer constants with the comment "must match game.cpp"** — a third parallel layer vocabulary alongside `render_layers.h` and `rts_controls.cpp`'s hand-copied `50`.
  - **Caps at 64 sites** while the galaxy has ~10,000 nodes, so the Voronoi layer only ever covers the hot-cache neighbourhood. `galaxy_spatial.h` records that this fixed cap was one reason the spatial grid replaced Voronoi for nearest-site queries.
  - **`GalaxyVoronoi` is a large fixed POD** — 512 Voronoi edges, 512 Delaunay edges, and 64 cells each holding 32 vertices — roughly 40 KB embedded by value wherever it lives.
  - **Carries two fields owned by a different module** (`hovered_cell`, `hover_head_dist`), annotated "written by `voronoi_cell_hover_effect.cpp`" — presentation state living inside a simulation structure.
  - Notes the algorithm is Fortune's sweep-line at O(N log N) "for ~50 star systems", a scale that no longer matches the galaxy's size.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/ai_ship.cpp

- **Path:** `sandbox/source/sim/ai_ship.cpp`
- **Purpose:** The local NPC agent tier — hull and weapon templates, population materialisation, the behaviour state machine, perception, combat wings, and per-archetype behaviours (patrol, trader, miner, mission traveler). At 2524 lines it is the fourth-largest sandbox file.
- **Key types/functions:**
  - Lifecycle: `ai_profile`, `ai_ships_init`, `ai_ships_update`, `ai_ships_register_combat`, `ai_ship_damage`, `ai_ships_debug_spawn_strike`.
  - Population: `spawn_npc`, `system_anchor`, `materialize_system`, `materialize_wild_system`, `ai_ships_populate`, `ai_ships_sync_missions`.
  - Wings: `wing_slot_offset`, `wing_resolve_leader`, `wing_fly_slot`, `assign_wings`.
  - Behaviour: `ai_sense`, `ai_trader_tick`, `ai_miner_tick`, `ai_ambient_trader_tick`, `ai_fly_mission_leg`, `ai_miner_deliver`, `miner_acquire`, `nearest_station`, `planet_export_good`.
  - Statics `sm64`, `faction_tint`, `archetype_hull_path`, `archetype_weapon_id`.
- **Notable non-obvious dependencies:**
  - **`ai_ships_init` is the release path for shared weapon instances**, and the header explains why: the engine's `Game` struct exposes no shutdown hook, so idempotent re-init *is* how a previous set gets freed. A missing engine feature dictates the module's lifetime design.
  - **Loads hull templates from disk by archetype** (`archetype_hull_path`) and instantiates one shared `Weapon` per archetype from the registry — so all agents of a type share a weapon object, and its cooldown is therefore shared too.
  - **The NPC combat-entity window must stay packed after the player window** (`npc_combat_base`), a layout invariant maintained jointly with `combat_arena_rebuild_player_entities`.
  - **Bridges the macro and local tiers**: `ai_ships_sync_missions` materialises `ShipMission` travelers into live `NpcShip` hulls when they enter the player's system, and `ai_ship_damage` calls back into `ship_mission_notify_destroyed`.
  - **Kill attribution is faction-sensitive** — only `FACTION_PLAYER` kills raid the victim's civilization and cost reputation; NPC-vs-NPC kills merely shrink the garrison or retire the mission.
  - **Population density is read from the history's garrison layer**, so how many agents appear is an emergent output of the deep-time simulation rather than a constant.
  - **Uses a fourth independent splitmix64 (`sm64`)** for per-agent rolls, described as "deterministic-ish" — the qualifier acknowledging that agent spawning is not fully reproducible.
  - **Miners drive the economy directly** — they mine per-system asteroids, pick a good, and deliver to the nearest station via `station_market_apply`, closing a loop from ambient AI into market stock.
  - Combat wings are a triangular formation with leader resolution and slot-following, layered on top of the individual FSM.
  - The archetype table is the stated extension mechanism: "new ship kinds are added as data rows — no new AI code", though several `AiState` values remain unimplemented.
- **Interface vs internal:** Public interface within the sandbox — six exported functions, but the module reaches into combat, missions, markets, history, and asteroids.

---

## sandbox/source/sim/ai_ship.h

- **Path:** `sandbox/source/sim/ai_ship.h`
- **Purpose:** Declares the NPC agent archetypes, behaviour states, per-archetype tuning profile, and the six lifecycle/damage entry points.
- **Key types/functions:** `enum ShipArchetype` (7 + count); `enum AiState` (10 states); `struct AiProfile` (14 tunables); `ai_profile`, `ai_ships_init`, `_update`, `_register_combat`, `ai_ship_damage`, `ai_ships_debug_spawn_strike`.
- **Notable non-obvious dependencies:**
  - **States the design thesis plainly**: one FSM for every ship type, differentiated only by profile data and an `enabled_states` bitmask, "so new ship kinds are added as data rows — no new AI code".
  - **Phase annotations mark what is unbuilt** — several archetypes and FSM states are tagged "(Phase C)", so the enums describe an intended design larger than the implementation.
  - **The `ai_ships_init` comment documents an engine limitation as an architectural constraint** — no shutdown hook exists, so repeated idempotent init is the only release path for the shared weapon instances.
  - **`ai_ship_damage` takes the attacker's faction explicitly** and documents the differing consequences of player versus NPC kills — a gameplay rule stated at the interface.
  - `AiProfile` mixes implemented tunables with ones annotated for future phases, so the struct is partly aspirational.
  - Types live here while `NpcShip` itself is in `state/game_state.h`, the same data/behaviour split the galaxy modules use.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/ship_mission.cpp

- **Path:** `sandbox/source/sim/ship_mission.cpp`
- **Purpose:** The macro traveler tier — routes persistent agents across the lane graph, issues trade and military contracts, runs the civ economy settlement, tracks node risk, and handles raids. At 2735 lines it is the second-largest file in the sandbox after `game_state.h`.
- **Key types/functions:**
  - Exported: `ship_missions_seed`, `ship_missions_update`, `ship_mission_node_risk`, `ship_mission_position`, `ship_mission_notify_destroyed`.
  - Economy: `civ_trade_income`, `civ_afford`, `civ_take_hull`, `civ_return_hull`, `civ_fleet_cap`, `ship_missions_economy_tick`.
  - Trade: `node_is_market`, `trade_value`, `trade_tier_weight`, `trade_pick_market`, `mission_issue_contract`, `mission_dock_complete`.
  - Military: `mission_alloc_military`, `mission_issue_military`, `ship_missions_military_tick`.
  - Movement: `node_jump_radius`, `node_jump_point`, `mission_move_leg`, `mission_leg_complete`, `mission_repath`, `mission_next_hop`, `mission_travel_step`, `mission_stall_check`.
  - Risk: `node_risk_add`, `node_risk_get`.
- **Notable non-obvious dependencies:**
  - **Implements a closed economic loop the header names**: trade → wealth → power → fleets. Trade income accrues to civs, `civ_afford` and `civ_fleet_cap` gate military spending, and hull availability (`civ_take_hull` / `civ_return_hull`) limits how many missions a civ can field.
  - **Node risk is a feedback channel** — raider sorties and successful ambushes raise it, it decays each economy settlement, and both trade routing and the live AI read it, so interceptors are posted where lanes are actually bleeding.
  - **`mission_stall_check` exists because the state machine can deadlock**, an explicit watchdog on missions that wait too long at a target.
  - **Time is in in-game hours, and the header notes `1 real second == 1 in-game hour at 1x`** so `sim_dt_hours` doubles as seconds — the unit pun that lets one clock drive both tiers.
  - **Deliberately separate from the local tier** (`ai_ship.*`): macro travelers exist everywhere at all times, `NpcShip`s only in the player's system. `ai_ships_sync_missions` and `ship_mission_notify_destroyed` are the two seams between them.
  - **Writes into station markets** through the dock hooks, making trader arrivals the source of the delta pool `station_market.cpp` decays.
  - Uses a fifth independent splitmix64 for its own rolls.
  - The multi-stage travel machine (fly to station → load → fly to jump circle → jump → cross systems → dock) is documented in the header and implemented across ~600 lines of `mission_travel_step`.
- **Interface vs internal:** Public interface within the sandbox — five exported functions fronting by far the largest amount of hidden simulation.

---

## sandbox/source/sim/ship_mission.h

- **Path:** `sandbox/source/sim/ship_mission.h`
- **Purpose:** Declares the macro traveler tier's five entry points and explains its relationship to the local agent tier.
- **Key types/functions:** `ship_missions_seed`, `ship_missions_update`, `ship_mission_node_risk`, `ship_mission_position`, `ship_mission_notify_destroyed`; forward-declares `game_state` and `ShipMission`.
- **Notable non-obvious dependencies:**
  - **Draws the two-tier boundary explicitly** — macro travelers are persistent and galaxy-wide, moving in in-game hours; `NpcShip`s are transient and local. That separation is the module's organising principle.
  - **Documents the seeding precondition**: call once at generation end, after garrison seeding, when the node/lane graph and civ ownership are final.
  - **`ship_mission_position` is documented as stage-dependent** — station anchor during dock and cooldown, otherwise the integrated position — which is why rendering must call it rather than reading the field.
  - **Names the writeback path** from the local tier (`notify_destroyed`), so the bidirectional coupling is visible at the interface.
  - Notes that speeds come from `GalaxyState::ai_speed_in_system` / `ai_speed_jump`, i.e. editor-tunable fields in the god struct rather than constants here.
  - `ShipMission` itself is declared in `state/game_state.h`, continuing the data/behaviour split.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/station_market.cpp

- **Path:** `sandbox/source/sim/station_market.cpp`
- **Purpose:** Implements the station economy — the good catalogue, deterministic baseline markets, live stock with deltas, price formation, decay, and revenue tracking.
- **Key types/functions:** `TRADE_GOOD_NAMES`, `TRADE_CATEGORY_NAMES`, `GOOD_INFO` table; `trade_good_category`, `trade_good_base_price`, `station_specialization`, `station_rooms`, `station_market_baseline`, `station_market_get`, `station_market_apply`, `station_markets_decay`, `station_revenue_add`, `station_revenue_get`; `MARKET_DECAY_PER_HOUR` (2.0).
- **Notable non-obvious dependencies:**
  - **The entire price model is one documented formula** — `price = base_price * demand_mul * clamp(2 - stock/base_stock, 0.5, 2.0)` — so scarcity and glut map to price through a single clamped ratio.
  - **Ten goods in four categories with per-good volatility**, where refined and luxury goods swing more than raw staples — a data table that is the whole economic content.
  - **Baselines are pure functions of the station id**, biased by the *node's* abundance signals (habitability, biosphere, ore and volatile richness, civ industry), so a fertile system naturally hosts agricultural hubs. Nothing is stored per station.
  - **The only mutable state is a bounded delta pool** with eviction of the closest-to-baseline entry when full — so a busy galaxy silently loses the least-displaced market's history.
  - **Deltas decay toward baseline at a fixed 2 units per in-game hour**, tying the economy's memory to the shared sim clock.
  - Depends on `galaxy_map.h` only for the station-id packing helpers, keeping the coupling narrow.
  - Revenue entries share the same pool but never decay, so two different lifetimes coexist in one structure.
- **Interface vs internal:** Public interface within the sandbox — consumed by missions, miners, and the station inspector UI.

---

## sandbox/source/sim/station_market.h

- **Path:** `sandbox/source/sim/station_market.h`
- **Purpose:** Declares the market snapshot type, the name tables, and the eleven economy functions.
- **Key types/functions:** `struct MarketGood` (stock, base_stock, price); `TRADE_GOOD_NAMES`, `TRADE_CATEGORY_NAMES`; `trade_good_category`, `trade_good_base_price`, `station_specialization`, `station_rooms`, `station_market_baseline`, `station_market_get`, `station_market_apply`, `station_markets_decay`, `station_revenue_add`, `station_revenue_get`.
- **Notable non-obvious dependencies:**
  - **States the design philosophy and its lineage** — rooms and baseline markets are pure deterministic functions of the station id, "same philosophy as the station layout core in `galaxy_map.cpp`" — so the economy inherits worldgen's stored-nothing approach.
  - **Isolates the one piece of mutable state** (`GalaxyState::market_deltas`) and describes its whole lifecycle: written by trader docks, decaying back to baseline over in-game hours.
  - **Reduces the model to one rule** in the header comment: scarce → expensive, glutted → cheap.
  - **Names the caller of `station_market_apply`** (the mission dock hooks in `ship_mission.cpp`), which is the only record of who mutates the economy.
  - Documents the baseline stock ranges before and after bias (50–250 → 10–400), so a reader can reason about the price clamp without the implementation.
  - `trade_good_base_price` is annotated as the UI's trade signal — local price versus galactic average.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/ship.h

- **Path:** `sandbox/source/sim/ship.h`
- **Purpose:** Defines the ship — the hardpoint skeleton, faction identity, sensor suite, point-defense laser, size classes and motion tuning, the `Ship` struct itself, and ~25 query and mutation functions. At 558 lines it is the sandbox's core simulation type.
- **Key types/functions:**
  - Capacities `SHIP_MAX_COLLIDER_VERTS` (32), `SHIP_MAX_WEAPONS` (4), `SHIP_MAX_HARDPOINTS` (16), `SHIP_MAX_MODULES` (4), `SHIP_WEAPON_GROUPS` (5).
  - `MODULE_TYPE_*` bitmask, `enum HardpointSize`, `struct HardpointDef`, `hardpoint_half_extent`, `hardpoint_size_name`.
  - `enum VesselFaction`; `FACTION_NONE` / `_PLAYER` / `_PIRATE`; `vessel_faction_name` / `_desc`.
  - `SENSOR_LAYER0/1/2_RADIUS` (250k / 500k / 1M), `struct SensorSuite`.
  - `enum PdStance`, `enum PdPriority`, `struct DefenseLaser` (tuning plus runtime lock state).
  - `enum ShipSizeClass` (6 classes by world length), `struct ShipMotion`.
  - `struct Ship` — pose, visual, collider, hardpoints, mounts, module mounts and stash, weapon stash, glow, radiation, sensors and `sensors_base`, capacitor quintet, point defense, and per-hardpoint turret aim arrays.
  - Functions: `hardpoint_accepts`, `ship_first_free_hardpoint`, `hardpoint_fits_module`, `ship_recompute_stats`, `ship_try_spend_cap`, `ship_capacitor_update`, `ship_hardpoint_fire_origin`, `ship_hardpoint_can_aim`, `ship_select_bearing_weapon`, `ship_turret_aim_at`, `ship_update_turrets`, plus load/collider/transform helpers.
- **Notable non-obvious dependencies:**
  - **States the rigid-body coordinate contract as explicit formulas** — `world = origin + rotate(local * world_scale, angle)` and its inverse — which every render, collision, and hardpoint calculation depends on.
  - **Two parallel faction systems coexist during a migration**: the legacy `VesselFaction` enum for visuals and friendly fire, and the unified `i16 faction_id` where non-negative values index civs and three negative sentinels encode player, pirate, and unset. Diplomacy resolves only from the latter.
  - **Derived-versus-baseline stats are a documented discipline** — `sensors` and the capacitor pair are re-derived by `ship_recompute_stats()` from `sensors_base` / `cap_*_base` times mounted modules, with the instruction "tune the baseline, not it". Every mount or unmount must be followed by that call.
  - **The sensor radii were rebalanced for a specific reason** recorded in the header: the player previously saw 30k while combat hulls saw millions, so NPCs detected and closed long before anything appeared on the player's scope. Layer 1 is now pinned to the NPC sensor range, and the suite is strictly ordered with the editor enforcing `l0 < l1 < l2`.
  - **`DefenseLaser::range = 0` means live-coupled to Layer 0**, so a sensor module silently changes point-defense reach — and the header argues the Layer 0 choice ("last-ditch screen… deliberately does NOT reach identification range"), contradicting `point_defense.h`'s stale Layer 1 claim.
  - **A hardpoint holds at most one occupant across three separate arrays** (`mounts`, `point_defense_mount`, `module_mounts`) — an invariant maintained by convention across the loadout code with no central enforcement.
  - **Weapon groups are player-only**: the point defense and AI-piloted ships ignore them and fire via `ship_select_bearing_weapon` instead, so two different weapon-selection paths exist.
  - **Size class is derived automatically from hull world length at load** (or authored), and drives `ShipMotion` — so a cruiser's sluggishness is data, not code.
  - **Turret aim uses a three-array engaged/goal/current pattern** where gameplay must re-assert the goal every frame or the turret slews back to rest — an implicit per-frame contract.
  - References `docs/POINT_DEFENSE_AND_MISSILES.md` three times for rules not stated in code.
  - The `Ship` struct's size (several MB with its embedded arrays) is what forced placement-new in `game_init`.
- **Interface vs internal:** Public interface within the sandbox — 16 direct includers and the type at the centre of combat, rendering, AI, and the loadout UI.

---

## sandbox/source/sim/ship.cpp

- **Path:** `sandbox/source/sim/ship.cpp`
- **Purpose:** Implements ship loading from `.ship` files, collider and transform math, hardpoint and mount queries, stat composition, the capacitor, turret traverse, and hull collision.
- **Key types/functions:** `ship_load`, `ship_collider_corners`, `ship_bounding_radius`, `ship_local_dir`, `ship_local_to_world`, `ships_collide`, `hardpoint_accepts`, `hardpoint_fits_module`, `ship_first_free_hardpoint`, `ship_recompute_stats`, `ship_try_spend_cap`, `ship_capacitor_update`, `ship_hardpoint_fire_origin`, `ship_hardpoint_can_aim`, `ship_select_bearing_weapon`, `ship_turret_aim_at`, `ship_update_turrets`, `hardpoint_half_extent`, `hardpoint_size_name`, `vessel_faction_name` / `_desc`.
- **Notable non-obvious dependencies:**
  - **Parses the `.ship` text format from disk** — name, faction, class, visual layers, collider polygon, and hardpoint lines — the third hand-rolled `sscanf` parser in the sandbox alongside `module.cpp` and `weapon_def.cpp`.
  - **Derives the size class from hull world length when not authored**, mapping it to a `ShipMotion` row, so flight feel emerges from art dimensions.
  - **`ship_recompute_stats` is the single composition point** for sensors and capacitor limits, and `ship_load` calls it itself so a freshly loaded hull is consistent.
  - **`ships_collide` implements SAT with a minimum-translation vector** over the authored collider polygons — consumed by `resolve_ship_collision` and nothing else.
  - **`ship_select_bearing_weapon` encodes the AI's firing policy**: prefer the active weapon when ready and in arc, else the first other ready weapon that bears — deliberately bypassing the player's weapon groups.
  - **Turret slewing clamps the goal inside the hardpoint's traverse arc** and decays toward rest facing when no goal was asserted, so the procedural mount art reflects real constraints.
  - Positions come back as `HierPos2` (`ship_hardpoint_fire_origin`), keeping fire origins precision-safe at galaxy scale.
- **Interface vs internal:** Implementation of the sandbox's central public type.

---

## sandbox/source/sim/ss_generation.h

- **Path:** `sandbox/source/sim/ss_generation.h`
- **Purpose:** Declares the procedural star-system generator — orbital layout configuration, system generation, orbital mechanics, and the worldgen property model for stars, planets, and appearance genomes.
- **Key types/functions:** `struct SSGenConfig` (12 defaults); `SSGEN_DEFAULT`; `generate_star_system`, `update_planet_positions`, `solve_eccentric_anomaly`; `worldgen_star`, `worldgen_orbit_range_au`, `worldgen_planet`, `worldgen_planet_genome`; label helpers `spectral_class_name`, `planet_type_name`, `planet_subtype_name`, `planet_anomaly_name`, `planet_trait_names`; `blackbody_color`.
- **Notable non-obvious dependencies:**
  - **Orbits are generated non-intersecting by construction** — a spacing factor between 1.40 and 2.00 plus a 5% safety margin between inner apoapsis and outer periapsis — which is the constraint `galaxy_gen.h`'s minimum-separation reasoning builds on.
  - **`SSGenEnv` threads galactic structure into stellar content**, so a star's spectral class and metallicity follow spiral-arm proximity and galactocentric radius. The default argument is a neutral mid-disc environment.
  - **Uses real orbital mechanics** — `solve_eccentric_anomaly` via Newton-Raphson, documented as converging in 4–5 iterations — rather than circular approximations.
  - **`#include "game.h"` pulls the entire god struct into a generator header**, unlike the sibling galaxy headers that forward-declare.
  - **The genome model is explicitly a blend** of physical theme (temperature, mass, age, metallicity bias the subtype and palette) and free random variety "so every planet of a type looks distinct" — which is what fills `bs_planetsurface_params`'s genome fields.
  - `blackbody_color` is a physics-derived colour function shared by star rendering and the map dots.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/ss_generation.cpp

- **Path:** `sandbox/source/sim/ss_generation.cpp`
- **Purpose:** Implements star and planet generation, the orbital layout solver, per-frame orbital motion, the appearance genome, and the naming and colour helpers.
- **Key types/functions:** `SSGEN_DEFAULT`; `generate_star_system`, `update_planet_positions`, `solve_eccentric_anomaly`, `worldgen_star`, `worldgen_orbit_range_au`, `worldgen_planet`, `worldgen_planet_genome`, the five label functions, and `blackbody_color`.
- **Notable non-obvious dependencies:**
  - **Keeps its own splitmix64 seeded from the node seed**, which `galaxy_rng.h` documents as deliberate — a node's contents are fully determined by its seed, independent of the galaxy-level stream.
  - **`generate_star_system` drives the four-phase evolution pipeline** in `system_evolution.cpp` and derives the render views from its output, so system content is a product of simulated formation history rather than direct rolls.
  - **Runs for every galaxy node during generation** via `fill_node_summary`, which is why `system_evolution.h` states a ~30–50 µs per-system budget.
  - **Habitability computed here becomes the substrate for civilization cradles**, linking planet generation to the galaxy history.
  - `update_planet_positions` is called each frame but only for materialised systems, per `galaxy_map.h`'s hot-cache rule.
- **Interface vs internal:** Implementation of a public sandbox interface.

---

## sandbox/source/sim/system_evolution.h

- **Path:** `sandbox/source/sim/system_evolution.h`
- **Purpose:** Declares the four-phase epoch-based planetary evolution pipeline and its debug self-test.
- **Key types/functions:** `evolve_star_system(EvolvedSystem*, const StarProperties&, u64 seed)`, `evo_event_name(u8)`, `system_evolution_selftest()`.
- **Notable non-obvious dependencies:**
  - **Documents the whole model in the header** — disk condensation, accretion and migration over ~8 epochs, geophysics and atmosphere over ~6 epochs, then present-day synthesis — so the header is the design document for the game's most elaborate generator.
  - **States a hard performance budget** (~30–50 µs per system) because `fill_node_summary` runs it for all ~10,000 nodes, making it the dominant cost of galaxy generation.
  - **Determinism is guaranteed by per-body/per-epoch RNG salts** so "one body's outcome never reorders another's" — the same index-invariance principle `galaxy_rng.h` applies at galaxy scale, applied here within a system.
  - **Requires the caller to have rolled the star already** so the caller controls the star RNG stream — an explicit ordering contract on the seed hierarchy.
  - **`system_evolution_selftest` is called from `game_init` under `BS_DEBUG`** and checks determinism, composition sums, Hill spacing, moon orbits, and finiteness — one of only two self-tests in the project, and unlike the engine's, this one actually runs.
  - Explicitly disclaims N-body simulation: the epochs are a narrative model, not physics integration.
- **Interface vs internal:** Public interface within the sandbox.

---

## sandbox/source/sim/system_evolution.cpp

- **Path:** `sandbox/source/sim/system_evolution.cpp`
- **Purpose:** Implements the four-phase evolution pipeline that turns a star and a seed into an evolved body list plus a chronicle of formation events.
- **Key types/functions:** `evolve_star_system`, `evo_event_name`, `system_evolution_selftest`, plus the per-phase internal passes.
- **Notable non-obvious dependencies:**
  - **Produces both state and narrative** — the `EvolvedSystem` body array (star, planets sorted by semi-major axis, moons, belts) *and* an event log that the System Inspector renders as a system's history.
  - **Belts emerge from "frustrated annuli"** where accretion failed, so asteroid fields are an outcome of the simulation rather than a separate generator — and those belts are what miners later work.
  - **Phase 3 spans the star's actual age**, so an old star's planets have had longer for tectonics to decay and atmospheres to escape; stellar properties feed planetary outcomes.
  - **Habitability, environmental hazard, and resource richness are all Phase 4 outputs**, which is how one pipeline feeds the galaxy history's cradles, the market's abundance bias, and the planet inspector's gauges.
  - The self-test's invariant list (determinism, composition sums, Hill spacing, moon orbits, finiteness) is the closest thing the project has to a test suite.
- **Interface vs internal:** Implementation of a public sandbox interface, invoked once per node during generation.

---

## sandbox/source/state/game_state.h

- **Path:** `sandbox/source/state/game_state.h`
- **Purpose:** Defines the entire game's data model in one header — every enum, record, and capacity constant, culminating in the `game_state` god struct — plus the four `game_*` entry-point declarations. At 3652 lines it is the largest file in the project.
- **Key types/functions:**
  - Lifecycle: `enum GameMode`, `enum AppPhase`, `struct GalaxySetupParams`.
  - Celestial: `CelestialBody`, `SpectralClass`, `StarProperties`, `PlanetType`, `PlanetTrait`, `PlanetGenome`, `PlanetProperties`, `StarSystem`.
  - Evolution: `BodyKind`, `BodyComposition`, `EvolvedBody`, `EvoEventKind`, `EvolutionEvent`, `EvolvedSystem`.
  - Economy: `TradeCategory`, `TradeGood`, `StationRoomKind`, `SystemStation`, `StationMarketDelta`, `StationRevenue`, `NodeRisk`.
  - System content: `SystemAsteroid`, `SystemResource`, `SystemDecoration`.
  - Galaxy: `GalaxyNode`, `GalaxyLaneGraph`, `MissionObjective`, `MissionStage`, `ShipMission`, `CivGovernment`, `GovInteractionWindow`, `CivEthos`, `Civilization`, `GalaxyHouse`, `HistoryEventType`, `HistoryEvent`, `MapEntity`.
  - Combat and agents: `CombatEntity`, `NpcShip`, `DefenseBeam`.
  - Discovery and editor: `DiscoveredNpc`, `DiscoveryKind`, `DiscoveryLogEntry`, `EditEntityKind`, `EditSelection`, `EditDragMode`, `EditorDrag`.
  - `struct game_state` with nested `CameraState`, `PlanetApproachState`, `FleetState`, `RenderState`, `GalaxyState`, and an embedded `Profiler`, `RtsControls`, `ProjectileSystem`, `TravelState`, `OutSensorDetectionFX`, and both registries.
  - ~30 capacity macros; four `extern const f32` star constants; five `extern const f32` ship constants; two `extern` sensor functions; the four `game_*` declarations.
- **Notable non-obvious dependencies:**
  - **The project's structural centre.** Reached through `game.h` by 48 files, so almost every sandbox TU compiles against all 3652 lines — the cost `game_modules.h` was created to stop growing.
  - **Data lives here, behaviour lives in `sim/`** — a consistent split stated in four other headers (`galaxy_history.h`, `ai_ship.h`, `ship_mission.h`, `ss_generation.h`), all of which delegate their types here so `GalaxyState` can embed the arrays by value.
  - **Declares functions it does not define.** `sensor_visibility_from_dist` and `get_sensor_visibility` are `extern` here and defined in `sim/galaxy_map.cpp`; the five `SHIP_*` constants are defined in `game.cpp`; the four `STAR_*` constants in `render/galaxy_map_render.cpp`. Four files supply symbols this header promises.
  - **Includes eleven project headers** — ship, module, weapon_def, projectile, star_fx, global_background, voronoi_galaxy, galaxy_spatial, galaxy_params, travel, fleet, rts_controls — so pulling in the state pulls in most of the game's type universe.
  - **Sheer size is a runtime constraint, not just a style issue**: two `Ship` members at ~3 MB each are what forced `game_init` to placement-new the struct rather than value-initialise a stack temporary.
  - **Capacity constants define the game's scale ceilings** in one place — 6 planets, 8 moons and 2 belts per system, 4096 asteroids, 24 stations, 8192 missions, 384 NPCs, 416 combat entities, 512 discovered NPCs, 64 Voronoi sites, 16 map entities.
  - **`MAX_COMBAT_ENTITIES` (416) is sized to hold the packed windows** — enemy + fleet + `NPC_SHIP_MAX` (384) — an arithmetic relationship maintained by hand across three files.
  - **`GALAXY_MAX_SYSTEMS` (64) is the hot-cache size**, so `s->galaxy.current_system` indexes 64 slots while `node_count` reaches ~10,000 — the distinction behind every "slot valid this frame only" warning.
  - **`ShipMission` at ~8192 entries is heap-allocated** while nearly everything else is a fixed inline array, the one capacity too large to embed.
  - `EditDragMode` is declared `: int` specifically so `editor_tools.h` can forward-declare it opaquely.
  - The closing comment restates the `game_modules.h` rule and its rationale, so the include-hygiene policy is recorded at both ends.
- **Interface vs internal:** Public interface within the sandbox in the broadest possible sense — it is the shared vocabulary of the entire game, and the four `game_*` declarations are what the engine links against.

---

## sandbox/source/ui/editor_ui.cpp

- **Path:** `sandbox/source/ui/editor_ui.cpp`
- **Purpose:** Builds the three ImGui developer panels — the large multi-section editor panel, the transform window for the selected entity, and the profiler readout.
- **Key types/functions:** `build_editor_panel` (~640 lines), `build_transform_panel`, `build_profiler_panel`.
- **Notable non-obvious dependencies:**
  - **`build_editor_panel` is a single 640-line function** covering edit mode, lights, glow, bloom, background layers, coordinate diagnostics, nebula, camera, starfield, travel, and system view — the widest single reach into `game_state` anywhere, touching dozens of unrelated subsystems' tuning fields.
  - **It is the writer for several globals other modules own** — `g_debug_cell_grid` (`debug_overlay.cpp`), `g_zoom_out_speed_gain` (`view_transform.cpp`), and `g_sensor_fade_distance` (`sensor_overlay.cpp`) — so three modules' `extern` declarations exist for this file.
  - **Toggling "Multiple ship command" here calls into the fleet and combat arena** (`set_count` then `combat_arena_rebuild_player_entities`), making a checkbox responsible for maintaining the combat-entity window invariant.
  - **Uses `BS_UI_TYPE_EDITOR` for two panels and `BS_UI_TYPE_GAME` for the profiler** — and since the implementation ignores anchoring for the editor type, the two editor panels' anchor and margin arguments have no effect.
  - **`build_profiler_panel` is a thin wrapper** that opens a panel and delegates the body to `Profiler::build_ui`, so the profiler owns its own presentation.
  - Panels are built from `game_render` while the state they mutate is consumed by the next `game_update` — a one-frame lag inherent to editing during render.
- **Interface vs internal:** Internal — three exported panel builders called only from `game_render`.

---

## sandbox/source/ui/editor_ui.h

- **Path:** `sandbox/source/ui/editor_ui.h`
- **Purpose:** Declares the three editor and debug panel builders.
- **Key types/functions:** `build_editor_panel`, `build_transform_panel`, `build_profiler_panel`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Enumerates the editor panel's twelve sections in a comment**, which is the only inventory of what that 640-line function contains.
  - **States each panel's screen position** (top-left, top-right, bottom-left) — the anchoring the implementation requests but `bs_ui` ignores for editor-type panels.
  - Notes the transform panel is edit-mode only, a gating condition enforced by the caller rather than the function.
  - Thirteen lines with no includes, deliberately keeping the heaviest UI module's interface trivial.
- **Interface vs internal:** Internal interface.

---

## sandbox/source/ui/new_game_setup.cpp

- **Path:** `sandbox/source/ui/new_game_setup.cpp`
- **Purpose:** Builds the New Game setup screen that edits the galaxy setup parameters, and the progress panel shown during staged generation.
- **Key types/functions:** `build_new_game_setup`, `build_generation_progress`; static `value_to_index`; ten option label tables.
- **Notable non-obvious dependencies:**
  - **The combo tables are the authoritative option sets** — galaxy sizes (2000/6000/10000), history depths (100k–5M years), and seven qualitative scales — and they are `\0`-separated ImGui strings, the convention `bs_ui.h` passes through unchanged despite claiming to hide ImGui.
  - **`value_to_index` reverse-maps a stored value back to a combo index** by nearest match, so the UI can restore a selection from a raw number rather than storing the index — a small impedance mismatch between the data model and the widget.
  - **Rerolls the seed with `galaxy_splitmix64`**, using the same generator the galaxy itself derives from, so "Randomize seed" walks the same hash space.
  - **The "Generate Galaxy" button is the phase transition** — it flips `app_phase` to `APP_GENERATING` and resets the stage counter, handing control to `run_generation_stage` in `game.cpp`.
  - **Computes a qualitative cost estimate** from size × history depth × civ density × chronicle detail, mapping to Fast/Moderate/Slow/Very slow — a heuristic that encodes which parameters actually drive generation time.
  - The `civ_density` combo is labelled "Dynastic Houses" while the field name says density, reflecting a renamed concept.
- **Interface vs internal:** Internal — two exported builders called from `game_render`'s phase gate.

---

## sandbox/source/ui/new_game_setup.h

- **Path:** `sandbox/source/ui/new_game_setup.h`
- **Purpose:** Declares the setup screen and generation progress panel.
- **Key types/functions:** `build_new_game_setup`, `build_generation_progress`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Documents the phase transition as the panel's purpose** — the Generate button flips `app_phase` to start the staged pipeline — so the header records the control-flow consequence of a button press.
  - Names the state each panel reads (`s->setup`, `s->gen_label` / `gen_progress`), making the data dependency explicit in ten lines.
- **Interface vs internal:** Internal interface.

---

## sandbox/source/ui/system_inspector.cpp

- **Path:** `sandbox/source/ui/system_inspector.cpp`
- **Purpose:** Builds the System Inspector window — the evolved-body model of the current star system with per-body detail and the formation chronicle.
- **Key types/functions:** `build_system_inspector`; statics `gauge`, `comp_line`, `hazard_badge`, `body_detail`, and the `HDR` / `DIM` colour arrays.
- **Notable non-obvious dependencies:**
  - **It is the only consumer of the evolution chronicle** — `EvolutionEvent` and `evo_event_name` exist so this window can narrate how a system formed, making the 775-line evolution pipeline's event log player-visible in exactly one place.
  - **Renders the `EvolvedSystem` model rather than the render-facing `StarSystem`**, so it shows the simulation's internal representation directly — composition fractions, atmospheric pressure, water fraction, geology gauges.
  - **Suppresses water and atmosphere gauges for gas-dominated bodies** (`comp.gas > 0.35`), a presentation rule derived from the physical model.
  - **`comp_line` accumulates into a fixed 128-byte buffer with running `snprintf` offsets** and no bound re-check against the remaining space — the same pattern as the engine's memory-usage string.
  - Uses C++ default arguments (`overlay_fmt = "%.0f%%"`) in a codebase that otherwise avoids them.
  - Hazard severity thresholds (0.25, 0.55) are duplicated here as display bands with no shared constant.
- **Interface vs internal:** Internal — one exported builder called from `game_render`.

---

## sandbox/source/ui/system_inspector.h

- **Path:** `sandbox/source/ui/system_inspector.h`
- **Purpose:** Declares the System Inspector window builder.
- **Key types/functions:** `build_system_inspector(game_state*)`; forward-declares `game_state`.
- **Notable non-obvious dependencies:**
  - **Describes the full window contents in one comment** — star card, body tree with nested moons and belts, per-body evolved state, and the evolution chronicle — which is the only summary of what the evolution pipeline's output looks like to a player.
  - **Names both the data source (`s->galaxy.current_system`) and the gate (`s->galaxy.show_system_inspector`)**, so the caller's two preconditions are documented in ten lines.
  - Notes the toggle lives in the editor panel, tying this window to `editor_ui.cpp`.
- **Interface vs internal:** Internal interface.

---
