# engine.dll API boundary

What actually crosses between `engine.dll` and `sandbox.exe`, verified against the export
declarations rather than the headers' intent.

**Input note.** This document was asked to use
`docs/architecture/_raw/clustering-approved-engine.md`. **That file does not exist.** The
subsystem grouping below is taken from `docs/architecture/engine-subsystems.md` instead, which
is a *proposal* with three provisional groupings (UiFacade, HierCoords/MathCore, camera2d) and
was never marked approved. Grouping is presentational only — every boundary fact below is
verified directly against source, independent of how the files are clustered.

**Verification method.** Export surface from `grep bs__api__` over `engine/source/**/*.h`;
crossing traffic from `_raw/dependency-graph.json` `boundary_edges` plus a call-site grep
(`\bsymbol\s*\(`) over all 128 sandbox files. Every claim cites a file and symbol. Anything I
could not verify directly is marked as such.

---

## 1. The mechanism

`engine/source/defines.h:87-101` — the only export machinery in the project:

```c
#ifdef BSEXPORT
  #ifdef _MSC_VER
    #define bs__api__ __declspec(dllexport)
  #else
    #define bs__api__ __attribute__((visibility("default")))
  #endif
#else
  #ifdef _MSC_VER
    #define bs__api__ __declspec(dllimport)
  #else
    #define bs__api__
  #endif
#endif
```

- `engine/build.bat:114` passes `-DBSEXPORT`; `engine/build.bat:110` passes `-shared`. So the
  engine compiles every `bs__api__` symbol as `dllexport`.
- `sandbox/build.bat:17` passes `-DBSIMPORT`, and `sandbox/build.bat:16` links `-lengine.lib`.
- **`BSIMPORT` is never tested anywhere in either tree** (verified: zero matches outside
  `build.bat`). The sandbox gets `dllimport` by *absence* of `BSEXPORT`, not by presence of
  `BSIMPORT`. The flag is inert.
- The block carries one comment: `// TODO: Explain this further.` (`defines.h:85`).

**Export surface: 157 `bs__api__` functions across 14 headers, plus 4 `bs__api__`-annotated
structs** (`renderer_portrait_begin`/`_thumb_begin`/`_portrait_end` joined `renderer.h`
2026-08-12; `renderer_draw_godrays` + the by-pointer `bs_godray_params` joined 2026-08-15). Per-header counts (`grep -c bs__api__`, before those additions):

| header | decls | header | decls |
|---|--:|---|--:|
| `renderer/renderer.h` | 42 | `renderer/bs_imgui.h` | 6 |
| `renderer/bs_ui.h` | 24 | `renderer/camera2d.h` | 4 |
| `math/math_utils.h` | 22 | `core/memory/arena.h` | 4 |
| `renderer/bs_rml.h` | 17 | `core/event.h` | 3 |
| `core/memory/bs_memory.h` | 11 | `core/application.h` | 2 |
| `core/input.h` | 11 | `platform/platform.h` | 1 |
| `math/bs_hierpos.h` | 8 | `core/logger.h` | 1 |
| | | `core/asserts.h` | 1 |

(`defines.h`'s 4 hits are the macro definition itself, not declarations. Arithmetic check:
161 `bs__api__` occurrences in headers = 153 function declarations + 4 struct annotations +
4 macro-definition lines.)

---

## 2. Public headers

The sandbox directly includes **15** engine headers (`boundary_edges`, 204 edges):

| header | sandbox files | owning subsystem |
|---|--:|---|
| `defines.h` | 55 | Foundation |
| `renderer/renderer.h` | 27 | RenderFrontend |
| `math/bs_hierpos.h` | 23 | HierCoords |
| `math/math_utils.h` | 23 | MathCore |
| `renderer/camera2d.h` | 17 | RenderFrontend |
| `core/logger.h` | 14 | Diagnostics |
| `renderer/renderer_types.h` | 12 | RenderFrontend |
| `renderer/bs_ui.h` | 8 | Widgets |
| `core/input.h` | 7 | Input |
| `core/memory/bs_memory.h` | 6 | Memory |
| `renderer/bs_rml.h` | 5 | UiFacade |
| `renderer/bs_imgui.h` | 4 | UiFacade |
| `entry.h` | 1 | AppLifecycle |
| `game_types.h` | 1 | AppLifecycle |
| `containers/vector.h` | 1 | Containers |

