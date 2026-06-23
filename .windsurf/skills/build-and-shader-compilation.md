# Build & Shader Compilation Skill

How to correctly compile the Black Stride engine, sandbox, and shaders so that code and shader changes actually reach the running game.

---

## 1. High-Level Pipeline

```
HLSL source (.hlsl)          C++ source (.cpp)
       |                           |
       v                           v
   dxc compiler              clang++ compiler
       |                           |
       v                           v
DXIL/SPIR-V blobs          engine.dll + sandbox.exe
   (assets/shaders/)              (bin/)
       |                           |
       +----------->   XCOPY   <---+
                         |
                         v
                    bin/assets/
                    bin/shaders/
```

**Critical rule:** The game loads shaders from `bin/assets/shaders/dxil/` (or `spirv/`), NOT from `assets/shaders/src/`. Editing HLSL source files has **zero effect** until the shaders are recompiled and the blobs are copied into `bin/`.

---

## 2. Engine + Sandbox Build (C++)

### Files
- `build-all.bat` — top-level orchestrator
- `engine/build.bat` — compiles `engine.dll` (shared library)
- `sandbox/build.bat` — compiles `sandbox.exe` (links against `engine.dll`)

### Steps
```batch
:: From repo root
cd c:\dev\blackstride
build-all.bat
```

This:
1. Runs `engine/build.bat` → produces `bin/engine.dll`
2. Runs `sandbox/build.bat` → produces `bin/sandbox.exe`
3. **Copies** `assets/` tree into `bin/assets/` (shaders, textures, ship data) via `XCOPY /E /Y /I`

**Important:** Step 3 is a *copy*, not a *compile*. If shader source files were edited but the pre-compiled DXIL/SPIR-V blobs in `assets/shaders/dxil/` are stale, the copied blobs are also stale. The game will show old visuals.

---

## 3. Shader Compilation (HLSL → DXIL / SPIR-V)

### Toolchain
- **Compiler:** `dxc` (DirectX Shader Compiler), included in the **Vulkan SDK** or Windows SDK
- **Required in PATH:** `dxc.exe` must be findable
- **Script:** `tools/compile_shaders.sh` (bash — requires Git Bash, MSYS2, or WSL on Windows)

### Input / Output Layout
```
assets/shaders/src/<name>.<stage>.hlsl   →   assets/shaders/dxil/<name>.<stage>.dxil
                                             assets/shaders/spirv/<name>.<stage>.spv
```

### Current Shader Inventory (as of 2026-06-17)
| Name | Stage | Description |
|------|-------|-------------|
| `sprite` | `vert`, `frag` | Main sprite pipeline (glow, temperature, distortion) |
| `quad` | `vert`, `frag` | Fullscreen quad (post-process) |
| `fullscreen` | `vert` | Shared fullscreen vertex shader for post-process passes |
| `bloom_extract` | `frag` | Brightness threshold pass |
| `bloom_blur_h` | `frag` | Horizontal Gaussian blur |
| `bloom_blur_v` | `frag` | Vertical Gaussian blur |
| `bloom_streak` | `frag` | Anamorphic directional streak blur |
| `bloom_composite` | `frag` | Final composite (scene + bloom + streak) |
| `starfield` | `vert`, `frag` | Procedural starfield background (if still in use) |

### Known Issue: compile_shaders.sh is INCOMPLETE
The current script only compiles `quad` and `sprite`. It is **missing** entries for:
- `fullscreen.vert`
- `bloom_extract.frag`
- `bloom_blur_h.frag`
- `bloom_blur_v.frag`
- `bloom_streak.frag`
- `bloom_composite.frag`
- `starfield.vert`, `starfield.frag`

If any of these are edited, the script must be updated or they must be compiled manually.

### Manual Compilation (Windows Command Line)
If `compile_shaders.sh` is unavailable or incomplete, compile each shader directly:

```batch
:: DXIL (for D3D12 backend — used on this machine)
dxc -T vs_6_0 -E main assets/shaders/src/sprite.vert.hlsl -Fo assets/shaders/dxil/sprite.vert.dxil
dxc -T ps_6_0 -E main assets/shaders/src/sprite.frag.hlsl -Fo assets/shaders/dxil/sprite.frag.dxil

:: SPIR-V (for Vulkan backend — portability fallback)
dxc -T vs_6_0 -E main -spirv -fspv-target-env=vulkan1.0 assets/shaders/src/sprite.vert.hlsl -Fo assets/shaders/spirv/sprite.vert.spv
dxc -T ps_6_0 -E main -spirv -fspv-target-env=vulkan1.0 assets/shaders/src/sprite.frag.hlsl -Fo assets/shaders/spirv/sprite.frag.spv
```

