# Dear ImGui + SDL3 GPU Engine-Wide Integration Plan

> **For Hermes:** Use the `subagent-driven-development` skill to implement this plan task-by-task.

**Goal:** Stand up Dear ImGui on top of the existing SDL3 + SDL3-GPU backend, wired into the event pump, the GPU frame lifecycle, and the application loop — then migrate the sandbox's bespoke immediate-mode `ui`/`text` HUD (the Crew Job Panel) onto ImGui.

**Architecture:** ImGui's two backends (`imgui_impl_sdl3` + `imgui_impl_sdlgpu3`) need both SDL and the GPU device, so per the engine's seam rule ("only the backend `.cpp` includes SDL / touches the GPU") they live **inside the engine DLL**, fronted by a thin SDL-free `bs_imgui_*` facade in a new public header. The event hook is the one exception that must reach into `platform_sdl3.cpp`. ImGui draw data is recorded inside the existing `end_frame` render pass, after the sprite batch, before `SDL_EndGPURenderPass`.

**Tech Stack:** C++ (clang++, `-Wall -Werror -Wvarargs`), SDL 3.4.4 GPU API (DXIL/SPIRV), Dear ImGui (docking or master branch), engine DLL + sandbox EXE, `.bat` build scripts (no CMake).

---

## Current state (verified against the codebase)

| Fact | Location | Implication |
|------|----------|-------------|
| Seam rule: SDL only in backend TU | `renderer_backend_sdlgpu.cpp:25` is the only `#include <SDL3/...>` | ImGui SDL/GPU backends must compile inside this TU (or a sibling backend TU), never in public headers. |
| Engine is a DLL | `defines.h:87-101`, `bs__api__` = dllexport/dllimport | ImGui lives in `engine.dll`; the facade is exported with `bs__api__`. Sandbox never sees `imgui.h`. |
| Device + window claim | `sdlgpu_backend_initialize` `:480` `SDL_CreateGPUDevice`, `:484` `SDL_ClaimWindowForGPUDevice` | ImGui init goes right after the claim (needs device + window + swapchain format). |
| Shader format chosen at runtime | `load_shader` `:120` `SDL_GetGPUShaderFormats` → DXIL on this machine | **ImGui's SDLGPU3 backend ships embedded shaders for every format — no shader build step needed.** Resolves the open shader-toolchain question. |
| `begin_frame` only acquires cmd buffer | `:580`, no render pass | ImGui `NewFrame` can run here (or in the loop). Swapchain + pass are deferred to `end_frame`. |
| Sprite copy-pass upload | `end_frame` `:762-769` (before any render pass) | ImGui `Prepare`/upload obeys the same "copy pass before render pass" rule — slot it adjacent. |
| Render pass open→close | `end_frame` `:797` `SDL_BeginGPURenderPass` … `:862` `SDL_EndGPURenderPass`, submit `:865` | ImGui `RenderDrawData` records **after** sprite runs (`:856`), **before** `:862`. |
| Minimized window path | `end_frame` `:782-788` submits with NULL swapchain, returns early | ImGui draw recording must be **skipped** on this path (no render pass exists). |
| Shutdown | `sdlgpu_backend_shutdown` `:528` `SDL_WaitForGPUIdle` then releases | ImGui shutdown hooks here, after idle, before device destroy. |
| Loop order | `application.cpp:136` `update(dt)` → `:145` `renderer_begin_frame` → `:147` `render(dt)` → `:154` `renderer_end_frame` | Game builds UI in **update** (`game.cpp:608`), before `begin_frame`. This is the NewFrame-placement hazard (see Task 6). |
| Event pump | `platform_sdl3.cpp` `SDL_PollEvent` loop | `ImGui_ImplSDL3_ProcessEvent` hooks here; gate engine input on `WantCaptureMouse/Keyboard`. |
| Build globs all cpp | `engine/build.bat:7` `FOR /R %%f in (*.cpp)` under `-Wall -Werror -Wvarargs` | ImGui sources would be swept into the strict-warning build — **the #1 build hazard** (Task 1). |
| Existing HUD | `game.cpp:608` `build_crew_job_panel` + `:615` `ui_update` in **update**; `:790` `ui_draw` in **render** | Migration target. `ui_wants_mouse()` (gates world clicks at `game.cpp:239,254`) maps to ImGui `WantCaptureMouse`. |

