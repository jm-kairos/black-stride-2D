# Memory

**Responsibility:** Owns the engine's allocation facade — tagged heap allocation with per-tag
byte accounting, thin wrappers over the platform's block operations, and a reserve/commit bump
arena. It owns the `EMemoryTag` taxonomy and the running statistics. It explicitly does not own
the allocation itself: every function here forwards to Platform (`platform_allocate`,
`platform_allocate_virtual_memory_reserve`, etc.), and this layer adds only accounting. It also
does not own alignment — despite the tag enum's stated purpose (`bs_memory.h:5`), no aligned
path exists.

**Public interface:** `engine/source/core/memory/bs_memory.h` — `enum EMemoryTag`;
`bs_memory_allocator` / `bs_memory_free`; `bs_memory_zero` / `_copy` / `_set`;
`bs_memory_allocator_virtual_memory_reserve` / `_commit` / `bs_memory_virtual_free`;
`bs_memory_initialize` / `_terminate`; `bs_memory_get_memory_usage_string`. All are `bs__api__`.
`engine/source/core/memory/arena.h` — `arena_t`, `ARENA_PTR`, `arena_initialize` /
`_allocate` / `_reset` / `_terminate`.
From the sandbox only `bs_memory_allocator` (6 files), `bs_memory_free` (5) and the
`MEMORY_TAG_*` values (9) are used.

**Depends on:** Foundation, Diagnostics, Platform.
**Depended on by:** AppLifecycle, EventBus, Input, RenderFrontend, plus the sandbox.

**Key invariants:**
- **`bs_memory_free` must be given the same size and tag as the matching allocation.** It
  subtracts both from unsigned counters (`bs_memory.cpp:81-82`) with no validation, so a
  mismatch silently corrupts — and can wrap — the accounting. Enforced nowhere.
- `EMemoryTag` order must match `bs_memory_tag_strings[]` positionally
  (`bs_memory.cpp:16-35`); both are sized by `MEMORY_TAG_MAX_TAGS`. **No `static_assert` ties
  them** — verified: the only assertions in the project are the scalar-width ones in
  `defines.h`. Inserting a tag mid-enum silently relabels every later row.
- `tag` is never bounds-checked before indexing `stats.tagged_allocations[tag]`
  (`bs_memory.cpp:68,82`).
- Arena commit grows monotonically; `arena_reset` sets `current_offset = 0` and never decommits
  (`arena.cpp:80-82`). Deliberate for a per-frame arena, but it means the resident footprint is
  the high-water mark for the process lifetime.
- `arena_terminate` passes size `0` to `bs_memory_virtual_free` (`arena.cpp:86`), relying on the
  "0 means release the whole block" convention documented only in a trailing comment there.

**Extension points:** Adding a memory tag means two coordinated edits — the enum
(`bs_memory.h:6-27`) and the string table at the same index (`bs_memory.cpp:16-35`). That is the
only real extension point; the allocator itself has no hooks (no custom-allocator callback, no
per-tag policy dispatch).

**Known limitations / tech debt:**
- **`arena_allocate` has zero call sites in either tree** (declaration `arena.h:28`, definition
  `arena.cpp:37`). `core/application.cpp:114` creates a 64 MB frame arena, resets it every
  frame (`:122`) and destroys it (`:199`), but nothing ever allocates from it. The arena is
  currently pure overhead.
- **Arena allocations are unaligned.** `arena_allocate` returns `base_ptr + current_offset`
  after a raw `+= size` (`arena.cpp:73-74`); only page *commit* boundaries are aligned. Callers
  needing natural alignment must pad `size` themselves, and nothing says so.
- Memory is not zeroed on arena allocate or reset — a reset arena hands back the previous
  frame's bytes.
- **The virtual-memory trio bypasses the tally entirely** (`bs_memory.cpp:48-60`), so the 64 MB
  arena body never appears in the statistics; only the small `arena_t` header does. `arena.h:7`
  carries a TODO acknowledging this gap.
- `bs_memory_get_memory_usage_string` returns `_strdup`'d memory the caller must free
  (`bs_memory.cpp:128`); its only call site, `core/application.cpp:116`, neither frees it **nor**
  treats it as data — it passes the returned string as the *format* argument to `BS_LOG_INFO`,
  so any `%` in the text would be read as a conversion specifier.
- The same function passes a constant `8000` as `snprintf`'s bound while advancing
  `buffer + offset` (`bs_memory.cpp:126`) instead of the remaining space.
- `bs_memory.h:29` reads `// TODO: these shall not be exported !` above
  `bs_memory_initialize`/`_terminate`, but **that TODO cannot be actioned as written**:
  `engine/source/entry.h:18,45` calls both, and `entry.h` compiles into the host executable.
- `bs_memory_copy`'s `const VOID_PTR source` (`bs_memory.h:41`) constifies the pointer, not the
  pointee — the `const` is not doing what it looks like.
- `arena.h` pulls `core/logger.h` and `bs_memory.h` into every includer although only the `.cpp`
  needs them; and `arena_t` is fully defined rather than opaque, so callers can poke
  `current_offset` directly.
- `bs_memory_terminate` (`bs_memory.cpp:44`) has an empty body — no leak reporting on shutdown.
- One file-static `stats` global (`bs_memory.cpp:37`) mutated by every alloc and free with no
  synchronisation (see the engine-wide note: there is no threading at all).

**Source paths:** `engine/source/core/memory/**`

**Last verified:** 2026-08-07, commit `812680c`
