# Engine file summaries (raw)

Per-file structural notes for the `engine` side (`engine/source`, 43 files).
Files are described in isolation — no subsystem clustering or naming at this stage.
Companion to `dependency-graph.json` in this directory.

---

## engine/source/containers/array.h

- **Path:** `engine/source/containers/array.h`
- **Purpose:** Provides a one-line macro alias mapping `Array(type, n)` onto `std::array<type, n>`.
- **Key types/functions:** `Array(_data_type, _num_elems)` — object-like macro expanding to `std::array`. No types, functions, or storage of its own.
- **Notable non-obvious dependencies:** None. Includes only `<array>`; no global state, callbacks, or file I/O.
- **Interface vs internal:** Nominally public (a named container alias other code could adopt), but effectively unused — exactly one includer, `core/event.cpp`. The macro form means it participates in no overload resolution or namespacing; it is a naming-convention shim rather than an abstraction boundary.

---

## engine/source/containers/string.h

- **Path:** `engine/source/containers/string.h`
- **Purpose:** Provides a one-line macro alias mapping `String` onto `std::string`.
- **Key types/functions:** `String` — object-like macro expanding to `std::string`. Nothing else.
- **Notable non-obvious dependencies:** None. Includes only `<string>`.
- **Interface vs internal:** Dead as of this scan — **zero includers** across both trees (`fan_in: 0`). Structurally it is a public-looking alias header that nothing consumes; a macro named `String` with no include guard against conflicting definitions is also a latent collision risk for any third-party header included after it.

---

## engine/source/containers/vector.h

- **Path:** `engine/source/containers/vector.h`
- **Purpose:** Provides a one-line macro alias mapping `Vector(T)` onto `std::vector<T>`.
- **Key types/functions:** `Vector(_data_type)` — object-like macro expanding to `std::vector`. Nothing else.
- **Notable non-obvious dependencies:** None. Includes only `<vector>`.
- **Interface vs internal:** Public-facing in intent and the only one of the three container aliases consumed across the tree boundary — its single includer is a sandbox file, not an engine one. Like its siblings it is a macro shim, not an abstraction.

---

## engine/source/core/application.cpp

- **Path:** `engine/source/core/application.cpp`
- **Purpose:** Owns the process-wide application singleton and drives the main loop — subsystem bring-up, per-frame update/render/present sequencing, frame pacing, and ordered shutdown.
- **Key types/functions:**
  - `struct ApplicationState` — file-local; holds `Game* game_inst`, `is_running` / `is_suspended` flags, an embedded `PlatformState`, window `width`/`height`, and `last_time`.
  - `application_init(Game*)` — ordered bring-up: logger → input → event → platform (window) → renderer → the game's own `init`, then a synthetic `on_resize`.
  - `application_run()` — the frame loop; returns only at shutdown, then tears subsystems down in reverse.
  - `application_on_event` / `application_on_key` / `application_on_resized` — event-bus handlers, forward-declared at the top and registered during init.
- **Notable non-obvious dependencies:**
  - **Two file-static globals**, not passed or injected: `static ApplicationState app_state` and `static b8 initialized`. The `initialized` flag makes `application_init` explicitly single-shot — a second call logs an error and fails.
  - **Registers four callbacks on the global event bus** (`event_register` for `APPLICATION_QUIT`, `KEY_PRESSED`, `KEY_RELEASED`, `WINDOW_RESIZED`) and symmetrically unregisters all four at the end of `application_run`.
  - **Fires** `EVENT_CODE_APPLICATION_QUIT` from the key handler on `KEY_ESCAPE` — a hardcoded quit binding living in the application layer, not in any config.
  - **Calls into the game through function pointers** on `Game` (`init`, `update`, `render`, `on_resize`), so the concrete game is late-bound; the engine never names a sandbox symbol.
  - **Allocates a 64 MB frame arena** (`arena_initialize(1024*1024*64)`) inside `application_run` and `arena_reset`s it at the top of every iteration — a per-frame scratch lifetime other subsystems implicitly rely on.
  - **Queries the renderer for present mode** (`renderer_is_present_immediate`) to decide whether to apply its own 60 FPS sleep-based cap, and pushes timing back via `renderer_report_frame_timing`; warns on frames over 100 ms.
  - Zero-size window from the resize event is interpreted as minimize → sets `is_suspended`, which gates update/render/input entirely.
  - `application_on_resized` deliberately returns `FALSE` even after handling, so the event continues propagating to other listeners.
- **Interface vs internal:** Implementation. The public surface is the two functions declared in `application.h`; everything here — the state struct, the statics, the handlers — is private to the translation unit. It is, however, the *orchestrator*: it defines the initialization and shutdown order every other engine subsystem is bound by, and the one-way call into `Game`'s function pointers is the engine→game inversion point.

---

## engine/source/core/application.h

- **Path:** `engine/source/core/application.h`
- **Purpose:** Declares the application's two-call public lifecycle (`init` then `run`) and the config struct a host fills in to describe its window.
- **Key types/functions:**
  - `struct ApplicationConfig` — window `start_pos_x` / `start_pos_y` / `start_width` / `start_height` (all `i16`) plus a `const char* name`. Every field is commented "if applicable", i.e. advisory to the platform layer.
  - `bs__api__ b8 application_init(Game* game_inst)` and `bs__api__ b8 application_run()` — the entire exported surface.
  - Forward-declares `struct Game` rather than including `game_types.h`, keeping the header dependency-light.
- **Notable non-obvious dependencies:** Both functions carry `bs__api__`, the dllexport/dllimport macro from `defines.h` — these cross the DLL boundary into `sandbox.exe`. The header declares no state, but `application_init`'s contract is single-shot and its ordering guarantees live entirely in the `.cpp`; nothing here signals that. `ApplicationConfig` is declared here but *consumed* by way of `Game::app_config`, so its real reader is `application.cpp`.
- **Interface vs internal:** Public interface — one of the engine's primary exported entry points. Deliberately minimal: a host supplies a `Game`, calls two functions, and never touches application state directly.

---

## engine/source/core/asserts.h

- **Path:** `engine/source/core/asserts.h`
- **Purpose:** Defines the assertion macro family and the compiler-specific debug-break primitive.
- **Key types/functions:**
  - `debugBreak()` — `__debugbreak()` under `_MSC_VER` (the comment notes clang-on-Windows honours the MS extension), `__builtin_trap()` otherwise.
  - `bs__api__ void report_assertion_failure(const char*, const char*, const char*, i32)` — declared here only.
  - `BS_ASSERT(expr)`, `BS_ASSERT_MSG(expr, message)`, `BS_ASSERT_DEBUG(expr)` — all report-then-break; `BS_ASSERT_DEBUG` compiles to nothing unless `BS_DEBUG`.
  - `BS_ASSERTIONS_ENABLED` is `#define`d unconditionally at the top of the file, so the "assertions off" branch is currently unreachable.
- **Notable non-obvious dependencies:**
  - **`report_assertion_failure` is declared here but implemented in `core/logger.cpp`** — a link-time dependency invisible in the include graph, and the reason asserting pulls in the logger.
  - **The whole macro family is unused**: no `BS_ASSERT`, `BS_ASSERT_MSG`, or `BS_ASSERT_DEBUG` call sites exist anywhere in either tree.
  - Two latent defects, both masked by that non-use: `BS_ASSERT_DEBUG(expr)` expands to `report_assertion_failure(#expr, message, ...)` but has **no `message` parameter**, so any use fails to compile; and in the assertions-disabled branch `BS_ASSERT_MSG(expr)` is declared with **one parameter instead of two**, so toggling assertions off would break every two-argument call site.
  - Macros are unhygienic (`{ if(expr){}else{...} }` with an empty true-branch), so a trailing `else` after `BS_ASSERT(x);` binds unexpectedly.
- **Interface vs internal:** Public interface by intent — `bs__api__` marks `report_assertion_failure` for export and the macros are written for engine-wide and game-wide use. In practice it is inert: nothing includes it for the macros, and its one function is reached only through the logger's definition.

---

## engine/source/core/event.cpp

- **Path:** `engine/source/core/event.cpp`
- **Purpose:** Implements the global publish/subscribe event bus — a fixed code-indexed table of listener lists with first-handler-wins dispatch.
- **Key types/functions:**
  - `struct __EventRegistered` — one `{ VOID_PTR listener; PFN_on_event callback; }` pair.
  - `struct __EventCodeEntry` — `Vector(__EventRegistered) events`, the listener list for a single code.
  - `struct __EventSystemState` — `Array(__EventCodeEntry, MAX_MESSAGE_CODES) registered`, a direct-indexed lookup table; `MAX_MESSAGE_CODES` is 10000.
  - `event_initialize` / `event_terminate` / `event_register` / `event_unregister` / `event_fire`.
- **Notable non-obvious dependencies:**
  - **Two file-static globals**: `static __EventSystemState state` and `static b8 initialized`. `event_initialize` is single-shot (returns `FALSE` on a second call); every other entry point silently returns `FALSE` when uninitialized, so registration before init fails quietly rather than loudly.
  - **The static table is large and eagerly sized** — 10000 `std::vector` slots live in the DLL's static storage regardless of how many codes are used (a few hundred KB, and 10000 vector constructions at load).
  - **`state = {}` on init destroys and reconstructs all 10000 vectors**, which is also the only path that clears stale registrations.
  - **`event_terminate()` is an empty body** — it does not clear the table or drop listeners, so the "unregister everything" symmetry `application.cpp` performs by hand is the only real teardown.
  - **`code` is never bounds-checked.** The parameter is `u16` (up to 65535) but `registered` holds 10000 entries, and `std::array::operator[]` is unchecked — any code ≥ 10000 is an out-of-bounds access. The header's "application should use codes beyond 255" invites exactly this.
  - **Dispatch is first-handler-wins**: `event_fire` stops at the first callback returning `TRUE`, so listener order (registration order) is semantically significant and invisible at the call site.
  - **`registered_count` is captured before the dispatch loop**, so a callback that registers or unregisters during `event_fire` on the same code invalidates the vector under the iteration. `application.cpp` does re-enter `event_fire` from inside a handler, though on a different code.
  - Includes `core/memory/bs_memory.h` but makes no call into it — the container aliases resolve to `std::vector`/`std::array`, which use the global allocator, so the bus is **not** routed through the engine's own memory tracking.
  - `event_unregister` carries a self-noted O(n) `erase`; combined with the linear duplicate scan in `event_register`, all operations are linear in listeners-per-code.
- **Interface vs internal:** Implementation. The `__`-prefixed structs and both statics are private to the TU; the surface is what `event.h` declares. Behaviourally it is a hub — anything that registers a callback is coupled to it invisibly through the include graph.

---

## engine/source/core/event.h

- **Path:** `engine/source/core/event.h`
- **Purpose:** Declares the event bus's public API, the type-punned payload struct callbacks receive, and the reserved system event codes.
- **Key types/functions:**
  - `event_context_t` / `event_context` — a struct wrapping a single anonymous `union data` viewable as `i64/u64/f64[2]`, `i32/u32/f32[2]`, `i16/u16[8]`, `i8/u8[16]`, or `char c[16]`. Stack-allocated by contract.
  - `PFN_on_event` — `b8 (*)(u16 code, VOID_PTR sender, VOID_PTR listener, event_context_t data)`; the indirection that keeps sender and listener from referencing each other directly.
  - `event_initialize` / `event_terminate` (unexported) and `event_register` / `event_unregister` / `event_fire` (all `bs__api__`).
  - `enum ESystemEventCode` — `APPLICATION_QUIT` (0x01) through `WINDOW_RESIZED` (0x08).
- **Notable non-obvious dependencies:**
  - **The payload is an untagged union** — nothing in the type records which member was written, so every code's field layout is a convention agreed out-of-band between firer and handler. Only `WINDOW_RESIZED` documents its layout (`u16[0]`=width, `u16[1]`=height) in a comment; the rest are undocumented.
  - The comment claims "128 bytes"; the union is actually **16 bytes** (largest member `i64[2]`/`char[16]`).
  - **`initialize`/`terminate` lack `bs__api__` while the other three have it** — deliberate, and it means only the engine can bring the bus up; the game may only use it.
  - The "application should use codes beyond 255" note is the only statement of the code-space split, and it is unenforced — as is the 10000-code ceiling the implementation actually imposes.
  - Returning `TRUE` from a handler silently suppresses delivery to every later listener; the header does not say so.
- **Interface vs internal:** Public interface, and one of the engine's main extension points — it crosses the DLL boundary and is how the game subscribes to input and window events without linking against the emitters.

---

## engine/source/core/input.cpp

- **Path:** `engine/source/core/input.cpp`
- **Purpose:** Implements the input subsystem — holds current/previous keyboard and mouse snapshots, converts platform-pushed edges into bus events, and answers polling queries.
- **Key types/functions:**
  - `struct __KeyboardState` — `b8 keys[256]`.
  - `struct __MouseState` — `i16 x`, `i16 y`, `u8 buttons[BUTTON_MAX_BUTTONS]`.
  - `struct __InputState` — current/previous pairs of both.
  - Push side (called by the platform layer): `input_process_key`, `input_process_button`, `input_process_mouse_move`, `input_process_mouse_wheel`.
  - Poll side (called by the game): the eight `is_/was_ key/button` predicates, `input_get_mouse_position`, `input_get_previous_mouse_position`, `input_get_mouse_wheel`.
  - Lifecycle: `input_initialize`, `input_terminate`, `input_update(real dt)`.
