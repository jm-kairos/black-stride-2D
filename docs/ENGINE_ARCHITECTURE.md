# Black Stride — Engine Architecture

> Reference map of the Black Stride engine + sandbox as it actually exists on disk.
> Companion to `docs/renderer/SDL3_GPU_INTEGRATION_PLAN.md` (renderer deep-dive).
> Keep this in sync when you change subsystem boundaries, the run loop, the render
> model, or the build pipeline. Line numbers are intentionally omitted — they rot;
> describe *contracts and invariants* instead.

Engine style: a **Kohi-style (Travis Vroman) layered "C-with-structs" engine**, written
in C++ but using C idioms (PODs, free functions, explicit init/shutdown, function
pointers). Built as a **DLL (`engine`)** consumed by a thin **EXE (`sandbox`)**.

Compiler: `clang++` (LLVM 22.x). Platform today: Windows x64 only (SDL3 GPU → D3D12).

---

## 1. Module map

```
engine/source/
  defines.h              Fixed-width typedefs (u8..i64,f32,b8), TRUE/FALSE, platform
                         detection, bs__api__ DLL export/import macro.
  entry.h                main(): the program entry. Calls game_create() (game-supplied),
                         then bs_memory_initialize -> application_init -> application_run.
  game_types.h           struct Game = app_config + 4 fn-ptrs (init/update/render/on_resize)
                         + void* state. The engine<->game contract.
  core/
    application.{h,cpp}   Singleton ApplicationState. Owns subsystem init order + the
                          main loop. THE place that drives everything.
    logger.{h,cpp}        BS_LOG_{FATAL,ERROR,WARN,INFO,DEBUG,TRACE}. Routes to platform
                          console write. FATAL/ERROR gated always; DEBUG/TRACE compile-gated.
    asserts.h             BS_ASSERT / BS_ASSERT_MSG -> debug break.
    event.{h,cpp}         Synchronous pub/sub. event_register/unregister/fire by u16 code.
                          Fixed listener table per code. EVENT_CODE_* enum (QUIT, KEY_*,
                          BUTTON_*, MOUSE_*, WINDOW_RESIZED).
    input.{h,cpp}         Keyboard/mouse state (current + previous frame). keys/buttons
                          enums. input_update() copies current->previous each frame end.
                          Mouse-wheel accumulator drained by input_get_mouse_wheel().
    memory/
      bs_memory.{h,cpp}   Tagged allocator (EMemoryTag), usage tracking, virtual reserve/
                          commit. bs_memory_allocator(size, tag) is the game's allocator.
      arena.{h,cpp}       Linear/bump arena. application_run creates a 64MB frame arena,
                          arena_reset() at the top of every frame.
  containers/
    vector.h / array.h    Thin aliases over std::vector / std::array.
    string.h              #define String std::string.
  math/
    math_utils.{h,cpp}    namespace bs_math. Vec2/Vec3/Mat4 (COLUMN-MAJOR), ortho, rotate,
                          clampf. No SIMD; straightforward scalar.
  platform/
    platform.h            Opaque PlatformState + platform_* contract (window, pump messages,
                          absolute time, console write, alloc).
    platform_sdl3.cpp     The ONLY SDL window/event/time impl. Translates SDL events ->
                          engine events + input calls. Console write uses WriteConsoleA.
    platform_commons.cpp  Cross-platform helpers.
  renderer/
    renderer.h/.cpp       FRONTEND. Public draw API the game calls. Owns immediate-mode
                          helpers (quad/line/rect/circle/grid) built on top of sprites.
    renderer_types.h      Game-facing POD types & handles: bs_color, bs_sprite, bs_texture,
                          bs_rect, bs_frame_stats, Camera2D, EBlendMode. NO SDL types.
    renderer_backend.{h,cpp}  Backend INTERFACE (vtable of fn-ptrs) + factory that picks
                              the SDL GPU backend. Decouples frontend from SDL.
    camera2d.{h,cpp}      Camera2D -> view-proj Mat4; screen<->world helpers.
    backend/
      renderer_backend_sdlgpu.{h,cpp}  The SDL3 GPU implementation. The ONLY file that
                                       includes <SDL3/SDL_gpu.h>. Sprite batch, sort,
                                       vertex upload, render pass, draw-call merging.

sandbox/source/
  entry.cpp     game_create(): fills the Game struct (config + fn-ptrs), allocates state.
  game.{h,cpp}  The prototype: tile-ship, single crew, WASD, zoom-driven local/global mode.
  ship.{h,cpp}  Ship tilemap: parse assets/ship.tmap (ASCII grid) -> TileType array;
                tile<->world coordinate conversions; solidity/structure queries.

assets/
  ship.tmap                 The ship layout (data, NOT hardcoded). ASCII grid + header.
  shaders/src/*.hlsl        sprite/quad vertex+fragment HLSL source.
  shaders/{dxil,spirv}/     compiled blobs (runtime picks per SDL_GetGPUShaderFormats).
tools/
  compile_shaders.sh        Offline HLSL->DXIL+SPIR-V via dxc (Vulkan SDK).
  gen_test_texture.c/.exe   Generates a test texture.
```

