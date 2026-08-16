# Engine subsystems — proposed clustering

A proposed grouping of the 43 files under `engine/source` into subsystems, each with a
single responsibility statable in one sentence.

**Status:** proposal. Three grouping decisions are marked *provisional* below — they are
open questions with a working default applied, not settled calls.

**Inputs:** `_raw/dependency-graph.json` (side == "engine") and `_raw/file-summaries-engine.md`.

**How the numbers were produced.** `tools/dependency_graph/cluster_report.py` holds the
cluster map and measures it against the real graph: 96 engine-internal include edges, 204
sandbox→engine boundary edges, plus a word-boundary symbol grep for what is actually
referenced from outside each cluster. Re-run it after any change:

```
python tools/dependency_graph/cluster_report.py --symbols
```

Every "used externally" list below is measured, not assumed. The symbol pass counts textual
references including comments, so single-file hits are worth eyeballing (one is called out
under Platform).

---

## Layering

```
                          AppLifecycle
                                |
              +-----------------+-----------------+
              |                 |                 |
        RenderBackend        Widgets         (Input, EventBus,
              |                 |              Memory, Platform)
        RenderFrontend  <-  UiFacade
              |
   MathCore   |   HierCoords          Diagnostics <-> Platform   (cycle)
              |                              |
          Foundation  <---------------------- everything
```

Acyclic apart from two cycles: `Diagnostics <-> Platform` (1 edge each way) and
`RenderBackend <-> DeadStarfield` (dead code). `AppLifecycle` is the only cluster nothing
depends on; `Foundation` is the only one that depends on nothing.

`HierCoords` and `Widgets` have zero engine-side consumers — they exist purely to serve the
sandbox.

| cluster | files | intra | out | in | confidence |
|---|--:|--:|--:|--:|---|
| Foundation | 1 | 0 | 0 | 20 | high |
| Containers | 3 | 0 | 0 | 2 | — (see "doesn't fit") |
| Diagnostics | 3 | 2 | 3 | 10 | high |
| Memory | 4 | 3 | 6 | 6 | high |
| Platform | 4 | 2 | 7 | 5 | high |
| EventBus | 2 | 1 | 4 | 3 | high |
| Input | 2 | 1 | 4 | 2 | high |
| AppLifecycle | 4 | 5 | 10 | 0 | high |
| MathCore | 2 | 1 | 1 | 5 | high |
| HierCoords | 2 | 1 | 2 | 0 | provisional |
| RenderFrontend | 6 | 7 | 10 | 7 | provisional |
| RenderBackend | 4 | 2 | 12 | 1 | high |
| UiFacade | 2 | 0 | 2 | 7 | provisional |
| Widgets | 2 | 1 | 3 | 0 | high |
| DeadStarfield | 2 | 1 | 5 | 1 | high (delete) |

---

## Subsystems

### Foundation
**Responsibility:** Defines the engine's scalar type vocabulary, boolean and pointer macros,
compile-time platform detection, and the `bs__api__` DLL export/import macro.

**Files:** `defines.h`

**Used externally:** the `u8`–`i64`/`f32`/`f64`/`b8` typedefs, `TRUE`/`FALSE`/`VOID_PTR`,
`bs__api__`. 20 engine includers, 55 sandbox — the graph's universal root.

**Depends on:** nothing. **Depended on by:** every other subsystem.

**Confidence:** high. One file, but a genuine single responsibility and the base of both trees.

---

### Diagnostics
**Responsibility:** Formats and emits levelled log messages, and supplies the
assertion-failure reporter.

**Files:** `core/logger.cpp`, `core/logger.h`, `core/asserts.h`

**Used externally:** all six `BS_LOG_*` macros (each referenced by 5–7 engine and 1–13
sandbox files); `logger_initialize`. **Not used outside:** `logger_output` (reached only
through the macros), `logger_terminate`, `report_assertion_failure`, `BS_ASSERT`.