- **Notable non-obvious dependencies:**
  - **Three file-static globals**: `static __InputState state`, `static b8 initialized`, and `static i32 mouse_wheel_accum` — the last deliberately kept outside `__InputState` because it is an event-driven delta rather than a sampled level (comment says so).
  - **Fires events on the global bus for every state change** — `KEY_PRESSED`/`KEY_RELEASED`, `BUTTON_PRESSED`/`BUTTON_RELEASED`, `MOUSE_MOVED`, `MOUSE_WHEEL`. Dispatch is synchronous inside the platform's message pump, so handlers run mid-pump, not at a frame boundary.
  - **Edge-triggered, not level-triggered**: `input_process_key`/`_button` only fire when the value actually changes, so no auto-repeat events reach the bus. The wheel is the exception — it fires unconditionally on every notch.
  - **Frame-phase coupling**: `input_update` copies current→previous and zeroes the wheel accumulator, and `application.cpp` calls it *after* the game's update. The `was_*` predicates therefore mean "as of last frame", and `input_get_mouse_wheel` must be polled from the game's update or the value is already gone.
  - **Routes its snapshot copy through `bs_memory_copy`**, unlike the event bus which uses raw `std::vector`.
  - **No bounds checking on any index** — `keys` and `buttons` are used as raw array subscripts in both the push and poll paths.
  - `input_process_mouse_move` takes signed `i16` coordinates but packs them into `context.data.u16[0..1]`, so negative positions (cursor dragged off the top/left of the window) reach listeners wrapped; the accessor pair returns them correctly as `i32` because it reads the struct, not the event.
  - Includes its own header twice, as `"core/input.h"` and `"input.h"` — harmless under `#pragma once`, but it means two distinct spellings resolve to one file.
  - `input_terminate` only clears the `initialized` flag; key and button state survives, so a re-`initialize` is what actually resets it.
- **Interface vs internal:** Implementation. The structs and statics are TU-private. Note the file serves **two different callers with different rights**: the `input_process_*` push functions are unexported and meant only for the platform backend, while the polling half is exported to the game.

---

## engine/source/core/input.h

- **Path:** `engine/source/core/input.h`
- **Purpose:** Declares the input subsystem's key/button enumerations and splits its API into an exported polling half and an engine-only push and lifecycle half.
- **Key types/functions:**
  - `enum buttons` — `BUTTON_LEFT`, `BUTTON_RIGHT`, `BUTTON_MIDDLE`, plus the `BUTTON_MAX_BUTTONS` count sentinel.
  - `enum keys` — ~120 entries built through the `DEFINE_KEY(name, code)` macro, terminated by `KEYS_MAX_KEYS`.
  - Exported (`bs__api__`): the eight key/button predicates, both mouse-position getters, and `input_get_mouse_wheel`.
  - Unexported: `input_initialize`, `input_terminate`, `input_update`, and all four `input_process_*` functions.
- **Notable non-obvious dependencies:**
  - **The key codes are Windows virtual-key codes** (`BACKSPACE` 0x08, `ESCAPE` 0x1B, `A` 0x41, `GRAVE` 0xC0…). They are declared platform-neutrally, but any non-Windows backend must translate into this numbering — an implicit contract with the platform layer.
  - `DEFINE_KEY` leaks into the including TU: the macro is defined and never `#undef`'d.
  - **The `bs__api__` split is the access-control mechanism** — the game can poll input but cannot inject it or drive the lifecycle, since `input_process_*` is not exported from the DLL.
  - `KEYS_MAX_KEYS` is not a usable array bound: it evaluates to 0xC1 while the implementation's array is 256, and because the codes are sparse the enum is not densely packed.
  - The wheel accessor's contract is documented here in unusual detail (reset each `input_update`, poll from the game's update, idempotent within a frame) — a frame-ordering dependency that would otherwise be invisible.
- **Interface vs internal:** Both, explicitly. It is a public interface for input *queries* and an internal header for input *injection*; the two halves are distinguished only by the presence of `bs__api__`.

---

## engine/source/core/logger.cpp

- **Path:** `engine/source/core/logger.cpp`
- **Purpose:** Implements formatted log output — expands varargs, prefixes a level tag, and hands the finished line to the platform console writer.
- **Key types/functions:**
  - `logger_output(ELogLevel, const char* message, ...)` — the single real function; formats and dispatches.
  - `report_assertion_failure(expression, message, file, line)` — emits a `LOG_LEVEL_FATAL` line.
  - `logger_initialize()` / `logger_terminate()` — both empty stubs carrying TODOs about a not-yet-existing log file and batched flush.
- **Notable non-obvious dependencies:**
  - **This is where `report_assertion_failure` (declared in `asserts.h`) is actually defined** — a link-time coupling between the assertion and logging subsystems that the include graph does not show, and the reason `logger.cpp` is `asserts.h`'s only includer.
  - **Delegates all output to the platform layer** (`platform_console_write` / `platform_console_write_error`), passing the log `level` through as a second argument the platform uses for colouring — so log level doubles as a presentation parameter.
  - Severity routing: `is_error = level < LOG_LEVEL_WARN`, i.e. only FATAL and ERROR go to the error stream.
  - **Two 32000-byte stack buffers per call** (`out_message` and `final_message`) — roughly 64 KB of stack for every log line, notable for deep call stacks or any threaded use.
  - `level_strings[6]` is indexed directly by `level` with **no bounds check**, and is rebuilt on the stack on every call.
  - Uses `__builtin_va_list` rather than `va_list`, flagged in-file as a compiler-specific workaround — a hard dependency on clang/GCC builtins.
  - The `memset` of `out_message` before `vsnprintf` is redundant, and the two-buffer split is deliberate (commented) to avoid overlapping source/destination UB when prefixing.
  - **No file output and no batching exist yet** despite the name and the TODOs; nothing is buffered, so nothing needs flushing — which is why the empty `logger_terminate` is currently harmless.
  - No runtime level filter: every call reaching here is formatted and written. Filtering is entirely compile-time, in the header.
- **Interface vs internal:** Implementation, with one important exception — it silently supplies `asserts.h`'s only symbol. Everything else is reached through `logger.h`'s macros.

---

## engine/source/core/logger.h

- **Path:** `engine/source/core/logger.h`
- **Purpose:** Declares the log level enum and the `BS_LOG_*` macro family, with per-level compile-time enable switches.
- **Key types/functions:**
  - `enum ELogLevel` — `FATAL`=0 through `TRACE`=5, ordered most-to-least severe (so numeric comparison means severity).
  - `bs__api__ void logger_output(ELogLevel, const char*, ...)`; unexported `logger_initialize` / `logger_terminate`.
  - `BS_LOG_FATAL`, `BS_LOG_ERROR`, `BS_LOG_WARN`, `BS_LOG_INFO`, `BS_LOG_DEBUG`, `BS_LOG_TRACE`.
  - `LOG_WARN_ENABLED` / `LOG_INFO_ENABLED` / `LOG_DEBUG_ENABLED` / `LOG_TRACE_ENABLED`, all `1`, with DEBUG and TRACE forced to `0` under `#if BSRELEASE == 1`.
- **Notable non-obvious dependencies:**
  - **The highest fan-in header after `defines.h`** — 25 includers across both trees. Almost every subsystem is compile-time coupled to it, and through it to the platform console writer.
  - **`BSRELEASE` is never defined by either build script** (both pass `-DBS_DEBUG`), so the release branch is currently dead and DEBUG/TRACE are always compiled in. Because it is tested with `#if` rather than `#ifdef`, an undefined `BSRELEASE` quietly evaluates to `0` instead of erroring.
  - **FATAL and ERROR have no enable switch** — the comment states they are always reported, so those two macros can never be compiled out.
  - `BS_LOG_ERROR` alone is wrapped in `#ifndef`, letting a prior definition win; the other five would collide. The asymmetry is unexplained.
  - **Every macro ends in a trailing semicolon**, so a call site written `BS_LOG_INFO("x");` emits an empty statement. Combined with the disabled variants expanding to *nothing*, an unbraced `if (c) BS_LOG_DEBUG("x"); else …` changes meaning between configurations.
  - Level gating is purely preprocessor — there is no runtime verbosity control anywhere in the subsystem.
- **Interface vs internal:** Public interface, and the most widely consumed one in the engine. `logger_output` is exported across the DLL boundary so the game's `BS_LOG_*` expansions link against the engine's single implementation; the lifecycle pair stays engine-private.

---

## engine/source/core/memory/arena.cpp

- **Path:** `engine/source/core/memory/arena.cpp`
- **Purpose:** Implements a reserve-then-commit-on-demand bump allocator: virtual address space is reserved up front and physical pages are committed lazily as the offset grows.
- **Key types/functions:**
  - `arena_initialize(u64 reserve_size)` — allocates the `arena_t` header, page-aligns the reservation, reserves the block, zeroes the offsets.
  - `arena_allocate(ARENA_PTR, u64 size)` — bumps `current_offset`, committing more pages first if the new offset passes `commited_size`.
  - `arena_reset(ARENA_PTR)` — sets `current_offset = 0`.
  - `arena_terminate(ARENA_PTR)` — releases the whole reservation, then frees the header.
  - `_aligment_to_nearest_page_size(_v)` — file-local round-up macro.
- **Notable non-obvious dependencies:**
  - **Depends on `PAGE_SIZE` from `platform/platform_commons.h`** — the only reason this file includes the platform layer, and the value that drives every commit calculation.
  - **The arena header itself is heap-allocated through `bs_memory_allocator` under `MEMORY_TAG_ARENA`**, so arenas show up in the engine's memory accounting even though the arena *body* is raw virtual memory that the tagged accounting never sees. `arena.h` carries a TODO to add arena statistics for exactly this gap.
  - **Commit grows monotonically and `arena_reset` never decommits** — deliberate for a per-frame arena (pages stay warm), but it means an arena's resident footprint is its high-water mark for the process lifetime.
  - **Allocations are not aligned.** `arena_allocate` returns `base_ptr + current_offset` after a raw `+= size`, so the returned pointer inherits whatever alignment the running total happens to have. Only page *commit* boundaries are aligned. Callers needing natural alignment must pad `size` themselves, and nothing in the API says so.
  - Memory is **not zeroed** on allocate or reset — a reset arena hands back the previous frame's bytes.
  - Error handling is asymmetric: `arena_initialize` and `arena_allocate` validate and log through `BS_LOG_ERROR`, while `arena_reset` and `arena_terminate` dereference their argument with **no null check**.
  - `arena_terminate` passes size `0` to `bs_memory_virtual_free`, relying on the documented "0 means release the entire block" convention — a platform-level contract encoded only in a trailing comment.
  - The alignment macro takes `_v` unparenthesised in `(_v + PAGE_SIZE - 1)`, so a low-precedence argument expression would expand wrong; both call sites pass simple variables.
- **Interface vs internal:** Implementation of a public API — all four functions are exported. Its one live consumer is `application.cpp`, which builds the 64 MB per-frame arena.

---

## engine/source/core/memory/arena.h

- **Path:** `engine/source/core/memory/arena.h`
- **Purpose:** Declares the arena allocator's state struct and its four-call exported API.
- **Key types/functions:**
  - `struct arena_t` — `u8* base_ptr`, `u64 reserved_size`, `u64 commited_size`, `u64 current_offset`, each documented inline.
  - `#define ARENA_PTR arena_t*` — the handle type callers use.
  - `bs__api__` on all of `arena_initialize`, `arena_allocate`, `arena_reset`, `arena_terminate`.
- **Notable non-obvious dependencies:**
  - **Pulls `core/logger.h` and `bs_memory.h` into every includer** — a header-level dependency that is only needed by the `.cpp`, so including the arena transitively drags in the logging macros and the memory-tag enum.
  - **`arena_t` is fully defined rather than opaque**, so any consumer can read or write `current_offset` and `base_ptr` directly and bypass the API. Combined with `ARENA_PTR` being a macro rather than a typedef, the handle offers no encapsulation.
  - Carries a TODO to surface arena allocations in `bs_memory.h`'s statistics — acknowledging that arena-backed memory is currently invisible to the engine's accounting.
  - `struct arena_t;` is forward-declared immediately before its own definition, which is redundant.
  - Cites an external blog post as the design's source, which is the only documentation of the reserve/commit strategy's intent.
  - The header states nothing about alignment, thread-safety, or the fact that `arena_reset` leaves pages committed — all three are contracts a caller must infer from the implementation.
- **Interface vs internal:** Public interface, exported across the DLL boundary. In practice narrowly used: the engine's own frame arena is the only consumer in the tree.

---

## engine/source/core/memory/bs_memory.cpp

- **Path:** `engine/source/core/memory/bs_memory.cpp`
- **Purpose:** Implements the engine's memory facade — thin tagged wrappers over the platform allocator plus a running per-tag byte-count tally and a human-readable usage dump.
- **Key types/functions:**
  - `struct bs_memory_stats` — `u64 total_allocated` and `u64 tagged_allocations[MEMORY_TAG_MAX_TAGS]`.
  - `bs_memory_tag_strings[]` — fixed-width display labels, one per tag.
  - `bs_memory_allocator` / `bs_memory_free` — tagged heap alloc/free that update the tally.
  - `bs_memory_allocator_virtual_memory_reserve` / `_commit` / `bs_memory_virtual_free` — untagged pass-throughs used by the arena.
  - `bs_memory_zero` / `bs_memory_copy` / `bs_memory_set` — pass-throughs to the platform equivalents.
  - `bs_memory_get_memory_usage_string` — formats the tally with automatic b/kib/mib/gib scaling.
  - `bs_memory_initialize` (zeroes stats) and `bs_memory_terminate` (empty stub).