**SDL version:** 3.4.4 (`SDL_version.h` MAJOR=3 MINOR=4 MICRO=4) — above ImGui's 3.2.0 floor for the SDLGPU3 backend. Good.

---

## Proposed approach

**Where ImGui lives.** ImGui core (`imgui*.cpp`) + `imgui_impl_sdl3.cpp` + `imgui_impl_sdlgpu3.cpp` are vendored under `engine/vendor/imgui/` and compiled **into the engine DLL**. A new SDL-free facade `engine/source/renderer/bs_imgui.h` (declarations only, `bs__api__`) exposes exactly what the rest of the engine + sandbox need:

```c
// bs_imgui.h — NO SDL, NO imgui.h. Safe for public/game consumption.
bs__api__ b8   bs_imgui_initialize(struct PlatformState* plat);  // after device claim
bs__api__ void bs_imgui_shutdown();
bs__api__ void bs_imgui_new_frame();        // begins an ImGui frame
bs__api__ void bs_imgui_render();           // ends the ImGui frame, records draw data into the live pass
bs__api__ b8   bs_imgui_process_event(void* sdl_event);  // returns TRUE if ImGui consumed it
bs__api__ b8   bs_imgui_wants_mouse();      // maps old ui_wants_mouse()
bs__api__ b8   bs_imgui_wants_keyboard();
```

The **implementation** of this facade is a new TU `engine/source/renderer/backend/bs_imgui_sdlgpu.cpp` — it is allowed to `#include <SDL3/...>` and `imgui*.h` because it is a backend TU (same seam class as `renderer_backend_sdlgpu.cpp`). It reaches the device/cmd/pass through a small accessor the GPU backend already owns (see Task 3).

**Why a facade and not direct ImGui calls in game code:** the sandbox builds with `-DBSIMPORT` and only `-I../engine/source/`; it must never see `imgui.h` or SDL. Game UI code calls `bs_imgui_*` (engine-exported) and — for actual widgets — a second thin wrapper (Task 8) so `imgui.h` stays inside the DLL. (Alternative considered: expose `imgui.h` to the sandbox. Rejected: breaks the seam, forces ImGui's headers + `IMGUI_API` dllexport across the ABI, and contradicts the established `bs_*` handle pattern.)

**Coexistence, then migration.** Phases 1-4 stand ImGui up *alongside* the existing `ui`/`text` HUD (both render; ImGui draws a demo window to prove the pipeline). Phase 5 migrates the Crew Job Panel. Phase 6 retires the now-dead `ui.cpp` immediate-mode widget code (keep `text.cpp` if anything still needs the 8x8 bitmap font; otherwise retire too).

### Key decisions (and rejected alternatives)

1. **ImGui in the DLL, facade out.** Keeps the seam intact and the DLL ABI SDL/ImGui-free. *Rejected:* ImGui in the sandbox EXE (would need the device handle to cross the boundary — worse seam break).
2. **No shader toolchain work.** `imgui_impl_sdlgpu3` provides embedded DXIL + SPIRV + MSL shaders and picks the format from the device, exactly like `load_shader`. The existing "adopt SDL_shadercross" open question **does not block ImGui**.
3. **Compile ImGui in its own build step / object list**, NOT swept by the strict `-Wall -Werror` glob. ImGui does not compile clean under `-Werror -Wvarargs`. Options in Task 1; chosen: a separate `clang++` invocation without `-Werror` producing a static `.lib`/objects linked into the DLL.
4. **Single shared frame.** ImGui's frame brackets the existing one: `NewFrame` after `renderer_begin_frame` succeeds, `Render`-record inside `end_frame`'s pass. The game's UI-build-in-`update` ordering forces an explicit decision (Task 6) — we move widget building into `render`, OR start the ImGui frame at the top of `update`. Plan picks **build-in-render** to keep ImGui's NewFrame/Render strictly inside the renderer's accepted-frame gate.
5. **`docking` branch** of ImGui for multi-viewport/dockspace headroom (tools, debug panels). If undesired, `master` works unchanged; the impl TU guards viewport calls behind `IMGUI_HAS_VIEWPORT`.

---

## Phase 0 — Vendoring & build (no engine code yet)

### Task 1: Vendor ImGui sources

**Objective:** Drop ImGui + its two backends into the tree.

**Files:**
- Create dir: `engine/vendor/imgui/` with, from ImGui `docking` branch:
  - `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_demo.cpp`
  - `imgui.h`, `imgui_internal.h`, `imstb_*.h`, `imconfig.h`
  - `backends/imgui_impl_sdl3.cpp` + `.h`
  - `backends/imgui_impl_sdlgpu3.cpp` + `.h`

**Steps:**
1. `git clone --branch docking --depth 1 https://github.com/ocornut/imgui` to a temp dir.
2. Copy the files above into `engine/vendor/imgui/` (flatten `backends/` into the same dir or keep the subdir — pick one and use it consistently in the include path).
3. (Optional) In `engine/vendor/imgui/imconfig.h` add `#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS` and, if needed for the DLL, leave `IMGUI_API` undefined (we link ImGui statically into the DLL, so no per-symbol export needed).

**Verify:** `search_files("ImGui_ImplSDLGPU3_Init", path="engine/vendor/imgui")` returns a hit in `imgui_impl_sdlgpu3.cpp`.

---

### Task 2: Compile ImGui without breaking the strict engine build

**Objective:** Build ImGui's TUs without `-Werror -Wvarargs` (it won't pass them) and link the objects into `engine.dll`, while keeping the engine's own `-Wall -Werror` glob intact.