**Depends on:** Foundation, Platform. **Depended on by:** Memory, Input, Platform,
AppLifecycle, RenderFrontend, RenderBackend.

**Confidence:** high. `asserts.h` belongs here by linkage — its only function is defined in
`logger.cpp`, which is also its only includer.

---

### Platform
**Responsibility:** Abstracts the OS and SDL3 behind windowing, an event pump, memory
primitives, console output, and timing.

**Files:** `platform/platform.h`, `platform/platform_sdl3.cpp`,
`platform/platform_commons.cpp`, `platform/platform_commons.h`

**Used externally:** every `platform_*` function, each by exactly 1–2 engine files, plus
`PAGE_SIZE` (arena only). **Zero real sandbox usage** — the single apparent hit is a comment
in `sandbox/source/core/profiler.h` noting that `platform_get_absolute_time` is
engine-internal, which is why the sandbox uses `std::chrono` instead.

**Depends on:** Foundation, Diagnostics, Input, EventBus, UiFacade.
**Depended on by:** Memory, Diagnostics, AppLifecycle, RenderBackend.

**Confidence:** high. Note it is not a clean bottom layer: `platform_sdl3.cpp` feeds events
to the UI facades, so Platform depends upward.

---

### Memory
**Responsibility:** Provides tagged heap allocation with per-tag accounting and a
reserve/commit bump arena over the platform's memory primitives.

**Files:** `core/memory/bs_memory.{cpp,h}`, `core/memory/arena.{cpp,h}`

**Used externally:** `bs_memory_allocator` (1 engine + 7 sandbox), `bs_memory_free` (1 + 5),
`MEMORY_TAG_*` (1 + 9), `bs_memory_initialize`/`_terminate` (from `entry.h`),
`bs_memory_copy`, `bs_memory_get_memory_usage_string`, `arena_initialize`/`_reset`/
`_terminate`. **Not used outside:** `bs_memory_zero`, `bs_memory_set`, the virtual-memory
trio, and `arena_allocate` — see "doesn't fit" below.

**Depends on:** Foundation, Diagnostics, Platform.
**Depended on by:** AppLifecycle, EventBus, Input, RenderFrontend.

**Confidence:** high for the cluster; the arena's deadness is a separate finding.

---

### EventBus
**Responsibility:** Routes coded events from publishers to registered listener callbacks with
first-handler-wins dispatch.

**Files:** `core/event.{cpp,h}`

**Used externally:** `event_fire` and `EVENT_CODE_*` (3 engine files each);
`event_register`/`_unregister`/`_initialize`/`_terminate` (application only). Zero sandbox
usage, despite `event_register`/`_fire` being exported.

**Depends on:** Foundation, Containers, Memory. **Depended on by:** AppLifecycle, Input, Platform.

**Confidence:** high.

---

### Input
**Responsibility:** Holds current and previous keyboard and mouse state, turning
platform-pushed edges into bus events and answering polling queries.

**Files:** `core/input.{cpp,h}`

**Used externally — two disjoint audiences:**
- *Push side* (`input_process_key`/`_button`/`_mouse_move`/`_mouse_wheel`, `input_update`,
  lifecycle): Platform and AppLifecycle only, never exported.
- *Poll side*: sandbox only — `input_get_mouse_position` (4), `input_is_key_down` (3),
  `input_is_button_down` (3), `input_was_button_down` (3), `input_was_key_down` (2),
  `input_get_mouse_wheel` (1).

**Not used outside:** `input_is_key_up`, `input_was_key_up`, `input_get_previous_mouse_position`.

**Depends on:** Foundation, EventBus, Memory, Diagnostics. **Depended on by:** Platform, AppLifecycle.

**Confidence:** high. The `bs__api__` split already encodes the two audiences.

---

### AppLifecycle
**Responsibility:** Owns process startup, subsystem bring-up order, the frame loop, and the
`Game` callback contract the host implements.

