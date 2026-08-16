# DeadStarfield

> **This subsystem is dead code and is recommended for deletion.** It is documented here for
> completeness because `engine-subsystems.md` lists it as a cluster, not because it is part of
> the working engine.

**Responsibility:** *(Superseded.)* Owned the GPU-side half of a VBO-based starfield: an
additive graphics pipeline, per-layer vertex buffers, and a tiled draw that culled to the
visible region of a wrapping star field. It owns nothing today — the class is never
instantiated, so no responsibility is actually discharged at runtime.

**Public interface:** None in practice. `engine/source/renderer/starfield_gpu_resources.h`
declares `class StarfieldGpuResources` with `init`, `shutdown`, `upload_layer`, `draw` and
`has_pipeline`. Nothing is `bs__api__`, so it is invisible outside `engine.dll`, and no code
calls any of it.

**Depends on:** RenderFrontend, RenderBackend, Diagnostics, MathCore, Foundation.
**Depended on by:** nothing functionally. `renderer/backend/renderer_backend_sdlgpu.cpp:20`
includes the header and uses nothing from it — that single include is the only reference, and
it forms a 1-edge-each-way cycle with RenderBackend.

**Key invariants:** None are live, since no code path reaches this module. Two assumptions the
implementation would rely on if it ever ran, both unvalidated:
- `widthMod` must be a power of two minus one — `gx & ld.widthMod`
  (`starfield_gpu_resources.cpp:247`) is a masked wrap, and `minX &= ~(TILE_SIZE - 1)`
  (`:232`) assumes the same for the 256-unit tile size.
- The vertex format must be 7 interleaved floats matching the pipeline layout
  (`:26`, `:118`) — and the three descriptions of that format in the codebase disagree
  (see below).

**Extension points:** None. Do not extend this module. The live starfield is the procedural
shader path: `sandbox/source/render/starfield_layer.cpp:28` sets `params.layer_data = nullptr`
("procedural path"), and the effect is rendered by `sdlgpu_backend_draw_starfield` in
RenderBackend. New starfield work belongs there.

**Known limitations / tech debt:**
- **The class is never instantiated.** Verified: `StarfieldGpuResources` appears only in its own
  two files plus the unused include at `renderer_backend_sdlgpu.cpp:20`.
- **It is half of a two-tree dead pipeline.** Its CPU-side producer,
  `sandbox/source/render/starfield_generator.{cpp,h}`, is equally dead and produces exactly the
  7-float vertex format this file consumes. Both halves are kept alive only by
  `starfield_layer.cpp` having switched to the procedural path.
- **It is the only real breach of the SDL seam rule.** `starfield_gpu_resources.h:4` includes
  `<SDL3/SDL_gpu.h>`, making it one of only two files in the engine that do — the other being
  the backend TU itself. Five other files assert that "only the backend touches SDL". The
  violation is contained purely by accident of usage: its sole includer happens to be the
  backend, which is already permitted. Any other includer would silently breach the invariant.
- **Its own documentation is internally inconsistent.** `LayerVbo::vertexCount` is commented
  "floatCount / 4" (`starfield_gpu_resources.h:44`) while the implementation divides by 7
  (`.cpp:143`); `init` declares 6 vertex attributes but fills only 4 (`.cpp:31,41`).
- `upload_layer` submits its own command buffer per call and creates then immediately releases a
  transfer buffer (`.cpp:117-141`) — the same pattern the backend later optimised away with
  pooling for RmlUi.
- It uses `std::vector` and a C++ class with trailing-underscore members, a naming and style
  convention appearing nowhere else in the engine.
- `shutdown` takes a `device` parameter although the class already stores `device_`, which is
  otherwise unused.

**Recommended action:** Delete `engine/source/renderer/starfield_gpu_resources.{cpp,h}` and the
include at `renderer_backend_sdlgpu.cpp:20`, together with the sandbox's
`render/starfield_generator.{cpp,h}`. That removes the dead code, the RenderBackend cycle, and
the SDL seam breach in one change. *Not yet done — awaiting approval.*

**Source paths:** `engine/source/renderer/starfield_gpu_resources.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
