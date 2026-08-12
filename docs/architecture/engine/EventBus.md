# EventBus

**Responsibility:** Owns a global publish/subscribe bus: a code-indexed table of listener
callbacks, registration and unregistration, and synchronous first-handler-wins dispatch. It owns
the reserved system event codes and the type-punned payload union. It explicitly does not own
what the payload *means* for any given code — the field layout is an out-of-band convention
between firer and handler, documented for exactly one code (`WINDOW_RESIZED`, `event.h:57-58`)
and undocumented for the rest. It also does not own event *sources*: Platform and Input fire
into it; the bus never originates an event.

**Public interface:** `engine/source/core/event.h` — `event_context` / `event_context_t`;
`PFN_on_event`; `enum ESystemEventCode`; `event_register`, `event_unregister`, `event_fire`
(all `bs__api__`); `event_initialize`, `event_terminate` (deliberately unexported, so a host can
use the bus but not bring it up).

**Depends on:** Foundation, Containers, Memory.
**Depended on by:** AppLifecycle, Input, Platform. **No sandbox consumers** — despite
`event_register` and `event_fire` being exported, no sandbox file calls either.

**Key invariants:**
- `event_initialize` must run before any other entry point. Enforced — but *silently*: every
  other function returns `FALSE` when `initialized` is false (`event.cpp:49-52,75-78,98-101`),
  so registering before init fails without a diagnostic.
- Initialisation is single-shot: a second `event_initialize` returns `FALSE`
  (`event.cpp:31-34`).
- **`code` must be below `MAX_MESSAGE_CODES` (10000) — and this is not enforced.** The parameter
  is `u16` (up to 65535) while `registered` is a `std::array` of 10000 entries indexed with
  unchecked `operator[]` (`event.cpp:22,55,80,103`). `event.h:48` actively invites high codes
  ("application should use codes beyond 255").
- Dispatch is first-handler-wins: `event_fire` returns as soon as a callback returns `TRUE`
  (`event.cpp:107-111`), so registration order is semantically significant and invisible at the
  call site.
- A listener may register only once per code — enforced by the duplicate scan at
  `event.cpp:55-63`, which compares `listener` only (not the callback).

**Extension points:** Adding an event means adding a value to `ESystemEventCode`
(`event.h:49-60`) or picking an application code above 255, then `event_register`ing a
`PFN_on_event`. The payload convention for the new code has to be agreed by hand — there is no
schema, tag, or helper. Handlers opt into consuming an event by returning `TRUE`.

**Known limitations / tech debt:**
- **The static table is large and eagerly constructed.** `__EventSystemState` holds
  `Array(__EventCodeEntry, 10000)` (`event.cpp:19-23`), i.e. 10000 `std::vector`s in the DLL's
  static storage — a few hundred KB, and 10000 vector constructions at load, regardless of how
  many codes are used.
- `event_terminate` has an **empty body** (`event.cpp:42-45`). It clears nothing and drops no
  listeners, so the hand-written unregister block in `core/application.cpp:190-193` is the only
  real teardown. `state = {}` in `event_initialize` is the only path that clears stale entries.
- **Mutation during dispatch is unsafe.** `registered_count` is captured before the loop
  (`event.cpp:103`), so a callback that registers or unregisters on the same code invalidates
  the vector under iteration. `core/application.cpp:229` does re-enter `event_fire` from inside
  a handler, though on a different code.
- The payload is an **untagged union** (`event.h:11-30`): nothing records which member was
  written. The comment claims "128 bytes"; the union is actually **16** (largest member
  `i64[2]` / `char[16]`).
- `core/input.cpp:97-98` packs signed `i16` mouse coordinates into `context.data.u16[0..1]`, so
  negative positions reach listeners wrapped.
- All operations are linear in listeners-per-code: `event_register` linear-scans for duplicates,
  and `event_unregister` uses an O(n) `erase` flagged in-file at `event.cpp:86`.
- `event.cpp:3` includes `core/memory/bs_memory.h` but makes no call into it — the containers
  resolve to `std::vector`/`std::array` and use the global allocator, so **the bus is not routed
  through the engine's memory accounting**.
- ~~The whole subsystem has no game-side consumer~~ — `event_fire` gained its first sandbox
  caller 2026-08-12 (`game.cpp` fires `EVENT_CODE_APPLICATION_QUIT` from its modal-aware ESC
  handler); `event_register`/`event_unregister` remain unexercised across the boundary.

**Source paths:** `engine/source/core/event.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
