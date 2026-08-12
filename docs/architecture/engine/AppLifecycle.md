# AppLifecycle

**Responsibility:** Owns process startup and the frame loop — the order in which subsystems are
brought up and torn down, the per-frame update/render/present sequence, frame pacing, and the
`Game` callback contract a host must implement. It owns `main()` itself (in `entry.h`). It
explicitly does not own any subsystem's behaviour, only their *sequencing*; and it does not own
the game — the concrete game is late-bound through four function pointers, so the engine never
names a sandbox symbol.

**Public interface:** `engine/source/game_types.h` — `struct Game` (the `init`/`update`/
`render`/`on_resize` function pointers plus an opaque `VOID_PTR state`).
`engine/source/core/application.h` — `struct ApplicationConfig`; `application_init`,
`application_run` (both `bs__api__`).
`engine/source/entry.h` — defines `int main(void)` and declares `extern b8 game_create(Game*)`,
the one symbol the host must supply.
The interface is **inverted**: the host does not call `application_init` (that happens inside
`entry.h`); it *defines* `game_create` and fills in `Game`.

**Depends on:** Memory, Diagnostics, Platform, RenderFrontend, EventBus, Input, Foundation —
more than any other subsystem.
**Depended on by:** nothing. It is the top of the dependency graph (in-degree 0).

**Key invariants:**
- **`application_init` is single-shot.** Enforced by `static b8 initialized`
  (`application.cpp:25`); a second call logs an error and returns `FALSE` (`:35-39`).
- **Bring-up order is fixed**: logger → input → event → platform (window) → renderer → the
  game's own `init`, then a synthetic `on_resize` (`application.cpp:50-105`). Enforced only by
  statement order. Teardown reverses it (`:190-205`).
- The renderer must be up before the game's `init` so the game can create GPU resources during
  its own initialisation — stated at `renderer/renderer.h:20-21` and satisfied by
  `application.cpp:89` preceding `:95`.
- **All four `Game` function pointers must be non-null** — enforced at `entry.h:27-31`, which
  returns `-2` before starting if any is missing. This is the only validation of the host
  contract.
- **`end_frame` runs only after a successful `begin_frame`.** `application.cpp:145` gates
  `render` + `end_frame` on `renderer_begin_frame(dt)`. RenderFrontend's `frame_active` flag
  (`renderer/renderer.cpp:137,148`) is the second half of the same guarantee, and it is what
  keeps the backend's ImGui `NewFrame`/`Render` pair balanced.
- Four bus callbacks registered in `application_init` are unregistered symmetrically in
  `application_run` (`application.cpp:71-74`, `:190-193`).
- A zero-size window means minimised → `is_suspended`, which gates update, render and input
  entirely (`application.cpp:266-271`, `:134`).

**Extension points:** A host implements `game_create(Game*)` (see
`sandbox/source/entry.cpp:5`), fills `app_config`, assigns the four callbacks, and allocates its
own state into `Game::state`. Bringing up a new engine subsystem means adding it to the ordered
block in `application_init` and its mirror in `application_run` — there is no registry or
init-list, so the order is hand-maintained. New global bus handlers are added by
`event_register` in `application_init` with a matching `event_unregister`.

**Known limitations / tech debt:**
- **`entry.h` is a header that defines `main()`** (`entry.h:16`). It therefore must have exactly
  one includer — and does: `sandbox/source/entry.cpp:2`. It is never compiled into `engine.dll`
  (the engine build globs only `.cpp`, `engine/build.bat:103`), so this is the one engine file
  whose build membership and tree location disagree.
- **The memory subsystem's lifecycle lives outside `application.cpp`.** `bs_memory_initialize`
  and `_terminate` are called from `entry.h:18,45`, i.e. from the host binary, before and after
  everything else. This is also why the `// TODO: these shall not be exported !` at
  `core/memory/bs_memory.h:29` cannot be actioned as written.
- **`bs_memory_terminate` is skipped on all four failure paths** in `main` (`entry.h:24,30,36,42`);
  only a clean run reaches it.
- `Game game_inst;` (`entry.h:20`) is an uninitialised stack object passed to `game_create` to
  fill, so any field the host does not set holds garbage; the null check covers only the four
  pointers.
- Failures are logged with `BS_LOG_FATAL` *before* `logger_initialize()` runs
  (`entry.h:23` vs `application.cpp:50`) — safe only because the logger's init is currently an
  empty stub.
- ~~**`ESCAPE` is a hardcoded quit binding** in the application layer~~ — **moved to game
  policy** (2026-08-12): `application_on_key` no longer fires the quit; `game.cpp`'s
  edge-triggered ESC handler closes an open ship inspector first and otherwise fires
  `EVENT_CODE_APPLICATION_QUIT` through the exported `event_fire` — the EventBus's first
  game-side caller.
- Six `BS_LOG_*` test messages are left in `application_init` behind a `// TODO: remove this`
  (`application.cpp:53-59`), firing on every launch.
- The 60 FPS software frame cap is a hardcoded `1.0 / 60.0` (`application.cpp:177`) with no
  configuration hook, and is skipped in IMMEDIATE present mode.
- A stray `;` terminates the `application_init` `if` block at `entry.h:37`.
- `update`/`render` take `f32` while the loop computes in `real` (double) and narrows at the
  call; `on_resize` takes `u32` while the application stores `i16` dimensions.

**Source paths:** `engine/source/core/application.{cpp,h}`, `engine/source/entry.h`,
`engine/source/game_types.h`

**Last verified:** 2026-08-07, commit `812680c`