**Header discipline holds.** No sandbox file includes an engine `.cpp`, and none reaches
`platform/*`, `core/event.h`, `core/application.h`, `core/asserts.h`, `core/memory/arena.h`,
or anything under `renderer/backend/`. Verified by enumerating the distinct targets of
`boundary_edges`.

---

## 3. What crosses, by owning subsystem

**88 of the 153 exported functions are called from sandbox source**, plus 4 more called from
`engine/source/entry.h` (see §4) — 92 exercised in total. The remaining **61 are exported but
never invoked from either side of the boundary**.

### RenderFrontend — 33 called of 46 exported
`renderer/renderer.h`, `renderer/camera2d.h`

Draw submission: `renderer_draw_sprite` (6 files), `renderer_draw_line` (11),
`renderer_draw_quad` (6), `renderer_draw_circle` (6), `renderer_draw_rect_outline` (2),
`renderer_draw_mapped_sprite`, `renderer_draw_starfield`, `renderer_draw_sunburst`,
`renderer_draw_starsurface`, `renderer_draw_planetsurface`, `renderer_draw_heat_map`,
`renderer_draw_nebula` (1 each).
Textures: `renderer_load_texture` (2), `renderer_create_texture` (4),
`renderer_destroy_texture` (2).
State: `renderer_set_camera` (3), `renderer_set_draw_alpha` (3), `renderer_set_clear_color`,
`renderer_set_lights`, `renderer_set_glow_params`, `renderer_set_bloom_enabled`,
`renderer_set_bloom_params`, and the five streak setters plus `renderer_set_aux_bloom_mode`.
Instrumentation: `renderer_set_present_mode` (2), `renderer_is_present_immediate`,
`renderer_get_frame_timing`, `renderer_get_present_breakdown`.
Camera: `camera2d_world_to_screen` (8), `camera2d_screen_to_world` (5), `camera2d_default` (1).

Never called from the sandbox: `renderer_initialize`¹, `renderer_shutdown`,
`renderer_on_resize`, `renderer_begin_frame`, `renderer_end_frame`,
`renderer_report_frame_timing`, `renderer_update_texture`, `renderer_get_draw_alpha`,
`renderer_draw_grid`, `renderer_get_frame_stats`, `camera2d_view_proj`.
¹ `renderer_initialize` matches in one sandbox file only inside a comment.

### MathCore — 9 called of 22
`vec2_length` (25 files), `vec2_add` (22), `vec2_scale` (22), `clampf` (21), `vec2_sub` (18),
`vec2_rotate` (11), `clamp` (6), `vec2_dot` (3), `vec2_normalized` (1).
Never called from the sandbox: **all six `mat4_*`** and **all three `vec3_*`**.

### HierCoords — 7 called of 8
`hierpos_diff` (31 files), `hierpos_to_f64` (11), `hierpos_from_vec2` (9),
`hierpos_to_vec2` (4), `hierpos_lerp` (2), `hierpos_add_f64` (2), `bs_hierpos_selftest` (1).
Never called: `hierpos_normalize`.
**Caveat — not every one of these calls actually crosses the DLL.** See §7.1.

### Widgets — 18 called of 24
`bs_ui_text` (6), `bs_ui_text_colored` (6), `bs_ui_button` (5), `bs_ui_separator` (5),
`bs_ui_begin_window`/`_end_window`/`_combo` (3 each), `bs_ui_begin_panel`/`_end_panel`/
`_progress`/`_same_line`/`_checkbox`/`_slider_float`/`_color_edit3` (2 each),
`bs_ui_button_sized`/`_set_cursor_pos_x`/`_label_at`/`_selectable` (1 each).
Never called: `bs_ui_begin_hud_panel`, `bs_ui_end_hud_panel`, `bs_ui_push_alpha`,
`bs_ui_pop_alpha`, `bs_ui_color_button`, `bs_ui_is_window_hovered`.

### UiFacade — 10 called of 23
`bs_rml_wants_mouse` (4), `bs_imgui_wants_mouse` (4), `bs_rml_set_sharpen` (2),
`bs_imgui_wants_keyboard`, `bs_rml_wants_keyboard`, `bs_rml_load_fonts`,
`bs_rml_debugger_toggle`, `bs_rml_hud_init`, `bs_rml_hud_update`, `bs_rml_hud_poll_action`.
Never called: the ImGui lifecycle four, and nine `bs_rml_*` including the whole document API
(`bs_rml_load_document`, `_show`, `_unload_document`) — the sandbox only ever uses the HUD
convenience path, which loads its document internally.

