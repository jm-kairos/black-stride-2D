# Diagnostics

**Responsibility:** Owns levelled log output — expanding varargs, prefixing a severity tag, and
routing the finished line to the platform console writer — plus the assertion-failure reporter
that `core/asserts.h` declares. It owns the six `BS_LOG_*` macros and their compile-time enable
switches. It explicitly does not own where the text ends up: the actual write and the
level→colour mapping live in Platform (`platform_console_write` / `_write_error`, and the
`levels[6]` attribute table at `platform/platform_sdl3.cpp:245`). It also does not own any log
file — despite the TODOs at `logger.cpp:16` and `:29`, no file output or batching exists.

**Public interface:** `engine/source/core/logger.h` — `BS_LOG_FATAL`, `BS_LOG_ERROR`,
`BS_LOG_WARN`, `BS_LOG_INFO`, `BS_LOG_DEBUG`, `BS_LOG_TRACE`; `enum ELogLevel`;
`logger_output` (exported, but only ever reached through the macros);
`logger_initialize` / `logger_terminate` (unexported, called by AppLifecycle).
`engine/source/core/asserts.h` — `BS_ASSERT`, `BS_ASSERT_MSG`, `BS_ASSERT_DEBUG`,
`debugBreak`, `report_assertion_failure`. Consumed by 7 engine subsystems and 14 sandbox files.

**Depends on:** Foundation, Platform.
**Depended on by:** AppLifecycle, DeadStarfield, Input, Memory, Platform, RenderBackend,
RenderFrontend, plus the sandbox.

**Key invariants:**
- `report_assertion_failure`, declared at `asserts.h:24`, must have exactly one definition —
  it is at `core/logger.cpp:10`. Enforced by the linker; invisible in the include graph.
- `ELogLevel` must stay ordered most-severe-first. `logger.cpp:42` computes
  `is_error = level < LOG_LEVEL_WARN` to choose the error stream, and
  `platform_sdl3.cpp:246` indexes its colour table by the same value. **Not asserted anywhere.**
- `level` must be a valid `ELogLevel`: `logger.cpp:58` indexes `level_strings[6]` with no bounds
  check. The platform side does clamp (`levels[colour < 6 ? colour : 5]`,
  `platform_sdl3.cpp:246`), so the engine-side index is the unguarded one.
- FATAL and ERROR can never be compiled out — enforced structurally: unlike the other four,
  their macros at `logger.h:32-37` have no `#if` guard.

**Extension points:** Adding a log level requires four coordinated edits: the enum
(`logger.h:17-24`), the `level_strings[6]` table (`logger.cpp:32-40`), a `BS_LOG_*` macro with
its `LOG_*_ENABLED` switch (`logger.h:39-69`), and the `levels[6]` console-attribute table in
Platform (`platform_sdl3.cpp:245`). Nothing ties those four together; all are positional.

**Known limitations / tech debt:**
- **The assertion family is entirely unused.** No `BS_ASSERT`, `BS_ASSERT_MSG` or
  `BS_ASSERT_DEBUG` call site exists in either tree. Two latent defects are masked by that
  non-use: `BS_ASSERT_DEBUG(expr)` expands to `report_assertion_failure(#expr, message, …)` but
  has **no `message` parameter** (`asserts.h:46-54`), so any use fails to compile; and in the
  assertions-disabled branch `BS_ASSERT_MSG(expr)` is declared with **one** parameter instead of
  two (`asserts.h:61`).
- `BS_ASSERTIONS_ENABLED` is `#define`d unconditionally at `asserts.h:5`, so the disabled branch
  is unreachable.
- Every `BS_LOG_*` macro ends in a trailing semicolon (`logger.h:32-69`), and the disabled
  variants expand to *nothing* — so an unbraced `if (c) BS_LOG_DEBUG("x"); else …` changes
  meaning between configurations.
- `BSRELEASE` (`logger.h:12`) is never defined by either build script, so the release branch is
  dead and DEBUG/TRACE are always compiled in. Tested with `#if` rather than `#ifdef`, so an
  undefined macro quietly evaluates to 0 instead of erroring.
- `logger_output` allocates **two 32000-byte stack buffers per call** (`logger.cpp:45,57`) —
  roughly 64 KB of stack for every log line.
- There is no runtime verbosity control; filtering is entirely preprocessor-time.
- `logger_terminate` (`logger.h:27`) has no callers and an empty body.
- `logger_output` uses `__builtin_va_list` rather than `va_list` (`logger.cpp:50`), flagged
  in-file as a compiler-specific workaround — a hard dependency on clang/GCC builtins.
- `asserts.h` is included by exactly one file, `logger.cpp` — and only so the definition of
  `report_assertion_failure` matches its declaration.

**Source paths:** `engine/source/core/logger.{cpp,h}`, `engine/source/core/asserts.h`

**Last verified:** 2026-08-07, commit `812680c`