- **Notable non-obvious dependencies:**
  - **One file-static global**, `static struct bs_memory_stats stats`, mutated by every tagged alloc and free with no synchronisation.
  - **Every function is a forwarding shim to `platform/platform.h`** — this layer adds accounting and nothing else; the real allocation happens in the platform backend.
  - **The lifecycle is driven from outside the engine**: `bs_memory_initialize` and `bs_memory_terminate` are called only from `entry.h`, which is compiled into the *sandbox* executable, not the DLL. So the memory subsystem is brought up by the game before `application_init` and torn down after `application_run` — a bring-up ordering that lives in a header, not in `application.cpp` alongside every other subsystem.
  - **The virtual-memory trio bypasses the tally entirely**, so arena-backed memory (the 64 MB frame arena) never appears in the statistics — only the small `arena_t` headers do.
  - **`bs_memory_get_memory_usage_string` returns `_strdup`'d memory the caller must free**, flagged only by a trailing comment. Its single call site, `application.cpp:116`, neither frees it nor treats it as data — it passes the returned string **as the format argument** to `BS_LOG_INFO`, so any `%` in the text would be interpreted as a conversion specifier.
  - **The formatting loop passes a constant `8000` as `snprintf`'s size bound while advancing `buffer + offset`**, instead of the remaining space — the bound does not shrink as the buffer fills.
  - `bs_memory_free` subtracts the caller-supplied size from the counters, so a size that does not match the original allocation silently corrupts the tally (and wraps, since the counters are unsigned). Neither function bounds-checks `tag`.
  - `platform_allocate` is called with `aligned = FALSE` at the one call site, with a TODO to revisit alignment — the tag enum's stated purpose (choosing aligned vs unaligned) is unimplemented.
  - Allocations are zeroed on every `bs_memory_allocator` call, which is not stated in the header.
- **Interface vs internal:** Implementation of a fully exported API. The `stats` struct and tag strings are TU-private; everything else is reachable from the game.

---

## engine/source/core/memory/bs_memory.h

- **Path:** `engine/source/core/memory/bs_memory.h`
- **Purpose:** Declares the memory tag taxonomy and the exported allocation, virtual-memory, and raw-block-manipulation API.
- **Key types/functions:**
  - `enum EMemoryTag` — 18 categories (`UNKNOWN`, `ARRAY`, `VECTOR`, `DICT`, `RING_QUEUE`, `BST`, `STRING`, `APPLICATION`, `JOB`, `TEXTURE`, `MATERIAL_INSTANCE`, `RENDERER`, `GAME`, `TRANSFORM`, `ENTITY`, `ENTITY_NODE`, `SCENE`, `ARENA`) plus the `MEMORY_TAG_MAX_TAGS` sentinel.
  - Lifecycle: `bs_memory_initialize`, `bs_memory_terminate`.
  - Virtual memory: `bs_memory_allocator_virtual_memory_commit`, `_reserve`, `bs_memory_virtual_free`.
  - Heap: `bs_memory_allocator`, `bs_memory_free`.
  - Block ops: `bs_memory_zero`, `bs_memory_copy`, `bs_memory_set`.
  - Debug: `bs_memory_get_memory_usage_string`.
- **Notable non-obvious dependencies:**
  - **The enum is the coupling surface, not the functions.** `MEMORY_TAG_MAX_TAGS` sizes the stats array *and* the parallel label table in the `.cpp`, so adding a tag anywhere in the middle silently shifts both unless the string table is edited in lockstep — the two are ordered by position, not by name.
  - **`bs_memory_initialize`/`_terminate` carry an explicit TODO saying they "shall not be exported"**, yet both are `bs__api__` — and that export is load-bearing, because `entry.h` calls them from the sandbox side. Removing the export as the TODO suggests would break the game's startup.
  - The enum's leading comment states tags will select aligned versus unaligned allocation; no such branch exists in the implementation, so the tag is currently accounting-only.
  - `bs_memory_copy` takes `const VOID_PTR source`, which (because `VOID_PTR` is a pointer typedef/macro) constifies the pointer rather than the pointee — the `const` is not doing what it appears to.
  - Nothing here documents that `bs_memory_allocator` zeroes its block, that `bs_memory_get_memory_usage_string` returns an allocation the caller owns, or that the virtual-memory calls skip accounting — all three are implementation-side contracts.
  - Typo in the declaration parameter name: `resherve_size`.
- **Interface vs internal:** Public interface, exported wholesale. Widely included across both trees, and one of the engine headers the sandbox depends on directly.

---

## engine/source/defines.h

- **Path:** `engine/source/defines.h`
- **Purpose:** Establishes the engine's foundational vocabulary — fixed-width scalar typedefs, size static-assertions, boolean and pointer macros, compile-time platform detection, and the DLL export/import macro.
- **Key types/functions:**
  - Integer typedefs `u8`/`u16`/`u32`/`u64` and `i8`/`i16`/`i32`/`i64`; floating point `f32`/`f64` plus `real` (an alias for `double`); booleans `b32` (`int`) and `b8` (`char`).
  - `STATIC_ASSERT` — `_Static_assert` under clang/gcc, `static_assert` otherwise — used immediately to pin all ten integer and float sizes.
  - `TRUE` / `FALSE` / `VOID_PTR` macros.
  - Platform detection defining exactly one of `BS_PLATFORM_WINDOWS` / `_LINUX` / `_ANDROID` / `_POSIX` / `_APPLE` / `_IOS`, with `#error` on an unrecognised platform and on 32-bit Windows.
  - `bs__api__` — `__declspec(dllexport)` when `BSEXPORT` is defined, `__declspec(dllimport)` otherwise (with `visibility("default")` / empty as the non-MSVC fallbacks).
- **Notable non-obvious dependencies:**
  - **The single highest-fan-in file in the project: 75 includers**, and 55 of the 204 sandbox→engine boundary edges point at it. It has zero project includes of its own, so it is the graph's universal root — any change here recompiles essentially everything.
  - **`bs__api__` is the mechanism that makes the whole engine/sandbox split work.** It keys off `BSEXPORT`, which `engine/build.bat` passes and `sandbox/build.bat` does not — the same header therefore means "export" inside the DLL and "import" in the game.
  - **`sandbox/build.bat` defines `BSIMPORT`, but nothing in this file (or anywhere else) tests it** — the import branch is selected by the *absence* of `BSEXPORT`, so `BSIMPORT` is inert.
  - `real` is `double` while most of the codebase computes in `f32`; the mixed precision is a deliberate choice visible in `application.cpp`'s timing path.
  - `b8` is plain `char`, whose signedness is implementation-defined, and neither `b8` nor `b32` gets a size static-assertion despite every other type having one.
  - Typos disable two branches: `__gcc__` in the `STATIC_ASSERT` check is not a real predefined macro (`__GNUC__` is), and `__gnu_linus__` in the Linux check is misspelled. Under clang the first is harmless because `__clang__` matches.
  - `TRUE`, `FALSE`, and `VOID_PTR` are unscoped object-like macros that will collide with any third-party header using those names; because `VOID_PTR` is a macro rather than a typedef, `const VOID_PTR` binds the `const` to the pointer, not the pointee.
  - The `bs__api__` block is marked with a bare "TODO: Explain this further" — the export/import contract is the most consequential thing in the file and the least documented.
- **Interface vs internal:** Public interface in the strongest sense — it is the base of both trees' include graphs and defines the ABI-visibility macro every exported symbol carries.

---

## engine/source/entry.h

- **Path:** `engine/source/entry.h`
- **Purpose:** Supplies the program's `main()` as a header, wiring the externally-defined `game_create` into the engine's initialize→run→terminate sequence.
- **Key types/functions:**
  - `extern b8 game_create(Game* out_game)` — declared, never defined here; the hook the host must implement.
  - `int main(void)` — **a full function definition living in a header**: brings up memory, calls `game_create`, validates the resulting `Game`, then `application_init` and `application_run`.
- **Notable non-obvious dependencies:**
  - **This is the engine/game inversion point.** The engine declares `game_create` and calls it; the sandbox defines it (`sandbox/source/entry.cpp:5`). Because `main` is in a header, the *engine* owns the startup sequence while the *executable* owns the symbol — nothing links unless the game provides `game_create`.
  - **It is a header defining `main`, so it must have exactly one includer** — and does: `sandbox/source/entry.cpp` alone. It is never compiled into `engine.dll` (the engine build globs only `.cpp`), so this code physically lives in the engine tree but is only ever built as part of `sandbox.exe`.
  - **Calls `bs_memory_initialize()` before anything else and `bs_memory_terminate()` at the very end** — the memory subsystem's entire lifecycle, sitting outside `application.cpp` where every other subsystem is brought up.
  - **`bs_memory_terminate()` is skipped on all four failure paths** (return -1, -2, 1, 2); only a clean run reaches it.
  - **Validates the four `Game` function pointers explicitly** (`init`, `update`, `render`, `on_resize`) before handing the instance to `application_init` — the engine's only guard against a partially-filled `Game`.
  - `Game game_inst;` is an uninitialised stack object passed to `game_create` to fill, so any field the game does not set holds garbage; the pointer check covers only those four.
  - Logs failures through `BS_LOG_FATAL` **before** `application_init` runs, i.e. before `logger_initialize()` — safe only because the logger's init is currently a stub.
  - A stray `;` terminates the `application_init` `if` block.
- **Interface vs internal:** Public interface, and an unusual one — it is consumed by inclusion rather than by linking, and including it commits the translation unit to being the program entry point.

---

## engine/source/game_types.h

- **Path:** `engine/source/game_types.h`
- **Purpose:** Defines the `Game` struct — the vtable-by-hand interface through which the engine drives a host game without knowing its type.
- **Key types/functions:**
  - `struct Game` — an embedded `ApplicationConfig app_config`; four function pointers `init(Game*)`, `update(Game*, f32 dt)`, `render(Game*, f32 dt)`, `on_resize(Game*, u32, u32)`; and `VOID_PTR state` for the game's own opaque data.
- **Notable non-obvious dependencies:**
  - **This is the engine→game callback contract.** `application.cpp` invokes all four pointers every frame or on resize, and `entry.h` refuses to start unless all four are non-null. The engine never names a sandbox symbol; this struct is the entire seam.
  - **`VOID_PTR state` is the ownership escape hatch** — the engine stores and passes it back untouched, never allocating or freeing it. Its comment ("created and managed by the game") is the only statement of that lifetime rule.
  - Each callback receives `Game*` back as its first argument, so the game recovers its own state through `game_inst->state` rather than through any engine-held context.
  - **Includes `core/application.h` purely to get `ApplicationConfig` by value** — which is why `application.h` forward-declares `Game` instead of including this header, avoiding a cycle. The two files are mutually dependent by design, broken with one forward declaration.
  - `update` and `render` take `f32` while `application.cpp` computes time in `real` (double) and narrows at the call; `on_resize` takes `u32` while the application stores `i16` dimensions.
  - Consumed on both sides of the boundary: the engine's `application.cpp` and `entry.h`, and the sandbox's `state/game_state.h`.
- **Interface vs internal:** Public interface — the primary extension point of the engine. It defines no behaviour, only the shape a host must fill in.

---

## engine/source/math/bs_hierpos.cpp

- **Path:** `engine/source/math/bs_hierpos.cpp`
- **Purpose:** Implements hierarchical (cell + local offset) world coordinates, routing every operation through `f64` so positions stay exact far from the origin.
- **Key types/functions:**
  - `static HierPos2 hierpos_from_f64(f64 wx, f64 wy, f32 cell_size)` — the file-local canonicalizer every other function funnels through; `floor`s into a cell then folds the remainder into `[-half, +half)`.
  - `hierpos_from_vec2`, `hierpos_to_vec2`, `hierpos_to_f64`, `hierpos_normalize`, `hierpos_lerp`, `hierpos_add_f64`, `hierpos_diff`.
  - `bs_hierpos_selftest(void)` — a ~190-line invariant suite returning `b8`.
- **Notable non-obvious dependencies:**
  - **One private function is the linchpin**: every public conversion and mutation is expressed as "widen to `f64` → operate → re-canonicalize through `hierpos_from_f64`". That makes the canonical-form invariant a single-point guarantee rather than a per-function obligation.
  - **`hierpos_to_vec2` is the designated lossy path** — it collapses `cell * cell_size + local` into `f32` and is documented as safe only near the origin. `hierpos_diff` exists precisely so callers can get an `f32` vector between two far-apart points without going through absolute world space; this is the floating-origin technique the renderer depends on.
  - **Contains an embedded test suite that no one calls.** `bs_hierpos_selftest` has zero call sites in either tree, so it is compiled into the DLL and exported but never runs. Its cases encode the intended invariants (boundary rounding at ±half, lerp monotonicity across a cell edge, sub-unit precision after `add_f64` at cell 100, denormalize/recover round-trip) — it is effectively executable documentation that is currently dead.
  - **The selftest hardcodes gameplay-scale constants** — a "travel-scale, ~50k units" lerp described in-comment as "the exact scenario used by the travel system". An engine-side test is therefore pinned to a sandbox-side feature's numbers.
  - Boundary handling is asymmetric by construction: `+half` rounds up into the next cell while `-half` stays in the current one, giving the half-open `[-half, +half)` range. Three selftest cases exist only to lock that asymmetry down.
  - `hierpos_to_f64` null-checks its two out-parameters, but no other function null-checks its `HierPos2*` inputs.
  - The `f32 t` loop counters in the selftest accumulate `+= 0.1f`, so `t` never lands exactly on 1.0 — the final step is skipped. Harmless for a monotonicity check, but it means the endpoint is untested.