---

## 2. Lifecycle & the main loop

`main()` (in `entry.h`, compiled into the sandbox via `#include <entry.h>`) is:

```
main():
  Game game = {};
  game_create(&game);            // sandbox fills config + fn-ptrs + allocates state
  bs_memory_initialize();
  application_init(&game);       // subsystem bring-up (see order below)
  application_run();             // the loop
  // teardown handled at the tail of application_run()
```

### `application_init` bring-up order (do not reorder casually)

```
1. stash game_inst, seed width/height from app_config
2. logger_initialize()
3. input_initialize()
4. event_initialize()  + register QUIT / KEY_PRESSED / KEY_RELEASED / WINDOW_RESIZED
5. platform_initialize()   <- creates the SDL window
6. renderer_initialize()   <- AFTER window exists, BEFORE game init (so the game may
                              create GPU resources, e.g. load textures, during its init)
7. game_inst->init(game_inst)
8. game_inst->on_resize(...) once with the initial size
```

### `application_run` per-frame loop

```
create 64MB frame arena (once)
loop while is_running:
  arena_reset(frame_arena)                 // frame-scratch memory reclaimed
  dt = now - last_time
  platform_pump_messages()                 // -> fires engine events, updates input state
  if not suspended:
     game.update(dt)                        // game logic; FATAL return shuts down
     if renderer_begin_frame(dt):           // may be skipped (minimized) -> don't render
        game.render(dt)
        renderer_end_frame(dt)              // <-- ALL GPU work happens here
     input_update(dt)                       // copy current input -> previous
teardown: unregister events, event_terminate, input_terminate, arena_terminate,
          renderer_shutdown, platform_terminate
```

Key invariants:
- **`game.update` runs every frame; `game.render` only when `renderer_begin_frame` returns
  TRUE.** Minimized window → begin_frame returns FALSE → skip render+end_frame.
- **`input_update` runs at the END of the frame.** "Pressed this frame" = down now & up
  last frame. Read input in `update`, not after.
- Window minimize (size 0×0) sets `is_suspended`, which pauses update+render entirely.

---

## 3. The engine↔game contract (`struct Game`)

```c
struct Game {
    ApplicationConfig app_config;                 // name, start_pos_x/y, start_width/height
    b8   (*init)     (Game*);
    b8   (*update)   (Game*, f32 dt);
    b8   (*render)   (Game*, f32 dt);
    void (*on_resize)(Game*, u32 w, u32 h);
    void* state;                                  // game-owned; opaque to the engine
};
```

The sandbox’s `game_create` (in `entry.cpp`) fills this and allocates `state` via
`bs_memory_allocator(sizeof(game_state), MEMORY_TAG_GAME)`. The engine never dereferences
`state`; the game casts it back in each callback. Returning `FALSE` from `update`/`render`
is fatal and shuts the app down.

---

## 4. Renderer architecture

Three layers, strictly separated so **SDL types never leak to the game**:

```
game  ->  renderer.h (frontend)  ->  renderer_backend vtable  ->  sdlgpu backend
          POD types & handles         fn-ptr interface            <SDL_gpu.h> here only
```