**Problem:** `engine/build.bat:7` does `FOR /R %%f in (*.cpp)` rooted at the engine dir. If ImGui lands under `engine/vendor/imgui/`, that glob will sweep ImGui's `.cpp` into the `-Wall -Werror -Wvarargs` compile and fail.

**Chosen fix — separate ImGui compile, then link objects:**

Modify `engine/build.bat` to (a) exclude the vendored ImGui dir from the strict glob, (b) compile ImGui separately with relaxed flags, (c) add the resulting objects to the link.

```bat
REM --- Build vendored ImGui ONCE into objects (relaxed warnings) ---
SET imguiDir=vendor\imgui
SET imguiObjs=
IF NOT EXIST ..\bin\imgui_obj MKDIR ..\bin\imgui_obj
FOR %%f in (%imguiDir%\imgui.cpp %imguiDir%\imgui_draw.cpp %imguiDir%\imgui_tables.cpp %imguiDir%\imgui_widgets.cpp %imguiDir%\imgui_impl_sdl3.cpp %imguiDir%\imgui_impl_sdlgpu3.cpp) do (
    clang++ -g -c %%f -o ..\bin\imgui_obj\%%~nf.o -I%imguiDir% -Ivendor/include -D_CRT_SECURE_NO_WARNINGS
    SET imguiObjs=!imguiObjs! ..\bin\imgui_obj\%%~nf.o
)

REM --- Engine glob must SKIP the vendor/imgui dir ---
SET cppFilenames=
FOR /R %%f in (*.cpp) do (
    echo %%f | findstr /I /C:"vendor\imgui" >nul || SET cppFilenames=!cppFilenames! %%f
)
```

Then add `%imguiObjs%` and `-Ivendor/imgui` to the final `clang++` link line:

```bat
SET includeFlags=-Isource -Ivendor/include -Ivendor/imgui
clang++ %cppFilenames% %imguiObjs% %compilerFlags% -o ../bin/%assembly%.dll %defines% %includeFlags% %linkerFlags%
```

**Why objects, not a separate glob entry:** ImGui must compile *once* with relaxed flags; the engine TUs that `#include "imgui.h"` (only `bs_imgui_sdlgpu.cpp`) still build under the strict engine flags but only include ImGui *headers*, which are warning-clean enough (add `-Wno-...` locally only if a specific warning fires).

**Pitfall (`-Wvarargs`):** the engine's `BS_LOG_*` macros already interact badly with this flag elsewhere; ImGui's `IM_FMTARGS`/va usage is exactly why ImGui must NOT be in the `-Wvarargs` glob.

**Pitfall (double-semicolon `BS_LOG_*`):** per engine convention the `BS_LOG_INFO/WARN/...` macros expand *with* a trailing `;`. Any new engine-side code in this plan that uses them inside a braceless `if/else` must brace both branches, or the orphaned `else` won't compile.

**Verify:** `cd engine && build.bat` produces `../bin/engine.dll` with zero new warnings from engine TUs and ImGui objects present in `bin/imgui_obj/`.

---

## Phase 1 — Stand ImGui up (coexists with the old HUD)

### Task 3: Expose device/cmd/pass from the GPU backend to the ImGui TU

**Objective:** Give `bs_imgui_sdlgpu.cpp` access to the live `SDL_GPUDevice*`, swapchain format, current `SDL_GPUCommandBuffer*`, and current `SDL_GPURenderPass*` without leaking them outside the backend.