Profiles:
- Vertex shaders: `-T vs_6_0`
- Fragment (pixel) shaders: `-T ps_6_0`
- Entry point: always `-E main`

### After Recompiling Shaders
You **must** run `build-all.bat` (or manually `XCOPY assets bin\assets /E /Y /I`) so the new blobs are staged into `bin/assets/shaders/`. The game only reads from `bin/`.

---

## 4. Complete Fresh Build Checklist

When pulling fresh code, editing shaders, or seeing stale visuals:

1. **Compile shaders** (if any `.hlsl` changed):
   ```batch
   :: Option A: Git Bash
   bash tools/compile_shaders.sh
   
   :: Option B: Manual dxc calls for each changed shader
   dxc -T ps_6_0 -E main assets/shaders/src/bloom_streak.frag.hlsl -Fo assets/shaders/dxil/bloom_streak.frag.dxil
   :: ... repeat for each changed shader
   ```

2. **Build engine + sandbox + stage assets**:
   ```batch
   cd c:\dev\blackstride
   build-all.bat
   ```

3. **Verify the right blobs are in `bin/`:**
   ```batch
   dir bin\assets\shaders\dxil\bloom_streak.frag.dxil
   ```
   Check the timestamp. If it's older than the source `.hlsl`, the shader was not recompiled or not staged.

4. **Run**:
   ```batch
   cd bin
   sandbox.exe
   ```

---

## 5. Common Pitfalls

| Symptom | Likely Cause | Fix |
|---------|-------------|-----|
| Shader code change has no visual effect | Stale DXIL/SPIR-V blob in `bin/assets/shaders/` | Recompile shader, run `build-all.bat` to restage |
| `dxc` not found | Vulkan SDK / Windows SDK not in PATH | Install Vulkan SDK and add its `Bin` directory to PATH |
| `build-all.bat` succeeds but game crashes on shader load | Shader compiled with wrong profile (e.g. `vs_5_0` instead of `vs_6_0`) | Use `-T vs_6_0` / `-T ps_6_0` |
| `compile_shaders.sh` fails on Windows | Running in `cmd.exe` instead of Git Bash / MSYS2 | Use Git Bash, or compile manually with `dxc` |
| `SDL_CreateGPUShader` fails at runtime | SPIR-V descriptor set mismatch | Ensure `-spirv` flags match SDL_GPU binding contract (see script comments) |
| New shader added to project but not loaded | Missing entry in `compile_shaders.sh` + missing pipeline creation in backend | Add to script AND add `load_shader` + `create_postprocess_pipeline` call in `renderer_backend_sdlgpu.cpp` |

---

## 6. SDL GPU Binding Contract

The engine's `load_shader()` calls specify `num_samplers` and `num_uniform_buffers`. These must match the HLSL register declarations:

| Shader | Vertex UBOs | Fragment Samplers | Fragment UBOs |
|--------|-------------|-------------------|---------------|
| `sprite` | 1 (`b0, space1`) | 1 (`t0, space2`) | 1 (`b0, space3`) |
| Post-process (extract, blur, streak) | 0 | 1 | 1 |
| `bloom_composite` | 0 | 3 | 1 |

When adding a new shader, verify both the HLSL source registers and the `load_shader()` call agree.

---

## 7. Quick Reference: One-Liners

```batch
:: Full rebuild from repo root
cd c:\dev\blackstride && bash tools/compile_shaders.sh && build-all.bat && cd bin && sandbox.exe

:: Just recompile one shader and restage
dxc -T ps_6_0 -E main assets/shaders/src/bloom_streak.frag.hlsl -Fo assets/shaders/dxil/bloom_streak.frag.dxil
xcopy assets bin\assets /E /Y /I
cd bin && sandbox.exe

:: Verify blob freshness
dir assets\shaders\dxil\bloom_streak.frag.dxil bin\assets\shaders\dxil\bloom_streak.frag.dxil
```