### Frontend (`renderer.cpp`)
Public API (all `bs__api__`-exported):
- Lifecycle: `renderer_initialize/shutdown/on_resize/begin_frame/end_frame`, `set_clear_color`.
- Resources: `renderer_load_texture(path)`, `renderer_destroy_texture`.
- Camera: `renderer_set_camera(Camera2D)`.
- Draw: `renderer_draw_sprite(const bs_sprite*)` plus immediate-mode helpers
  `renderer_draw_quad / draw_line / draw_rect_outline / draw_circle / draw_grid`.
- Stats: `renderer_get_frame_stats()` → `{sprite_count, draw_calls}`.

**Every immediate-mode helper is built on the sprite batch.** A quad is a sprite with the
1×1 white texture; a line is a thin rotated quad; a circle/grid is many quads. So
`sprite_count` counts debug primitives too.

### Backend interface (`renderer_backend.{h,cpp}`)
A struct of function pointers (initialize, shutdown, begin/end frame, create/destroy
texture, set_camera, draw_sprite, get_frame_stats, set_clear_color, on_resize). The
factory binds them to the `sdlgpu_backend_*` functions. Adding a backend = implement the
vtable; the frontend and game never change.

### SDL3 GPU backend (`renderer_backend_sdlgpu.cpp`) — the render model

**Sprite batch, sorted, painter's-algorithm, NO depth buffer.**

1. `draw_sprite` appends a `bs_sprite` to a CPU batch (cap `BS_MAX_SPRITES`) and computes a
   **sort key**: `(layer << 20) | (blend << 18) | tex_index`. No GPU work yet.
2. `end_frame`:
   - **Sort the batch** with `SDL_qsort` by that key. *(Unstable sort — see Pitfalls.)*
   - Build 4 world-space corner vertices per sprite on the CPU (applies origin, rotation;
     bakes tint into per-vertex color). UVs from the sprite's atlas sub-rect.
   - **Copy pass** uploads vertices into the dynamic vertex buffer — done BEFORE any render
     pass (SDL3 GPU forbids copy passes inside a render pass).
   - Acquire swapchain image (may be NULL when minimized → submit empty & bail).
   - Open ONE render pass (clear to clear_color).
   - Rebuild view-proj from the LIVE swapchain size (stays correct across resize).
   - **Walk contiguous runs** sharing the same `(blend, texture)`. Per run: bind pipeline
     for that blend mode, bind texture+sampler, push view-proj uniform, one indexed draw.
     This is the draw-call merging — fewer state changes for same-material runs.
   - Submit. Snapshot `{sprite_count, draw_calls}` into stats.

**Resources:** one shared dynamic vertex buffer + a static index buffer (quad indices),
a nearest-neighbour clamp sampler (crisp pixels), one pipeline per `EBlendMode`
(NONE/ALPHA/ADDITIVE/MULTIPLY), and a 1×1 white texture (id resolves so untextured quads
reuse the sprite pipeline: white × tint = tint). Textures live in a generation-indexed
pool; stale handles resolve to white.

### Coordinate & camera conventions
- **World is y-up**, origin at screen center. At zoom 1.0, 1 world unit = 1 pixel and
  `camera.position` sits at the window center.
- `Camera2D = {Vec2 position, f32 zoom (>0; >1 zooms in), f32 rotation}`.
- `camera2d_view_proj(cam, fb_w, fb_h)` builds an **orthographic** matrix with Vulkan/SDL
  clip space **z ∈ [0,1]**. Mat4 is **column-major** (`data[col*4 + row]`).
- `camera2d_screen_to_world(...)` exists for picking/cursor mapping.

### Shaders (`assets/shaders/src/*.hlsl`)
HLSL compiled offline by `tools/compile_shaders.sh` (dxc) to **both** DXIL (D3D12) and
SPIR-V (Vulkan fallback); runtime picks per `SDL_GetGPUShaderFormats`. SDL3 GPU binding
contract is encoded via register spaces:
- vertex uniform buffer → `register(b0, space1)` (view_proj Mat4, uploaded column-major).
- fragment sampled texture/sampler → `register(t0/s0, space2)`.
Vertex inputs are mapped to `TEXCOORD0..2` = position / uv / color (SDL maps non-system
semantics to TEXCOORD on D3D12). The pipeline vertex layout must match this order.