### Input — 6 called of 11
`input_get_mouse_position` (4), `input_is_key_down` (3), `input_is_button_down` (3),
`input_was_button_down` (3), `input_was_key_down` (2), `input_get_mouse_wheel` (1).
Never called: `input_is_key_up`, `input_was_key_up`, `input_is_button_up`,
`input_was_button_up`, `input_get_previous_mouse_position`.
The `input_process_*` push functions are correctly **not** exported (`core/input.h:182-185`).

### Memory — 2 called of 11
`bs_memory_allocator` (6 files), `bs_memory_free` (5). Plus `bs_memory_initialize` /
`bs_memory_terminate` from `entry.h` (§4).
Never called: `bs_memory_zero`, `_set`, `_copy`, `_get_memory_usage_string`, the three
virtual-memory functions, and **all four `arena_*`** — `core/memory/arena.h` is fully exported
and entirely unused across the boundary.

### Diagnostics — 0 functions, 6 macros
`logger_output` (`core/logger.h:29`) is exported but never named directly. All traffic goes
through the macros: `BS_LOG_INFO` (13 files), `BS_LOG_WARN` (7), `BS_LOG_ERROR` (6),
`BS_LOG_FATAL` (1), `BS_LOG_DEBUG` (1). Each expands to a `logger_output` call compiled in the
sandbox TU, so the crossing is real but invisible to a symbol grep.
`report_assertion_failure` (`core/asserts.h:24`) is exported and never used.

### Platform — 1 exported, 0 called
`platform_get_window_handle` (`platform/platform.h:24`) is the only exported platform symbol
and is called only by `renderer/backend/renderer_backend_sdlgpu.cpp`. The single sandbox match
for `platform_get_absolute_time` is a comment in `sandbox/source/core/profiler.h:13` noting it
is engine-internal — which is why the sandbox uses `std::chrono` instead.

### EventBus — 3 exported, 1 called
`event_register`, `event_unregister`, `event_fire` (`core/event.h:44-46`). As of 2026-08-12
`event_fire` has its first sandbox caller: `game.cpp`'s ESC handler fires
`EVENT_CODE_APPLICATION_QUIT` (quit-on-ESC moved out of `application_on_key` so the key can be
modal-aware — the press that collapses the ship inspector is consumed by it). `event_register`
and `event_unregister` remain unused by the sandbox.

### AppLifecycle — see §4.

---

## 4. The boundary is bidirectional

**Engine → sandbox.** `engine/source/core/application.cpp` calls back into game code through
function pointers on `Game` (`engine/source/game_types.h:9-17`):

- `application.cpp:95` — `game_inst->init(game_inst)`
- `application.cpp:136` — `game_inst->update(game_inst, dt)`
- `application.cpp:148` — `game_inst->render(game_inst, dt)`
- `application.cpp:101`, `:279` — `game_inst->on_resize(game_inst, w, h)`

These four are the only engine→sandbox calls, and they are **unversioned raw function
pointers**. `engine/source/entry.h:27-31` validates all four are non-null before starting;
nothing validates their signatures.

**A third case: engine code compiled into the executable.** `engine/source/entry.h` defines
`int main(void)` and is compiled into `sandbox.exe` (it is included only by
`sandbox/source/entry.cpp:2`). Calls made *from* that header:

- `entry.h:18` `bs_memory_initialize()` and `entry.h:45` `bs_memory_terminate()`
- `entry.h:34` `application_init(&game_inst)` and `entry.h:40` `application_run()`

All four cross the DLL boundary at runtime, but the call sites live in the engine tree. A grep
of sandbox sources alone misses them. This is also why `bs_memory_initialize`/`_terminate`
must stay exported despite `core/memory/bs_memory.h:29` reading:

```c
// TODO: these shall not be exported !
bs__api__ void bs_memory_initialize();
bs__api__ void bs_memory_terminate();
```

**Acting on that TODO would break startup.** `entry.h` is compiled into the executable and
calls both across the boundary.