**Files:** `core/application.{cpp,h}`, `entry.h`, `game_types.h`

**Used externally:** almost nothing by call. `application_init` and `application_run` are
invoked only from `entry.h`, which is inside the cluster. The real external surface is
**inverted** — the host must *define* `game_create` and populate `Game`'s four function
pointers. One sandbox file references `entry.h`, one `game_types.h`.

**Depends on:** Memory, Diagnostics, Platform, RenderFrontend, EventBus, Input, Foundation.
**Depended on by:** nothing.

**Confidence:** high.

---

### MathCore
**Responsibility:** Provides 2D/3D vector, matrix, and scalar-clamp primitives under a fixed
column-major, Vulkan-clip convention.

**Files:** `math/math_utils.{cpp,h}`

**Used externally — split audience:** `vec2_length` (25 sandbox), `vec2_add`/`vec2_scale`
(22), `clampf` (21), `vec2_sub` (18), `BS_PI` (15), `vec2_rotate` (11) are sandbox-dominated;
**every `mat4_*` is engine-only** (camera2d and the backend, 1–2 files, zero sandbox).
`vec3_add`/`_sub`/`_scale` are unused outside.

**Depends on:** Foundation. **Depended on by:** HierCoords, RenderFrontend, RenderBackend.

**Confidence:** high.

---

### HierCoords — *provisional*
**Responsibility:** Provides precision-safe galaxy-scale positions as an integer cell plus a
local offset, with conversions and arithmetic.

**Files:** `math/bs_hierpos.{cpp,h}`

**Used externally:** `BS_HIERPOS_CELL_SIZE` (33 files), `hierpos_diff` (32),
`hierpos_add_vec2` (15), `hierpos_to_f64` (11), `hierpos_from_vec2` (9), `hierpos_to_vec2`
(4), `hierpos_lerp`/`hierpos_add_f64` (2 each), `bs_hierpos_selftest` (1).
**All sandbox — zero engine consumers outside the cluster.**

**Depends on:** Foundation, MathCore. **Depended on by:** nothing engine-side; 23 sandbox files.

**Open question:** merge with MathCore into one `Math` subsystem, or keep split?
*Working default: keep split.* The audiences are disjoint (nothing in the engine calls
`hierpos_*`; nothing in the sandbox calls `mat4_*`), and a merged responsibility sentence
needs a "plus", which fails the one-sentence test.

---

### RenderFrontend — *provisional*
**Responsibility:** Exposes the backend-agnostic rendering API and shared render vocabulary,
forwarding draw and state calls through a backend vtable.

**Files:** `renderer/renderer.{cpp,h}`, `renderer/renderer_types.h`,
`renderer/renderer_backend.h`, `renderer/camera2d.{cpp,h}`

**Used externally:** ~40 `renderer_*` functions, essentially all sandbox-facing. Heaviest:
`renderer_draw_line` (11), `camera2d_world_to_screen` (8), `renderer_draw_sprite`/`_quad`/
`_circle` and `camera2d_screen_to_world` (6 each), `renderer_create_texture` (4).
Engine-internal only: the lifecycle four (`initialize`, `on_resize`, `begin_frame`,
`end_frame`, called by AppLifecycle), `renderer_report_frame_timing`, and
`camera2d_view_proj` (backend). **Not used outside:** `renderer_update_texture`,
`renderer_get_draw_alpha`, `renderer_draw_grid`, `renderer_get_frame_stats`.

**Depends on:** Foundation, MathCore, Diagnostics, Memory, UiFacade.
**Depended on by:** AppLifecycle, RenderBackend, Widgets.

**Open question:** does `camera2d` belong here, in Math, or in its own `ViewTransform`
subsystem? *Working default: keep it here.* It is stateless coordinate math with no renderer
state, but its `Camera2D` type is defined in `renderer_types.h`, so moving it would relocate
the coupling rather than remove it. Weakest of the three provisional calls — 17 sandbox files
use it for picking and HUD anchoring rather than drawing.