---

## 5. The game layer (current prototype)

A **2D space sandbox**: a tile-based ship with one crew member, viewed through a
zoom-driven local/global mode switch. Spec lives in the prototype brief; this is the
*as-built*.

### Ship tilemap (`ship.{h,cpp}` + `assets/ship.tmap`)
- Layout is **data, not hardcoded.** `assets/ship.tmap` is an ASCII grid with a small
  header. Parsed by `ship_load(path)` into a flat row-major `TileType[SHIP_MAX_TILES]`.
- Format:
  ```
  # comment lines start with '#' (outside the grid block)
  tile_size 32
  size <cols> <rows>
  grid
  <rows lines, each exactly cols chars>
  end
  ```
- Legend (6 types, `enum TileType`): `.`=empty/space, `#`=hull wall (exterior, solid),
  `W`=interior wall (solid), `F`=floor (walkable), `D`=door (walkable), `G`=hull window /
  glass (`TILE_HULL_WINDOW` — **solid like hull but drawn translucent**; a window, not a
  passage; used in the ship's nose). Unknown glyphs → empty.
- Coordinate model: grid is **centered on the ship origin**; **row 0 is the TOP (+Y)**, so
  world-Y decreases as `row` increases. `ship.origin` is a world offset moved in global
  mode (the whole ship slides; the crew rides along).
- Queries: `ship_tile_at`, `ship_tile_is_solid` (hull|wall|window), `ship_tile_is_structure`
  (anything non-empty = roof footprint), `ship_tile_center_world`, `ship_world_to_tile`,
  `ship_world_size`.

### Modes & camera (`game.cpp`)
- **`MODE_LOCAL`** (zoomed in): draw interior tiles (floor/door on `LAYER_FLOOR`,
  wall/hull on `LAYER_WALL`) + the crew quad (`LAYER_CREW`). WASD moves the crew with
  acceleration + exponential friction + per-axis AABB tile collision. Camera follows crew.
- **`MODE_GLOBAL`** (zoomed out): draw every structure tile as one flat roof silhouette
  (`LAYER_ROOF`), hide the crew. WASD moves `ship.origin` (and the crew with it). Camera
  follows the ship.
- **Mode switch is hysteresis-latched** on zoom to avoid boundary flicker: LOCAL→GLOBAL
  only when `zoom < ZOOM_TO_GLOBAL (0.80)`; GLOBAL→LOCAL only when `zoom > ZOOM_TO_LOCAL
  (1.00)`. Tuning: `ZOOM_MIN 0.35 / MAX 3.00 / START 1.40 / STEP 1.12` (multiplicative per
  wheel notch).
- **Cross-fade:** `roof_alpha` lerps 0↔1 at `ROOF_FADE_SPEED 8.0/s`. Interior is drawn at
  alpha `1-roof_alpha`, roof at `roof_alpha`, and the camera target lerps between crew and
  ship by the same factor → seamless transition.
- Mouse wheel drives zoom via `input_get_mouse_wheel()` (the accumulator added for this).
- No textures this phase: tiles are flat colored quads (`color_for_tile`).

### Render-layer ordering (the game's convention)
`bs_sprite.layer` is the primary sort key; lower draws first (behind). Current values in
`game.cpp`:
```
LAYER_DEBUG = 0   (debug grid; behind everything in the current on-disk build)
LAYER_FLOOR = 1
LAYER_WALL  = 2
LAYER_CREW  = 5
LAYER_ROOF  = 10
```
A `#if BS_DEBUG` grid (`renderer_draw_grid`) is drawn each frame. **See Pitfalls §6 for the
layer-tie flicker class of bug** — two primitives sharing the same `(layer, blend,
texture)` will swap paint order frame-to-frame under the unstable sort.

---

## 6. Pitfalls & non-obvious invariants

These are the traps that cost real debugging time. Read before touching the renderer or
the build.

1. **Unstable sort → flicker on layer ties.** The batch is ordered with `SDL_qsort`, which
   is **not stable**. Two overlapping primitives with an identical `(layer, blend,
   texture)` key have undefined relative order *that can change every frame* → visible
   flicker (e.g. a debug line on `LAYER_FLOOR` fighting the floor quads). **Fix:** give
   overlapping content distinct layers, or add a stable tiebreak (insertion index) to the
   sort key / comparator. There is **no depth buffer** — draw order is the only arbiter.

2. **Assets must be staged into `bin/`.** The game loads data files (e.g.
   `assets/ship.tmap`, shader blobs) **relative to the working directory, which is `bin/`
   at runtime.** `build-all.bat` mirrors the repo-root `assets/` tree into `bin/assets/`
   on every build (`XCOPY assets bin\assets /E /Y /I`). **Editing root `assets/…` without
   rebuilding (or editing only `bin/assets/…`) means your change silently won't take
   effect or will be overwritten.** Always edit the source `assets/` and rebuild.

3. **Windows logging is invisible to stdout.** `platform_console_write` uses
   `WriteConsoleA`, not stdio. Piping `sandbox.exe` to a file/grep captures **nothing**.
   **Verify runtime behavior with a screenshot + vision, not stdout.** A clean run that
   doesn't FATAL-abort implies `ship_load` etc. succeeded (failures abort init).

4. **Engine builds `-Wall -Werror`; sandbox does not.** Any warning in `engine/source/**`
   fails the build. The sandbox is lenient. `engine/build.bat` also defines
   `_CRT_SECURE_NO_WARNINGS`; the sandbox does NOT, so `ship.cpp` defines it itself before
   including headers (it uses `fopen`/`sscanf`). Keep engine edits warning-clean.

5. **SDL3 GPU: copy passes cannot run inside a render pass.** That's why `end_frame` does
   the vertex upload (copy pass) *before* opening the render pass, and why `begin_frame`
   only acquires a command buffer — the swapchain image + render pass are deferred to
   `end_frame`. Don't move the upload.

6. **Frame stats count everything.** `sprite_count` includes debug-layer quads (lines,
   circles, grid are all quads). Don't treat it as "game sprites only."

7. **`input_update` is end-of-frame.** Edge-triggered input ("just pressed") is only valid
   during `update`/`render` of the same frame; after `input_update` the current state
   becomes previous.

8. **Column-major Mat4 + Vulkan z[0,1].** If you add math or a new shader uniform, match
   `data[col*4+row]` storage and the `[0,1]` depth convention (`mat4_ortho`), or geometry
   vanishes / inverts.

9. **No nested CALL-without-restore in build scripts.** `build-all.bat` uses
   `PUSHD/CALL/POPD` and checks `ERRORLEVEL` after each. The sandbox build copies
   `SDL3.dll` next to the exe. If you add a new engine dependency DLL, stage it too.

---

## 7. Build & run

```bash
# From repo root (Windows, clang++ on PATH):
./build-all.bat            # engine.dll -> sandbox.exe -> stage assets into bin/

# Shaders (only when HLSL changes; requires dxc from the Vulkan SDK):
bash tools/compile_shaders.sh

# Run (working dir MUST be bin/ so relative asset paths resolve):
cd bin && ./sandbox.exe
```

- `engine/build.bat`: globs `engine/source/**/*.cpp` → `clang++ -g -shared -Wall -Werror`
  with `-DBS_DEBUG -DBSEXPORT -D_CRT_SECURE_NO_WARNINGS` → `bin/engine.dll` (+ import lib).
- `sandbox/build.bat`: globs `sandbox/source/**/*.cpp` → links `-lengine.lib` →
  `bin/sandbox.exe`, then copies `SDL3.dll` into `bin/`.
- `build-all.bat`: runs both, aborts on either `ERRORLEVEL`, then stages `assets/` → `bin/`.

**Verification loop** (because of pitfall §3): build → run `sandbox.exe` (background) →
focus the window titled "Black Stride Engine Sandbox" → screenshot → inspect with vision →
kill. Captured as the `blackstride-build-verify` skill.

---

## 8. Out of scope (future phases)

Combat, AI, multiple crew, RPG/progression, space travel & navigation, resources,
crafting, trading, diplomacy, fleet management, save/load, a proper room system,
on-screen text/UI, and exterior hull/roof *art* (roof is a flat silhouette for now). The
prototype deliberately covers only: tile ship, one crew, WASD, zoom, local↔global switch,
interior render, roof render.
```