**Files:**
- Modify: `engine/source/renderer/backend/renderer_backend_sdlgpu.h` — add backend-internal accessors.
- Modify: `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp` — implement them against `g_sdl`.

**Add (backend-internal header, included only by the two backend TUs):**

```c
// These return SDL/GPU pointers as void* so the header stays include-light; the ImGui TU
// casts them back. Only backend TUs include this header.
void* sdlgpu_get_device();              // SDL_GPUDevice*
u32   sdlgpu_get_swapchain_format();    // SDL_GPUTextureFormat
void* sdlgpu_get_current_cmd();         // SDL_GPUCommandBuffer* (valid begin->end frame)
void* sdlgpu_get_current_pass();        // SDL_GPURenderPass* (valid only inside end_frame pass)
```

**Implement in the cpp** (return `g_sdl.device`, `SDL_GetGPUSwapchainTextureFormat(g_sdl.device, g_sdl.window)`, `g_sdl.cmd`, `g_sdl.pass`).

**Verify:** engine builds; accessors compile.

---

### Task 4: Implement the ImGui facade + init/shutdown

**Objective:** Create `bs_imgui.h` (SDL-free, exported) and `bs_imgui_sdlgpu.cpp` (backend TU) with init and shutdown working.

**Files:**
- Create: `engine/source/renderer/bs_imgui.h` (the facade from "Proposed approach").
- Create: `engine/source/renderer/backend/bs_imgui_sdlgpu.cpp`.

**`bs_imgui_initialize` body (in the cpp):**

```cpp
#include "renderer/bs_imgui.h"
#include "renderer/backend/renderer_backend_sdlgpu.h"
#include "platform/platform.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"
#include <SDL3/SDL_gpu.h>

b8 bs_imgui_initialize(struct PlatformState* plat)
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    SDL_Window* win = (SDL_Window*)platform_get_window_handle(plat);
    if (!ImGui_ImplSDL3_InitForSDLGPU(win)) { return FALSE; }

    ImGui_ImplSDLGPU3_InitInfo info = {};
    info.Device             = (SDL_GPUDevice*)sdlgpu_get_device();
    info.ColorTargetFormat  = (SDL_GPUTextureFormat)sdlgpu_get_swapchain_format();
    info.MSAASamples        = SDL_GPU_SAMPLECOUNT_1;
    if (!ImGui_ImplSDLGPU3_Init(&info)) { return FALSE; }
    return TRUE;
}
```

**`bs_imgui_shutdown`:** `ImGui_ImplSDLGPU3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext();`

