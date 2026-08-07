# Bootstrap

**Responsibility:** Owns the single symbol the engine links against — `game_create` — and what
it must do: fill in the window configuration, bind the four lifecycle callbacks, and allocate
the game's state. That is the entire scope; the file is 19 lines. It explicitly does not own the
program entry point (`main()` is defined in the engine's `entry.h`, which this file includes),
does not own initialisation (that happens later in `game_init`, one of the callbacks it binds),
and owns no state of its own.

**Public interface:** `sandbox/source/entry.cpp` defines `b8 game_create(Game* out_game)`. It
declares nothing — there is no `entry.h` on the sandbox side. The symbol is declared `extern` by
the engine at `engine/source/entry.h`, and `game_create` is deliberately **not** `bs__api__`:
both the caller (`main`, inside `entry.h`) and the definition end up in `sandbox.exe`, so no DLL
crossing occurs.

**Depends on:** GameStateModel (for `sizeof(game_state)` and the four `game_*` declarations);
engine `entry.h`, `core/memory/bs_memory.h`.
**Depended on by:** nothing in the sandbox. The engine links against it.

**Key invariants:**
- **This file must define `game_create` or the program does not link.** It is the whole sandbox
  side of the engine/game inversion.
- **All four function pointers must be assigned.** `engine/source/entry.h:27-31` validates they
  are non-null before starting and returns `-2` otherwise — the only validation of the host
  contract, and it happens engine-side.
- **Including `<entry.h>` is what pulls `main()` into this translation unit.** `entry.h` defines
  `int main(void)`, so exactly one sandbox file may include it — and exactly one does.
- **`Game::state` is allocated here and never freed.** `bs_memory_allocator(sizeof(game_state),
  MEMORY_TAG_GAME)` at `sandbox/source/entry.cpp:17`; the engine treats `state` as opaque, passes
  it back to every callback, and never touches it. There is no shutdown hook, so it lives until
  process exit.
- **`sizeof(game_state)` is computed sandbox-side.** The engine only ever sees a byte count,
  which is why the god struct's size is not itself an ABI concern — though the engine types
  embedded *within* it are (see `engine-api-boundary.md` §7.3).
- The allocation is zeroed by `bs_memory_allocator` but no constructor runs here; `game_init`
  placement-news `game_state` later.

**Extension points:** Essentially none, and that is correct — this file is a fixed contract, not
a place to add behaviour. Window title, position and size are set here (`app_config`), so those
are the only values a change would touch. Anything else belongs in `game_init`, which is the
callback this file binds.

**Known limitations / tech debt:**
- **The allocation result is never checked.** `game_create` returns `TRUE` unconditionally, so an
  allocation failure produces a null `state` that every later call dereferences.
- **Window configuration is hardcoded** — title "Black Stride Engine Sandbox", position
  (100, 100), 1280×720 — with no config file or command-line override anywhere in the path.
- `game_state` is never freed. Harmless at exit, but it means there is no clean-shutdown path
  and no way to tear down and restart the game in-process. This is the same missing-hook problem
  `sim/ai_ship.h` works around by making `ai_ships_init` idempotent.
- The subsystem is one file with no header, so it appears in the dependency graph only as a
  leaf — easy to overlook despite being the linchpin of the engine/game contract.
- *Inferred:* the four callbacks are bound here rather than in `game_init` because
  `application_init` needs them populated before it runs; the code does not state this, but
  `entry.h`'s validation happening before `application_init` is consistent with it.

**Source paths:** `sandbox/source/entry.cpp`

**Last verified:** 2026-08-07, commit `812680c`