`game_create` (`entry.h:11`, defined `sandbox/source/entry.cpp:5`) is a plain `extern`, not
`bs__api__` — correctly, since both the caller (`main` in `entry.h`) and the definition end up
in `sandbox.exe`. No DLL crossing.

---

## 5. Types that cross

### Handle-based indirection — one type, correctly opaque
`bs_texture` (`renderer/renderer_types.h:17`) is `struct { u32 id; }`. The engine packs a
generation counter into it (`renderer_backend_sdlgpu.cpp:485`):

```c
handle.id = ((u32)slot->generation << 18) | (i + 1u); // index+1 so id 0 stays invalid
```

and validates on resolve (`:495-504`), so use-after-destroy is detectable.
**Verified: the sandbox never decodes the bits.** The only uses are truth tests against zero —
`sandbox/source/render/ship_scene.cpp:106`, `sandbox/source/game.cpp:728,730`. Opacity holds.

`bs_shader`, `bs_pipeline`, `bs_buffer`, `bs_sampler` (`renderer_types.h:18-21`) follow the
same shape but are unused anywhere.

`bs_rml_document` (`renderer/bs_rml.h:26-29`) is `struct { void* ptr; }` wrapping
`Rml::ElementDocument*`. Sandbox never uses it — the document API is unused (§3).

### Structs passed by value across the DLL
Verified by declaration. These couple the two binaries to struct *layout*, not just to symbol
names:

- `Vec2` — `vec2_add/_sub/_scale/_dot/_length/_normalized/_rotate` (`math_utils.h:44-51`),
  `renderer_draw_line/_quad/_rect_outline/_circle/_grid` (`renderer.h:152-171`),
  `renderer_set_streak_source` (`:134`), `hierpos_from_vec2` (`bs_hierpos.h:23`).
- `Vec3` — `vec3_add/_sub/_scale`, `mat4_translation`, `mat4_scale` (`math_utils.h:54-67`).
- `Mat4` — returned by `mat4_identity/_mul/_ortho/_translation/_scale/_rotation_z`
  (`math_utils.h:59-69`) and `camera2d_view_proj` (`camera2d.h:22`); `mat4_mul` also takes two
  by value (128 bytes copied per call).
- `HierPos2` — returned by `hierpos_from_vec2/_normalize/_lerp/_add_f64` and taken by value by
  `hierpos_normalize` (`bs_hierpos.h:23-40`).
- `Camera2D` — `renderer_set_camera(Camera2D)` (`renderer.h:66`), returned by
  `camera2d_default()` (`camera2d.h:17`).
- `bs_color` — `renderer_set_clear_color` (`renderer.h:39`), `renderer_set_lights` ambient
  (`:115`), `bs_ui_label_at` tint (`bs_ui.h:118`).
- `bs_texture` — `renderer_update_texture`, `renderer_destroy_texture` (`renderer.h:58,61`).
- `bs_frame_stats` — returned by `renderer_get_frame_stats()` (`renderer.h:174`).
- `event_context` — `event_fire(u16, VOID_PTR, event_context)` (`core/event.h:46`), 16 bytes.
- `bs_rml_document` — `bs_rml_show`, `bs_rml_unload_document` (`bs_rml.h:49,52`).

`Vec2`, `Vec3`, `Vec4` and `Mat4` are additionally declared `typedef struct bs__api__ Vec2 {…}`
(`math_utils.h:14,20,27,38`) — `dllexport` applied to the *type*. For PODs with no member
functions or statics this exports nothing; value semantics work regardless of the annotation.

### Structs passed by pointer (not retained — verified)
- `renderer_set_lights(const bs_light2d*, count, …)` — copied element-wise,
  `renderer_backend_sdlgpu.cpp:1733`: `for (…) g_sdl.lights[i] = lights[i];`
- `renderer_set_glow_params(const bs_glow_params*)` — copied,
  `renderer_backend_sdlgpu.cpp:1743`: `g_sdl.glow_params = *params;`
- `bs_rml_hud_update(const bs_rml_hud_state*)` — copied field-by-field into the engine-side
  data model via `bs_rml_assign`, `renderer_backend_sdlgpu.cpp:4675+`.
- `renderer_create_texture(const u8* pixels, …)` — pixels uploaded to the GPU; the caller
  retains the CPU buffer.

