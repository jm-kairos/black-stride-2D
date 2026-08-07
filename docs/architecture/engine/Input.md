# Input

**Responsibility:** Owns keyboard and mouse state — current and previous snapshots, plus a
per-frame wheel accumulator — and converts platform-pushed edges into bus events. It owns the
`keys` and `buttons` enumerations. It explicitly does not own event *acquisition*: Platform
polls SDL and calls `input_process_*`. It does not own key bindings either; the mapping from key
to game action lives entirely in sandbox call sites (~25 inline edge tests in
`sandbox/source/game.cpp`).

**Public interface:** `engine/source/core/input.h`, split by intent into two halves.
*Exported* (`bs__api__`) and game-facing: `input_is_key_down` / `_was_key_down` / `_is_key_up` /
`_was_key_up`, `input_is_button_down` / `_was_button_down` / `_is_button_up` / `_was_button_up`,
`input_get_mouse_position`, `input_get_previous_mouse_position`, `input_get_mouse_wheel`, plus
`enum keys` and `enum buttons`. *Unexported* and engine-only: `input_initialize`,
`input_terminate`, `input_update`, and the four `input_process_*` push functions.

**Depends on:** Foundation, EventBus, Memory, Diagnostics.
**Depended on by:** AppLifecycle, Platform (engine side); the sandbox uses six of the polling
functions across 7 files.

**Key invariants:**
- **The `bs__api__` split is the access-control mechanism.** The push half is not exported
  (`input.h:182-185`), so a host can poll input but cannot inject it or drive the lifecycle.
  Enforced at link time.
- `input_update` must run **after** the game's update each frame — `core/application.cpp:166`
  places it there. This is what gives `was_*` its "as of last frame" meaning and what makes
  `input_get_mouse_wheel` valid only during the game's update; the contract is documented at
  `input.h:176-180` and enforced only by that one call site's position.
- Key and button state are edge-triggered: `input_process_key` / `_button` fire only on an actual
  change (`input.cpp:59,74`), so no auto-repeat reaches the bus. The wheel is the deliberate
  exception — it fires unconditionally (`input.cpp:105-110`).
- `keys` values are Win32 virtual-key codes, so any platform backend must translate into that
  numbering. A convention; the translation lives at `platform/platform_sdl3.cpp:287-417`.
- Indices are never bounds-checked. `state.keyboard_current.keys[key]` and
  `mouse_current.buttons[button]` are raw subscripts on both the push and poll paths
  (`input.cpp:59,74,115,139`).

**Extension points:** Adding a key means a `DEFINE_KEY(name, code)` line in the enum
(`input.h:14-154`) using the matching Win32 VK value, plus a `case` in `sdl_scancode_to_bs_key`
(`platform_sdl3.cpp:287-417`); unmapped scancodes return `KEYS_MAX_KEYS` and are dropped by the
caller (`platform_sdl3.cpp:122-123`). Adding a mouse button means an entry in `enum buttons`
before `BUTTON_MAX_BUTTONS` (which sizes the state array at `input.cpp:14`) plus a `case` at
`platform_sdl3.cpp:137-143`.

**Known limitations / tech debt:**
- `KEYS_MAX_KEYS` is **not** a usable array bound: it evaluates to 0xC1 while the state array is
  `b8 keys[256]` (`input.cpp:8`), and because the codes are sparse the enum is not densely
  packed. Anything treating it as a count will be wrong.
- `input_terminate` only clears the `initialized` flag (`input.cpp:39-41`); key and button state
  survives, so a re-`initialize` is what actually resets it.
- `DEFINE_KEY` (`input.h:12`) is defined and never `#undef`'d, so it leaks into every including
  translation unit.
- `input.cpp` includes its own header twice, as `"core/input.h"` and `"input.h"` (`:1,5`) —
  harmless under `#pragma once`, but two spellings resolve to one file.
- Events are dispatched synchronously from inside the platform message pump, so handlers run
  mid-pump rather than at a frame boundary.
- Three exported predicates have no callers anywhere: `input_is_key_up`, `input_was_key_up`,
  `input_get_previous_mouse_position`.
- Mouse position is stored as `i16` but returned through `i32*`; the event path packs the same
  signed values into `u16` fields (`input.cpp:97-98`), so listeners see negative coordinates
  wrapped while the accessor pair reports them correctly.
- Three file-static globals hold all state (`input.cpp:25,26,31`) with no synchronisation.

**Source paths:** `engine/source/core/input.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
