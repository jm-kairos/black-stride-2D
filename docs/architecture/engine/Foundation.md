# Foundation

**Responsibility:** Owns the engine's base vocabulary: the fixed-width scalar typedefs
(`u8`–`i64`, `f32`, `f64`, `real`, `b8`, `b32`), the `TRUE`/`FALSE`/`VOID_PTR` macros,
compile-time platform detection, and `bs__api__` — the macro that decides whether a symbol is
exported from `engine.dll` or imported into a host. It owns no behaviour, no state, and no
allocation; it is a header of declarations and preprocessor logic only. It explicitly does not
own the *values* of the platform macros at runtime (nothing reads them after preprocessing),
nor the build flags that drive `bs__api__` — those live in `engine/build.bat:114` and
`sandbox/build.bat:17`.

**Public interface:** `engine/source/defines.h` — the scalar typedefs; `TRUE`, `FALSE`,
`VOID_PTR`; `STATIC_ASSERT`; the `BS_PLATFORM_*` detection macros; `bs__api__`. Included by
13 of the 14 other engine subsystems and by 55 sandbox files, making it the highest-fan-in
file in the project.

**Depends on:** nothing.
**Depended on by:** AppLifecycle, DeadStarfield, Diagnostics, EventBus, HierCoords, Input,
MathCore, Memory, Platform, RenderBackend, RenderFrontend, UiFacade, Widgets (13 subsystems),
plus the sandbox directly.

**Key invariants:**
- Integer and float widths are exactly as named — enforced at compile time by ten
  `STATIC_ASSERT`s at `defines.h:32-43`. **`b8` and `b32` are not covered**; `b8` is plain
  `char`, whose signedness is implementation-defined.
- Exactly one platform is detected, or the build fails — enforced by the `#error "Unknown
  platform!"` at `defines.h:81` and `#error "64-bit is required on Windows!"` at `defines.h:53`.
- `bs__api__` must resolve to `dllexport` when building the DLL and `dllimport` when building a
  host — **not enforced in code**. It depends entirely on `BSEXPORT` being passed by
  `engine/build.bat:114` and absent elsewhere.

**Extension points:** Adding a scalar typedef means adding it here plus a matching
`STATIC_ASSERT` at `defines.h:32-43` (the existing ten establish that convention). Adding a
platform means a new `#elif defined(...)` branch in the detection block at `defines.h:50-82`
and a corresponding `BS_PLATFORM_*` macro; note that adding the macro alone does nothing — the
platform backend under `engine/source/platform/` must also be written.

**Known limitations / tech debt:**
- `BSIMPORT` is passed by `sandbox/build.bat:17` but **is never tested anywhere in either
  tree**. The import path is selected by the *absence* of `BSEXPORT`. The flag is inert.
- Two typos disable branches: `__gcc__` in the `STATIC_ASSERT` selector (`defines.h:25`) is not
  a real predefined macro (`__GNUC__` is), and `__gnu_linus__` in the Linux check
  (`defines.h:55`) is misspelled. Under clang the first is harmless because `__clang__` matches.
- `TRUE`, `FALSE` and `VOID_PTR` are unscoped object-like macros that will collide with any
  third-party header using those names. Because `VOID_PTR` is a macro rather than a typedef,
  `const VOID_PTR` binds `const` to the pointer, not the pointee — this actually occurs at
  `core/memory/bs_memory.h:41` and `platform/platform.h:35`, where the `const` does not mean
  what it appears to.
- `real` is a `double` alias while most of the codebase computes in `f32`; the mixed precision
  is live in `core/application.cpp`'s timing path.
- The `bs__api__` block, the most consequential thing in the file, carries only
  `// TODO: Explain this further.` (`defines.h:85`).
- Because every subsystem includes this header, any edit recompiles essentially the whole
  project. Both build scripts glob unconditionally with no dependency tracking.

**Source paths:** `engine/source/defines.h`

**Last verified:** 2026-08-07, commit `812680c`