**Wire into the GPU backend lifecycle** (so init happens after the device+window+format exist, and shutdown before device destroy):
- In `sdlgpu_backend_initialize` after `create_white_texture()` (`:517`), the renderer frontend should call `bs_imgui_initialize(plat)`. **Decision:** call it from `renderer.cpp` right after `renderer_backend.initialize(...)` succeeds, passing the `PlatformState*` the frontend already holds — keeps ImGui init out of the GPU device TU but still after device bring-up. (The font atlas is uploaded lazily by `imgui_impl_sdlgpu3` on first `NewFrame`/`Render`, so no manual atlas upload like the old plan's texture path is needed.)
- In `sdlgpu_backend_shutdown` path: call `bs_imgui_shutdown()` from `renderer.cpp` *before* `renderer_backend.shutdown(...)` (which calls `SDL_WaitForGPUIdle` then destroys the device). ImGui shutdown must precede `SDL_DestroyGPUDevice`.

**Pitfall:** `ImGui_ImplSDLGPU3_Init` needs the swapchain color format. `SDL_GetGPUSwapchainTextureFormat` is valid only **after** `SDL_ClaimWindowForGPUDevice` (`:484`) — which is why init is sequenced after backend initialize returns.

**Verify:** engine builds and links (ImGui symbols resolve from the objects in Task 2); sandbox still runs unchanged (ImGui initialized but drawing nothing yet).

---

### Task 5: Hook the event pump

**Objective:** Feed SDL events to ImGui and let ImGui claim mouse/keyboard.

**Files:**
- Modify: `engine/source/platform/platform_sdl3.cpp` — in the `SDL_PollEvent` loop, call the facade before the engine's own handling.

**Steps:**
1. At the top of the event-processing switch (right after `SDL_PollEvent(&event)` fills `event`), add:
   ```cpp
   bs_imgui_process_event(&event);   // facade -> ImGui_ImplSDL3_ProcessEvent
   ```
   `platform_sdl3.cpp` includes `renderer/bs_imgui.h` (SDL-free header — no seam violation; the *implementation* in the backend TU does the SDL cast).
2. The facade impl: `b8 bs_imgui_process_event(void* e){ return ImGui_ImplSDL3_ProcessEvent((SDL_Event*)e) ? TRUE : FALSE; }`

**Input arbitration (replaces `ui_wants_mouse`):** Engine input should not act on mouse/keyboard ImGui owns. Two layers:
- **World picking** in `game.cpp` (currently gated by `ui_wants_mouse()` at `:239,:254`) switches to `bs_imgui_wants_mouse()`.
- Optionally, in `platform_sdl3.cpp`, skip forwarding mouse-button/motion to the engine `input_*` system when `ImGui::GetIO().WantCaptureMouse` — but prefer doing it at the game-picking layer to avoid starving the engine input state ImGui itself may need. **Decision:** gate at the game layer only (minimal, matches current design).

**Verify:** with an ImGui demo window up (Task 6), clicking the window does not also issue a crew move order.

---

### Task 6: Frame lifecycle — NewFrame and draw recording

**Objective:** Bracket each frame with ImGui `NewFrame` … record draw data inside the GPU render pass.

**This is the highest-risk task** — get the ordering exactly right.

**Files:**
- Modify: `engine/source/renderer/renderer.cpp` — call `bs_imgui_new_frame()` inside `renderer_begin_frame` (after the backend accepts the frame), and trigger draw recording from `renderer_end_frame`.
- Modify: `engine/source/renderer/backend/renderer_backend_sdlgpu.cpp` — in `sdlgpu_backend_end_frame`, call into the ImGui facade at the right point inside the pass.

**The ordering constraint (from the verified code):**

```
renderer_begin_frame(dt):                      [renderer.cpp]
  backend.begin_frame()  -> acquires cmd        (sdlgpu :580)
  if accepted: bs_imgui_new_frame()             <-- NEW: ImGui_ImplSDLGPU3_NewFrame + ImGui_ImplSDL3_NewFrame + ImGui::NewFrame

game.render(dt):                                [game.cpp]
  ... draw_sprite calls ...
  ... ImGui widget calls (Task 8) ...           <-- build UI here (see decision below)

renderer_end_frame(dt) -> backend.end_frame:    [sdlgpu :707]
  sprite copy-pass upload          (:762-769)
  bs_imgui_prepare()               <-- NEW: ImGui::Render(); ImGui_ImplSDLGPU3_PrepareDrawData(draw_data, cmd)  -- BEFORE the render pass (it records a copy pass internally)
  acquire swapchain  (:773)  -- if NULL -> early-out, but ImGui::Render already called; see pitfall
  SDL_BeginGPURenderPass  (:797)
    ... sprite runs ...  (:800-856)
    bs_imgui_record(pass)          <-- NEW: ImGui_ImplSDLGPU3_RenderDrawData(draw_data, cmd, pass)  -- AFTER sprites, BEFORE end pass
  SDL_EndGPURenderPass    (:862)
  SDL_SubmitGPUCommandBuffer (:865)
```

**Split the facade `bs_imgui_render()` into two backend-internal calls** so they straddle the pass boundary:
- `bs_imgui_prepare()` → `ImGui::Render(); ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), (SDL_GPUCommandBuffer*)sdlgpu_get_current_cmd());` — called just before swapchain acquire (it does a copy pass; must be outside the render pass, exactly like the sprite upload).
- `bs_imgui_record()` → `ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);` — called inside the pass after sprite draws.

**Critical pitfall — NewFrame/Render must always balance.** If `begin_frame` calls `NewFrame` but the swapchain comes back NULL at `:782` (minimized), the code early-outs and submits without a render pass. You must still call `ImGui::Render()` (or `ImGui::EndFrame()`) to balance the `NewFrame`, otherwise the next frame's `NewFrame` asserts. **Fix:** in the minimized early-out branch (`:782-788`), call `bs_imgui_prepare()` (which calls `ImGui::Render()`) but **skip** `bs_imgui_record()` (no pass to record into). Simplest robust handling: always call `ImGui::Render()` once per `NewFrame`, even when discarding the draw data.

**Decision — where the game builds ImGui widgets.** The old UI is built in `game_update` (`:608`) because `ui_wants_mouse()` had to be set before world picking later in the *same* update. With ImGui, `WantCaptureMouse` is updated by `ImGui_ImplSDL3_NewFrame` from the *previous* frame's hover state, so the game can build widgets in `game_render` and still read `bs_imgui_wants_mouse()` correctly in `game_update` (1-frame-latent, standard ImGui behavior, imperceptible). **Therefore:** move Crew Job Panel building into `game_render`, keep the `bs_imgui_wants_mouse()` check in `game_update` for world-pick gating. This keeps every ImGui call inside the renderer's accepted-frame window (`NewFrame` in `begin_frame`, widgets in `render`, `Render` in `end_frame`).

> If a future requirement needs zero-latency mouse capture, the alternative is to start the ImGui frame at the very top of `game_update` instead of in `begin_frame`. Documented, not chosen.

**Verify:** add `ImGui::ShowDemoWindow()` in `game_render` temporarily; the demo window renders over the sprites, is interactive, and survives window resize + minimize/restore without asserting.

---

## Phase 2 — Migrate the Crew Job Panel HUD

### Task 7: Decide the game→ImGui widget surface

**Objective:** Let `game.cpp` build panels without seeing `imgui.h`.

**Two options:**
- **(A) Thin `bs_ui_*` wrapper** in the engine that forwards to ImGui (`bs_ui_begin_panel`, `bs_ui_label`, `bs_ui_button`, `bs_ui_progress`, …). Keeps the sandbox fully SDL/ImGui-free, mirrors the current `ui_*` API so `game.cpp` changes are mechanical. **Recommended** — smallest blast radius, preserves the seam.
- **(B) Expose `imgui.h` to the sandbox** and call ImGui directly. Most expressive, but the sandbox would link ImGui and include its headers — a seam break and ABI complication across the DLL. **Rejected** for the engine boundary; revisit only if the game needs deep ImGui features.

**Chosen: (A).** Define `engine/source/renderer/bs_ui.h` (SDL/ImGui-free, exported) with just the widgets the Crew Job Panel needs, implemented in `bs_imgui_sdlgpu.cpp` (or a sibling `bs_ui_imgui.cpp` backend TU). Map the existing `UiAction` returns through it.

**Minimum wrapper surface (driven by the current panel at `game.cpp:459-593`):**
```c
bs__api__ void bs_ui_begin_panel(const char* title, f32 x, f32 y, f32 w, f32 h);
bs__api__ void bs_ui_end_panel();
bs__api__ void bs_ui_label(const char* text);
bs__api__ b8   bs_ui_button(const char* label);     // returns TRUE on click
bs__api__ void bs_ui_progress(f32 fraction, const char* overlay);
bs__api__ void bs_ui_same_line();
bs__api__ void bs_ui_separator();
```

**Verify:** wrapper compiles; a hand-built test panel in `game_render` shows a titled window with a label, a button that logs on click, and a progress bar.

---

### Task 8: Port the Crew Job Panel

**Objective:** Reproduce `build_crew_job_panel` (`game.cpp:459-593`) on the `bs_ui_*` wrapper, preserving every `UiAction` (`UI_ACTION_REORDER_UP/DOWN`, `UI_ACTION_REMOVE_JOB`, `UI_ACTION_ASSIGN_PILOTING`, `UI_ACTION_CANCEL_CURRENT`).

**Files:**
- Modify: `sandbox/source/game.cpp` — rewrite `build_crew_job_panel` to call `bs_ui_*`; move the call from `game_update` (`:608`) into `game_render` (per Task 6 decision). Keep `apply_crew_job_action(s, fired, param)` — wire each `bs_ui_button` return to the matching action instead of the `ui_update` return.
- Modify: `sandbox/source/game.cpp` — world-pick gate at `:239,:254`: replace `ui_wants_mouse()` with `bs_imgui_wants_mouse()`.

**Mapping (current → ImGui):**
| Current (`ui.cpp`) | ImGui wrapper |
|---|---|
| `ui_begin` + `ui_panel` | `bs_ui_begin_panel("Crew Jobs", x,y,w,h)` |
| `ui_label(fmt…)` | `bs_ui_label(buf)` (game formats into a buffer first) |
| `ui_button` returning `UiAction` | `if (bs_ui_button("^")) apply_crew_job_action(s, UI_ACTION_REORDER_UP, i);` |
| `ui_progress(frac)` | `bs_ui_progress(frac, overlay)` |
| `ui_wants_mouse()` | `bs_imgui_wants_mouse()` |
| `ui_draw` (`game.cpp:790`) | *(deleted — ImGui records its own draw data in `end_frame`)* |

**Per-job-row controls:** loop the crew's `queue[0..job_count)`; for each, `bs_ui_label(job summary)`, `bs_ui_same_line()`, `bs_ui_button("^")`/`"v"`/`"X"` → reorder/remove with the row index as `param`. Use `ImGui::PushID(i)`/`PopID` inside the wrapper's button (add a `bs_ui_push_id(i)`/`bs_ui_pop_id()` pair) so repeated `"^"` labels don't collide.

**Pitfall (ID collisions):** identical button labels across job rows need unique ImGui IDs — hence the push/pop-id pair. Without it only the first row's buttons work.

**Verify:** select a crew member → the panel appears; reorder/remove/assign/cancel all fire the correct `apply_crew_job_action`; clicking the panel does not move the crew; deselecting hides the panel.

---

### Task 9: Retire the dead custom-UI code

**Status: ✅ COMPLETE & VERIFIED** (2026-06-06) — `UiAction` relocated into `job.h` (dead `UI_ACTION_TEST` dropped; 6 live enumerators kept, all used by `apply_crew_job_action`); `game.h` lost both `#include "ui.h"` and the `UiContext ui;` field; `ui.cpp`/`ui.h` deleted (sandbox.exe shrank 646,656→640,512 bytes); `sandbox/build.bat` `FOR /R *.cpp` glob auto-dropped the file (no edit). `text.*` KEPT — `text_draw` still live at `game.cpp:767`. Full `build-all.bat` GREEN; zero residual `ui_*`/`UiContext`/`UiWidget`/`LAYER_UI` refs in `sandbox/source`. Runtime re-verified: panel parity (j01 `Job: Idle`/`Queue (0)`/Assign+Cancel; j02 `Job: Piloting`/`Moving 33%`/`Target: Helm (6,2)`/A* path) AND global-mode bitmap HUD `HELM: UNMANNED - FLIGHT LOCKED` (h00) — proving the retained `text.cpp` layer renders post-deletion.

**Objective:** Remove the now-unused immediate-mode widget layer.

**Files:**
- Delete or empty: `sandbox/source/ui.cpp`, `sandbox/source/ui.h` (the `ui_*` widget API + `UiContext`). Keep `UiAction` enum if `apply_crew_job_action` still switches on it — move it to a small header (e.g. into `job.h`) so the panel code keeps the enum without the widget engine.
- Modify: `sandbox/source/game.h` — drop `#include "ui.h"` (`:9`) and the `UiContext ui;` field (`:110`) from `game_state` if fully replaced.
- Decide on `text.cpp`/`text.h`: if the 8x8 bitmap HUD text (`text_draw`) is no longer used anywhere after the panel migrates to ImGui fonts, retire it too; otherwise keep for any non-ImGui debug text. Search first: `search_files("text_draw|text_init", path="sandbox/source")`.

**Pitfall:** `game.h:110` `UiContext ui;` is embedded in `game_state`; removing it changes the struct. Rebuild the sandbox fully (no incremental stale objects).

**Verify:** full `build-all.bat` clean; no references to `ui_begin`/`ui_draw`/`ui_update`/`UiContext` remain (`search_files` returns zero in `sandbox/source` except possibly the retained `UiAction`).

---

## Files likely to change (rollup)

**New:**
- `engine/vendor/imgui/**` — vendored ImGui + 2 backends
- `engine/source/renderer/bs_imgui.h` — SDL-free facade (exported)
- `engine/source/renderer/bs_ui.h` — SDL-free widget wrapper (exported)
- `engine/source/renderer/backend/bs_imgui_sdlgpu.cpp` — facade + wrapper impl (backend TU; includes SDL + imgui)

**Modified (engine):**
- `engine/build.bat` — separate ImGui compile, exclude from strict glob, link objects, add `-Ivendor/imgui`
- `engine/source/renderer/backend/renderer_backend_sdlgpu.h` / `.cpp` — accessors (Task 3); `bs_imgui_prepare`/`record` calls in `end_frame` (Task 6)
- `engine/source/renderer/renderer.cpp` — ImGui init/shutdown ordering; `bs_imgui_new_frame` in `begin_frame`
- `engine/source/platform/platform_sdl3.cpp` — `bs_imgui_process_event` in the poll loop (Task 5)

**Modified (sandbox):**
- `sandbox/source/game.cpp` — panel ported to `bs_ui_*`, moved to `game_render`, `bs_imgui_wants_mouse()` gating
- `sandbox/source/game.h` — drop `ui.h` include + `UiContext ui;` field
- `sandbox/source/ui.cpp` / `ui.h` — retired (keep `UiAction` enum elsewhere)
- `sandbox/source/text.*` — conditionally retired

**No change needed:** `sandbox/build.bat` (globs sandbox cpp only; never sees ImGui), `renderer_types.h`/`renderer.h`/`renderer_backend.h` (stay SDL-free — facade is a separate header), shader assets (ImGui ships embedded shaders).

---

## Validation

There is no test harness in the repo, so validation is build + visual (matches the established Black Stride workflow).

1. **Build:** `build-all.bat` clean — engine DLL links ImGui objects, sandbox EXE builds, `assets`/`SDL3.dll` staged to `bin/`.
2. **Visual smoke (Phase 1):** `ImGui::ShowDemoWindow()` renders over the sprite scene, interactive, correct under HiDPI.
3. **Visual regression (Phase 2):** the Crew Job Panel looks/behaves like before — selection shows it, all five actions fire, world-click suppressed over the panel.
4. **Stability:** resize, minimize→restore, and quit produce no ImGui asserts (the NewFrame/Render balance pitfall) and no `SDL_GetGPUSwapchainTextureFormat`/validation errors.
5. **Visual verification harness:** reuse `bin/verify_jobs.ps1` (per-monitor DPI-aware PrintWindow capture to `bin/shots/`). Relaunch `sandbox.exe` before each run (it self-exits after a finite runtime). Confirm the panel renders in the top-right HUD region the harness already targets.

---

## Risks, tradeoffs & open questions

| # | Risk / question | Disposition |
|---|---|---|
| 1 | ImGui won't compile under `-Wall -Werror -Wvarargs` | **Resolved** — separate relaxed compile to objects (Task 2). |
| 2 | Shader toolchain for ImGui | **Resolved/none** — `imgui_impl_sdlgpu3` ships embedded DXIL/SPIRV/MSL and selects by device format, mirroring `load_shader`. No shadercross needed. |
| 3 | NewFrame/Render imbalance on minimized frames → assert | **Mitigated** — always call `ImGui::Render()` once per `NewFrame`; record only when a pass exists (Task 6). |
| 4 | DLL boundary: ImGui state/context inside `engine.dll` | **By design** — context lives in the DLL; sandbox only touches `bs_*` exports. No ImGui ABI across the boundary. |
| 5 | 1-frame latency on `WantCaptureMouse` (widgets built in `render`) | **Accepted** — standard ImGui behavior, imperceptible; alternative (frame-start in `update`) documented. |
| 6 | Font atlas upload timing vs. the frame cmd buffer | **Handled by backend** — `imgui_impl_sdlgpu3` uploads the atlas on first use via its own path; no manual upload like the sprite texture loader. |
| 7 | `docking` vs `master` branch | **Open (low)** — plan uses `docking` for headroom; `master` works with no code change. Confirm with user if multi-viewport is unwanted. |
| 8 | HiDPI (125%) scaling — known sandbox quirk | **Watch** — set `ImGui::GetIO().DisplayFramebufferScale` / use `SDL_GetWindowDisplayScale`; verify with the DPI-aware harness. |
| 9 | Keep or retire `text.cpp` | **Open (small)** — decide in Task 9 based on remaining `text_draw` usage. |

---

## Phase rollup (independently buildable checkpoints)

- **Phase 0 (Tasks 1-2):** ImGui vendored + compiling into `engine.dll`. *Exit:* clean `build-all.bat`, no ImGui drawing yet.
- **Phase 1 (Tasks 3-6):** ImGui live — init/shutdown, event hook, frame lifecycle, demo window over the scene. *Exit:* interactive demo window, stable under resize/minimize, old HUD still works.
- **Phase 2 (Tasks 7-9):** Crew Job Panel ported to ImGui; custom `ui`/`text` retired. *Exit:* feature-parity panel on ImGui, dead code removed, clean build.

**Definition of done:** the sandbox renders its Crew Job Panel through Dear ImGui on the SDL3 GPU backend; SDL/ImGui remain confined to backend TUs; the game uses only `bs_*` engine exports; `build-all.bat` builds clean and the DPI-aware harness shows the panel correctly.