### One struct pointer that IS retained across the call — a real lifetime contract
`bs_sprite::glow_override` (`renderer_types.h:113`) is a `const bs_glow_params*`.
`renderer_draw_sprite` copies the sprite **including that pointer** into the CPU batch
(`renderer_backend_sdlgpu.cpp:1960-1961`), and the pointee is dereferenced later, during
`end_frame` (`:2380`, `:2387`, `:2403`).

The pointee must therefore stay alive and unmoved from the `draw_sprite` call until
`end_frame` completes. Current sandbox call sites all satisfy this by pointing at long-lived
storage — `&s->render.bullet_glow` (`render/gameplay_overlays.cpp:151`), a file-static
`&EMBLEM_NO_GLOW` (`render/ship_render.cpp:85`), `&overlay_glow`
(`render/out_sensor_detection_fx.cpp:167`) — but **nothing enforces it and the header does not
state it.** A stack-local `bs_glow_params` would compile and produce a dangling read.

Second-order effect: the backend breaks draw runs on pointer *identity*
(`renderer_backend_sdlgpu.cpp:2388`), so *where* the sandbox stores its glow params affects
batching efficiency, not just correctness.

---

## 6. Ownership and lifetime rules

| Object | Allocated by | Freed by | Verified at |
|---|---|---|---|
| GPU textures | engine, on `renderer_load_texture` / `_create_texture` | engine, on `renderer_destroy_texture` | `renderer.h:48,54,61` |
| Texture pixel buffers (CPU) | sandbox caller | sandbox caller | engine copies to GPU only |
| Tagged heap blocks | engine, via `bs_memory_allocator` | engine, via `bs_memory_free` — caller supplies matching size **and** tag | `bs_memory.h:37-38` |
| `game_state` | sandbox, `bs_memory_allocator(sizeof(game_state), MEMORY_TAG_GAME)` | **never freed** | `sandbox/source/entry.cpp:17` |
| `Game::state` (`VOID_PTR`) | sandbox | sandbox — engine stores and passes back, never touches | `game_types.h:16` |
| `bs_memory_get_memory_usage_string` result | engine, `_strdup` | **caller must free; the one caller does not** | `bs_memory.cpp:128`, leaked at `application.cpp:116` |
| Arena (`ARENA_PTR`) | engine | engine | unused across the boundary |
| RmlUi documents | engine | engine | sandbox uses the HUD path only |

Notes verified:

- **No cross-heap frees.** Every sandbox `new`/`delete` (`render/global_background.cpp:21-34`,
  `sim/ai_ship.cpp:307`) and every `malloc`/`free` (`jc_voronoi.h`) operates on sandbox-side
  memory only. Nothing allocated by `bs_memory_allocator` is released with raw `free`, and
  nothing allocated in the sandbox is passed to `bs_memory_free`. The `bs_memory_*` pair keeps
  allocation and release inside `engine.dll`'s CRT, which is the correct arrangement.
- **`bs_memory_free` requires the original size and tag** (`bs_memory.cpp:77-85`): it subtracts
  them from unsigned counters, so a mismatch silently corrupts (and can wrap) the accounting
  without any diagnostic.
- **The 64 MB frame arena is created, reset per frame, and destroyed, but never allocated
  from** — `arena_allocate` has no call site in either tree (declaration
  `arena.h:28`, definition `arena.cpp:37`, zero callers).

---

## 7. Places the sandbox bypasses the intended interface — tech debt

### 7.1 Overload resolution silently decides whether a call crosses the DLL
`math/bs_hierpos.h` declares each conversion twice: an exported form taking `cell_size`, and an
`inline` default-cell-size form compiled into the caller.

```c
bs__api__ HierPos2 hierpos_from_vec2(Vec2 world, f32 cell_size);      // :23  crosses the DLL
inline    HierPos2 hierpos_from_vec2(Vec2 world) { … }                // :53  compiled inline
bs__api__ Vec2 hierpos_to_vec2(const HierPos2* hp, f32 cell_size);    // :27
inline    Vec2 hierpos_to_vec2(const HierPos2* hp) { … }              // :54
bs__api__ Vec2 hierpos_diff(const HierPos2* a, const HierPos2* b, f32 cell_size);  // :50
inline    Vec2 hierpos_diff(const HierPos2* a, const HierPos2* b) { … }            // :55
inline    HierPos2 hierpos_add_vec2(const HierPos2* hp, Vec2 d) { … }              // :44  inline-only
```

