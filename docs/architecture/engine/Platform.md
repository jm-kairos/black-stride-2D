# Platform

**Responsibility:** Owns everything OS- and SDL3-specific: window creation and teardown, the
event pump that translates SDL events into engine input calls and bus events, virtual and heap
memory primitives, coloured console output, and timing. It is the engine's single inbound edge
— every OS event enters the process here. It explicitly does not own input *state* (that is
Input; this layer only pushes edges into it), nor the log message format (that is Diagnostics;
this layer only writes the finished string and picks a console colour), nor memory *accounting*
(that is Memory, which wraps these primitives).

**Public interface:** `engine/source/platform/platform.h` — `PlatformState`;
`platform_initialize` / `_terminate` / `_pump_messages` / `_get_window_handle`; the
virtual-memory trio `platform_allocate_virtual_memory_reserve` / `_commit` /
`platform_virtual_free`; `platform_allocate` / `_free` / `_zero_memory` / `_copy_memory` /
`_set_memory`; `platform_console_write` / `_write_error`; `platform_get_absolute_time` /
`platform_sleep`. `engine/source/platform/platform_commons.h` — `PAGE_SIZE`.
**Only `platform_get_window_handle` carries `bs__api__`** (`platform.h:24`); everything else is
engine-internal by design, and `platform_sleep` documents its non-export as deliberate policy
(`platform.h:43-45`).

**Depends on:** Foundation, Diagnostics, Input, EventBus, UiFacade.
**Depended on by:** Memory, Diagnostics, AppLifecycle, RenderBackend. **No sandbox consumers** —
the one apparent match, `platform_get_absolute_time` in `sandbox/source/core/profiler.h:13`, is
a comment explaining the function is engine-internal.

**Key invariants:**
- `platform_initialize` must run before any timing call: `perf_freq` and `perf_start`
  (`platform_sdl3.cpp:22-23`) are captured there, and `platform_get_absolute_time` divides by
  `perf_freq` (`:277`). **Not guarded** — an early call divides by zero.
- `PlatformState::internal_state` is opaque to callers. Held to in practice: only
  `platform_sdl3.cpp` casts it to `InternalState*`. Note the struct itself is fully visible and
  `core/application.cpp:18` embeds one by value, so its size is fixed at one pointer.
- The event pump must feed ImGui and RmlUi *before* the engine's own switch, so UI capture flags
  are current — enforced by statement order at `platform_sdl3.cpp:102,107`, with the reason
  commented at `:98-107`.
- The `keys` enum values are Win32 virtual-key codes, so any backend must translate into that
  numbering — done by `sdl_scancode_to_bs_key` (`platform_sdl3.cpp:287-417`). A convention only;
  nothing checks it.
- Resize uses `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED` (`platform_sdl3.cpp:152`), not the logical
  event, because the renderer's drawable is sized in pixels.

**Extension points:** A new platform backend is a new `.cpp` implementing every declaration in
`platform.h`, replacing `platform_sdl3.cpp` in the build glob (`engine/build.bat:103`). The
`#if BS_PLATFORM_WINDOWS` scaffolding suggests this was intended — but see below; the
non-Windows branches do not currently compile. Adding a key means a `DEFINE_KEY` entry in
`core/input.h` plus a `case` in `sdl_scancode_to_bs_key`.

**Known limitations / tech debt:**
- **Only the Windows path is implemented, and the others do not compile.** The virtual-memory
  functions return `NULL` off Windows with TODOs (`platform_sdl3.cpp:179,189`); the non-Windows
  console path calls `fputs` at `:260,270` although **`<stdio.h>` is never included** (the file
  includes only `stdlib.h` and `string.h`, `:11-12`); and `platform_commons.cpp:15-17` has an
  empty `#else` branch, leaving its initialiser lambda with no return statement.
- `PAGE_SIZE` (`platform_commons.cpp:10`) is a dynamically-initialised global whose initialiser
  calls `GetSystemInfo`. It runs during static init, so any other TU reading it from *its* static
  initialiser may observe zero. Currently latent — `arena.cpp` only reads it inside functions.
  The name is also unqualified, uppercase, and collides with the POSIX `PAGE_SIZE` macro.
- `platform_initialize` leaks on both failure paths: the `InternalState` `malloc`
  (`platform_sdl3.cpp:35`) is unchecked and neither the `SDL_Init` nor the `SDL_CreateWindow`
  early return frees it or calls `SDL_Quit` (`:39-50`).
- `SDL_EVENT_QUIT` returns `FALSE` from inside the poll loop (`platform_sdl3.cpp:115`),
  abandoning any events still queued that frame — and it does so *after* already firing
  `EVENT_CODE_APPLICATION_QUIT`, so shutdown is signalled twice.
- `mouse_logical_to_pixel` queries both window sizes from SDL on **every** mouse-motion event
  (`platform_sdl3.cpp:85-86`) rather than caching on resize.
- Wheel input is quantised to ±1 (`platform_sdl3.cpp:150`), discarding high-resolution and
  horizontal scroll.
- `platform_allocate`/`_free` accept an `aligned` flag and ignore it (`:205,211`); the aligned
  path the memory-tag system implies does not exist. `platform_virtual_free` likewise ignores
  `size` and always `MEM_RELEASE`s (`:196-197`).
- `SetConsoleTextAttribute` is set but never restored (`platform_sdl3.cpp:246`).
- Platform depends *upward* on UiFacade (`platform_sdl3.cpp:6-7`), so it is not a clean bottom
  layer, and it forms a 1-edge-each-way cycle with Diagnostics (`logger.cpp` → `platform.h`,
  `platform_sdl3.cpp` → `logger.h`).

**Source paths:** `engine/source/platform/**`

**Last verified:** 2026-08-07, commit `812680c`
