# BlackStride2D — Copilot Instructions

2D space game written in C-style C++ for Windows, built with clang++ (LLVM) from PATH using plain batch scripts — **no CMake for the main build**.

## Build, run, and validate

There is no test suite or linter. Validation = a clean build (the engine compiles with `-Wall -Werror`, so warnings in engine code are build failures).

**One-time setup after cloning:**

```bat
git submodule update --init            :: fetches engine/vendor/freetype
engine\build_freetype.bat              :: builds vendor\freetype\lib\freetype.lib (cmake + ninja + clang)
```

**Regular builds:**

| Command | Run from | What it does |
|---|---|---|
| `build-all.bat` | repo root | engine → sandbox → incremental shader compile → stages `assets\` into `bin\assets\` |
| `engine\build.bat` | `engine\` | engine.dll only |
| `sandbox\build.bat` | `sandbox\` | sandbox.exe only (links against `bin\engine.lib`) |

Matching VS Code tasks exist ("Build Everything" is the default build task).

- Run the game as `bin\sandbox.exe` with working directory `bin\` — data files are loaded relative to cwd.
- Vendored ImGui/RmlUi objects are cached under `engine\obj\imgui` and `engine\obj\rmlui` with relaxed warnings; delete those folders to force a clean vendor rebuild. Vendored code must never pass through the engine's `-Wall -Werror` glob.
- Both engine and sandbox build with `-Wall -Werror`.

### Shaders

- Sources live in `assets\shaders\src\<name>.<stage>.hlsl` where `<stage>` is `vert`, `frag`, or `comp`.
- `build-all.bat` incrementally compiles each source with dxc (from PATH or `%VULKAN_SDK%\Bin`) into **both** `assets\shaders\dxil\` (D3D12) and `assets\shaders\spirv\` (Vulkan).
- Never edit anything under `bin\assets\` — it is overwritten by the staging step on every build. Edit the repo-root `assets\` tree.

## Architecture

Two-project split, Kohi-engine style:

- **`engine\` → `bin\engine.dll`** — SDL3 platform layer (`platform\platform_sdl3.cpp`), SDL_GPU renderer with D3D12/Vulkan backends (`renderer\backend\renderer_backend_sdlgpu.cpp`), application loop, event/input systems, logger, arena allocator (`core\memory\`), custom containers, math (`math\`). Vendored Dear ImGui (debug UI) and RmlUi + FreeType (game UI facade in `renderer\bs_rml.h` / `bs_ui.h`).
- **`sandbox\` → `bin\sandbox.exe`** — the actual game.

**Inverted entry point:** the engine's `entry.h` defines `main()` and expects the game to supply `game_create(Game*)`, filling the `Game` struct's function pointers (`init`, `update`, `render`, `on_resize`) plus a `state` void pointer (see `engine\source\game_types.h`, `sandbox\source\entry.cpp`).

**Sandbox layout:** `sim\` (gameplay systems: galaxy gen, ship AI, combat, markets, sensors), `render\` (ordered draw passes, orchestrated by `render\scene_renderer.h`), `core\` (coordinates, view transforms, geometry), `ui\` (editor/setup panels). `game.cpp` is the frame orchestrator.

**Module include discipline (enforced by convention):**
- Only `game.cpp` may include the `game_modules.h` aggregate.
- Every other module .cpp includes the *specific* peer headers it calls and gets the `game_state` definition via `game.h`. Do not add new includes to `game_modules.h` unless `game.cpp` itself needs to call the module.
- The `game_state` god-struct and `game_*` entry-point declarations live in `state\game_state.h`; `game.h` is a thin shim kept for existing includes.

## Key conventions

- **Types:** use the `defines.h` typedefs — `u8/u16/u32/u64`, `i8..i64`, `f32/f64`, booleans `b8`/`b32` with `TRUE`/`FALSE`. Engine API functions are exported with `bs__api__` (driven by `BSEXPORT` in the engine build, `BSIMPORT` in the sandbox build).
- **Logging:** `BS_LOG_FATAL/ERROR/WARN/INFO/DEBUG/TRACE` macros from `core\logger.h`.
- **Style:** procedural C-style C++ (structs + free functions, function pointers, arenas); avoid introducing STL-heavy or exception-based patterns.
- **HierPos2 coordinates:** world positions far from the origin use `bs_math::HierPos2` (an `i64` grid cell of 16384 units + a small float local offset) instead of raw floats. Author positions with `hierpos_from_vec2`, measure/render with `hierpos_diff` (precision-safe relative vector), and only collapse to `Vec2` near the camera/origin. Read `docs\HIERPOS2_AND_ENTITY_COOKBOOK.md` before touching positioning, movement, or spawning code.
- **Design docs:** `docs\` holds the authoritative design notes (galaxy generation architecture, planetary evolution model, trade economy, per-system ship AI, map generation). Check there before reworking those systems. Read `docs\PLANETARY_EVOLUTION_MODEL.md` before touching `sim\system_evolution.*` or any consumer of `StarSystem.evo`.