So an argument count — not an API decision — determines whether the sandbox calls into
`engine.dll` or executes an engine-authored function body compiled into `sandbox.exe`. The
one-argument forms also bake `BS_HIERPOS_CELL_SIZE` (16384.0f) into every caller's object code,
so changing that constant requires recompiling the sandbox, not just the DLL. `hierpos_add_vec2`
(15 sandbox files) has no exported form at all and is *always* compiled into the game.

### 7.2 Macros carry a meaningful share of the boundary
These are engine semantics compiled into `sandbox.exe`, invisible to the export table and to
any symbol-level audit:

- `BS_LOG_*` (`core/logger.h:32-69`) — 6 macros used across 13+ sandbox files, each expanding
  to a `logger_output` call. The macros also embed the compile-time level gates, so a sandbox
  built against different `LOG_*_ENABLED` values than the DLL would disagree about what logs.
- `TRUE` / `FALSE` (`defines.h:45-46`) — 54 and 52 sandbox files.
- `Vector(T)` (`containers/vector.h:5`) — `sandbox/source/state/game_state.h:2378` declares
  `Vector(bs_light2d) lights`, i.e. a `std::vector` of an engine POD living in sandbox memory.
- `BS_INVALID_HANDLE`, `BS_MAX_HEAT_SOURCES`, `BS_LAYER_BLOOM_THRESHOLD`
  (`renderer_types.h:23,119,304`) — the last is read by
  `sandbox/source/core/render_layers.h:15-16` to derive `LAYER_DEBUG`/`LAYER_GIZMO`, so the
  game's draw-order constants are pinned to an engine macro at compile time.
- `BS_RML_*` capacities (`bs_rml.h:96-102`) — seven macros sizing arrays inside the HUD snapshot
  struct the sandbox fills.

### 7.3 Engine structs embedded by value in sandbox state — undetectable ABI coupling
`sandbox/source/state/game_state.h` embeds engine types by value in **37 member declarations**
(`Camera2D`, `bs_color`, `bs_glow_params`, `bs_light2d`, `bs_texture`, `bs_math::Vec2`,
`HierPos2`, and others). `sizeof(game_state)` is therefore a function of engine struct layouts,
and it is computed sandbox-side at `sandbox/source/entry.cpp:17`.

If any engine struct changes size or field order and only one binary is rebuilt, the two
disagree silently — there is no version check, no size assertion, and no build-order
enforcement (`build-all.bat` builds the engine first but does not force a sandbox rebuild when
engine headers change; both `build.bat` files glob sources unconditionally with no dependency
tracking).

### 7.4 `bs_memory_initialize` / `_terminate` exported against stated intent
`core/memory/bs_memory.h:29` says `// TODO: these shall not be exported !`, but both are
`bs__api__` and both are called from `entry.h:18,45`, which compiles into `sandbox.exe`.
The TODO cannot be actioned as written without moving the memory lifecycle out of `entry.h`.

### 7.5 A header that defines `main()`
`engine/source/entry.h:16` defines `int main(void)`. It is engine-tree code that never enters
`engine.dll` (the engine build globs only `.cpp`, `engine/build.bat:103`) and is compiled
exclusively into `sandbox.exe`. Including it commits a translation unit to being the program
entry point. It is the one file whose cluster membership and build membership disagree.

### 7.6 40% of the export surface is unused
61 of 153 exported functions are never called from either binary — including all four
`arena_*`, all six `mat4_*`, all three `vec3_*`, the three `event_*`, `report_assertion_failure`,
and nine `bs_rml_*`. Each is a `dllexport` commitment nothing depends on, and each one widens
the surface a future ABI guarantee would have to cover.

### What is *not* a bypass — verified negatives
- **No sandbox file calls a non-exported engine function.** Checked explicitly for
  `platform_*`, `event_initialize`/`_terminate`, `logger_initialize`/`_terminate`,
  `input_initialize`/`_update`/`_process_*`, `renderer_backend_create`, `arena_allocate`:
  zero call sites. The `bs__api__` gate holds at link level.
- **No sandbox file includes a private engine header** (§2).
- **The sandbox never decodes texture handle bits** (§5).
- **No cross-heap allocation/free pairs** (§6).

---

## 8. Threading