- **Interface vs internal:** Implementation of an exported API. `hierpos_from_f64` is deliberately private despite being the most capable entry point — callers are steered toward the `Vec2` and delta forms.

---

## engine/source/math/bs_hierpos.h

- **Path:** `engine/source/math/bs_hierpos.h`
- **Purpose:** Declares the hierarchical position type and its conversion/arithmetic API, plus the default cell size the game layer binds to.
- **Key types/functions:**
  - `struct GridCell` — `i64 x`, `i64 y`.
  - `struct HierPos2` — `GridCell cell` + `Vec2 local`.
  - `constexpr f32 BS_HIERPOS_CELL_SIZE = 16384.0f` and `BS_HIERPOS_HALF_CELL`.
  - Exported: `hierpos_from_vec2`, `hierpos_to_vec2`, `hierpos_to_f64`, `hierpos_normalize`, `hierpos_lerp`, `hierpos_add_f64`, `hierpos_diff`, `bs_hierpos_selftest`.
  - Header-inline: `hierpos_add_vec2(hp, d)` and three default-cell-size overloads of `hierpos_from_vec2` / `hierpos_to_vec2` / `hierpos_diff`.
  - All of it inside `namespace bs_math`.
- **Notable non-obvious dependencies:**
  - **The `inline` convenience wrappers bake `BS_HIERPOS_CELL_SIZE` into every caller's object code.** Because they overload the same names as the exported `cell_size`-taking functions, a call site's behaviour depends on its argument count — the one-argument form silently commits to the default cell size, the two-argument form does not. Changing the constant requires recompiling every consumer, not just the DLL.
  - **`i64` cells with a 16384-unit cell size** is the deliberate design point: the addressable world is effectively unbounded while `local` stays small enough for `f32` to hold sub-millimetre precision.
  - The comment calling `Vec2` positions "legacy" marks this type as a migration target — `hierpos_from_vec2`/`to_vec2` are named as bridges out of an older flat coordinate system, so their presence in a file is a signal about that file's conversion status.
  - **`hierpos_to_vec2` carries an explicit safety caveat in its declaration** (valid only near the origin); nothing enforces it, and the header is the only place that warning appears.
  - `bs_hierpos_selftest` is declared and exported but has no caller anywhere — the header advertises a validation entry point nothing invokes.
  - Includes `math/math_utils.h` for `Vec2`, making the hierarchical type structurally dependent on the flat math library it is meant to supersede.
  - `hierpos_add_vec2` is the only mutation helper that cannot take a custom cell size — velocity integration is therefore hard-bound to the game's default.
- **Interface vs internal:** Public interface, and a heavily used one — 24 includers, 23 of them sandbox files. It is one of the widest engine→sandbox contact points after `defines.h` and the renderer.

---

## engine/source/math/math_utils.cpp

- **Path:** `engine/source/math/math_utils.cpp`
- **Purpose:** Implements the scalar clamps, `Vec2`/`Vec3` arithmetic, and `Mat4` construction/multiplication declared in `math_utils.h`.
- **Key types/functions:**
  - `clamp(u32)` / `clampf(f32)`.
  - Vec2: `vec2_add`, `vec2_sub`, `vec2_scale`, `vec2_dot`, `vec2_length`, `vec2_normalized`, `vec2_rotate`.
  - Vec3: `vec3_add`, `vec3_sub`, `vec3_scale`.
  - Mat4: `mat4_identity`, `mat4_mul`, `mat4_ortho`, `mat4_translation`, `mat4_scale`, `mat4_rotation_z`.
- **Notable non-obvious dependencies:**
  - **Entirely stateless** — no globals, no allocation, no callbacks, no I/O. The only external dependency is `<math.h>` for `sqrtf`/`cosf`/`sinf`. This is the one engine file with no coupling beyond its own header.
  - **`mat4_ortho` encodes a graphics-API convention, not just math**: it maps z into `[0,1]` for Vulkan/SDL-GPU rather than the OpenGL `[-1,1]`. Every matrix produced here is built for direct upload as a column-major uniform, so the storage order (`data[col*4+row]`) is an implicit contract with the shaders.
  - **`mat4_ortho` also negates z** (`m.data[10] = -1.0f / fn`), which flips handedness relative to the other builders — a sign convention the renderer and shaders must agree with and which nothing here validates.
  - No division-by-zero guard in `mat4_ortho`: degenerate `left == right`, `bottom == top`, or `near == far` produce infinities silently. Given a window can legitimately reach zero width when minimized, this is reachable from resize handling.
  - `vec2_normalized` is the only function that guards a degenerate input, returning the zero vector for zero length.
  - `mat4_mul` takes both operands **by value** (128 bytes copied per call) and runs a scalar triple loop — no SIMD, no in-place variant.
  - `clamp`/`clampf` do not check `min <= max`; inverted bounds return `max` without complaint.
- **Interface vs internal:** Implementation of a fully exported API. No private helpers at all — every function in the file is public, and the file is effectively a leaf of the dependency graph.

---

## engine/source/math/math_utils.h