---

### RenderBackend
**Responsibility:** Implements all GPU work against SDL3 GPU and binds it into the frontend's
vtable.

**Files:** `renderer/backend/renderer_backend_sdlgpu.{cpp,h}`,
`renderer/backend/stb_image_impl.cpp`, `renderer/renderer_backend.cpp`

**Used externally:** only `renderer_backend_create` and `renderer_backend_destroy`, from one
engine file. Every `sdlgpu_backend_*` symbol is reached solely through the vtable; nothing is
sandbox-facing.

**Depends on:** RenderFrontend, UiFacade, Platform, Diagnostics, MathCore, Foundation.
**Depended on by:** nothing (except the dead starfield module).

**Confidence:** high. Note `renderer_backend.cpp` (the factory) is grouped here rather than
with the frontend: it is the only file naming concrete `sdlgpu_*` symbols, and moving it
removed the frontend↔backend cycle the directory layout implies.

---

### UiFacade — *provisional*
**Responsibility:** Presents SDL-free, third-party-free control surfaces for Dear ImGui and
RmlUi, including the in-game HUD data model.

**Files:** `renderer/bs_imgui.h`, `renderer/bs_rml.h`

**Used externally:** `bs_imgui_wants_mouse` (2 engine + 5 sandbox), `bs_rml_wants_mouse`
(2 + 4), `bs_rml_set_sharpen` (1 + 3), the HUD trio `bs_rml_hud_init`/`_update`/
`_poll_action` (1 + 1 each), `bs_imgui_wants_keyboard`/`bs_rml_wants_keyboard`/
`bs_rml_load_fonts`/`bs_rml_debugger_toggle` (1 + 1 each). The lifecycle and event functions
are used only by RenderFrontend and Platform.

**Depends on:** Foundation. **Depended on by:** Platform, RenderFrontend, RenderBackend, Widgets.

**Open question:** is this a subsystem, or part of RenderBackend? *Working default: keep it
separate and record the debt.* See the coupling flag below — these two headers have no
implementation files of their own.

---

### Widgets
**Responsibility:** Translates an engine-native immediate-mode widget vocabulary into calls on
ImGui's shared context.

**Files:** `renderer/bs_ui.{cpp,h}`

**Used externally:** 18 of 24 `bs_ui_*` functions, **100% sandbox** — `bs_ui_text` and
`_text_colored` (6 files each), `_button` and `_separator` (5), `_begin_window`/`_end_window`/
`_combo` (3). **Not used outside:** `bs_ui_color_button`, `_is_window_hovered`, `_push_alpha`,
`_pop_alpha`, `_begin_hud_panel`, `_end_hud_panel`.

**Depends on:** UiFacade, Foundation, RenderFrontend (only for `bs_color`, in one signature).
**Depended on by:** nothing engine-side; 8 sandbox files.

**Confidence:** high. Distinct from UiFacade: it has its own translation unit and drives
ImGui's backend-agnostic core, never the platform backends.

---

### DeadStarfield
**Responsibility:** (Superseded) Owned the VBO starfield pipeline's GPU resources.

**Files:** `renderer/starfield_gpu_resources.{cpp,h}`

**Used externally:** nothing. The class is never instantiated.

**Confidence:** high — recommend deletion. See below.

---

## Files that don't fit cleanly

**`containers/array.h`, `string.h`, `vector.h` — not a subsystem.** Zero internal edges, no
shared behaviour, three independent one-line macro aliases over `std::`. `string.h` has zero
includers anywhere; `array.h` has one (`event.cpp`); `vector.h`'s only includer is a *sandbox*
file. Loose shared utility at best. `event.cpp` is the sole engine consumer and could use
`std::` directly, after which the directory could go.

