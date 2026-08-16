# ActionLog

**Responsibility:** Owns the rolling HUD message buffer — appending printf-formatted lines,
evicting the oldest at capacity, and resetting the idle-fade timer. That is its entire scope. It
explicitly does not own the buffer's *storage* (`s->action_log` is declared in
`state/game_state.h`), and it does not render: `sim/action_log.h` records that the buffer is
drawn by the RmlUi HUD via `game_push_hud`, which replaced a retired `bs_ui` panel.

**Public interface:** `sandbox/source/sim/action_log.h` — `action_log_push(game_state*, const
char* fmt, ...)`. One variadic function. Used from outside by **6 subsystems**, making it one of
the most widely invoked sandbox functions.

**Depends on:** GameStateModel; engine `defines.h`.
**Depended on by:** CombatArena, Discovery, GalaxyHistory, DevPanels, InWorldOverlays,
FrameOrchestrator — plus `sim/point_defense.cpp` within CombatArena.

**Key invariants:**
- **`ACTION_LOG_MAX` (30) must match the array in `state/game_state.h`.** The capacity constant
  is defined in `sim/action_log.cpp` while the array it indexes is sized in the god struct;
  nothing ties them. Enforced by nothing.
- **The 128-byte entry width appears three times in one function** — as the `memcpy` size, the
  `vsnprintf` bound, and the NUL index — rather than being derived from the array type. All
  three must agree with the declaration in `game_state.h`.
- **Pushing resets `inactivity_timer`**, which drives the HUD panel's idle fade. So logging has a
  visual side effect beyond adding a line; anything that pushes frequently keeps the panel
  visible.
- Eviction preserves order: at capacity the buffer shifts down one and appends, so index 0 is
  always the oldest.

**Extension points:** There is essentially one — call `action_log_push` from anywhere with a
`game_state*`. There is no severity level, no category, no filtering hook, and no second sink.
If a message should also appear in the Discoveries feed, the established pattern is the reverse
direction: `discovery_log_push` calls `action_log_push`, not the other way round.

**Known limitations / tech debt:**
- **Eviction is an O(n) `memcpy` shuffle of 30 × 128-byte entries on every push once full**,
  rather than a ring buffer. Cheap in absolute terms, but it is a linear cost per message in a
  function called from combat, AI, missions and trade.
- **No `printf` format attribute** is applied, so mismatched varargs are not diagnosed despite
  the function being printf-style. Under `-Wall -Werror` this is a missed check the compiler
  could otherwise make.
- The module owns no storage — it is a mutator of state declared elsewhere, so the capacity,
  the entry width and the timer semantics are all split between two files.
- There is no severity or category on entries, so the HUD cannot filter or colour by kind; every
  message is equal. Contrast the Discoveries feed, which does carry a `DiscoveryKind`.
- Messages are formatted at push time into fixed 128-byte buffers, so long lines are silently
  truncated with no indication.
- *Inferred:* the module exists as a separate file mainly because it was extracted from
  `game.cpp` during the module split — the comment in `game.cpp` recording the move supports
  that, and there is no behaviour here that needed isolating. It is a two-file subsystem for a
  27-line function.

**Source paths:** `sandbox/source/sim/action_log.{cpp,h}`

**Last verified:** 2026-08-07, commit `e4d88d1`