**There are no threading assumptions because there is no threading.** Verified by grep across
all of `engine/source` for `std::thread`, `std::mutex`, `std::atomic`, `CreateThread`,
`SDL_CreateThread`, `SDL_Mutex`, `_Interlocked*` and `volatile`: **zero matches.** (The only
two hits are the substring "vola" inside `station_insp_spec_vola` /
`station_insp_market_vola` in `renderer/bs_rml.h:287,291`.)

Consequences, all verified:

- Every boundary call must be made from the thread that runs `main()` (`entry.h:16`).
- The engine's mutable state is file-scope globals with no synchronisation:
  `app_state` (`core/application.cpp:28`), `state` (`core/event.cpp:27`),
  `state` + `mouse_wheel_accum` (`core/input.cpp:26,31`), `stats`
  (`core/memory/bs_memory.cpp:37`), `perf_freq`/`perf_start`
  (`platform/platform_sdl3.cpp:22-23`), `state` (`renderer/renderer.cpp:41`),
  `g_sdl` and `g_rml` (`renderer/backend/renderer_backend_sdlgpu.cpp:377`, `:3222`).
- `renderer/bs_ui.cpp` and the ImGui facade in the backend share one ImGui context across two
  translation units by global state alone — safe only single-threaded.
- Frame-phase ordering is a real precondition on several calls and is documented only in
  comments: `renderer_begin_frame` must precede any `renderer_draw_*`
  (`renderer.h:68-72`), `input_get_mouse_wheel` must be polled from the game's update or the
  accumulator is already cleared (`core/input.h:176-180`), and `bs_rml_update` must run once
  per frame before rendering (`bs_rml.h:54-56`).

The one place concurrency is even mentioned is on the *sandbox* side —
`sandbox/source/sim/galaxy_spatial.h:10` says its queries are "safe to call lock-free from any
thread" because the grid is immutable after build. Nothing acts on that.

---

## 9. Versioning and ABI stability

**None exists. This is a statement of fact, not an omission.**

Verified absent across `engine/source`:

- No version symbol, macro, or exported query of any kind (no `BS_VERSION`, no
  `engine_get_version`, no build stamp).
- No `.def` file, no export ordinals, no `#pragma comment(linker, "/EXPORT:…")`. The export set
  is whatever `bs__api__` happens to mark at compile time.
- No struct size or layout assertions for any boundary type. The only `STATIC_ASSERT`s in the
  project are the ten scalar-width checks at `defines.h:32-43`, which constrain `u8`…`f64` but
  say nothing about `Vec2`, `Camera2D`, `bs_sprite`, `bs_rml_hud_state`, or any other crossing
  struct.
- No handshake at startup. `application_init` (`core/application.h:21`) takes a `Game*` and
  validates only that four function pointers are non-null (`entry.h:27-31`) — not a version,
  not a struct size, not a field count.

**What this means in practice.** `engine.dll` and `sandbox.exe` are a matched pair that must be
built from the same source tree at the same time. Any of the following will produce silent
memory corruption rather than a load failure or a diagnostic:

- adding, removing, or reordering a field in any struct listed in §5;
- changing `BS_HIERPOS_CELL_SIZE`, `BS_LAYER_BLOOM_THRESHOLD`, or any `BS_RML_*` capacity
  without rebuilding both;
- changing the `Game` struct's function-pointer signatures (§4);
- rebuilding one binary and not the other, which the build scripts do not prevent.

If ABI stability ever becomes a goal, the minimum viable steps the current code does not take
are: an exported version query checked during `application_init`; `STATIC_ASSERT`s on
`sizeof`/`offsetof` for the ~15 structs that cross by value; and narrowing the export surface
from 153 symbols to the 92 actually used (§7.6).

---

## 10. Open items

- This document was asked to consume `_raw/clustering-approved-engine.md`, which does not
  exist. Subsystem names come from `engine-subsystems.md`, whose UiFacade, HierCoords/MathCore,
  and camera2d groupings are still provisional. If those settle differently, §3's grouping
  changes; no other section does.
- I did not inspect the built `bin/engine.dll` export table (e.g. with `dumpbin /exports` or
  `llvm-nm`) — every claim here is from source declarations. A binary check would confirm that
  the 155 `bs__api__` declarations map 1:1 to actual exports and catch anything exported by
  other means. Worth doing before treating §7.6's count as exact.