**`renderer/starfield_gpu_resources.{cpp,h}` — dead code, and a seam breach.**
`StarfieldGpuResources` is never instantiated. Its CPU-side producer
(`sandbox/source/render/starfield_generator`) is equally dead, because
`sandbox/source/render/starfield_layer.cpp` sets `layer_data = nullptr` ("procedural path"),
which kills both halves of the old VBO pipeline. Separately, its header is the **only file
outside the backend TU that includes `<SDL3/SDL_gpu.h>`** — the one real breach of the
"only the backend touches SDL" rule asserted in five other files. Deleting it removes the dead
code and the breach together.

**`core/memory/arena.{cpp,h}` — live but unreachable.** `application.cpp` creates a 64 MB frame
arena, resets it at the top of every frame, and destroys it at shutdown, but **`arena_allocate`
has zero call sites in either tree** (declaration and definition only). The arena is currently
a 64 MB reservation and a per-frame reset serving no allocation.

**`core/asserts.h` — inert.** No `BS_ASSERT*` call sites exist anywhere; its only function is
defined in `logger.cpp`. It sits in Diagnostics by linkage rather than by use. It also carries
two latent defects that non-use is hiding (`BS_ASSERT_DEBUG` references an undeclared
`message`; the assertions-off `BS_ASSERT_MSG` takes one parameter instead of two).

**`renderer/backend/stb_image_impl.cpp` — a build artifact, not a module.** Declares nothing,
has no includers, exists solely to compile stb_image's body once under warning suppressions.
Fine where it is; just not a design element.

**`entry.h` — cluster and build membership disagree.** It belongs to AppLifecycle by role, but
it is a header that *defines* `main()` and is compiled only into `sandbox.exe`. It is the one
engine file that never enters `engine.dll`.

**`renderer_backend_sdlgpu.cpp` — a god object.** 4888 lines carrying three unrelated public
surfaces: the backend vtable, the ImGui facade, and the RmlUi facade plus HUD data model.
Assigned to RenderBackend, but see below.

---

## Coupling flags

### RenderBackend ⇄ UiFacade — *maybe actually one subsystem*

The include graph shows only 2 edges, which badly understates the relationship.
**`bs_imgui.h` and `bs_rml.h` have no implementation files of their own** — all 29 of their
functions are defined inside `renderer_backend_sdlgpu.cpp`, occupying roughly **1866 of its
4888 lines (38%)**. The facades read the backend's `g_sdl` global directly rather than going
through an accessor.

So by *interface* they are a separate layer; by *code* they are the backend. This is the one
pair worth genuinely considering merging. The alternative — and the better fix — is to split
the backend TU so `bs_imgui.cpp` and `bs_rml.cpp` exist as real files and the boundary the
headers describe becomes structurally true.

### Diagnostics ⇄ Platform — a cycle, not a merge

`logger.cpp` → `platform.h` (needs console write); `platform_sdl3.cpp` → `logger.h` (logs).
One edge each way. A classic logging cycle between two clearly distinct responsibilities.
Worth knowing about; not worth merging.

### RenderBackend ⇄ DeadStarfield

A cycle entirely between the backend and dead code. Resolves itself on deletion.

### Platform → UiFacade — upward dependency

The platform event pump calls `bs_imgui_process_event` and `bs_rml_process_event`, so the
bottom-most layer depends on a UI layer. Deliberate and commented, but it means Platform
cannot be treated as a clean foundation layer.

---

## Open questions

Three grouping decisions have a working default applied but are not settled:

1. **UiFacade** — its own subsystem (current default, debt recorded), merged into
   RenderBackend, or kept separate *with* a planned file split extracting `bs_imgui.cpp` /
   `bs_rml.cpp`?
2. **HierCoords vs MathCore** — split (current default) or one `Math` subsystem?
3. **camera2d** — RenderFrontend (current default), its own `ViewTransform`, or Math?

Nothing else in this document depends on how these land.