- **Path:** `engine/source/math/math_utils.h`
- **Purpose:** Declares the engine's core math vocabulary — angle constants, the `Vec2`/`Vec3`/`Vec4`/`Mat4` types, and the operations on them.
- **Key types/functions:**
  - `constexpr f32 BS_PI`, `BS_DEG2RAD`, `BS_RAD2DEG`.
  - `struct Vec2` (typedef'd `Vec2f32`), `Vec3` (`Vec3f32`), `Vec4` (`Vec4f32`), `Mat4` (`f32 data[16]`).
  - `clamp` / `clampf`, the seven Vec2 ops, three Vec3 ops, and six Mat4 ops.
  - All inside `namespace bs_math`.
- **Notable non-obvious dependencies:**
  - **These types are the engine's shared data currency, not just math helpers** — `Vec2` appears inside `HierPos2`, and `Mat4`'s documented column-major layout (`data[col*4+row]`) is stated here specifically so matrices "can be uploaded directly" to SDL GPU. The header is therefore a silent contract with the shader uniform layout.
  - **`bs__api__` is applied to the struct definitions themselves** (`typedef struct bs__api__ Vec2 {...}`), dllexporting plain PODs. For aggregate types with no member functions this exports nothing useful and only matters for the class-level attribute; the value semantics work regardless of the annotation.
  - **`Vec4` is declared but has no operations** — no `vec4_*` functions exist in the header or the implementation. It is a data type only.
  - The `Vec2f32` / `Vec3f32` / `Vec4f32` typedef names are declared but the codebase consistently uses `Vec2` / `Vec3` / `Vec4`; the aliases are effectively unused.
  - `mat4_rotation_z`'s comment ("the only rotation a 2D game needs") pins the whole math layer to a 2D use case, which is why there is no `mat4_rotation_x/y`, no quaternion type, and no matrix inverse or transpose.
  - `mat4_mul`'s composition order is documented in the header only — `out = a * b` applies `b` first — and is the kind of convention that silently produces wrong transforms if a caller assumes the reverse.
  - No `Vec2` operator overloads: all arithmetic is free functions, so expressions are verbose but the ABI stays C-like across the DLL boundary.
- **Interface vs internal:** Public interface, third-highest fan-in in the project (29 includers, 23 of them from the sandbox). Together with `defines.h` it forms the base layer everything else builds on.

---

## engine/source/platform/platform.h

- **Path:** `engine/source/platform/platform.h`
- **Purpose:** Declares the platform abstraction layer — windowing lifecycle, message pumping, raw and virtual memory, console output, and timing — behind an opaque state handle.
- **Key types/functions:**
  - `struct PlatformState` — a single `VOID_PTR internal_state`, deliberately untyped so the backend chooses its own representation.
  - Windowing: `platform_initialize`, `platform_terminate`, `platform_pump_messages`, `platform_get_window_handle`.
  - Virtual memory: `platform_allocate_virtual_memory_reserve`, `_commit`, `platform_virtual_free`.
  - Heap and blocks: `platform_allocate`, `platform_free`, `platform_zero_memory`, `platform_copy_memory`, `platform_set_memory`.
  - Console: `platform_console_write`, `platform_console_write_error`.
  - Timing: `platform_get_absolute_time`, `platform_sleep`.
- **Notable non-obvious dependencies:**
  - **`platform_get_window_handle` is the only exported symbol in the file**, and its comment explains why: the renderer backend needs to claim the window for the GPU device without knowing the platform's SDL types. It is the one deliberate hole in the abstraction — an `SDL_Window*` smuggled through as `VOID_PTR`.
  - **`platform_sleep` carries an explicit note that it is not exported on purpose** ("clocks the main thread… should only be used for giving time back to the OS"), making the non-export a documented policy rather than an oversight.
  - **`platform_console_write(const char*, u8 colour)` takes a colour the logger supplies as an `ELogLevel`** — the two enums are never formally related, so the log-level→colour mapping lives implicitly in the backend and would break silently if `ELogLevel`'s ordering changed.
  - **`PlatformState` is opaque by convention only** — the struct is fully visible, and `application.cpp` embeds one by value in its own state, so the *size* is fixed at one pointer regardless of what the backend needs behind it.
  - Every allocation the engine performs bottoms out here: `bs_memory.cpp` forwards to these functions, and `arena.cpp` reaches the virtual-memory trio through that facade.
  - `platform_copy_memory` repeats the `const VOID_PTR` mistake from `bs_memory.h` — the `const` applies to the pointer, not the pointee.
  - No error reporting beyond `b8` returns; allocation failures are signalled by null with no diagnostic channel.
- **Interface vs internal:** Internal interface, almost entirely. It is the engine's own portability seam — the game cannot call any of it except `platform_get_window_handle`. `PlatformState` is the only type here that escapes, and only as an opaque pointer.

---

## engine/source/platform/platform_commons.cpp

- **Path:** `engine/source/platform/platform_commons.cpp`
- **Purpose:** Defines the single cross-platform constant `PAGE_SIZE`, queried from the OS once at load time.
- **Key types/functions:**
  - `const unsigned long PAGE_SIZE` — initialized by an immediately-invoked lambda that calls `GetSystemInfo` and returns `sysInfo.dwPageSize`.
- **Notable non-obvious dependencies:**
  - **This is a dynamically-initialized global with a real OS call in its initializer**, not a compile-time constant. It runs during the DLL's static initialization phase, before `main`.
  - **That makes it a static-initialization-order hazard**: any other translation unit whose own static initializer reads `PAGE_SIZE` may observe zero, since the relative order across TUs is unspecified. The current sole consumer (`arena.cpp`) only reads it inside functions, so the hazard is latent rather than live.
  - **Platform-gated with no fallback**: on anything other than Windows the lambda body is an empty `#else` branch with a TODO, so the lambda has no return statement and no deduced type — the file does not compile off Windows. The `#error "Unknown platform!"` in `defines.h` is the only other portability tripwire, and this one is quieter.
  - Depends on `<windows.h>`, one of only two places in the engine that includes it directly.
  - The value is `unsigned long` rather than one of the engine's own `u32`/`u64` typedefs — the only scalar in the codebase declared in raw C types, matching the Win32 field it comes from.
- **Interface vs internal:** Internal implementation detail with a public-looking spelling. `PAGE_SIZE` is an unnamespaced, all-caps global that looks like a macro and will collide with the POSIX `PAGE_SIZE` macro if any header defining it is included first.

---

## engine/source/platform/platform_commons.h

- **Path:** `engine/source/platform/platform_commons.h`
- **Purpose:** Declares the OS page size as an extern constant so the arena allocator can align its reservations.
- **Key types/functions:** `extern const unsigned long PAGE_SIZE;` — the file's entire contents besides `#pragma once`.
- **Notable non-obvious dependencies:**
  - **Declares a mutable-lifetime global across a translation-unit boundary** with no accessor. The value is produced by a runtime OS query in `platform_commons.cpp`, so a consumer reading it during its own static initialization can legitimately see `0` — nothing in the declaration hints that this is anything but a compile-time constant.
  - The header carries no include of `defines.h` and uses raw `unsigned long` rather than the engine's typedefs, so it is the one project header that does not sit on top of the common type vocabulary.
  - Its comment says "Used in arena.h", but `arena.h` does not include it — `arena.cpp` does. The stated consumer is off by one file.
  - The name is unqualified, uppercase, and matches the POSIX `PAGE_SIZE` macro; on any platform where `<limits.h>` or `<unistd.h>` defines that macro first, this declaration is macro-substituted and fails to compile.
- **Interface vs internal:** Internal — a single value shared between two engine files. It is not exported (`bs__api__` absent), so the game cannot link against it even though the declaration is visible.

---

## engine/source/platform/platform_sdl3.cpp

- **Path:** `engine/source/platform/platform_sdl3.cpp`
- **Purpose:** The SDL3 + Win32 implementation of the platform layer — window creation, the event pump that translates SDL events into engine input and bus events, memory primitives, coloured console output, and timing.
- **Key types/functions:**
  - `struct InternalState` — a single `SDL_Window* window`, the concrete type behind `PlatformState::internal_state`.
  - `platform_initialize` / `platform_terminate` / `platform_pump_messages` / `platform_get_window_handle`.
  - `static void mouse_logical_to_pixel(...)` — HiDPI coordinate conversion.
  - `static keys sdl_scancode_to_bs_key(SDL_Scancode)` — a ~120-case scancode translation table.
  - `static void win_console_write(HANDLE, const char*, u8)` — Windows console/file output with colour.
  - The memory, console, and timing implementations of every remaining `platform.h` declaration.
- **Notable non-obvious dependencies:**
  - **Two file-static globals for timing**, `perf_freq` and `perf_start`, captured in `platform_initialize`. `platform_get_absolute_time` returns seconds *since platform init*, not an absolute epoch — and both are zero before init, so an early call divides by zero.
  - **The pump drives three subsystems it does not own.** Every polled event is handed to `bs_imgui_process_event` and `bs_rml_process_event` **before** the engine's own switch, so UI capture flags are current when the game later gates on them. Both facades take the event as `void*` specifically so this TU needs no ImGui or RmlUi headers — an explicit, commented decoupling.
  - **Fires bus events directly** (`EVENT_CODE_APPLICATION_QUIT` on `SDL_EVENT_QUIT`, `EVENT_CODE_WINDOW_RESIZED` on pixel-size change) and pushes into the input subsystem through the unexported `input_process_*` functions. This file is the sole producer of every input event in the engine.
  - **`SDL_EVENT_QUIT` returns `FALSE` immediately from inside the poll loop**, abandoning any events still queued that frame — and it does so *after* already firing the quit event, so shutdown is signalled twice (once by the event, once by the return value the application reads as "stop running").
  - **Resize uses `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`, not the logical-size event** — deliberate, since the renderer's drawable is sized in pixels.
  - **HiDPI correction is centralized at this one boundary**: `mouse_logical_to_pixel` scales SDL's logical mouse coordinates into pixel space so hit-testing agrees with the framebuffer. The long comment explains that a 125%-scaled display would otherwise put input ~0.8x off. It queries both window sizes from SDL **on every mouse-motion event** rather than caching on resize.
  - **The scancode table maps physical positions to Win32 virtual-key values**, so the engine's `keys` enum is fed by *scancodes* — key identity follows physical layout, and a non-QWERTY layout reports the QWERTY-position key. Unmapped keys return `KEYS_MAX_KEYS` and are dropped by the caller.
  - **`win_console_write` colours by log level** via a `static const u8 levels[6]` table indexed by the `colour` argument (clamped to 5) — this is where `ELogLevel` is finally reinterpreted as a console attribute. It also calls `OutputDebugStringA` on every line, and falls back to `WriteFile` when the handle is not a console so redirected output is still captured (commented as deliberate). The console text attribute is set but never restored.
  - **Wheel input is quantised to ±1** regardless of SDL's actual delta, discarding high-resolution or horizontal scroll.
  - **Failure paths in `platform_initialize` leak**: the `InternalState` `malloc` is unchecked, and both the `SDL_Init` and `SDL_CreateWindow` early returns leave it allocated (and SDL initialized) with no cleanup.
  - `platform_allocate`/`platform_free` accept an `aligned` flag and ignore it — the aligned path the memory tag system implies does not exist. `platform_virtual_free` likewise ignores `size` and always `MEM_RELEASE`s.
  - **Only the Windows branch is implemented.** The virtual-memory functions return `NULL` on other platforms (TODOs for `mmap`), and the non-Windows console path calls `fputs` although `<stdio.h>` is never included — so, like `platform_commons.cpp`, this file cannot build off Windows despite the `#if` scaffolding.
- **Interface vs internal:** Implementation — the single concrete backend for `platform.h`. Architecturally it is the engine's inbound edge: everything the OS says enters the process here and leaves as either an engine input call or a bus event.

---

## engine/source/renderer/backend/renderer_backend_sdlgpu.cpp

- **Path:** `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp`
- **Purpose:** The SDL3 GPU rendering backend — device and swapchain management, a sorted dynamic sprite batcher, a procedural-effect pass chain (starfield, nebula, heat map, sunburst, star/planet surfaces), an HDR bloom and anamorphic-streak post stack, and the concrete implementations of both the ImGui and RmlUi facades. At 4887 lines it is by far the largest file in the project.
- **Key types/functions:**
  - `struct sdlgpu_state` (with the single global `g_sdl`) — device, window, per-frame command buffer / swapchain texture / render pass, ~30 graphics pipelines, two samplers, batch and mapped-sprite GPU buffers, a 1024-entry texture pool, camera, lights, glow/bloom/streak tuning, and six fixed-size per-frame effect queues.
  - Vertex formats `sprite_vertex` (pos/uv/tint/custom) and `mapped_vertex` (pos/uv/world/angle); uniform blocks `gpu_lights` and `mapped_light`.
  - Lifecycle: `sdlgpu_backend_initialize`, `_shutdown`, `_on_resize`, `_begin_frame`, `_end_frame` (the last spanning ~1100 lines).
  - Resources: `load_shader`, `create_pipelines_for_format`, `create_mapped_pipeline_for_format`, `create_batch_resources`, `create_bloom_targets`, `create_postprocess_pipelines`, `create_white_texture`, `create_circle_texture`, `pool_alloc_texture`, `pool_resolve_texture`.
  - Submission: `_draw_sprite`, `_draw_mapped_sprite`, `_draw_starfield`, `_draw_sunburst`, `_draw_starsurface`, `_draw_planetsurface`, `_draw_heat_map`, `_draw_nebula`, plus the `set_*` state setters.
  - ImGui facade: `bs_imgui_initialize` / `_shutdown` / `_process_event` / `_wants_mouse` / `_wants_keyboard` / `_get_hud_font`.
  - RmlUi backend: `BsRmlSystemInterface`, `BsRmlRenderInterface` (14 overrides), `bs_rml_state g_rml`, the buffer/transfer pools, and the whole `bs_rml_*` facade including the HUD data model.
- **Notable non-obvious dependencies:**
  - **This is the only TU in the engine permitted to include `<SDL3/SDL_gpu.h>`, the ImGui backends, or the RmlUi headers** — stated at the top and enforced by convention. That single rule is why `bs_imgui.h` and `bs_rml.h` exist as `void*`-based facades and why their implementations live at the bottom of this file rather than in their own translation units: they need direct access to `g_sdl`.
  - **All state is one file-scope global, `g_sdl`**, plus a second anonymous-namespace global `g_rml` and ~10 loose statics for the RmlUi upload batch, buffer pools, and per-frame churn counters. `backend->internal_state` is set to `&g_sdl`, so the "opaque instance pointer" in the frontend's interface is decorative — the backend is a singleton and every function ignores its `backend` argument.
  - **The frame is split across `begin_frame`/`end_frame` for a specific SDL3 GPU constraint**: copy passes cannot run inside a render pass. `begin_frame` only acquires a command buffer; the vertex upload, swapchain acquire, and all render passes happen in `end_frame`. The header comment calls this "the #1 SDL GPU bug source".
  - **ImGui's `NewFrame`/`Render` pairing is a documented invariant** spanning two functions: `begin_frame` issues `NewFrame`, and `end_frame` must call `Render()` even on the failed-acquire and minimized early-outs, which is why `renderer.cpp`'s `frame_active` gate exists.
  - **RmlUi can request GPU uploads while the main render pass is open**, so every RmlUi upload uses its own command buffer and copy pass, batched into one shared submit (`rml_upload_flush`) that must be submitted **before** the frame's main command buffer — an ordering dependency enforced only by call placement, relying on SDL executing command buffers in submission order.
  - **A pow2-bucketed buffer/transfer pool exists purely as a driver-overhead workaround** — RmlUi recompiles changed element geometry every frame (dozens of calls for live HUD text), and the comments quantify the costs being avoided (~0.25 ms per submit; ~100 buffer create/destroy pairs per frame).
  - **Hardcoded absolute path**: `bs_imgui_initialize` loads `C:\Windows\Fonts\consola.ttf` for the HUD font, warning but continuing if absent.
  - **Reads shaders from disk at init** — `assets/shaders/{dxil,spirv}/<name>.<stage>.<ext>`, resolved relative to the working directory, choosing DXIL or SPIR-V from what the device reports. Missing blobs are a fatal init failure, which couples the backend to the shader-compile and asset-staging steps in `build-all.bat`.
  - **A driver-specific correction is baked in**: `get_corrected_swapchain_format` forces `B8G8R8A8_UNORM` when D3D12 misreports `R8G8B8A8_UNORM`, otherwise every swapchain-facing draw fails validation.
  - **Fixed capacities everywhere, with divergent overflow behaviour** — 16384 sprites, 1024 textures, 256 mapped sprites, 16 lights, and queues of 8/4/4/32 for the effect passes. Sprite and mapped-sprite overflow logs a warning; **every effect queue drops silently**, deliberately, because logging would spam per frame. The 16384 sprite cap is a hard ceiling from the u16 index buffer, documented at length as a trap: raising the constant produces aliased indices rather than more capacity.
  - **Texture handles are generation-tagged** (`generation << 18 | index+1`) so use-after-destroy is detectable; `pool_resolve_texture` validates and callers silently fall back to the 1x1 white texture. `destroy_texture` calls `SDL_WaitForGPUIdle` — a full GPU stall per destroy.
  - **Draw batching is driven by a packed `u32` sort key** (12 bits layer, 2 bits blend, 18 bits texture), and runs additionally break on a differing `glow_override` **pointer** — so two identical-by-value glow structs at different addresses split the batch.
  - **`BS_LAYER_BLOOM_THRESHOLD` splits the sorted batch** into world sprites (bloomed) and UI/debug overlays (drawn after composite), making sprite layer numbers semantically load-bearing across the engine/game boundary.
  - **Per-frame submission state is reset in `begin_frame`, so the game must resubmit effects every frame** — heat map, nebula, streak source, and all queues are cleared; lights, camera, glow, and bloom settings are sticky.
  - **The offscreen path is forced on by three independent conditions** (`bloom_enabled`, an active streak with aux sprites, or `g_rml_frost_active()`), so enabling the in-game UI silently changes the whole render path — the frosted-glass decorator needs a blurred copy of `scene_rt`.
  - **`platform_get_absolute_time` is exposed to RmlUi** as its `GetElapsedTime`, and RmlUi's log messages are routed into `BS_LOG_*` — so a third-party library's timing and diagnostics run through engine primitives.
  - **Self-instrumenting**: end_frame times seven sections and emits a throttled (once per second) `BS_LOG_WARN` breakdown when a frame is slow, including RmlUi pool hit/miss counters. It also warns at init if the display refresh is below 50 Hz, since VSYNC would silently cap the frame rate.
  - `sdlgpu_backend_initialize` returns `FALSE` on a dozen paths **without releasing anything already created** — a failed init leaks the device, pipelines, and shaders.
  - The mapped-sprite path takes its light direction from `mapped_batch[0]` for the entire batch, on the stated assumption that all ships share one star direction.
- **Interface vs internal:** Implementation, and the most inward-facing file in the engine — but it carries three distinct public surfaces out through headers it does not own: the `renderer_backend` function-pointer table, the entire `bs_imgui_*` facade, and the entire `bs_rml_*` facade (including the game-facing HUD data model). Nearly everything else is `static` or `g_sdl`-private.

---

## engine/source/renderer/backend/renderer_backend_sdlgpu.h

- **Path:** `engine/source/renderer/backend/renderer_backend_sdlgpu.h`
- **Purpose:** Declares the SDL GPU backend's ~30 entry points so the backend factory can wire them into a function-pointer table without including any SDL header.
- **Key types/functions:** Forward declarations of `struct renderer_backend` and `struct PlatformState`; then `sdlgpu_backend_initialize`, `_shutdown`, `_on_resize`, `_begin_frame`, `_end_frame`, `_set_clear_color`, the texture trio, `_set_camera`, `_draw_sprite` / `_draw_mapped_sprite`, the six procedural-effect draw calls, `_set_lights`, `_set_glow_params`, the bloom and streak setters, `_get_frame_stats`, `_set_present_mode`, and `_get_present_timing`.
- **Notable non-obvious dependencies:**
  - **Its whole reason for existing is stated in the file**: to let `renderer_backend.cpp` reference these symbols *without* pulling in `<SDL3/SDL_gpu.h>`. It is a firewall header — the mechanism that keeps "only the backend TU touches SDL" true.
  - **Uses `struct renderer_backend` and `struct PlatformState` as elaborated-type-specifier forward declarations** in every signature, so it needs neither `renderer_backend.h` nor `platform.h`. Its only real include is `renderer_types.h`, for the value types (`bs_color`, `bs_texture`, `bs_sprite`, `Camera2D`, the param structs).
  - **No symbol here is `bs__api__`** — the backend is invisible outside the DLL. Everything the game sees goes through `renderer.h`.
  - The "Phase 3"/"Phase 4" comments date the sections and reveal build order; the interface has grown by accretion, with each effect adding a `draw_*` and its `set_*` companions rather than a generalised pass API.
  - Comments here carry contracts the signatures do not: `set_lights` documents that over-cap lights are dropped and that `unlit_layer` is a fullbright threshold; `set_present_mode` documents the VSYNC fallback; `get_present_timing` defines its two outputs as acquire-block versus submit.
- **Interface vs internal:** Internal interface — a private contract between exactly two files, the backend and the factory that binds it.

---

## engine/source/renderer/backend/stb_image_impl.cpp

- **Path:** `engine/source/renderer/backend/stb_image_impl.cpp`
- **Purpose:** The one translation unit that compiles the vendored stb_image decoder body, fenced behind warning suppressions.
- **Key types/functions:** No declarations of its own. Defines `STB_IMAGE_IMPLEMENTATION`, `STBI_ONLY_PNG`, and `STBI_NO_STDIO`, then includes `stb_image.h` between a `#pragma clang diagnostic push/pop` pair suppressing seven warning categories.
- **Notable non-obvious dependencies:**
  - **Exists because of a build-system constraint, not a design one**: the engine compiles with `-Wall -Werror` and stb_image is not warning-clean. The vendored ImGui and RmlUi trees solve the same problem by being compiled separately with relaxed flags in `build.bat`; stb_image is header-only, so it gets this fenced TU instead.
  - **It is the definition half of a two-part arrangement** — the backend includes the same header *without* `STB_IMAGE_IMPLEMENTATION` for declarations only, so the decoder body is emitted exactly once. Nothing enforces that invariant; a second TU defining the macro would produce duplicate symbols.
  - **`STBI_NO_STDIO` encodes a deliberate I/O policy**: bytes arrive via `SDL_LoadFile`, so no `fopen` path is compiled in. `STBI_ONLY_PNG` narrows the decoder to the one format the assets use, and the comment names adding formats here as the extension point.
  - The suppression list is a precise inventory of what stb_image trips (`-Wunused-function`, `-Wsign-compare`, `-Wimplicit-fallthrough`, `-Wcast-qual`, and three more) and is clang-specific — the pragmas would need changing for another compiler.
  - Its actual consumer is not the backend's sprite path but **RmlUi's `LoadTexture`**, which decodes PNG/JPG for UI skins and icons.
- **Interface vs internal:** Purely internal — it declares nothing, exports nothing, and has no includers. It exists only to place third-party object code into `engine.dll`.

---

## engine/source/renderer/bs_imgui.h

- **Path:** `engine/source/renderer/bs_imgui.h`
- **Purpose:** The SDL-free, ImGui-free facade through which the rest of the engine and the game reach Dear ImGui — six exported functions and not one third-party type.
- **Key types/functions:**
  - `bs_imgui_initialize()` / `bs_imgui_shutdown()` — context and backend lifecycle.
  - `bs_imgui_process_event(const void* sdl_event)` — one event, passed as an opaque pointer.
  - `bs_imgui_wants_mouse()` / `bs_imgui_wants_keyboard()` — input-capture queries.
  - `bs_imgui_get_hud_font()` — returns an opaque `void*` that is really an `ImFont*`.
  - Its only include is `defines.h`.
- **Notable non-obvious dependencies:**
  - **Declares functions no file in the renderer directory implements.** The bodies live at the bottom of `renderer_backend_sdlgpu.cpp`, because that is the only TU allowed to include the ImGui headers and because the implementations read `g_sdl` directly. The header says so explicitly.
  - **`void*` is the type-erasure mechanism in both directions** — `process_event` takes an `SDL_Event*` the backend casts back, and `get_hud_font` returns an `ImFont*` the caller must cast. Neither is type-checked; the seam trades safety for header isolation.
  - **The per-frame calls are deliberately absent from the API.** `NewFrame`/`Render`/record are sequenced inside the backend's `begin_frame`/`end_frame` specifically "so they can never fall out of balance with the GPU frame" — a design decision documented here and enforced there.
  - **Two ordering constraints are stated only in comments**: initialize must run *after* the GPU device and window exist, and shutdown *before* the device is destroyed (the SDL_GPU ImGui backend owns device-tied resources). `renderer.cpp` is named as the owner of that bracketing.
  - **The `wants_*` pair is the UI/world input arbitration contract** — the game is expected to gate world input on their negation, combined with `bs_rml_wants_*`. Nothing enforces it; a caller that ignores them gets clicks applied twice.
  - Everything is `bs__api__`, so ImGui's *control surface* crosses the DLL boundary even though ImGui's symbols never do (they stay inside `engine.dll`).
- **Interface vs internal:** Public interface, and an unusually deliberate one — its entire design is about what it refuses to expose. Four of its six functions are consumed from the sandbox; `process_event` is called by the engine's own platform pump.

---

## engine/source/renderer/bs_rml.h

- **Path:** `engine/source/renderer/bs_rml.h`
- **Purpose:** The SDL-free, RmlUi-free facade for the retained-mode in-game UI — document lifecycle, event and input plumbing, and a large POD snapshot type that carries the entire HUD's contents from the game to the engine each frame. At 326 lines it is the biggest header in the engine.
- **Key types/functions:**
  - `bs_rml_document` — opaque `{ void* ptr; }` wrapping an `Rml::ElementDocument*`.
  - Lifecycle and plumbing: `bs_rml_initialize`, `_shutdown`, `_load_fonts`, `_load_document`, `_show`, `_unload_document`, `_update`, `_process_event`, `_wants_mouse`, `_wants_keyboard`, `_on_resize`, `_debugger_toggle`, `_set_sharpen`.
  - Row types: `bs_rml_log_line`, `bs_rml_disc_line`, `bs_rml_weapon_line`, `bs_rml_bay_line`, `bs_rml_gm_cell`, `bs_rml_gm_row`.
  - `struct bs_rml_hud_state` — a ~145-field snapshot covering nav readout, ship readout, time controls, encounter modal, action log, discovery browser, fleet panel, jump banner, debug readout, map tooltip, flagship inspector (module bay + fire-group matrix + point-defense doctrine), station inspector (dock/market/contracts), and planet inspector.
  - HUD API: `bs_rml_hud_init`, `_hud_shutdown`, `_hud_update`, `_hud_poll_action`.
  - Capacity macros: `BS_RML_LOG_MAX` 12, `BS_RML_DISC_MAX` 64, `BS_RML_BAY_MAX` 12, `BS_RML_GROUP_MAX` 5, `BS_RML_GM_COLS` 8, `BS_RML_ACTION_CAP` 32.
- **Notable non-obvious dependencies:**
  - **Like `bs_imgui.h`, nothing here is implemented in its own TU** — every function is defined at the bottom of `renderer_backend_sdlgpu.cpp`. RmlUi is linked with `RMLUI_STATIC_LIB` so its symbols stay inside `engine.dll`, making these entry points the game's *only* possible access path.
  - **The HUD is one document driven by one data model named `"hud"`.** `bs_rml_hud_update` copies the snapshot into engine-side bound state and dirties every variable — so the cost is proportional to the struct, not to what changed.
  - **All display formatting is game-side, by explicit policy** — the struct carries pre-formatted strings ("Year N, Day D", "Habitability 62%", CSS widths like `"62%"` and pixel positions like `"NNNpx"`). The stated reason is to keep the engine game-agnostic: it only shows strings. The consequence is that this "engine" header enumerates an enormous amount of game domain vocabulary — fire groups, point-defense stances, market specializations, planet habitability.
  - **Interactions are inverted through a string queue, not callbacks.** Clicks enqueue short action strings (`"group:N"`, `"gm:W:G"`, `"inv:K"`, `"pd:stance:N"`, `"baydrop"`) that the game drains with `bs_rml_hud_poll_action` in a loop until it returns 0. The engine never mutates game state, and the action grammar is documented only in these comments.
  - **Every field is a fixed-size `char` array**, so the snapshot is a large POD copied whole each frame; oversized game strings are silently truncated at the array bound.
  - **`bs_rml_update` has a documented frame-ordering requirement** (once per frame, before the backend renders) and also syncs the context size to the swapchain — so skipping it desynchronises layout from the window.
  - **Several macros must agree with game-side constants** — `BS_RML_GROUP_MAX` is annotated "SHIP_WEAPON_GROUPS game-side", a duplicated constant with no compile-time check.
  - Comments reference external design docs (`docs/POINT_DEFENSE_AND_MISSILES.md`) and a shader (`rml.frag.hlsl`, for the sharpening contract), so the header is a hub of cross-artifact dependencies.
  - The division of labour with ImGui is stated up front: **RmlUi is the shipping in-game UI, ImGui is editor/dev tooling** — the two are rendered in a fixed order, RmlUi beneath the ImGui overlay.
- **Interface vs internal:** Public interface — the widest game-facing surface the engine exposes after `renderer.h`. Notably it is a *data* interface as much as a functional one: the shape of `bs_rml_hud_state` is the real contract, and it encodes the game's UI structure inside the engine tree.

---

## engine/source/renderer/bs_ui.cpp

- **Path:** `engine/source/renderer/bs_ui.cpp`
- **Purpose:** Translates the engine-native `bs_ui_*` widget calls into ImGui calls, driving only ImGui's backend-agnostic core.
- **Key types/functions:**
  - Containers: `bs_ui_begin_panel` / `_end_panel` (anchored, auto-sized, chrome-less), `bs_ui_begin_window` / `_end_window` (movable, resizable, native title bar), `bs_ui_begin_hud_panel` / `_end_hud_panel` (styled variant).
  - Widgets: `bs_ui_text`, `_text_colored`, `_progress`, `_button`, `_button_sized`, `_checkbox`, `_slider_float`, `_color_edit3`, `_combo`, `_selectable`, `_color_button`, `_label_at`.
  - Layout and state: `bs_ui_same_line`, `_set_cursor_pos_x`, `_separator`, `_is_window_hovered`, `_push_alpha` / `_pop_alpha`.
- **Notable non-obvious dependencies:**
  - **It shares ImGui's global context with the GPU backend without any handoff.** The context is created by `bs_imgui_initialize` inside `renderer_backend_sdlgpu.cpp`; because both TUs link into `engine.dll`, widgets built here land on the same draw list the backend records in `end_frame`. The coupling is entirely through ImGui's own global state — there is no shared engine variable, no accessor, and nothing that would fail at link time if the ordering were wrong.
  - **It includes `<imgui.h>` but never SDL or the backend header** — a deliberate second tier of the isolation rule: this TU touches ImGui's core but not its platform/renderer backends, so the "only the backend touches SDL/GPU" seam still holds.
  - **It calls into the other facade**: `bs_ui_begin_hud_panel` / `_end_hud_panel` fetch the monospace font through `bs_imgui_get_hud_font()` and push/pop it, so the HUD styling depends on a font loaded from a hardcoded Windows path in a different TU. Both guard on null, so a missing font degrades to the default face rather than unbalancing the stack.
  - **Several functions carry an implicit "must be paired" contract** the compiler cannot check — begin/end panel, begin/end window, push/pop alpha, and the font push inside the HUD pair. `bs_ui_end_hud_panel` unwinds a style colour, a style var, and a font pushed by its opener.
  - **`bs_ui_button` deliberately overrides ImGui's usual full-width behaviour.** A width of -1 does not enlarge an `AlwaysAutoResize` window, so long labels clipped; the code instead computes ImGui's own natural button width (`CalcTextSize` with `##` hidden, plus double frame padding) and takes the max. The comment notes this converges in one frame without oscillation — a layout feedback loop being managed by hand.
  - **`bs_ui_begin_panel` only applies anchoring for `BS_UI_TYPE_GAME`**; `BS_UI_TYPE_EDITOR` falls through the switch with no flags and no positioning, so the `anchor` and `margin` arguments are silently ignored for editor panels.
  - **Panel titles double as ImGui IDs**, and the code notes gameplay panels are singletons so collisions are assumed away. Two panels sharing a title would merge.
  - `bs_ui_label_at` creates a full borderless ImGui window per label and calls `SetWindowFontScale` — a heavyweight path for what is visually one line of text, and each needs a unique `id`.
  - Null-string defensiveness is uneven but present in most widgets (`text ? text : ""`), and pointer-taking widgets return `FALSE` early on null.
  - `bs_ui_checkbox` takes a native `bool*` while every other boolean in the API is `b8` — the one leak of a C++ type through an otherwise engine-native surface.
- **Interface vs internal:** Implementation of a fully exported API. It holds no state of its own; all state lives in ImGui's shared context.

---

## engine/source/renderer/bs_ui.h

- **Path:** `engine/source/renderer/bs_ui.h`
- **Purpose:** Declares an immediate-mode widget vocabulary in engine-native types, so gameplay code can build UI without seeing ImGui.
- **Key types/functions:**
  - `enum BsUiAnchor` — `TOP_LEFT`, `TOP_RIGHT`, `BOTTOM_LEFT`, `BOTTOM_RIGHT`, `TOP_CENTER`, `CENTER`.
  - `enum BsUiType` — `BS_UI_TYPE_EDITOR`, `BS_UI_TYPE_GAME`.
  - ~25 `bs__api__` functions: the three container pairs, the widget set, the layout helpers, and the alpha push/pop.
  - Includes `defines.h` and `renderer_types.h` (the latter only for `bs_color`, used by `bs_ui_label_at`).
- **Notable non-obvious dependencies:**
  - **Mirrors `bs_imgui.h`'s isolation strategy one level up**: that header hides ImGui's *lifecycle*, this one hides its *widget API*. Neither exposes an ImGui type, and together they let the game use ImGui without linking to it.
  - **The header teaches a usage protocol its signatures cannot enforce** — a worked example in the comment block, and an emphatic note that `bs_ui_end_panel()` must be called even when `begin` returns FALSE (ImGui's Begin/End rule). Getting this wrong corrupts ImGui's window stack at runtime with no compile-time signal.
  - **A frame-phase constraint is stated in prose**: calls must happen between `renderer_begin_frame` and `renderer_end_frame`, i.e. inside `game_render`. That is where the backend has an open ImGui frame; calling outside it is undefined.
  - **Input gating is delegated to the other facade** — the comment directs callers to `bs_imgui_wants_mouse()`/`_keyboard()` and notes these already cover `bs_ui` panels because they are ImGui windows. It also records that this "replaces the old `ui_wants_mouse()`", marking a superseded API.
  - **Labels double as ImGui identity** throughout, so the header repeatedly instructs callers to keep titles unique and to suffix `"##<n>"` on repeated rows — identity leaking through a parameter that looks purely cosmetic.
  - `bs_ui_combo` takes a `"A\0B\0C\0"` NUL-separated item list, an ImGui convention passed through unchanged despite the header claiming to hide ImGui.
  - `bs_ui_checkbox` is declared with a native `bool*`, breaking the `b8` convention every other function follows.
  - `BS_UI_TYPE_EDITOR` is declared here but the implementation does nothing for it — the enum promises a distinction the code does not yet make.
- **Interface vs internal:** Public interface, consumed almost entirely from the sandbox — it is the engine's editor/debug UI vocabulary, distinct from the RmlUi path used for the shipping HUD.

---

## engine/source/renderer/camera2d.cpp

- **Path:** `engine/source/renderer/camera2d.cpp`
- **Purpose:** Implements the 2D camera transforms — a default camera, the view-projection matrix, and the two-way screen/world projection pair.
- **Key types/functions:**
  - `camera2d_default()` — origin, zoom 1.0, no rotation.
  - `camera2d_view_proj(cam, fb_width, fb_height)` — composes `proj * scale(zoom) * rotateZ(-rotation) * translate(-position)`.
  - `camera2d_screen_to_world(...)` and `camera2d_world_to_screen(...)`.
- **Notable non-obvious dependencies:**
  - **Pure math with no state and no backend** — the file comment insists on this, and it is what lets the game project and unproject without touching the renderer.
  - **The two projection functions do NOT go through the matrix.** They reimplement the same transform in closed form over `Vec2`, deliberately mirrored so they are exact inverses of each other. That makes the matrix path and the picking path two independent expressions of one convention: a change to `camera2d_view_proj` that is not mirrored here would desynchronise rendering from mouse picking with nothing to catch it.
  - **Three coordinate conventions are reconciled in this one file** — screen pixels (top-left origin, y-down, as the platform layer reports them), centered y-up view space, and Vulkan/SDL clip space with z in [0,1]. The `hh - screen_px.y` flip appears in both directions and is the only place the y-axis inversion happens.
  - **Zoom is guarded against exactly zero** in all three functions (`z = (cam->zoom != 0.0f) ? cam->zoom : 1.0f`), silently substituting 1.0 rather than producing infinities — a defence not documented in the header.
  - **Framebuffer size is a parameter, never stored**, so every call site must supply the current pixel dimensions; the backend passes `swap_width`/`swap_height`, and the game is expected to pass the same values it tracks. A mismatch produces picking that is offset from what is drawn.
  - Relies on `mat4_ortho`'s Vulkan z-convention and `mat4_mul`'s documented "`a * b` applies `b` first" ordering — the composition `mat4_mul(s, mat4_mul(r, t))` is only correct under that reading.
  - The projection is rebuilt from scratch on every call, including inside the backend's per-draw-run loop, where the same matrix is recomputed for each run.
- **Interface vs internal:** Implementation of a fully exported API, with no private helpers. Small but structurally central — it is the shared definition of "where things are on screen" for both the renderer and the game.

---

## engine/source/renderer/camera2d.h

- **Path:** `engine/source/renderer/camera2d.h`
- **Purpose:** Declares the four camera functions and documents the coordinate conventions they enforce.
- **Key types/functions:** `camera2d_default`, `camera2d_view_proj`, `camera2d_screen_to_world`, `camera2d_world_to_screen` — all `bs__api__`. The `Camera2D` struct itself is not defined here; it comes from `renderer_types.h`.
- **Notable non-obvious dependencies:**
  - **Free functions over a POD the game owns**, stated as a deliberate choice: the game mutates `Camera2D` directly (pan/zoom/rotate) and hands it to the renderer, rather than the renderer owning a camera the game asks to move. The backend keeps its own copy set via `renderer_set_camera`, so the game's camera and the backend's are two objects synchronised by an explicit call each frame.
  - **The header is where the conventions are actually specified** — y-up, origin at screen center, clip z in [0,1], and the anchor fact that "at zoom 1 and no rotation, world point == pixel offset from the window center". That equivalence is what makes world units and pixels interchangeable at default zoom, an assumption gameplay and UI code lean on.
  - **`screen_to_world` and `world_to_screen` are documented as exact inverses**, and each names its intended use — mouse picking versus anchoring HUD text to world objects. That pairing is a contract the implementation upholds by mirroring, not by construction.
  - It declares functions taking `Camera2D` but relies on `renderer_types.h` for the type, so the camera's data layout and its operations live in different headers.
  - The comment "backend-agnostic — engine math only, no SDL/GPU" marks this as safe for gameplay code to include directly, which the fan-in confirms: 20 includers, 17 of them sandbox files.
- **Interface vs internal:** Public interface, and one of the more heavily used engine headers on the game side — it is the shared coordinate-system authority between simulation, rendering, and input.

---

## engine/source/renderer/renderer.cpp

- **Path:** `engine/source/renderer/renderer.cpp`
- **Purpose:** The renderer frontend — owns the backend vtable and the renderer's singleton state, forwards the public API to the backend, decodes image files on the CPU, and synthesises the debug/vector primitives out of sprite quads.
- **Key types/functions:**
  - `struct RendererState` (global `static RendererState state`) — the `renderer_backend` vtable by value, clear colour, a copy of the last camera, `frame_active`, `initialized`, `draw_alpha_mul`, the two frame-timing values, and `present_immediate`.
  - Lifecycle: `renderer_initialize`, `renderer_shutdown`, `renderer_on_resize`, `renderer_begin_frame`, `renderer_end_frame`.
  - Textures: `renderer_load_texture` (file → decode → upload), `renderer_create_texture`, `renderer_update_texture`, `renderer_destroy_texture`.
  - Forwarders: camera, sprite, mapped sprite, the six procedural effects, lights, glow, bloom, and the six streak/aux setters.
  - Debug layer: `renderer_draw_line`, `_draw_quad`, `_draw_rect_outline`, `_draw_circle`, `_draw_grid`.
  - Instrumentation: `renderer_get_frame_stats`, `_report_frame_timing`, `_get_frame_timing`, `_set_present_mode`, `_is_present_immediate`, `_get_present_breakdown`.
- **Notable non-obvious dependencies:**
  - **One file-static singleton, `state`**, holding the backend vtable *by value* — so the "backend instance" the whole engine talks to is a struct member of a global, and `renderer_initialize` is single-shot (a second call logs an error).
  - **It owns the ImGui and RmlUi lifecycles**, not the backend that implements them. `renderer_initialize` calls `bs_imgui_initialize()` and `bs_rml_initialize()` after the backend is up; `renderer_shutdown` tears both down *before* `backend.shutdown()` because they own device-bound GPU resources. Both failures are explicitly non-fatal — the engine logs a warning and runs without the overlay.
  - **`renderer_begin_frame` calls `bs_rml_update()`**, so advancing the in-game UI's layout is a side effect of starting a frame rather than something the game schedules.
  - **`frame_active` is the guard that keeps ImGui balanced.** It is set only on a successful `begin_frame` and is what makes `end_frame` a no-op otherwise — the invariant the backend's `NewFrame`/`Render` pairing depends on.
  - **CPU decode lives here, GPU upload lives in the backend** — a deliberate split (stated in the comments) that lets the frontend include `stb_image.h` and use plain `fopen`/`fread` rather than pulling SDL into the frontend. The file bytes are allocated through `bs_memory_allocator` under `MEMORY_TAG_TEXTURE`, but the decoded pixel buffer comes from stb's own allocator and is freed with `stbi_image_free`, so half the texture-load path is invisible to the engine's memory accounting.
  - **Nearly every forwarder null-checks the backend function pointer before calling it** (`if (!state.backend.draw_nebula) return;`), so the vtable is treated as sparsely populated even though the only backend fills it completely. Failures are silent.
  - **The debug/vector layer is not a separate GPU path** — every line, rect, circle, and grid lowers to white-texture sprite quads pushed through the normal batch. The header explains the reason: a real line pipeline would be "hostage to unreliable cross-backend line-width clamping". The consequence is that debug primitives consume the same 16384-sprite budget as game content, and a circle costs one sprite per segment.
  - **Line thickness is divided by camera zoom** so debug primitives keep a constant on-screen width — which is why the frontend keeps its own copy of the camera. That copy is only updated by `renderer_set_camera`, so a game that mutates its camera without re-submitting gets stale thickness scaling.
  - **`draw_alpha_mul` silently rewrites blend mode**: when a fade is active, an otherwise `BLEND_NONE` sprite is switched to `BLEND_ALPHA`, because an opaque draw cannot fade. This mutates a caller-owned value semantically without saying so at the call site.
  - **`renderer_draw_grid` refuses to draw** when the requested spacing would produce more than 4096 lines per axis — a silent early-return that protects the batch but gives no feedback.
  - **The debug primitives bypass the frontend's own `renderer_draw_sprite`** and call `state.backend.draw_sprite` directly, having already applied the alpha multiplier by hand — so the two paths must be kept in agreement manually.
  - **`renderer_set_present_mode` only mirrors the flag when the backend confirms it applied**, with a comment explaining that a wrongly-cached IMMEDIATE would also disable the application loop's software frame cap — coupling this state to `application.cpp`'s pacing logic.
  - `renderer_report_frame_timing` and `_get_frame_timing` make the frontend a passive mailbox between the application loop (which measures) and the game's debug UI (which displays).
  - `renderer_load_texture` resolves paths relative to the working directory, tying it to the asset staging that `build-all.bat` performs into `bin/`.
- **Interface vs internal:** Implementation of the engine's largest public API. `RendererState` and `state` are TU-private; everything else is exported. Structurally it is the isolation layer — it is what allows 27 sandbox files to include `renderer.h` while no sandbox file can reach an SDL type.

---

## engine/source/renderer/renderer.h

- **Path:** `engine/source/renderer/renderer.h`
- **Purpose:** Declares the renderer's public, game-facing API — lifecycle, textures, camera, sprite and procedural-effect submission, lighting and post-process tuning, the debug/vector helpers, and frame instrumentation.
- **Key types/functions:** ~40 `bs__api__` functions across six commented sections; a forward declaration of `struct PlatformState`; and one include of `renderer_types.h` for every value type it traffics in.
- **Notable non-obvious dependencies:**
  - **The second-highest fan-in header in the project (29 includers, 27 of them sandbox files)** — after `defines.h` it is the widest engine→game contact surface, and the reason so many boundary edges exist.
  - **It documents a lifecycle the game does not control.** `begin_frame`/`end_frame` are driven by the *application loop*, and the game's `render(dt)` merely runs between them submitting draws. Most functions inherit an unstated precondition from that: calling them outside the frame window is silently dropped by the `frame_active` check.
  - **Ordering constraints are stated only in prose**: initialize after the window exists but before the game's `init()` (so the game can create GPU resources during init), and shutdown before `platform_terminate()`.
  - **The header is where the per-frame resubmission contract is written down** — lights, glow params, and the effect draws must be re-supplied each frame, while camera and bloom settings persist. Nothing in the signatures distinguishes the two.
  - **`renderer_set_draw_alpha` is documented as excluding sunbursts, starfields, and nebula** because those carry their own visibility parameters — an exception a caller would otherwise discover only by testing.
  - **Sprite `layer` is described as a plain draw-order integer**, but the backend also uses it for the bloom split and the unlit threshold, so the same number carries three meanings across the boundary.
  - **`renderer_set_present_mode`'s comment explicitly warns callers not to assume the toggle succeeded** — the driver may refuse IMMEDIATE — and ties the mode to the application loop's frame cap.
  - The debug section documents its own implementation strategy (quads through the sprite batch, no line pipeline) and the reason, which is unusual for a public header but matters: it explains why thickness is in world units and why these calls consume sprite budget.
  - `renderer_load_texture` and `renderer_create_texture` both carry an explicit "create these in the game's init(), not per frame" instruction — a performance contract with no enforcement.
  - "Phase 3"/"Phase 4" section headings, like the backend's, record the order features were added rather than a designed grouping.
- **Interface vs internal:** Public interface, and the primary one. Every rendering capability the game has is declared here, and no SDL or backend type appears anywhere in it.

---

## engine/source/renderer/renderer_backend.cpp

- **Path:** `engine/source/renderer/renderer_backend.cpp`
- **Purpose:** The backend factory — binds the `renderer_backend` function-pointer table to a concrete implementation chosen by enum, and clears it again on destroy.
- **Key types/functions:**
  - `renderer_backend_create(ERendererBackend type, renderer_backend* out_backend)` — one `switch` case (`RENDERER_BACKEND_SDL_GPU`) assigning 30 function pointers, plus a `default` that logs `BS_LOG_FATAL` and returns `FALSE`.
  - `renderer_backend_destroy(renderer_backend*)` — nulls the same 30 pointers and `internal_state`.
- **Notable non-obvious dependencies:**
  - **It is the only file that includes both `renderer_backend.h` and `backend/renderer_backend_sdlgpu.h`** — the single point where the abstract interface meets a concrete implementation. That is what keeps `renderer.cpp` free of any backend knowledge beyond the enum.
  - **It works precisely because `renderer_backend_sdlgpu.h` is SDL-free.** Naming all 30 backend symbols without pulling in `<SDL3/SDL_gpu.h>` is the entire purpose of that firewall header; this file is its only beneficiary.
  - **The two functions are hand-maintained parallel lists.** Adding a backend entry point means editing three places in lockstep — the struct in `renderer_backend.h`, the assignment block here, and the null-out block below it. Nothing detects a pointer that is added to `create` but forgotten in `destroy`, or vice versa; a member missed in `create` silently stays whatever the caller's memory held.
  - **`internal_state` is zeroed at the top of `create` and again in `destroy`**, but the backend sets it to `&g_sdl` during `initialize`, so the field is only meaningful between initialize and shutdown.
  - **`destroy` deliberately does not release anything** — the comment on the declaration says to call `shutdown()` first. It only unwires the table, so calling it out of order leaks the GPU device with no diagnostic.
  - **`ERendererBackend` currently has exactly one usable value**, making the `default` branch unreachable in practice; the abstraction is built for a second backend that does not exist yet.
- **Interface vs internal:** Internal implementation. Neither function is exported, and both are called only from `renderer.cpp`'s initialize and shutdown.

---

## engine/source/renderer/renderer_backend.h

- **Path:** `engine/source/renderer/renderer_backend.h`
- **Purpose:** Defines the `renderer_backend` vtable — the 30-entry function-pointer contract every concrete rendering backend must satisfy — plus the factory declarations.
- **Key types/functions:**
  - `typedef struct renderer_backend` — `VOID_PTR internal_state` followed by 30 function pointers: lifecycle (`initialize`, `shutdown`, `on_resize`, `begin_frame`, `end_frame`), `set_clear_color`, the texture trio, `set_camera`, `draw_sprite` / `draw_mapped_sprite`, six procedural-effect draws, `set_lights`, `set_glow_params`, bloom and streak setters, `get_frame_stats`, `set_present_mode`, `get_present_timing`.
  - `renderer_backend_create` / `renderer_backend_destroy`.
  - A forward declaration of `struct PlatformState`; the include of `renderer_types.h` supplies every value type.
- **Notable non-obvious dependencies:**
  - **Every function pointer takes `struct renderer_backend*` as its first argument**, modelling an instance-based interface — but the only implementation ignores it and uses a file-scope global. The parameter is vestigial in practice, though it is what would make a second backend instance possible.
  - **`internal_state` is documented as never dereferenced by the frontend**, and that holds: `renderer.cpp` only passes `&state.backend` around.
  - **Two pointers are explicitly documented as optionally NULL** (`get_frame_stats`, `get_present_timing`), and the header says the frontend checks first. In practice `renderer.cpp` null-checks almost *every* pointer, so the actual convention is broader than what is written here.
  - **The header carries contracts the implementation must honour**, not just signatures: `create_texture` expects tightly-packed RGBA8 with a top-left origin and explains that the frontend decodes so this stays the only GPU-touching TU; `set_camera` specifies the view-projection is rebuilt at draw time from the live swapchain size; the effect draws all state that parameters are *queued* and rendered in `end_frame`, not immediately; `set_lights` documents fullbright-at-zero and silent over-cap dropping.
  - **`get_present_timing`'s comment encodes a profiling insight** — that VSYNC pacing and swapchain starvation surface as `acquire_ms` rather than as GPU work — which is why the split exists at the interface level at all.
  - The `Phase 3` / `Phase 4` markers show the vtable grew by accretion; each new effect added a `draw_*` plus its `set_*` companions rather than a generalised pass mechanism, which is why the table is 30 entries wide.
  - **This header is `renderer_types.h`'s reason for existing** — it and `renderer.h` and the backend all need the same value types, so they were factored out rather than duplicated.
- **Interface vs internal:** Internal interface — the engine's own extension point for adding a rendering backend. Nothing here is `bs__api__`, so it is invisible to the game.

---

## engine/source/renderer/renderer_types.h

- **Path:** `engine/source/renderer/renderer_types.h`
- **Purpose:** Defines every value type the renderer's three layers exchange — opaque resource handles, colour and rect primitives, the sprite and light structs, the camera, frame stats, and the seven large parameter blocks for the procedural effects.
- **Key types/functions:**
  - Handles: `bs_texture`, `bs_shader`, `bs_pipeline`, `bs_buffer`, `bs_sampler` (each a bare `u32 id`), plus `BS_INVALID_HANDLE` = 0.
  - `enum ERendererBackend` (one value), `enum EBlendMode` (`NONE`/`ALPHA`/`ADDITIVE`/`MULTIPLY` + `BLEND_MODE_COUNT`), `enum bs_heat_palette`.
  - Primitives: `bs_color`, `bs_rect`.
  - Draw data: `bs_sprite`, `bs_mapped_sprite`, `bs_glow_params`, `bs_light2d`, `Camera2D`, `bs_frame_stats`.
  - Effect parameters: `bs_starfield_layer_data`, `bs_starfield_params`, `bs_sunburst_params`, `bs_starsurface_params`, `bs_planetsurface_params`, `bs_heat_map_params`, `bs_nebula_params`.
  - Constants: `BS_LAYER_BLOOM_THRESHOLD` = 50, `BS_MAX_HEAT_SOURCES` = 256.
- **Notable non-obvious dependencies:**
  - **The seam rule is written into the file as a prohibition**: it must not include any backend header. It includes `math_utils.h` and the comment justifies the exception — engine math is not a backend. This is the header that makes the frontend/backend/game split expressible at all, since all three layers need these types.
  - **Handles are bare `u32`s whose bit layout is defined elsewhere.** The comment says the high bits "may encode a generation counter", and the backend does exactly that (`generation << 18 | index+1`) — so the packing is a real contract split across two files with nothing enforcing it here.
  - **`BS_LAYER_BLOOM_THRESHOLD` is a rendering *policy* living in a types header.** It decides which sprites go through bloom and which are drawn crisp afterwards, so gameplay code choosing a layer number is implicitly choosing a post-process path.
  - **`bs_sprite::glow_override` is a raw `const bs_glow_params*` the game owns.** The struct is copied into the batch but the pointer is not — so the pointee must outlive `end_frame`. Worse, the backend breaks draw runs on pointer *identity*, so where the game stores its glow params affects batching efficiency.
  - **`bs_sprite::custom` is an untyped `bs_color` reinterpreted by the fragment shader** — four floats whose meaning is defined only in HLSL, the same untagged-union pattern as the event bus.
  - **`bs_heat_map_params` is enormous** — three parallel arrays of 256 entries (positions, emissions, detector flags) make it roughly 3.3 KB, and it is passed by pointer but copied by value into backend state every frame.
  - **The parameter structs encode game concepts in engine headers** — `planet_type` is documented as "PlanetType enum value", an `i32` mirroring a sandbox enum with no shared definition; `bs_planetsurface_params` carries a full "genome" of palette stops and feature genes; `bs_starfield_params` still has a `layer_data` pointer for a "legacy VBO path" alongside the procedural tunables that replaced it.
  - **HierPos2's influence shows up as split coordinates**: `cam_cell` / `cam_local` pairs appear in both the starfield and nebula params, with a comment explaining that a single combined `Vec2` would suffer f32 snapping billions of units from the origin. The renderer never includes `bs_hierpos.h`; it takes the decomposition as plain floats.
  - `bs_shader`, `bs_pipeline`, `bs_buffer`, and `bs_sampler` are declared but **never used anywhere** — placeholders for a resource system that does not exist.
  - The doc comment above `bs_mapped_sprite` describes `bs_sprite` instead — the two are adjacent and the comment sits on the wrong one.
  - `Camera2D` is defined here while all its operations live in `camera2d.h`, splitting the type from its behaviour across two headers.
- **Interface vs internal:** Public interface, and the shared vocabulary of the entire rendering stack — the game, the frontend, the vtable, and the backend all program against it. Its fan-in (18) understates its reach, since `renderer.h` and `camera2d.h` pull it in transitively for nearly every sandbox render file.

---

## engine/source/renderer/starfield_gpu_resources.cpp

- **Path:** `engine/source/renderer/starfield_gpu_resources.cpp`
- **Purpose:** Implements a VBO-based starfield renderer — an additive graphics pipeline, per-layer vertex buffers, and a tiled draw that culls to the visible region of a wrapping star field.
- **Key types/functions:**
  - `StarfieldGpuResources::init(device, vs, fs, colorFmt)` — builds an additive (ONE/ONE) pipeline over a 7-float interleaved vertex layout.
  - `::shutdown(device)` — releases all layer VBOs and the pipeline.
  - `::upload_layer(device, layerId, data, floatCount)` — grows the layer vector, recreates the buffer, and uploads through a transient transfer buffer and command buffer.
  - `::draw(cmd, pass, params, blur, brightness)` — computes a scale/rotation uniform set, wraps the camera into the field period, and issues one draw per visible 256-unit tile using a prefix-sum tile index.
- **Notable non-obvious dependencies:**
  - **The class is never instantiated.** No `StarfieldGpuResources` object exists anywhere in either tree — the backend includes its header but constructs nothing. Pipeline, VBOs, and the entire tiled draw are compiled into `engine.dll` and never run.
  - **It is doubly dead**: even if constructed, `draw()` returns immediately unless `params.layer_data` is non-null, and the only producer of starfield params (`sandbox/source/render/starfield_layer.cpp`) explicitly sets `layer_data = nullptr` with the comment "procedural path". The live starfield is the shader-based one in the backend; this is the superseded implementation, which `renderer_types.h` labels the "legacy VBO path".
  - **`upload_layer` submits its own command buffer per call** and creates then immediately releases a transfer buffer — the same pattern the backend later optimised away with pooling for RmlUi.
  - **`init` declares 6 vertex attributes but fills only 4** (`SDL_zero(attribs)` then `num_vertex_attributes = 4`), and the comment describes a 7-float layout as "(offset_xy, size, corner, r, g, b)" — the header's `LayerVbo` comment says `floatCount / 4` while the code divides by 7. The three descriptions of the vertex format do not agree.
  - **Tile culling relies on `widthMod` being a power of two minus one** — `gx & ld.widthMod` is a masked wrap, and `minX &= ~(TILE_SIZE - 1)` assumes the same for the 256 tile size. Neither assumption is validated.
  - `shutdown` takes a `device` parameter although the class already stores `device_`, and the stored copy is otherwise unused.
  - Uses `std::vector` and RAII-style C++ classes with trailing-underscore members — a naming and style convention that appears nowhere else in the engine, marking it as a distinct authorship era.
  - Carries hardcoded look constants (`MIN_ZOOM` 0.15, `TILE_SIZE` 256, the `pow(pass+1, 0.2)` fullness curve) attributed in comments to "Endless Sky style".
- **Interface vs internal:** Nominally an internal implementation class; in practice **dead code**. It is the only engine file whose entire contents are unreachable.

---

## engine/source/renderer/starfield_gpu_resources.h

- **Path:** `engine/source/renderer/starfield_gpu_resources.h`
- **Purpose:** Declares the `StarfieldGpuResources` class — the VBO starfield's pipeline and per-layer buffer owner.
- **Key types/functions:**
  - `class StarfieldGpuResources` — public `init`, `shutdown`, `upload_layer`, `draw`, and the inline `has_pipeline()`.
  - Private nested `struct LayerVbo { SDL_GPUBuffer* buffer; u32 vertexCount; }` and the members `pipeline_`, `device_`, `layerVbos_`.
  - Forward-declares `struct bs_starfield_params` and `namespace bs_math { struct Vec2; }`.
- **Notable non-obvious dependencies:**
  - **This header includes `<SDL3/SDL_gpu.h>` directly** — making it the *only* file in the engine besides `renderer_backend_sdlgpu.cpp` that actually pulls in the SDL GPU header, and therefore the single structural exception to the seam rule stated all over the renderer. (`renderer_backend_sdlgpu.h` only *mentions* the header in a comment explaining why it avoids it.) The violation is contained only by accident of usage: its sole includer happens to be the backend TU, which is already permitted. Any other file including this header would silently breach the invariant.
  - **It is a C++ class in a codebase that is otherwise C-style structs and free functions** — the only such type in the engine outside the RmlUi interface subclasses, which are themselves forced by a third-party API.
  - **Uses `std::vector` and default member initialisers in a header**, unlike the engine's `Vector(T)` macro alias convention, and includes `<vector>` directly.
  - **Forward-declares `bs_math::Vec2` as a `struct`** to avoid including `math_utils.h` — correct here, but it means the header depends on `Vec2` never becoming a class or gaining a template parameter.
  - `LayerVbo::vertexCount` is commented "floatCount / 4" while the implementation computes `floatCount / 7` — the header's documentation of its own field is wrong.
  - Nothing here is `bs__api__`, so the class is invisible outside the DLL.
- **Interface vs internal:** Internal interface for code that is never used. Its lasting significance is structural rather than functional: it is where the renderer's "no SDL outside the backend" rule is actually broken.

---
