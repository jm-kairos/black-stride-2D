// =====================================================================================
// SDL3 GPU backend — the ONLY translation unit in the engine that includes the SDL GPU
// header. Everything backend-specific is confined here. Verified against SDL 3.4.4.
//
// Phase 1: device, window claim, swapchain, clear.
// Phase 2: offline shaders, a pipeline, one hardcoded colored quad.
// Phase 3 (this file): texture pool (+1x1 white), a sampler, four blend-mode pipelines that
//   share the sprite shaders, a world-space Camera2D, and a DYNAMIC sprite batcher. The game's
//   render(dt) calls draw_sprite to append CPU-side; end_frame sorts by (layer,blend,texture),
//   uploads one dynamic vertex/index buffer, then issues one draw per contiguous run.
//
// Frame structure CHANGED from Phase 2: begin_frame only ACQUIRES the swapchain image (no pass).
// The render pass now opens in end_frame, AFTER the sprite copy-pass upload — because SDL3 GPU
// forbids copy passes inside an active render pass (the #1 SDL GPU bug source).
// =====================================================================================

#include "renderer/backend/renderer_backend_sdlgpu.h"
#include "renderer/renderer_backend.h"
#include "renderer/bs_imgui.h" // SDL-free facade; implemented at the bottom of this TU
#include "renderer/starfield_gpu_resources.h"

#include "core/logger.h"
#include "platform/platform.h"
#include "math/math_utils.h"
#include "renderer/camera2d.h"

#include <SDL3/SDL_gpu.h>
#include <math.h> // cosf, sinf for occlusion atlas
#include <SDL3/SDL_iostream.h> // SDL_LoadFile

// Dear ImGui. Included via -isystem (see engine/build.bat) so ImGui's own headers never trip
// the engine's -Wall -Werror. This backend TU is the ONLY engine TU that includes the SDL GPU
// header, which is precisely where the ImGui SDL3 + SDL_GPU backend implementations belong —
// keeping the "only the backend touches SDL" invariant intact. The SDL-free public facade for
// the rest of the engine is renderer/bs_imgui.h, whose bs_imgui_* functions are implemented at
// the bottom of this file (they read the backend globals g_sdl directly, so no accessors are
// needed). Per-frame NewFrame/Render/Prepare/Record are driven internally from begin/end_frame.
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

// RmlUi (retained-mode in-game UI). Like ImGui, this is the ONLY TU permitted to include the
// RmlUi headers; they come in via -isystem (see engine/build.bat) so RmlUi's own headers never
// trip the engine's -Wall -Werror. RMLUI_STATIC_LIB keeps RmlUi's symbols internal to engine.dll.
// The SDL-free public facade for the rest of the engine and the game is renderer/bs_rml.h, whose
// bs_rml_* functions are implemented at the bottom of this file (they read the backend globals
// g_sdl directly). stb_image is included for declarations only — STB_IMAGE_IMPLEMENTATION lives
// in renderer/backend/stb_image_impl.cpp, so RmlUi's LoadTexture can decode PNG/JPG here.
#include "renderer/bs_rml.h"
#include <RmlUi/Core.h>
#include <RmlUi/Core/DataModelHandle.h> // CreateDataModel / DataModelConstructor (Phase 4 HUD)
#include <RmlUi/Debugger.h>
#include <utility>                      // std::move (custom "backdrop" decorator)
#include "stb_image.h"

#include <SDL3/SDL_video.h>      // SDL_GetWindowSizeInPixels
#include <SDL3/SDL_events.h>     // SDL_Event (RmlUi event translation)
#include <SDL3/SDL_keyboard.h>   // SDL_GetModState
#include <SDL3/SDL_mouse.h>      // SDL_BUTTON_LEFT/MIDDLE/RIGHT
#include <SDL3/SDL_filesystem.h> // SDL_GlobDirectory (font enumeration)
#include <SDL3/SDL_timer.h>      // SDL_GetPerformanceCounter/Frequency (present timing)

#include <stdlib.h> // qsort

using namespace bs_math; // Mat4, mat4_ortho, etc.

// RmlUi render is injected into the swapchain render pass in sdlgpu_backend_end_frame, BENEATH the
// ImGui editor overlay. Defined at the bottom of this file alongside the rest of the bs_rml facade.
static void bs_rml_render_internal(void);

// TRUE when the RmlUi UI is active (and thus its frosted-glass "backdrop" decorator may be visible),
// gating the per-frame scene-blur fill. Defined alongside the bs_rml facade at the bottom.
static b8 g_rml_frost_active(void);

// Submit the shared RmlUi buffer-upload command buffer (if any) for this frame. MUST be called
// before the frame's main command buffer is submitted — SDL GPU executes command buffers in
// submission order, so the geometry uploads land before the draws that reference them. Defined
// alongside the RmlUi render interface below.
static void rml_upload_flush(void);

// Per-frame RmlUi churn counters (reset after the breakdown log in end_frame).
static u32 g_rml_stat_compiles   = 0; // CompileGeometry calls this frame
static u32 g_rml_stat_creates    = 0; // actual SDL_CreateGPUBuffer/TransferBuffer calls (pool misses)
static u32 g_rml_stat_pool_hits  = 0; // buffers served from the pools

// Workaround: on D3D12, SDL_GetGPUSwapchainTextureFormat() may report R8G8B8A8_UNORM
// at init time while the actual swapchain is created as B8G8R8A8_UNORM.  If we build
// pipelines with the reported value we get a format-mismatch validation error on every
// swapchain-facing draw.  Force B8G8R8A8_UNORM when we detect this specific case.
static SDL_GPUTextureFormat get_corrected_swapchain_format(SDL_GPUDevice* device, SDL_Window* window)
{
    const char* driver = SDL_GetGPUDeviceDriver(device);
    SDL_GPUTextureFormat fmt = SDL_GetGPUSwapchainTextureFormat(device, window);
    if (driver && SDL_strcmp(driver, "direct3d12") == 0 &&
        fmt == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
    {
        BS_LOG_WARN("D3D12 init: SDL reports R8G8B8A8_UNORM but swapchain is B8G8R8A8_UNORM; forcing B8G8R8A8_UNORM for swapchain-facing pipelines.");
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    }
    return fmt;
}

// One interleaved sprite vertex: 2D position + UV + RGBA tint + custom float4.
// Matches the pipeline vertex layout and the HLSL VSInput
// (TEXCOORD0=position, TEXCOORD1=uv, TEXCOORD2=color, TEXCOORD3=custom).B
typedef struct sprite_vertex
{
    f32 x, y;       // world-space position (pre-transformed CPU-side)
    f32 u, v;       // atlas UV
    f32 r, g, b, a; // tint
    f32 cr, cg, cb, ca; // custom per-sprite shader parameters
} sprite_vertex;

// One interleaved mapped-sprite vertex: 2D position + UV + world pos + rotation angle.
typedef struct mapped_vertex
{
    f32 x, y;       // world-space position
    f32 u, v;       // atlas UV
    f32 wx, wy, wz; // world-space position for lighting
    f32 angle;      // sprite rotation in radians
} mapped_vertex;

// Directional light uniform for the mapped sprite fragment shader.
typedef struct mapped_light
{
    f32 light_dir[4];  // xyz = normalized world-space light direction, w = intensity
    f32 ambient[4];    // rgb = ambient color, a = unused
    f32 tuning[4];     // x = normal strength, y = depth parallax scale, z = unused, w = unused
} mapped_light;

// Max 2D lights packed into the fragment uniform. MUST equal BS_MAX_LIGHTS in sprite.frag.hlsl.
#define BS_BACKEND_MAX_LIGHTS 16

// Fragment light uniform — MUST match LightUBO in sprite.frag.hlsl (2 + 2*N float4s).
// Pushed per draw-run via SDL_PushGPUFragmentUniformData. params.y is the per-run lit flag
// (0 => fullbright), so unlit runs (HUD/UI) push y=0; each light's color.w is its enable flag.
typedef struct gpu_lights
{
    f32 params[4];                              // x = light count, y = lit flag (0 = fullbright)
    f32 ambient[4];                             // rgb = ambient floor
    f32 pos_radius[BS_BACKEND_MAX_LIGHTS][4];   // xy = pos, z = radius, w = intensity
    f32 color[BS_BACKEND_MAX_LIGHTS][4];        // rgb = color, w = per-light enabled
    f32 glow[8][4];                             // tunable glow/heat params (see sprite.frag.hlsl)
} gpu_lights;

// Capacity limits for the per-frame dynamic batch. HARD CEILING: the index buffer is u16
// (see ibuffer below), and the index init computes base = (u16)(s*4), so the largest
// addressable sprite is s where s*4 <= 65535  =>  s <= 16383, i.e. 16384 sprites max.
// Raising this above 16384 does NOT add capacity — it silently allocates a huge vbuffer
// and writes WRAPPED/aliased indices past s=16383 (corrupt geometry). To exceed 16384,
// switch the index buffer + draw path to u32 first. The sandbox keeps frames well under
// this via camera cull + greedy horizontal run-merge (draw_tile_span), so 16384 is ample.
#define BS_MAX_SPRITES        16384
#define BS_MAX_BATCH_VERTS    (BS_MAX_SPRITES * 4)
#define BS_MAX_BATCH_INDICES  (BS_MAX_SPRITES * 6)
#define BS_MAX_TEXTURES       1024
#define BS_MAX_MAPPED_SPRITES 256

// A pooled GPU texture. `generation` increments on destroy so stale handles are detectable.
typedef struct gpu_texture
{
    SDL_GPUTexture* tex;
    u32             width;
    u32             height;
    u16             generation; // bumped on destroy; 0 in a never-used slot
    b8              in_use;
} gpu_texture;

// CPU-side copy of a sprite for the current frame, kept until end_frame flushes the batch.
typedef struct batched_sprite
{
    bs_sprite  sprite;
    u32        sort_key; // packed (layer<<20)|(blend<<18)|(texture_index) — see make_sort_key
} batched_sprite;

// Backend-owned state. Frontend sees this only as opaque internal_state.
typedef struct sdlgpu_state
{
    SDL_GPUDevice* device;
    SDL_Window*    window;

    // Per-frame transient handles (valid only between begin_frame and end_frame).
    SDL_GPUCommandBuffer* cmd;
    SDL_GPUTexture*       swapchain_texture;
    SDL_GPURenderPass*    pass;
    Uint32                swap_width;
    Uint32                swap_height;

    SDL_FColor clear_color;

    // Sprite pipelines: one per EBlendMode. `pipelines` targets swapchain; `pipelines_offscreen`
    // targets scene_rt (RGBA8) when bloom is enabled so format matches the render target.
    SDL_GPUGraphicsPipeline* pipelines[BLEND_MODE_COUNT];
    SDL_GPUGraphicsPipeline* pipelines_offscreen[BLEND_MODE_COUNT];

    // 4-map mapped sprite pipeline (alpha blend only; drawn inside the scene pass).
    SDL_GPUGraphicsPipeline* pipeline_mapped;
    SDL_GPUGraphicsPipeline* pipeline_mapped_offscreen;

    SDL_GPUSampler*          sampler;        // NEAREST  — pixel-art sprites
    SDL_GPUSampler*          sampler_linear; // LINEAR   — post-process sampling

    // GPU-resident batch buffers. The index buffer is filled once (quad pattern is fixed);
    // the vertex buffer is re-uploaded each frame from the sorted sprite list.
    SDL_GPUBuffer*         vbuffer;
    SDL_GPUBuffer*         ibuffer;
    SDL_GPUTransferBuffer* vtransfer; // persistent UPLOAD transfer buffer for the vertex stream

    // GPU-resident buffers for 4-map mapped sprites (separate vertex layout + pipeline).
    SDL_GPUBuffer*         mapped_vbuffer;
    SDL_GPUBuffer*         mapped_ibuffer;
    SDL_GPUTransferBuffer* mapped_vtransfer;

    // Texture pool.
    gpu_texture textures[BS_MAX_TEXTURES];
    bs_texture  white_texture;  // 1x1 opaque white; lets solid sprites reuse the sprite pipeline
    bs_texture  circle_texture; // radial gradient circle for aux-bloom sunburst proxy

    // Camera (rebuilt into view-proj each frame from the live swapchain size).
    Camera2D camera;

    // 2D point lights pushed to the sprite fragment shader per draw-run. Defaults to count 0
    // (fullbright) in initialize so the renderer behaves exactly as before until the game sets some.
    bs_light2d lights[BS_BACKEND_MAX_LIGHTS];
    u32        light_count;
    bs_color   light_ambient;
    u32        light_unlit_layer; // sprite layers >= this draw fullbright (HUD/UI)

    // Tunable glow parameters pushed to the sprite fragment shader per draw-run.
    bs_glow_params glow_params;
    b8             glow_params_set; // TRUE if the game has ever set glow params (for defaults)

    // CPU-side per-frame batch.
    batched_sprite batch[BS_MAX_SPRITES];
    u32            batch_count;

    // 4-map mapped sprite queue: separate from the regular sprite batch because it uses a
    // different pipeline with four fragment samplers and a directional-light uniform.
    bs_mapped_sprite mapped_batch[BS_MAX_MAPPED_SPRITES];
    u32            mapped_batch_count;

    // Phase 4: stats snapshotted at the end of end_frame for the previous completed frame.
    bs_frame_stats last_stats;

    // Present-cost breakdown of the previous end_frame (ms). acquire = the blocking wait in
    // SDL_WaitAndAcquireGPUSwapchainTexture (VSYNC / swapchain starvation lands here);
    // submit = SDL_SubmitGPUCommandBuffer.
    f32 last_acquire_ms;
    f32 last_submit_ms;

    // Dear ImGui: TRUE between bs_imgui_initialize and bs_imgui_shutdown. Gates every ImGui
    // call in the frame lifecycle so the engine runs cleanly if ImGui ever fails to init.
    b8 imgui_active;

    // Monospace HUD font (loaded from system Consolas; NULL if unavailable).
    void* hud_font;

    // ---- HDR Bloom post-process -----------------------------------------------------------
    // Offscreen render targets (RGBA8). scene_rt is window-sized; bloom_a/b are half-res.
    SDL_GPUTexture* scene_rt;
    SDL_GPUTexture* bloom_a;
    SDL_GPUTexture* bloom_b;

    // Auxiliary bloom targets (half-res) for star-only streak effect.
    SDL_GPUTexture* aux_bloom_a;
    SDL_GPUTexture* aux_bloom_b;

    // Half-resolution nebula target. The nebula FBM shader is fill-rate bound (~50-75 noise
    // evals/pixel), so it is rendered once into this half-size target (premultiplied alpha) and
    // bilinearly upscaled during compositing — ~4x fewer shader invocations, near-invisible loss.
    SDL_GPUTexture* nebula_rt;

    // Half-resolution radiation heat map target. The heat_map shader is fill-rate bound (per-pixel
    // loop over up to BS_MAX_HEAT_SOURCES sources + domain-warp noise), so it is rendered into this
    // half-size target (premultiplied alpha) and upscaled during compositing — same pattern as nebula.
    SDL_GPUTexture* heat_rt;

    // Half-resolution UI backdrop target: a gaussian-blurred copy of the pre-UI scene, produced each
    // frame and sampled by the custom "backdrop" RmlUi decorator to give panels a real frosted-glass
    // look. Same half-res dimensions as the bloom ping-pong.
    SDL_GPUTexture* ui_backdrop_rt;

    // Post-process pipelines (fullscreen triangle, no blending except where noted).
    SDL_GPUGraphicsPipeline* pipeline_extract;
    SDL_GPUGraphicsPipeline* pipeline_blur_h;
    SDL_GPUGraphicsPipeline* pipeline_blur_v;
    SDL_GPUGraphicsPipeline* pipeline_composite;
    SDL_GPUGraphicsPipeline* pipeline_streak;   // directional anamorphic streak
    SDL_GPUGraphicsPipeline* pipeline_flare;    // lens-flare ghost pass
    // Procedural starfield layers (one fullscreen draw per layer).
    SDL_GPUGraphicsPipeline* pipeline_starfield_layer;          // -> offscreen (bloom path)
    SDL_GPUGraphicsPipeline* pipeline_starfield_layer_swapchain; // -> swapchain (non-bloom)

    // Procedural nebula/dust cloud layer, rendered half-res then composited (see below).
    // Half-res nebula: render into nebula_rt (premultiply-on-write), then composite (premult-over).
    SDL_GPUGraphicsPipeline* pipeline_nebula_halfres;              // nebula FBM -> nebula_rt
    SDL_GPUGraphicsPipeline* pipeline_nebula_composite;           // nebula_rt -> scene_rt (bloom path)
    SDL_GPUGraphicsPipeline* pipeline_nebula_composite_swapchain; // nebula_rt -> swapchain (non-bloom)

    // Sunburst star pipeline (additive blend — ONE/ONE).
    SDL_GPUGraphicsPipeline* pipeline_sunburst;          // -> offscreen (bloom path)
    SDL_GPUGraphicsPipeline* pipeline_sunburst_swapchain; // -> swapchain (non-bloom)

    // Real-time star surface pipeline (premult-over blend — occluding photosphere + corona glow).
    SDL_GPUGraphicsPipeline* pipeline_starsurface;           // -> offscreen (bloom path)
    SDL_GPUGraphicsPipeline* pipeline_starsurface_swapchain; // -> swapchain (non-bloom)

    // Real-time planet surface pipeline (premult-over blend — lit impostor sphere + rings/atmo).
    SDL_GPUGraphicsPipeline* pipeline_planetsurface;           // -> offscreen (bloom path)
    SDL_GPUGraphicsPipeline* pipeline_planetsurface_swapchain; // -> swapchain (non-bloom)

    // Procedural radiation heat map. Rendered half-res into heat_rt (premultiply-on-write), then
    // composited (premult-over) via the shared nebula composite pipelines — see composite_halfres.
    SDL_GPUGraphicsPipeline* pipeline_heat_map_halfres;  // heat_map -> heat_rt (half-res)
    bs_heat_map_params       heat_map_params;
    b8                       heat_map_set;

    // Procedural nebula/dust cloud layer state.
    bs_nebula_params         nebula_params;
    b8                       nebula_set;

    // Bloom tuning (editor-settable; defaults give a subtle glow).
    b8  bloom_enabled;
    f32 bloom_threshold;
    f32 bloom_intensity;
    u32 bloom_width;   // last known width (recreate targets on resize)
    u32 bloom_height;  // last known height

    // Anamorphic streak tuning.
    b8  streak_enabled;
    f32 streak_angle;
    f32 streak_length;
    f32 streak_intensity;
    bs_math::Vec2 streak_source;      // bright source position in screen pixels (for lens flare)
    b8  streak_source_set;            // whether streak_source has been set this frame
    f32 streak_flare_intensity;       // lens-flare ghost intensity

    // Auxiliary batch: sprites captured while aux_bloom_mode is active.
    b8             aux_bloom_mode;
    batched_sprite aux_batch[BS_MAX_SPRITES];
    u32            aux_batch_count;

    // Starfield layer queue: parameters queued during game_render, drawn in end_frame.
    // 2 starfield layers × 3 passes each = 6, round up to 8 for headroom.
    #define BS_MAX_STARFIELD_LAYERS 8
    struct {
        b8               active;
        bs_starfield_params params;
    } starfield_queue[BS_MAX_STARFIELD_LAYERS];
    u32 starfield_queue_count;

    // Sunburst star queue: typically 1-4 stars on screen at once.
    #define BS_MAX_SUNBURST_STARS 4
    struct {
        b8               active;
        bs_sunburst_params params;
    } sunburst_queue[BS_MAX_SUNBURST_STARS];
    u32 sunburst_queue_count;

    // Star surface queue: the close-up hero star(s) rendered with the procedural surface shader.
    #define BS_MAX_STARSURFACE_STARS 4
    struct {
        b8               active;
        bs_starsurface_params params;
    } starsurface_queue[BS_MAX_STARSURFACE_STARS];
    u32 starsurface_queue_count;

    // Planet surface queue: impostor-sphere planets large enough on screen to render in full.
    #define BS_MAX_PLANETSURFACE 32
    struct {
        b8               active;
        bs_planetsurface_params params;
    } planetsurface_queue[BS_MAX_PLANETSURFACE];
    u32 planetsurface_queue_count;
} sdlgpu_state;

static sdlgpu_state g_sdl;

// -------------------------------------------------------------------------------------
// Shader loading. Picks DXIL or SPIR-V at runtime based on what the device reports it can
// consume (this machine: direct3d12 => DXIL only, 0xC). The offline-compiled blobs live in
// assets/shaders/{dxil,spirv}/<name>.<stage>.<ext>.
// -------------------------------------------------------------------------------------
static SDL_GPUShader* load_shader(
    SDL_GPUDevice*     device,
    const char*        name,
    const char*        stage_ext,   // "vert" or "frag"
    SDL_GPUShaderStage stage,
    Uint32             num_samplers,
    Uint32             num_uniform_buffers)
{
    SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(device);

    const char* subdir = NULL;
    const char* ext    = NULL;
    SDL_GPUShaderFormat chosen = SDL_GPU_SHADERFORMAT_INVALID;

    if (fmts & SDL_GPU_SHADERFORMAT_DXIL)
    {
        subdir = "dxil"; ext = "dxil"; chosen = SDL_GPU_SHADERFORMAT_DXIL;
    }
    else if (fmts & SDL_GPU_SHADERFORMAT_SPIRV)
    {
        subdir = "spirv"; ext = "spv"; chosen = SDL_GPU_SHADERFORMAT_SPIRV;
    }
    else
    {
        BS_LOG_FATAL("load_shader: device exposes no shader format we compile for (0x%x).", (u32)fmts);
        return NULL;
    }

    char path[512];
    SDL_snprintf(path, sizeof(path), "assets/shaders/%s/%s.%s.%s", subdir, name, stage_ext, ext);

    size_t code_size = 0;
    void* code = SDL_LoadFile(path, &code_size);
    if (!code)
    {
        BS_LOG_FATAL("load_shader: failed to read '%s': %s", path, SDL_GetError());
        return NULL;
    }

    SDL_GPUShaderCreateInfo info;
    SDL_zero(info);
    info.code                 = (const Uint8*)code;
    info.code_size            = code_size;
    info.entrypoint           = "main";
    info.format               = chosen;
    info.stage                = stage;
    info.num_samplers         = num_samplers;
    info.num_uniform_buffers  = num_uniform_buffers;
    info.num_storage_buffers  = 0;
    info.num_storage_textures = 0;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    SDL_free(code);

    if (!shader)
    {
        BS_LOG_FATAL("load_shader: SDL_CreateGPUShader('%s') failed: %s", path, SDL_GetError());
        return NULL;
    }

    BS_LOG_DEBUG("load_shader: loaded '%s' (%llu bytes).", path, (unsigned long long)code_size);
    return shader;
}

// qsort comparator over batched_sprite by packed sort key (ascending): layer, then blend, then
// texture index. Contiguous equal-key runs share one draw call.
static int batch_compare(const void* a, const void* b)
{
    u32 ka = ((const batched_sprite*)a)->sort_key;
    u32 kb = ((const batched_sprite*)b)->sort_key;
    if (ka < kb) return -1;
    if (ka > kb) return 1;
    return 0;
}

// Pack (layer, blend, texture_index) into one sortable u32. layer dominates (draw order), then
// blend mode, then texture so equal-texture sprites cluster into one draw run.
static inline u32 make_sort_key(u32 layer, EBlendMode blend, u32 tex_index)
{
    u32 l = (layer & 0xFFFu) << 20;    // 12 bits of layer
    u32 b = ((u32)blend & 0x3u) << 18; // 2 bits of blend
    u32 t = (tex_index & 0x3FFFFu);    // 18 bits of texture index
    return l | b | t;
}

// Allocate a texture pool slot and return its handle (id encodes index+1 in low bits and the
// generation in the high bits). Returns id 0 if the pool is full.
static bs_texture pool_alloc_texture(SDL_GPUTexture* tex, u32 w, u32 h)
{
    for (u32 i = 0; i < BS_MAX_TEXTURES; ++i)
    {
        if (!g_sdl.textures[i].in_use)
        {
            gpu_texture* slot = &g_sdl.textures[i];
            slot->tex    = tex;
            slot->width  = w;
            slot->height = h;
            slot->in_use = TRUE;
            if (slot->generation == 0) slot->generation = 1;

            bs_texture handle;
            handle.id = ((u32)slot->generation << 18) | (i + 1u); // index+1 so id 0 stays invalid
            return handle;
        }
    }
    BS_LOG_ERROR("pool_alloc_texture: texture pool exhausted (%u).", (u32)BS_MAX_TEXTURES);
    bs_texture invalid; invalid.id = BS_INVALID_HANDLE;
    return invalid;
}

// Resolve a handle to a live pool slot, validating the generation. Returns NULL if stale/invalid.
static gpu_texture* pool_resolve_texture(bs_texture handle)
{
    if (handle.id == BS_INVALID_HANDLE) return NULL;
    u32 index = (handle.id & 0x3FFFFu);
    if (index == 0 || index > BS_MAX_TEXTURES) return NULL;
    gpu_texture* slot = &g_sdl.textures[index - 1u];
    u16 gen = (u16)(handle.id >> 18);
    if (!slot->in_use || slot->generation != gen) return NULL;
    return slot;
}

// -------------------------------------------------------------------------------------
// Pipeline construction. One graphics pipeline per blend mode, all sharing the sprite shaders
// and the same vertex layout. Only the color-target blend state differs.
// -------------------------------------------------------------------------------------
static void fill_blend_state(EBlendMode mode, SDL_GPUColorTargetBlendState* bs)
{
    SDL_zero(*bs);
    bs->color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
                           SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;

    switch (mode)
    {
        case BLEND_NONE:
            bs->enable_blend = false;
            break;

        case BLEND_ALPHA:
            bs->enable_blend           = true;
            bs->color_blend_op         = SDL_GPU_BLENDOP_ADD;
            bs->src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            bs->dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            bs->alpha_blend_op         = SDL_GPU_BLENDOP_ADD;
            bs->src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE;
            bs->dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
            break;

        case BLEND_ADDITIVE:
            bs->enable_blend           = true;
            bs->color_blend_op         = SDL_GPU_BLENDOP_ADD;
            bs->src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
            bs->dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE;
            bs->alpha_blend_op         = SDL_GPU_BLENDOP_ADD;
            bs->src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE;
            bs->dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE;
            break;

        case BLEND_MULTIPLY:
            bs->enable_blend           = true;
            bs->color_blend_op         = SDL_GPU_BLENDOP_ADD;
            bs->src_color_blendfactor  = SDL_GPU_BLENDFACTOR_DST_COLOR;
            bs->dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ZERO;
            bs->alpha_blend_op         = SDL_GPU_BLENDOP_ADD;
            bs->src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_DST_ALPHA;
            bs->dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ZERO;
            break;

        default:
            bs->enable_blend = false;
            break;
    }
}

static b8 create_pipelines_for_format(SDL_GPUShader* vs, SDL_GPUShader* fs,
                                      SDL_GPUTextureFormat color_fmt,
                                      SDL_GPUGraphicsPipeline** out_pipelines)
{
    // Vertex buffer: one interleaved stream, per-vertex.
    SDL_GPUVertexBufferDescription vbuf_desc;
    SDL_zero(vbuf_desc);
    vbuf_desc.slot               = 0;
    vbuf_desc.pitch              = sizeof(sprite_vertex);
    vbuf_desc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbuf_desc.instance_step_rate = 0;

    // Attributes: position (loc 0, float2), uv (loc 1, float2), color (loc 2, float4), custom (loc 3, float4).
    SDL_GPUVertexAttribute attrs[4];
    SDL_zero(attrs);
    attrs[0].location    = 0;
    attrs[0].buffer_slot = 0;
    attrs[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset      = offsetof(sprite_vertex, x);
    attrs[1].location    = 1;
    attrs[1].buffer_slot = 0;
    attrs[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[1].offset      = offsetof(sprite_vertex, u);
    attrs[2].location    = 2;
    attrs[2].buffer_slot = 0;
    attrs[2].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[2].offset      = offsetof(sprite_vertex, r);
    attrs[3].location    = 3;
    attrs[3].buffer_slot = 0;
    attrs[3].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4;
    attrs[3].offset      = offsetof(sprite_vertex, cr);

    for (u32 m = 0; m < BLEND_MODE_COUNT; ++m)
    {
        SDL_GPUColorTargetBlendState blend;
        fill_blend_state((EBlendMode)m, &blend);

        SDL_GPUColorTargetDescription color_target;
        SDL_zero(color_target);
        color_target.format      = color_fmt;
        color_target.blend_state = blend;

        SDL_GPUGraphicsPipelineCreateInfo info;
        SDL_zero(info);
        info.vertex_shader   = vs;
        info.fragment_shader = fs;
        info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

        info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
        info.vertex_input_state.num_vertex_buffers         = 1;
        info.vertex_input_state.vertex_attributes          = attrs;
        info.vertex_input_state.num_vertex_attributes      = 4;

        info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

        info.target_info.color_target_descriptions = &color_target;
        info.target_info.num_color_targets         = 1;
        info.target_info.has_depth_stencil_target  = false;

        out_pipelines[m] = SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
        if (!out_pipelines[m])
        {
            BS_LOG_FATAL("create_pipelines: pipeline %u (fmt %u) failed: %s", m, (u32)color_fmt, SDL_GetError());
            return FALSE;
        }
    }

    return TRUE;
}

static b8 create_mapped_pipeline_for_format(SDL_GPUShader* vs, SDL_GPUShader* fs,
                                             SDL_GPUTextureFormat color_fmt,
                                             SDL_GPUGraphicsPipeline** out_pipeline)
{
    SDL_GPUVertexBufferDescription vbuf_desc;
    SDL_zero(vbuf_desc);
    vbuf_desc.slot               = 0;
    vbuf_desc.pitch              = sizeof(mapped_vertex);
    vbuf_desc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbuf_desc.instance_step_rate = 0;

    SDL_GPUVertexAttribute attrs[4];
    SDL_zero(attrs);
    attrs[0].location    = 0;
    attrs[0].buffer_slot = 0;
    attrs[0].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset      = offsetof(mapped_vertex, x);
    attrs[1].location    = 1;
    attrs[1].buffer_slot = 0;
    attrs[1].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[1].offset      = offsetof(mapped_vertex, u);
    attrs[2].location    = 2;
    attrs[2].buffer_slot = 0;
    attrs[2].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attrs[2].offset      = offsetof(mapped_vertex, wx);
    attrs[3].location    = 3;
    attrs[3].buffer_slot = 0;
    attrs[3].format      = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    attrs[3].offset      = offsetof(mapped_vertex, angle);

    SDL_GPUColorTargetBlendState blend;
    SDL_zero(blend);
    blend.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
                             SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    blend.enable_blend           = true;
    blend.color_blend_op         = SDL_GPU_BLENDOP_ADD;
    blend.src_color_blendfactor  = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op         = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor  = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    SDL_GPUColorTargetDescription color_target;
    SDL_zero(color_target);
    color_target.format      = color_fmt;
    color_target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader   = vs;
    info.fragment_shader = fs;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    info.vertex_input_state.num_vertex_buffers         = 1;
    info.vertex_input_state.vertex_attributes          = attrs;
    info.vertex_input_state.num_vertex_attributes      = 4;

    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets         = 1;
    info.target_info.has_depth_stencil_target  = false;

    *out_pipeline = SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
    if (!*out_pipeline)
    {
        BS_LOG_FATAL("create_mapped_pipeline: failed (fmt %u): %s", (u32)color_fmt, SDL_GetError());
        return FALSE;
    }
    return TRUE;
}

// Build the persistent batch buffers and the 1x1 white texture. The index buffer is filled once
// with the fixed quad pattern (0,1,2, 2,3,0) repeated per sprite; the vertex buffer stays empty
// (uploaded per frame). Returns FALSE on any GPU allocation failure.
static b8 create_batch_resources()
{
    // Vertex buffer (dynamic, re-uploaded each frame).
    SDL_GPUBufferCreateInfo vinfo;
    SDL_zero(vinfo);
    vinfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vinfo.size  = sizeof(sprite_vertex) * BS_MAX_BATCH_VERTS;
    g_sdl.vbuffer = SDL_CreateGPUBuffer(g_sdl.device, &vinfo);
    if (!g_sdl.vbuffer) { BS_LOG_FATAL("create_batch_resources: vbuffer failed: %s", SDL_GetError()); return FALSE; }

    // Index buffer (static).
    SDL_GPUBufferCreateInfo iinfo;
    SDL_zero(iinfo);
    iinfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    iinfo.size  = sizeof(u16) * BS_MAX_BATCH_INDICES;
    g_sdl.ibuffer = SDL_CreateGPUBuffer(g_sdl.device, &iinfo);
    if (!g_sdl.ibuffer) { BS_LOG_FATAL("create_batch_resources: ibuffer failed: %s", SDL_GetError()); return FALSE; }

    // Persistent transfer buffer for streaming vertices each frame.
    SDL_GPUTransferBufferCreateInfo tinfo;
    SDL_zero(tinfo);
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size  = sizeof(sprite_vertex) * BS_MAX_BATCH_VERTS;
    g_sdl.vtransfer = SDL_CreateGPUTransferBuffer(g_sdl.device, &tinfo);
    if (!g_sdl.vtransfer) { BS_LOG_FATAL("create_batch_resources: vtransfer failed: %s", SDL_GetError()); return FALSE; }

    // Mapped sprite buffers: separate vertex layout, smaller capacity.
    SDL_GPUBufferCreateInfo mvinfo;
    SDL_zero(mvinfo);
    mvinfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    mvinfo.size  = sizeof(mapped_vertex) * BS_MAX_MAPPED_SPRITES * 4;
    g_sdl.mapped_vbuffer = SDL_CreateGPUBuffer(g_sdl.device, &mvinfo);
    if (!g_sdl.mapped_vbuffer) { BS_LOG_FATAL("create_batch_resources: mapped_vbuffer failed: %s", SDL_GetError()); return FALSE; }

    SDL_GPUBufferCreateInfo miinfo;
    SDL_zero(miinfo);
    miinfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    miinfo.size  = sizeof(u16) * BS_MAX_MAPPED_SPRITES * 6;
    g_sdl.mapped_ibuffer = SDL_CreateGPUBuffer(g_sdl.device, &miinfo);
    if (!g_sdl.mapped_ibuffer) { BS_LOG_FATAL("create_batch_resources: mapped_ibuffer failed: %s", SDL_GetError()); return FALSE; }

    SDL_GPUTransferBufferCreateInfo mtinfo;
    SDL_zero(mtinfo);
    mtinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    mtinfo.size  = sizeof(mapped_vertex) * BS_MAX_MAPPED_SPRITES * 4;
    g_sdl.mapped_vtransfer = SDL_CreateGPUTransferBuffer(g_sdl.device, &mtinfo);
    if (!g_sdl.mapped_vtransfer) { BS_LOG_FATAL("create_batch_resources: mapped_vtransfer failed: %s", SDL_GetError()); return FALSE; }

    // Fill the static index buffer once via a temporary transfer buffer.
    {
        SDL_GPUTransferBufferCreateInfo iti;
        SDL_zero(iti);
        iti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        iti.size  = sizeof(u16) * BS_MAX_BATCH_INDICES;
        SDL_GPUTransferBuffer* itb = SDL_CreateGPUTransferBuffer(g_sdl.device, &iti);
        if (!itb) { BS_LOG_FATAL("create_batch_resources: index transfer failed: %s", SDL_GetError()); return FALSE; }

        u16* idx = (u16*)SDL_MapGPUTransferBuffer(g_sdl.device, itb, false);
        for (u32 s = 0; s < BS_MAX_SPRITES; ++s)
        {
            u16 base = (u16)(s * 4u);
            u32 o    = s * 6u;
            idx[o + 0] = base + 0; idx[o + 1] = base + 1; idx[o + 2] = base + 2;
            idx[o + 3] = base + 2; idx[o + 4] = base + 3; idx[o + 5] = base + 0;
        }
        SDL_UnmapGPUTransferBuffer(g_sdl.device, itb);

        SDL_GPUCommandBuffer* up = SDL_AcquireGPUCommandBuffer(g_sdl.device);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(up);
        SDL_GPUTransferBufferLocation src; SDL_zero(src);
        src.transfer_buffer = itb; src.offset = 0;
        SDL_GPUBufferRegion dst; SDL_zero(dst);
        dst.buffer = g_sdl.ibuffer; dst.offset = 0; dst.size = sizeof(u16) * BS_MAX_BATCH_INDICES;
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
        SDL_SubmitGPUCommandBuffer(up);
        SDL_ReleaseGPUTransferBuffer(g_sdl.device, itb);
    }

    // Fill the mapped index buffer once.
    {
        SDL_GPUTransferBufferCreateInfo iti;
        SDL_zero(iti);
        iti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        iti.size  = sizeof(u16) * BS_MAX_MAPPED_SPRITES * 6;
        SDL_GPUTransferBuffer* itb = SDL_CreateGPUTransferBuffer(g_sdl.device, &iti);
        if (!itb) { BS_LOG_FATAL("create_batch_resources: mapped index transfer failed: %s", SDL_GetError()); return FALSE; }

        u16* idx = (u16*)SDL_MapGPUTransferBuffer(g_sdl.device, itb, false);
        for (u32 s = 0; s < BS_MAX_MAPPED_SPRITES; ++s)
        {
            u16 base = (u16)(s * 4u);
            u32 o    = s * 6u;
            idx[o + 0] = base + 0; idx[o + 1] = base + 1; idx[o + 2] = base + 2;
            idx[o + 3] = base + 2; idx[o + 4] = base + 3; idx[o + 5] = base + 0;
        }
        SDL_UnmapGPUTransferBuffer(g_sdl.device, itb);

        SDL_GPUCommandBuffer* up = SDL_AcquireGPUCommandBuffer(g_sdl.device);
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(up);
        SDL_GPUTransferBufferLocation src; SDL_zero(src);
        src.transfer_buffer = itb; src.offset = 0;
        SDL_GPUBufferRegion dst; SDL_zero(dst);
        dst.buffer = g_sdl.mapped_ibuffer; dst.offset = 0; dst.size = sizeof(u16) * BS_MAX_MAPPED_SPRITES * 6;
        SDL_UploadToGPUBuffer(cp, &src, &dst, false);
        SDL_EndGPUCopyPass(cp);
        SDL_SubmitGPUCommandBuffer(up);
        SDL_ReleaseGPUTransferBuffer(g_sdl.device, itb);
    }

    return TRUE;
}

// Create offscreen render targets for HDR bloom. scene_rt is window-sized; bloom_a/b are half-res.
static b8 create_bloom_targets(u32 width, u32 height)
{
    g_sdl.bloom_width  = width;
    g_sdl.bloom_height = height;

    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;

    // Scene render target: full window size, colour target + sampler.
    info.width  = width;
    info.height = height;
    info.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_PropertiesID rt_props = SDL_CreateProperties();
    SDL_SetFloatProperty(rt_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_R_FLOAT, 0.0f);
    SDL_SetFloatProperty(rt_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_G_FLOAT, 0.0f);
    SDL_SetFloatProperty(rt_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_B_FLOAT, 0.0f);
    SDL_SetFloatProperty(rt_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_A_FLOAT, 1.0f);
    info.props = rt_props;
    g_sdl.scene_rt = SDL_CreateGPUTexture(g_sdl.device, &info);
    SDL_DestroyProperties(rt_props);
    info.props = 0;
    if (!g_sdl.scene_rt) { BS_LOG_FATAL("create_bloom_targets: scene_rt failed: %s", SDL_GetError()); return FALSE; }

    // Bloom ping-pong: half resolution.
    info.width  = (width  > 1) ? (width  / 2) : 1;
    info.height = (height > 1) ? (height / 2) : 1;
    info.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    g_sdl.bloom_a = SDL_CreateGPUTexture(g_sdl.device, &info);
    if (!g_sdl.bloom_a) { BS_LOG_FATAL("create_bloom_targets: bloom_a failed: %s", SDL_GetError()); return FALSE; }
    g_sdl.bloom_b = SDL_CreateGPUTexture(g_sdl.device, &info);
    if (!g_sdl.bloom_b) { BS_LOG_FATAL("create_bloom_targets: bloom_b failed: %s", SDL_GetError()); return FALSE; }

    // Auxiliary bloom ping-pong (same half-res for streak effect).
    g_sdl.aux_bloom_a = SDL_CreateGPUTexture(g_sdl.device, &info);
    if (!g_sdl.aux_bloom_a) { BS_LOG_FATAL("create_bloom_targets: aux_bloom_a failed: %s", SDL_GetError()); return FALSE; }
    g_sdl.aux_bloom_b = SDL_CreateGPUTexture(g_sdl.device, &info);
    if (!g_sdl.aux_bloom_b) { BS_LOG_FATAL("create_bloom_targets: aux_bloom_b failed: %s", SDL_GetError()); return FALSE; }

    // Half-resolution nebula target (cleared transparent each frame; premultiplied-alpha content).
    // Same half-res dimensions as the bloom ping-pong. Cleared to (0,0,0,0) so the D3D12 clear hint
    // uses alpha 0 (distinct from scene_rt's opaque-black hint).
    {
        SDL_PropertiesID neb_props = SDL_CreateProperties();
        SDL_SetFloatProperty(neb_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_R_FLOAT, 0.0f);
        SDL_SetFloatProperty(neb_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_G_FLOAT, 0.0f);
        SDL_SetFloatProperty(neb_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_B_FLOAT, 0.0f);
        SDL_SetFloatProperty(neb_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_A_FLOAT, 0.0f);
        info.props = neb_props;
        g_sdl.nebula_rt = SDL_CreateGPUTexture(g_sdl.device, &info);
        SDL_DestroyProperties(neb_props);
        info.props = 0;
        if (!g_sdl.nebula_rt) { BS_LOG_FATAL("create_bloom_targets: nebula_rt failed: %s", SDL_GetError()); return FALSE; }
    }

    // Half-resolution radiation heat map target (cleared transparent each frame; premultiplied-alpha
    // content). Same half-res dimensions and transparent D3D12 clear hint as nebula_rt.
    {
        SDL_PropertiesID heat_props = SDL_CreateProperties();
        SDL_SetFloatProperty(heat_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_R_FLOAT, 0.0f);
        SDL_SetFloatProperty(heat_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_G_FLOAT, 0.0f);
        SDL_SetFloatProperty(heat_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_B_FLOAT, 0.0f);
        SDL_SetFloatProperty(heat_props, SDL_PROP_GPU_TEXTURE_CREATE_D3D12_CLEAR_A_FLOAT, 0.0f);
        info.props = heat_props;
        g_sdl.heat_rt = SDL_CreateGPUTexture(g_sdl.device, &info);
        SDL_DestroyProperties(heat_props);
        info.props = 0;
        if (!g_sdl.heat_rt) { BS_LOG_FATAL("create_bloom_targets: heat_rt failed: %s", SDL_GetError()); return FALSE; }
    }

    // Half-resolution UI backdrop target (opaque blurred scene for the frosted-glass decorator).
    info.width  = (width  > 1) ? (width  / 2) : 1;
    info.height = (height > 1) ? (height / 2) : 1;
    info.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    g_sdl.ui_backdrop_rt = SDL_CreateGPUTexture(g_sdl.device, &info);
    if (!g_sdl.ui_backdrop_rt) { BS_LOG_FATAL("create_bloom_targets: ui_backdrop_rt failed: %s", SDL_GetError()); return FALSE; }

    return TRUE;
}

// Create a fullscreen post-process pipeline from pre-loaded shaders.
static SDL_GPUGraphicsPipeline* create_postprocess_pipeline(
    SDL_GPUShader* vs, SDL_GPUShader* fs,
    SDL_GPUTextureFormat color_fmt)
{
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader   = vs;
    info.fragment_shader = fs;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUColorTargetDescription ct;
    SDL_zero(ct);
    ct.format = color_fmt;
    ct.blend_state.enable_blend = false;

    info.target_info.color_target_descriptions = &ct;
    info.target_info.num_color_targets         = 1;
    info.target_info.has_depth_stencil_target  = false;

    // No vertex attributes (fullscreen triangle uses SV_VertexID).
    info.vertex_input_state.num_vertex_buffers  = 0;
    info.vertex_input_state.vertex_buffer_descriptions = NULL;
    info.vertex_input_state.num_vertex_attributes = 0;
    info.vertex_input_state.vertex_attributes    = NULL;

    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    // No depth/stencil.
    info.depth_stencil_state.enable_depth_test   = false;
    info.depth_stencil_state.enable_depth_write  = false;
    info.depth_stencil_state.enable_stencil_test = false;

    return SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
}

// Same as above but with additive ONE/ONE blending (for sunburst / starfield-style effects).
static SDL_GPUGraphicsPipeline* create_additive_postprocess_pipeline(
    SDL_GPUShader* vs, SDL_GPUShader* fs,
    SDL_GPUTextureFormat color_fmt)
{
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader   = vs;
    info.fragment_shader = fs;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUColorTargetDescription ct;
    SDL_zero(ct);
    ct.format = color_fmt;
    ct.blend_state.enable_blend = true;
    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;

    info.target_info.color_target_descriptions = &ct;
    info.target_info.num_color_targets         = 1;
    info.target_info.has_depth_stencil_target  = false;

    info.vertex_input_state.num_vertex_buffers  = 0;
    info.vertex_input_state.vertex_buffer_descriptions = NULL;
    info.vertex_input_state.num_vertex_attributes = 0;
    info.vertex_input_state.vertex_attributes    = NULL;

    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    info.depth_stencil_state.enable_depth_test   = false;
    info.depth_stencil_state.enable_depth_write  = false;
    info.depth_stencil_state.enable_stencil_test = false;

    return SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
}

// Fullscreen pipeline that PREMULTIPLIES the shader output on write into a cleared-transparent
// target: rgb_out = src.rgb*src.a, a_out = src.a. Used to render the (straight-alpha) nebula FBM
// into nebula_rt so the half-res result can be bilinearly upscaled without dark edge fringing.
static SDL_GPUGraphicsPipeline* create_premult_write_postprocess_pipeline(
    SDL_GPUShader* vs, SDL_GPUShader* fs,
    SDL_GPUTextureFormat color_fmt)
{
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader   = vs;
    info.fragment_shader = fs;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUColorTargetDescription ct;
    SDL_zero(ct);
    ct.format = color_fmt;
    ct.blend_state.enable_blend = true;
    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;

    info.target_info.color_target_descriptions = &ct;
    info.target_info.num_color_targets         = 1;
    info.target_info.has_depth_stencil_target  = false;

    info.vertex_input_state.num_vertex_buffers  = 0;
    info.vertex_input_state.vertex_buffer_descriptions = NULL;
    info.vertex_input_state.num_vertex_attributes = 0;
    info.vertex_input_state.vertex_attributes    = NULL;

    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    info.depth_stencil_state.enable_depth_test   = false;
    info.depth_stencil_state.enable_depth_write  = false;
    info.depth_stencil_state.enable_stencil_test = false;

    return SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
}

// Fullscreen pipeline that composites a PREMULTIPLIED-alpha source over the destination:
// out = src + dst*(1-src.a). Used to upscale + composite nebula_rt over the scene/swapchain.
static SDL_GPUGraphicsPipeline* create_premult_over_postprocess_pipeline(
    SDL_GPUShader* vs, SDL_GPUShader* fs,
    SDL_GPUTextureFormat color_fmt)
{
    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader   = vs;
    info.fragment_shader = fs;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;

    SDL_GPUColorTargetDescription ct;
    SDL_zero(ct);
    ct.format = color_fmt;
    ct.blend_state.enable_blend = true;
    ct.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    ct.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    ct.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    ct.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    info.target_info.color_target_descriptions = &ct;
    info.target_info.num_color_targets         = 1;
    info.target_info.has_depth_stencil_target  = false;

    info.vertex_input_state.num_vertex_buffers  = 0;
    info.vertex_input_state.vertex_buffer_descriptions = NULL;
    info.vertex_input_state.num_vertex_attributes = 0;
    info.vertex_input_state.vertex_attributes    = NULL;

    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    info.depth_stencil_state.enable_depth_test   = false;
    info.depth_stencil_state.enable_depth_write  = false;
    info.depth_stencil_state.enable_stencil_test = false;

    return SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
}

static b8 create_postprocess_pipelines(void)
{
    SDL_GPUTextureFormat swap_fmt =
        get_corrected_swapchain_format(g_sdl.device, g_sdl.window);

    SDL_GPUShader* vs = load_shader(g_sdl.device, "fullscreen", "vert",
                                     SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    if (!vs) return FALSE;

    SDL_GPUTextureFormat offscreen_fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    // extract -> bloom_a (RGBA8)
    SDL_GPUShader* fs_extract = load_shader(g_sdl.device, "bloom_extract", "frag",
                                             SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!fs_extract) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_extract = create_postprocess_pipeline(vs, fs_extract, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_extract);
    if (!g_sdl.pipeline_extract) { BS_LOG_FATAL("create_postprocess_pipelines: extract failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // blur_h -> bloom_b (RGBA8)
    SDL_GPUShader* fs_blur_h = load_shader(g_sdl.device, "bloom_blur_h", "frag",
                                            SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!fs_blur_h) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_blur_h = create_postprocess_pipeline(vs, fs_blur_h, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_blur_h);
    if (!g_sdl.pipeline_blur_h) { BS_LOG_FATAL("create_postprocess_pipelines: blur_h failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // blur_v -> bloom_a (RGBA8)
    SDL_GPUShader* fs_blur_v = load_shader(g_sdl.device, "bloom_blur_v", "frag",
                                            SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!fs_blur_v) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_blur_v = create_postprocess_pipeline(vs, fs_blur_v, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_blur_v);
    if (!g_sdl.pipeline_blur_v) { BS_LOG_FATAL("create_postprocess_pipelines: blur_v failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // streak -> bloom_b (RGBA8)
    SDL_GPUShader* fs_streak = load_shader(g_sdl.device, "bloom_streak", "frag",
                                            SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!fs_streak) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_streak = create_postprocess_pipeline(vs, fs_streak, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_streak);
    if (!g_sdl.pipeline_streak) { BS_LOG_FATAL("create_postprocess_pipelines: streak failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // flare -> bloom_a (RGBA8) — lens-flare ghost pass
    SDL_GPUShader* fs_flare = load_shader(g_sdl.device, "bloom_flare", "frag",
                                           SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!fs_flare) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_flare = create_postprocess_pipeline(vs, fs_flare, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_flare);
    if (!g_sdl.pipeline_flare) { BS_LOG_FATAL("create_postprocess_pipelines: flare failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // starfield_layer -> offscreen (bloom path) — procedural per-layer starfield
    // Additive blending so multiple layers (far + mid) accumulate instead of overwriting.
    SDL_GPUShader* fs_starfield_layer = load_shader(g_sdl.device, "starfield_layer", "frag",
                                                   SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 2);
    if (!fs_starfield_layer) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_starfield_layer = create_additive_postprocess_pipeline(vs, fs_starfield_layer, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_starfield_layer);
    if (!g_sdl.pipeline_starfield_layer) { BS_LOG_FATAL("create_postprocess_pipelines: starfield_layer failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // starfield_layer -> swapchain (non-bloom direct-to-screen)
    fs_starfield_layer = load_shader(g_sdl.device, "starfield_layer", "frag",
                                    SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 2);
    if (!fs_starfield_layer) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_starfield_layer_swapchain = create_additive_postprocess_pipeline(vs, fs_starfield_layer, swap_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_starfield_layer);
    if (!g_sdl.pipeline_starfield_layer_swapchain) { BS_LOG_FATAL("create_postprocess_pipelines: starfield_layer_swapchain failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // nebula half-res: nebula FBM -> nebula_rt (RGBA8), premultiply-on-write so the half-res result
    // upscales cleanly. Reuses the nebula_layer fragment shader (resolution-independent).
    SDL_GPUShader* fs_nebula_layer = load_shader(g_sdl.device, "nebula_layer", "frag",
                                                 SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!fs_nebula_layer) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_nebula_halfres = create_premult_write_postprocess_pipeline(vs, fs_nebula_layer, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_nebula_layer);
    if (!g_sdl.pipeline_nebula_halfres) { BS_LOG_FATAL("create_postprocess_pipelines: nebula_halfres failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // nebula composite: nebula_rt -> scene_rt (bloom path) and -> swapchain (non-bloom), premult-over.
    SDL_GPUShader* fs_nebula_composite = load_shader(g_sdl.device, "nebula_composite", "frag",
                                                     SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    if (!fs_nebula_composite) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_nebula_composite = create_premult_over_postprocess_pipeline(vs, fs_nebula_composite, offscreen_fmt);
    if (!g_sdl.pipeline_nebula_composite) { BS_LOG_FATAL("create_postprocess_pipelines: nebula_composite failed"); SDL_ReleaseGPUShader(g_sdl.device, fs_nebula_composite); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_nebula_composite_swapchain = create_premult_over_postprocess_pipeline(vs, fs_nebula_composite, swap_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_nebula_composite);
    if (!g_sdl.pipeline_nebula_composite_swapchain) { BS_LOG_FATAL("create_postprocess_pipelines: nebula_composite_swapchain failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // sunburst -> offscreen (bloom path) — additive blend, custom quad shader
    SDL_GPUShader* vs_sunburst = load_shader(g_sdl.device, "sunburst", "vert",
                                              SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    if (!vs_sunburst) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    SDL_GPUShader* fs_sunburst = load_shader(g_sdl.device, "sunburst", "frag",
                                              SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!fs_sunburst) { SDL_ReleaseGPUShader(g_sdl.device, vs); SDL_ReleaseGPUShader(g_sdl.device, vs_sunburst); return FALSE; }
    g_sdl.pipeline_sunburst = create_additive_postprocess_pipeline(vs_sunburst, fs_sunburst, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_sunburst);
    SDL_ReleaseGPUShader(g_sdl.device, vs_sunburst);
    if (!g_sdl.pipeline_sunburst) { BS_LOG_FATAL("create_postprocess_pipelines: sunburst failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // sunburst -> swapchain (non-bloom direct-to-screen path)
    vs_sunburst = load_shader(g_sdl.device, "sunburst", "vert",
                               SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    if (!vs_sunburst) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    fs_sunburst = load_shader(g_sdl.device, "sunburst", "frag",
                               SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!fs_sunburst) { SDL_ReleaseGPUShader(g_sdl.device, vs); SDL_ReleaseGPUShader(g_sdl.device, vs_sunburst); return FALSE; }
    g_sdl.pipeline_sunburst_swapchain = create_additive_postprocess_pipeline(vs_sunburst, fs_sunburst, swap_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_sunburst);
    SDL_ReleaseGPUShader(g_sdl.device, vs_sunburst);
    if (!g_sdl.pipeline_sunburst_swapchain) { BS_LOG_FATAL("create_postprocess_pipelines: sunburst_swapchain failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // star_surface -> offscreen (bloom path) — premult-over blend so the disc occludes the scene.
    SDL_GPUShader* vs_starsurf = load_shader(g_sdl.device, "star_surface", "vert",
                                              SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    if (!vs_starsurf) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    SDL_GPUShader* fs_starsurf = load_shader(g_sdl.device, "star_surface", "frag",
                                              SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!fs_starsurf) { SDL_ReleaseGPUShader(g_sdl.device, vs); SDL_ReleaseGPUShader(g_sdl.device, vs_starsurf); return FALSE; }
    g_sdl.pipeline_starsurface = create_premult_over_postprocess_pipeline(vs_starsurf, fs_starsurf, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_starsurf);
    SDL_ReleaseGPUShader(g_sdl.device, vs_starsurf);
    if (!g_sdl.pipeline_starsurface) { BS_LOG_FATAL("create_postprocess_pipelines: star_surface failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // star_surface -> swapchain (non-bloom direct-to-screen path)
    vs_starsurf = load_shader(g_sdl.device, "star_surface", "vert",
                               SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    if (!vs_starsurf) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    fs_starsurf = load_shader(g_sdl.device, "star_surface", "frag",
                               SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!fs_starsurf) { SDL_ReleaseGPUShader(g_sdl.device, vs); SDL_ReleaseGPUShader(g_sdl.device, vs_starsurf); return FALSE; }
    g_sdl.pipeline_starsurface_swapchain = create_premult_over_postprocess_pipeline(vs_starsurf, fs_starsurf, swap_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_starsurf);
    SDL_ReleaseGPUShader(g_sdl.device, vs_starsurf);
    if (!g_sdl.pipeline_starsurface_swapchain) { BS_LOG_FATAL("create_postprocess_pipelines: star_surface_swapchain failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // planet_surface -> offscreen + swapchain (premult-over; reuses the star quad vertex shader).
    SDL_GPUShader* vs_planet = load_shader(g_sdl.device, "star_surface", "vert",
                                            SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    if (!vs_planet) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    SDL_GPUShader* fs_planet = load_shader(g_sdl.device, "planet_surface", "frag",
                                            SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!fs_planet) { SDL_ReleaseGPUShader(g_sdl.device, vs); SDL_ReleaseGPUShader(g_sdl.device, vs_planet); return FALSE; }
    g_sdl.pipeline_planetsurface = create_premult_over_postprocess_pipeline(vs_planet, fs_planet, offscreen_fmt);
    if (!g_sdl.pipeline_planetsurface) { BS_LOG_FATAL("create_postprocess_pipelines: planet_surface failed"); SDL_ReleaseGPUShader(g_sdl.device, fs_planet); SDL_ReleaseGPUShader(g_sdl.device, vs_planet); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_planetsurface_swapchain = create_premult_over_postprocess_pipeline(vs_planet, fs_planet, swap_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_planet);
    SDL_ReleaseGPUShader(g_sdl.device, vs_planet);
    if (!g_sdl.pipeline_planetsurface_swapchain) { BS_LOG_FATAL("create_postprocess_pipelines: planet_surface_swapchain failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // composite -> swapchain (must match swapchain format); 4 samplers (scene + bloom + streak + flare)
    SDL_GPUShader* fs_composite = load_shader(g_sdl.device, "bloom_composite", "frag",
                                               SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    if (!fs_composite) { SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_composite = create_postprocess_pipeline(vs, fs_composite, swap_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_composite);
    if (!g_sdl.pipeline_composite) { BS_LOG_FATAL("create_postprocess_pipelines: composite failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    // heat_map -> heat_rt (half-res, premultiply-on-write). Rendered once at half resolution then
    // upscaled + composited (premult-over) via the shared nebula composite pipelines. The shader
    // still returns straight-alpha float4; the premult-write blend premultiplies it into heat_rt.
    SDL_GPUShader* fs_heat_map = load_shader(g_sdl.device, "heat_map", "frag",
                                              SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 1);
    if (!fs_heat_map) { BS_LOG_FATAL("create_postprocess_pipelines: heat_map shader failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }
    g_sdl.pipeline_heat_map_halfres = create_premult_write_postprocess_pipeline(vs, fs_heat_map, offscreen_fmt);
    SDL_ReleaseGPUShader(g_sdl.device, fs_heat_map);
    if (!g_sdl.pipeline_heat_map_halfres) { BS_LOG_FATAL("create_postprocess_pipelines: heat_map halfres pipeline failed"); SDL_ReleaseGPUShader(g_sdl.device, vs); return FALSE; }

    SDL_ReleaseGPUShader(g_sdl.device, vs);
    return TRUE;
}

// Create the engine's 1x1 opaque-white texture. Sprites with an invalid texture handle sample
// this so a single pipeline serves both textured and solid-color draws (white * tint = tint).
static b8 create_white_texture()
{
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width                = 1;
    info.height               = 1;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;

    SDL_GPUTexture* tex = SDL_CreateGPUTexture(g_sdl.device, &info);
    if (!tex) { BS_LOG_FATAL("create_white_texture: SDL_CreateGPUTexture failed: %s", SDL_GetError()); return FALSE; }

    u8 white[4] = { 255, 255, 255, 255 };

    SDL_GPUTransferBufferCreateInfo tinfo;
    SDL_zero(tinfo);
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size  = 4;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_sdl.device, &tinfo);
    if (!tb) { BS_LOG_FATAL("create_white_texture: transfer buffer failed: %s", SDL_GetError()); return FALSE; }

    void* map = SDL_MapGPUTransferBuffer(g_sdl.device, tb, false);
    SDL_memcpy(map, white, 4);
    SDL_UnmapGPUTransferBuffer(g_sdl.device, tb);

    SDL_GPUCommandBuffer* up = SDL_AcquireGPUCommandBuffer(g_sdl.device);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(up);
    SDL_GPUTextureTransferInfo src; SDL_zero(src);
    src.transfer_buffer = tb; src.offset = 0; src.pixels_per_row = 1; src.rows_per_layer = 1;
    SDL_GPUTextureRegion dst; SDL_zero(dst);
    dst.texture = tex; dst.w = 1; dst.h = 1; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(up);
    SDL_ReleaseGPUTransferBuffer(g_sdl.device, tb);

    g_sdl.white_texture = pool_alloc_texture(tex, 1, 1);
    return g_sdl.white_texture.id != BS_INVALID_HANDLE;
}

// Create a radial-gradient circle texture for the sunburst aux-bloom proxy.
// White in the center, transparent at the edges, so it renders as a true circle.
static b8 create_circle_texture()
{
    const int W = 32;
    const int H = 32;
    const int C = 4;
    u8 pixels[W * H * C];

    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            float dx = (x + 0.5f) / (float)W - 0.5f;
            float dy = (y + 0.5f) / (float)H - 0.5f;
            float dist = sqrtf(dx * dx + dy * dy) * 2.0f; // 0..1 from center to edge
            float alpha = 1.0f - clampf(dist, 0.0f, 1.0f);
            alpha *= alpha; // smoother falloff
            u8 a = (u8)(alpha * 255.0f);
            u8* p = &pixels[(y * W + x) * C];
            p[0] = 255; p[1] = 255; p[2] = 255; p[3] = a;
        }
    }

    g_sdl.circle_texture = sdlgpu_backend_create_texture(nullptr, pixels, W, H);
    return g_sdl.circle_texture.id != BS_INVALID_HANDLE;
}

// =====================================================================================
// Backend lifecycle.
// =====================================================================================
b8 sdlgpu_backend_initialize(struct renderer_backend* backend, const char* app_name, struct PlatformState* plat)
{
    (void)app_name;
    SDL_zero(g_sdl);

    backend->internal_state = &g_sdl;

    g_sdl.window = (SDL_Window*)platform_get_window_handle(plat);
    if (!g_sdl.window) { BS_LOG_FATAL("sdlgpu_backend_initialize: null window handle."); return FALSE; }

    g_sdl.clear_color = SDL_FColor{ 0.0f, 0.0f, 0.05f, 1.0f }; // space-black default
    g_sdl.camera      = camera2d_default();

    // No lights => the scene renders fullbright exactly as before until the game calls
    // renderer_set_lights. Ambient defaults to full white (irrelevant while count is 0).
    g_sdl.light_count       = 0;
    g_sdl.light_ambient     = bs_color{ 1.0f, 1.0f, 1.0f, 1.0f };
    g_sdl.light_unlit_layer = 0;

    // Default glow params match hardcoded sprite.frag.hlsl defaults.
    g_sdl.glow_params = bs_glow_params{
        1.0f, 6.0f, 4.0f, 2.5f, 0.80f, 0.08f, 15.0f, 8.0f, 45.0f, 24.0f,
        bs_color{ 1.0f, 0.85f, 0.5f, 1.0f },
        bs_color{ 0.90f, 0.15f, 0.02f, 1.0f },
        bs_color{ 1.0f, 0.45f, 0.05f, 1.0f },
        bs_color{ 1.0f, 0.98f, 0.90f, 1.0f }
    };
    g_sdl.glow_params_set = FALSE;

    // Bloom defaults: disabled by default until the user opts in via the editor panel.
    g_sdl.bloom_enabled   = FALSE;
    g_sdl.bloom_threshold = 1.2f;
    g_sdl.bloom_intensity = 0.3f;
    g_sdl.bloom_width     = 0;
    g_sdl.bloom_height    = 0;

    // Streak defaults: disabled.
    g_sdl.streak_enabled   = FALSE;
    g_sdl.streak_angle     = 0.0f;
    g_sdl.streak_length    = 3.0f;
    g_sdl.streak_intensity = 0.5f;
    g_sdl.streak_source    = bs_math::Vec2{0.0f, 0.0f};
    g_sdl.streak_source_set = FALSE;
    g_sdl.streak_flare_intensity = 0.5f;

    // Aux batch capture off by default.
    g_sdl.aux_bloom_mode  = FALSE;
    g_sdl.aux_batch_count = 0;

    // Create the device (DXIL + SPIR-V requested; SDL picks an available driver).
    g_sdl.device = SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_DXIL | SDL_GPU_SHADERFORMAT_SPIRV, true, NULL);
    if (!g_sdl.device) { BS_LOG_FATAL("sdlgpu_backend_initialize: SDL_CreateGPUDevice failed: %s", SDL_GetError()); return FALSE; }

    if (!SDL_ClaimWindowForGPUDevice(g_sdl.device, g_sdl.window))
    {
        BS_LOG_FATAL("sdlgpu_backend_initialize: SDL_ClaimWindowForGPUDevice failed: %s", SDL_GetError());
        return FALSE;
    }

    BS_LOG_INFO("SDL GPU device created (driver: %s, shader formats: 0x%x).",
        SDL_GetGPUDeviceDriver(g_sdl.device), (u32)SDL_GetGPUShaderFormats(g_sdl.device));

    // Log the ACTUAL display refresh rate the swapchain will pace against. A 30 Hz mode
    // (4K TV over HDMI 1.4, misconfigured display) silently caps a VSYNC swapchain to 30 fps
    // and is invisible to GPU-side profiling — surface it here.
    {
        SDL_DisplayID disp = SDL_GetDisplayForWindow(g_sdl.window);
        const SDL_DisplayMode* dm = disp ? SDL_GetCurrentDisplayMode(disp) : NULL;
        if (dm)
        {
            BS_LOG_INFO("Display %u: %dx%d @ %.2f Hz (VSYNC present paces to this).",
                        (u32)disp, dm->w, dm->h, dm->refresh_rate);
            if (dm->refresh_rate > 0.0f && dm->refresh_rate < 50.0f)
            {
                BS_LOG_WARN("Display refresh is below 50 Hz — a VSYNC swapchain cannot exceed "
                            "%.0f fps on this display.", dm->refresh_rate);
            }
        }
        else
        {
            BS_LOG_WARN("Could not query current display mode: %s", SDL_GetError());
        }
    }

    // Load sprite shaders: vertex has 1 uniform buffer (view_proj); fragment has 1 sampler
    // (sprite texture) and 1 uniform buffer (lights at b0 space3).
    SDL_GPUShader* vs = load_shader(g_sdl.device, "sprite", "vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* fs = load_shader(g_sdl.device, "sprite", "frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vs || !fs) return FALSE;

    SDL_GPUTextureFormat swap_fmt =
        get_corrected_swapchain_format(g_sdl.device, g_sdl.window);

    // Swapchain-format pipelines (direct-to-screen path).
    if (!create_pipelines_for_format(vs, fs, swap_fmt, g_sdl.pipelines)) return FALSE;

    // Offscreen pipelines (bloom path targets scene_rt which is RGBA8).
    if (!create_pipelines_for_format(vs, fs, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, g_sdl.pipelines_offscreen))
        return FALSE;

    // Shaders are baked into the pipelines; release the standalone handles.
    SDL_ReleaseGPUShader(g_sdl.device, vs);
    SDL_ReleaseGPUShader(g_sdl.device, fs);

    // Load 4-map mapped sprite shaders: vertex has 1 UBO (camera), fragment has 4 samplers
    // (diffuse, normal, depth, position) and 1 UBO (directional light).
    SDL_GPUShader* mvs = load_shader(g_sdl.device, "mapped_sprite", "vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* mfs = load_shader(g_sdl.device, "mapped_sprite", "frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    if (!mvs || !mfs) return FALSE;

    if (!create_mapped_pipeline_for_format(mvs, mfs, swap_fmt, &g_sdl.pipeline_mapped)) return FALSE;
    if (!create_mapped_pipeline_for_format(mvs, mfs, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM, &g_sdl.pipeline_mapped_offscreen)) return FALSE;

    SDL_ReleaseGPUShader(g_sdl.device, mvs);
    SDL_ReleaseGPUShader(g_sdl.device, mfs);

    // Sampler: nearest filtering, clamp — crisp pixel-art sprites.
    SDL_GPUSamplerCreateInfo sinfo;
    SDL_zero(sinfo);
    sinfo.min_filter     = SDL_GPU_FILTER_NEAREST;
    sinfo.mag_filter     = SDL_GPU_FILTER_NEAREST;
    sinfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sinfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g_sdl.sampler = SDL_CreateGPUSampler(g_sdl.device, &sinfo);
    if (!g_sdl.sampler) { BS_LOG_FATAL("sdlgpu_backend_initialize: sampler failed: %s", SDL_GetError()); return FALSE; }

    // LINEAR sampler for post-process passes (blur / composite) — smooth interpolation.
    SDL_zero(sinfo);
    sinfo.min_filter     = SDL_GPU_FILTER_LINEAR;
    sinfo.mag_filter     = SDL_GPU_FILTER_LINEAR;
    sinfo.mipmap_mode    = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
    sinfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    g_sdl.sampler_linear = SDL_CreateGPUSampler(g_sdl.device, &sinfo);
    if (!g_sdl.sampler_linear) { BS_LOG_FATAL("sdlgpu_backend_initialize: linear sampler failed: %s", SDL_GetError()); return FALSE; }

    if (!create_batch_resources()) return FALSE;
    if (!create_white_texture())   return FALSE;
    if (!create_circle_texture())  return FALSE;

    // Query the actual window size for initial bloom targets (fallback to 1280x720).
    int win_w = 1280, win_h = 720;
    SDL_GetWindowSizeInPixels(g_sdl.window, &win_w, &win_h);

    if (!create_bloom_targets((u32)win_w, (u32)win_h)) return FALSE;
    if (!create_postprocess_pipelines())  return FALSE;

    BS_LOG_INFO("SDL GPU backend initialized (Phase 3: sprites + bloom).");
    return TRUE;
}

void sdlgpu_backend_shutdown(struct renderer_backend* backend)
{
    (void)backend;
    if (!g_sdl.device) return;

    SDL_WaitForGPUIdle(g_sdl.device);

    for (u32 i = 0; i < BS_MAX_TEXTURES; ++i)
    {
        if (g_sdl.textures[i].in_use && g_sdl.textures[i].tex)
        {
            SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.textures[i].tex);
            g_sdl.textures[i].tex    = NULL;
            g_sdl.textures[i].in_use = FALSE;
        }
    }

    if (g_sdl.sampler)        SDL_ReleaseGPUSampler(g_sdl.device, g_sdl.sampler);
    if (g_sdl.sampler_linear) SDL_ReleaseGPUSampler(g_sdl.device, g_sdl.sampler_linear);
    if (g_sdl.vtransfer)      SDL_ReleaseGPUTransferBuffer(g_sdl.device, g_sdl.vtransfer);
    if (g_sdl.vbuffer)        SDL_ReleaseGPUBuffer(g_sdl.device, g_sdl.vbuffer);
    if (g_sdl.ibuffer)        SDL_ReleaseGPUBuffer(g_sdl.device, g_sdl.ibuffer);
    if (g_sdl.mapped_vtransfer) SDL_ReleaseGPUTransferBuffer(g_sdl.device, g_sdl.mapped_vtransfer);
    if (g_sdl.mapped_vbuffer)   SDL_ReleaseGPUBuffer(g_sdl.device, g_sdl.mapped_vbuffer);
    if (g_sdl.mapped_ibuffer)   SDL_ReleaseGPUBuffer(g_sdl.device, g_sdl.mapped_ibuffer);

    // Bloom resources.
    if (g_sdl.scene_rt)   SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.scene_rt);
    if (g_sdl.bloom_a)    SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.bloom_a);
    if (g_sdl.bloom_b)    SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.bloom_b);
    if (g_sdl.aux_bloom_a) SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.aux_bloom_a);
    if (g_sdl.aux_bloom_b) SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.aux_bloom_b);
    if (g_sdl.nebula_rt)  SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.nebula_rt);
    if (g_sdl.heat_rt)    SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.heat_rt);
    if (g_sdl.ui_backdrop_rt) SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.ui_backdrop_rt);
    if (g_sdl.pipeline_extract)   SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_extract);
    if (g_sdl.pipeline_blur_h)    SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_blur_h);
    if (g_sdl.pipeline_blur_v)    SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_blur_v);
    if (g_sdl.pipeline_streak)    SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_streak);
    if (g_sdl.pipeline_flare)     SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_flare);
    if (g_sdl.pipeline_starfield_layer)          SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_starfield_layer);
    if (g_sdl.pipeline_starfield_layer_swapchain) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_starfield_layer_swapchain);
    if (g_sdl.pipeline_nebula_halfres)             SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_nebula_halfres);
    if (g_sdl.pipeline_nebula_composite)           SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_nebula_composite);
    if (g_sdl.pipeline_nebula_composite_swapchain) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_nebula_composite_swapchain);
    if (g_sdl.pipeline_sunburst)          SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_sunburst);
    if (g_sdl.pipeline_sunburst_swapchain) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_sunburst_swapchain);
    if (g_sdl.pipeline_starsurface)          SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_starsurface);
    if (g_sdl.pipeline_starsurface_swapchain) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_starsurface_swapchain);
    if (g_sdl.pipeline_planetsurface)          SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_planetsurface);
    if (g_sdl.pipeline_planetsurface_swapchain) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_planetsurface_swapchain);
    if (g_sdl.pipeline_composite) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_composite);
    if (g_sdl.pipeline_heat_map_halfres) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_heat_map_halfres);
    if (g_sdl.pipeline_mapped) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_mapped);
    if (g_sdl.pipeline_mapped_offscreen) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipeline_mapped_offscreen);

    for (u32 m = 0; m < BLEND_MODE_COUNT; ++m)
    {
        if (g_sdl.pipelines[m])          SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipelines[m]);
        if (g_sdl.pipelines_offscreen[m]) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipelines_offscreen[m]);
    }

    SDL_ReleaseWindowFromGPUDevice(g_sdl.device, g_sdl.window);
    SDL_DestroyGPUDevice(g_sdl.device);
    SDL_zero(g_sdl);

    BS_LOG_INFO("SDL GPU backend shut down.");
}

void sdlgpu_backend_on_resize(struct renderer_backend* backend, u16 width, u16 height)
{
    (void)backend;
    BS_LOG_DEBUG("sdlgpu_backend_on_resize: %ux%u.", (u32)width, (u32)height);

    // Recreate bloom targets if the window size changed.
    if (g_sdl.bloom_width != (u32)width || g_sdl.bloom_height != (u32)height)
    {
        SDL_WaitForGPUIdle(g_sdl.device);

        if (g_sdl.scene_rt) SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.scene_rt);
        if (g_sdl.bloom_a)  SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.bloom_a);
        if (g_sdl.bloom_b)  SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.bloom_b);
        if (g_sdl.aux_bloom_a) SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.aux_bloom_a);
        if (g_sdl.aux_bloom_b) SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.aux_bloom_b);
        if (g_sdl.nebula_rt)  SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.nebula_rt);
        if (g_sdl.heat_rt)    SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.heat_rt);
        if (g_sdl.ui_backdrop_rt) SDL_ReleaseGPUTexture(g_sdl.device, g_sdl.ui_backdrop_rt);
        g_sdl.scene_rt = NULL;
        g_sdl.bloom_a  = NULL;
        g_sdl.bloom_b  = NULL;
        g_sdl.aux_bloom_a = NULL;
        g_sdl.aux_bloom_b = NULL;
        g_sdl.nebula_rt   = NULL;
        g_sdl.heat_rt     = NULL;
        g_sdl.ui_backdrop_rt = NULL;

        if (!create_bloom_targets((u32)width, (u32)height))
            BS_LOG_ERROR("on_resize: failed to recreate bloom targets (%ux%u).", (u32)width, (u32)height);
    }
}

void sdlgpu_backend_set_clear_color(struct renderer_backend* backend, bs_color color)
{
    (void)backend;
    g_sdl.clear_color = SDL_FColor{ color.r, color.g, color.b, color.a };
}

// =====================================================================================
// Per-frame.
//
// begin_frame: acquire a command buffer only. The swapchain image and render pass are deferred
// to end_frame so the sprite vertex copy-pass can run BEFORE any render pass begins (SDL3 GPU
// forbids copy passes inside a render pass).
// =====================================================================================
b8 sdlgpu_backend_begin_frame(struct renderer_backend* backend, f32 dt)
{
    (void)backend; (void)dt;

    g_sdl.cmd = SDL_AcquireGPUCommandBuffer(g_sdl.device);
    if (!g_sdl.cmd) { BS_LOG_ERROR("begin_frame: SDL_AcquireGPUCommandBuffer failed: %s", SDL_GetError()); return FALSE; }

    g_sdl.swapchain_texture = NULL;
    g_sdl.pass              = NULL;
    g_sdl.batch_count       = 0; // reset CPU batch; the game refills it via draw_sprite
    g_sdl.mapped_batch_count = 0; // reset mapped sprite queue
    g_sdl.starfield_queue_count = 0; // reset starfield layer queue
    g_sdl.sunburst_queue_count  = 0; // reset sunburst star queue
    g_sdl.starsurface_queue_count = 0; // reset star surface queue
    g_sdl.planetsurface_queue_count = 0; // reset planet surface queue
    g_sdl.heat_map_set          = FALSE; // reset heat map; game must re-submit each frame
    g_sdl.nebula_set            = FALSE; // reset nebula layer; game must re-submit each frame
    g_sdl.streak_source_set     = FALSE; // source must be resubmitted each frame

    // Begin the ImGui frame here, AFTER a command buffer is secured, so the game can build
    // its UI during render(dt) (which runs between begin_frame and end_frame). The matching
    // ImGui::Render() is issued at the top of end_frame, which the frame_active gate in
    // renderer.cpp guarantees runs for every successful begin_frame — keeping NewFrame and
    // Render balanced even on the minimized/NULL-swapchain early-out.
    if (g_sdl.imgui_active)
    {
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
    }

    return TRUE;
}

// =====================================================================================
// Texture create / destroy.
// =====================================================================================
// Uploads RGBA8 pixel data to an already-created SDL GPU texture. Handles the transient
// upload transfer buffer and command buffer. Returns FALSE if any GPU resource fails.
static b8 sdlgpu_upload_texture_pixels(SDL_GPUTexture* tex, const u8* pixels, u32 width, u32 height, const char* ctx)
{
    u32 byte_count = width * height * 4u;

    SDL_GPUTransferBufferCreateInfo tinfo;
    SDL_zero(tinfo);
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size  = byte_count;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_sdl.device, &tinfo);
    if (!tb) { BS_LOG_ERROR("%s: transfer buffer failed: %s", ctx, SDL_GetError()); return FALSE; }

    void* map = SDL_MapGPUTransferBuffer(g_sdl.device, tb, false);
    SDL_memcpy(map, pixels, byte_count);
    SDL_UnmapGPUTransferBuffer(g_sdl.device, tb);

    SDL_GPUCommandBuffer* up = SDL_AcquireGPUCommandBuffer(g_sdl.device);
    if (!up) { BS_LOG_ERROR("%s: command buffer failed: %s", ctx, SDL_GetError()); SDL_ReleaseGPUTransferBuffer(g_sdl.device, tb); return FALSE; }

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(up);
    SDL_GPUTextureTransferInfo src; SDL_zero(src);
    src.transfer_buffer = tb; src.offset = 0; src.pixels_per_row = width; src.rows_per_layer = height;
    SDL_GPUTextureRegion dst; SDL_zero(dst);
    dst.texture = tex; dst.w = width; dst.h = height; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(up);
    SDL_ReleaseGPUTransferBuffer(g_sdl.device, tb);

    return TRUE;
}

bs_texture sdlgpu_backend_create_texture(struct renderer_backend* backend, const u8* pixels, u32 width, u32 height)
{
    (void)backend;
    bs_texture invalid; invalid.id = BS_INVALID_HANDLE;

    if (!pixels || width == 0 || height == 0) { BS_LOG_ERROR("create_texture: invalid args."); return invalid; }

    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width                = width;
    info.height               = height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;

    SDL_GPUTexture* tex = SDL_CreateGPUTexture(g_sdl.device, &info);
    if (!tex) { BS_LOG_ERROR("create_texture: SDL_CreateGPUTexture failed: %s", SDL_GetError()); return invalid; }

    if (!sdlgpu_upload_texture_pixels(tex, pixels, width, height, "create_texture"))
    {
        SDL_ReleaseGPUTexture(g_sdl.device, tex);
        return invalid;
    }

    bs_texture handle = pool_alloc_texture(tex, width, height);
    if (handle.id == BS_INVALID_HANDLE) { SDL_ReleaseGPUTexture(g_sdl.device, tex); return invalid; }

    BS_LOG_DEBUG("create_texture: %ux%u -> id 0x%x.", width, height, handle.id);
    return handle;
}

b8 sdlgpu_backend_update_texture(struct renderer_backend* backend, bs_texture texture, const u8* pixels, u32 width, u32 height)
{
    (void)backend;
    if (!pixels || width == 0 || height == 0) { BS_LOG_ERROR("update_texture: invalid args."); return FALSE; }

    gpu_texture* slot = pool_resolve_texture(texture);
    if (!slot) { BS_LOG_ERROR("update_texture: invalid texture handle."); return FALSE; }
    if (slot->width != width || slot->height != height)
    {
        BS_LOG_ERROR("update_texture: size mismatch (handle %ux%u, update %ux%u).", slot->width, slot->height, width, height);
        return FALSE;
    }

    return sdlgpu_upload_texture_pixels(slot->tex, pixels, width, height, "update_texture");
}

void sdlgpu_backend_destroy_texture(struct renderer_backend* backend, bs_texture texture)
{
    (void)backend;
    gpu_texture* slot = pool_resolve_texture(texture);
    if (!slot) return;

    SDL_WaitForGPUIdle(g_sdl.device);
    SDL_ReleaseGPUTexture(g_sdl.device, slot->tex);
    slot->tex    = NULL;
    slot->in_use = FALSE;
    slot->generation++; // invalidate any stale handles still holding the old generation
    if (slot->generation == 0) slot->generation = 1;
}

void sdlgpu_backend_set_camera(struct renderer_backend* backend, Camera2D camera)
{
    (void)backend;
    g_sdl.camera = camera;
}

void sdlgpu_backend_set_lights(struct renderer_backend* backend, const bs_light2d* lights,
                               u32 count, bs_color ambient, u32 unlit_layer)
{
    (void)backend;
    if (count > BS_BACKEND_MAX_LIGHTS) count = BS_BACKEND_MAX_LIGHTS;
    g_sdl.light_count = count;
    for (u32 i = 0; i < count; ++i) g_sdl.lights[i] = lights[i];
    g_sdl.light_ambient     = ambient;
    g_sdl.light_unlit_layer = unlit_layer;
}

void sdlgpu_backend_set_glow_params(struct renderer_backend* backend, const bs_glow_params* params)
{
    (void)backend;
    if (params)
    {
        g_sdl.glow_params = *params;
        g_sdl.glow_params_set = TRUE;
    }
    else
    {
        // Reset to defaults
        g_sdl.glow_params = bs_glow_params{
            1.0f, 6.0f, 4.0f, 2.5f, 0.80f, 0.08f, 15.0f, 8.0f, 45.0f, 24.0f,
            bs_color{ 1.0f, 0.85f, 0.5f, 1.0f },
            bs_color{ 0.90f, 0.15f, 0.02f, 1.0f },
            bs_color{ 1.0f, 0.45f, 0.05f, 1.0f },
            bs_color{ 1.0f, 0.98f, 0.90f, 1.0f }
        };
        g_sdl.glow_params_set = FALSE;
    }
}

void sdlgpu_backend_set_bloom_enabled(struct renderer_backend* backend, b8 enabled)
{
    (void)backend;
    g_sdl.bloom_enabled = enabled;
}

void sdlgpu_backend_set_bloom_params(struct renderer_backend* backend, f32 threshold, f32 intensity)
{
    (void)backend;
    g_sdl.bloom_threshold = threshold;
    g_sdl.bloom_intensity = intensity;
}

void sdlgpu_backend_set_streak_enabled(struct renderer_backend* backend, b8 enabled)
{
    (void)backend;
    g_sdl.streak_enabled = enabled;
}

void sdlgpu_backend_set_streak_params(struct renderer_backend* backend, f32 angle, f32 length)
{
    (void)backend;
    g_sdl.streak_angle  = angle;
    g_sdl.streak_length = length;
}

void sdlgpu_backend_set_streak_intensity(struct renderer_backend* backend, f32 intensity)
{
    (void)backend;
    g_sdl.streak_intensity = intensity;
}

void sdlgpu_backend_set_streak_source(struct renderer_backend* backend, bs_math::Vec2 screen_pos)
{
    (void)backend;
    g_sdl.streak_source = screen_pos;
    g_sdl.streak_source_set = TRUE;
}

void sdlgpu_backend_set_streak_flare_intensity(struct renderer_backend* backend, f32 intensity)
{
    (void)backend;
    g_sdl.streak_flare_intensity = intensity;
}

void sdlgpu_backend_set_aux_bloom_mode(struct renderer_backend* backend, b8 enabled)
{
    (void)backend;
    g_sdl.aux_bloom_mode = enabled;
    if (enabled) g_sdl.aux_batch_count = 0;
}

void sdlgpu_backend_draw_starfield(struct renderer_backend* backend, const bs_starfield_params* params)
{
    (void)backend;
    if (!params) return;

    if (g_sdl.starfield_queue_count >= BS_MAX_STARFIELD_LAYERS)
    {
        // Queue is a fixed-size safety net; the caller is expected to submit within budget.
        // Silently drop extras (logging here spams every frame).
        return;
    }
    auto& slot = g_sdl.starfield_queue[g_sdl.starfield_queue_count++];
    slot.active  = TRUE;
    slot.params  = *params;
}

void sdlgpu_backend_draw_sunburst(struct renderer_backend* backend, const bs_sunburst_params* params)
{
    (void)backend;
    if (!params) return;

    if (g_sdl.sunburst_queue_count >= BS_MAX_SUNBURST_STARS)
    {
        // Queue is a fixed-size safety net; the caller caps submissions to the brightest stars.
        // Silently drop extras (logging here spams every frame).
        return;
    }
    auto& slot = g_sdl.sunburst_queue[g_sdl.sunburst_queue_count++];
    slot.active  = TRUE;
    slot.params  = *params;

    // Aux-bloom proxy: the sunburst shader is not captured by the sprite batch, so emit a
    // bright additive proxy sprite into the aux batch when this star should streak.
    if (params->aux_bloom && g_sdl.aux_bloom_mode && g_sdl.aux_batch_count < BS_MAX_SPRITES)
    {
        bs_sprite proxy{};
        proxy.position  = bs_math::Vec2{ params->aux_bloom_world_pos.x, params->aux_bloom_world_pos.y };
        // body_radius is in screen pixels; proxy sprite size is in world units.
        f32 world_body  = params->body_radius / g_sdl.camera.zoom;
        proxy.size      = bs_math::Vec2{ world_body * 2.0f, world_body * 2.0f };
        proxy.origin    = bs_math::Vec2{ 0.5f, 0.5f };
        proxy.rotation  = 0.0f;
        proxy.uv        = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        proxy.tint      = params->color;
        proxy.tint.a    = params->visibility;
        proxy.texture   = g_sdl.circle_texture; // true circular shape
        proxy.blend     = BLEND_ADDITIVE;
        proxy.layer     = params->layer;
        proxy.glow_override = nullptr; // no extra glow; the texture is the shape

        u32 tex_index = (g_sdl.circle_texture.id & 0x3FFFFu);
        u32 sort_key  = make_sort_key(proxy.layer, proxy.blend, tex_index);

        batched_sprite* a = &g_sdl.aux_batch[g_sdl.aux_batch_count++];
        a->sprite   = proxy;
        a->sort_key = sort_key;
    }
}

void sdlgpu_backend_draw_starsurface(struct renderer_backend* backend, const bs_starsurface_params* params)
{
    (void)backend;
    if (!params) return;

    if (g_sdl.starsurface_queue_count >= BS_MAX_STARSURFACE_STARS)
        return; // fixed-size safety net; silently drop extras

    auto& slot = g_sdl.starsurface_queue[g_sdl.starsurface_queue_count++];
    slot.active = TRUE;
    slot.params = *params;

    // Aux-bloom proxy: the surface shader is not captured by the sprite batch, so emit a bright
    // additive proxy sprite into the aux batch when this star should streak (mirrors sunburst).
    if (params->aux_bloom && g_sdl.aux_bloom_mode && g_sdl.aux_batch_count < BS_MAX_SPRITES)
    {
        bs_sprite proxy{};
        proxy.position  = bs_math::Vec2{ params->aux_bloom_world_pos.x, params->aux_bloom_world_pos.y };
        f32 world_body  = params->body_radius / g_sdl.camera.zoom;
        proxy.size      = bs_math::Vec2{ world_body * 2.0f, world_body * 2.0f };
        proxy.origin    = bs_math::Vec2{ 0.5f, 0.5f };
        proxy.rotation  = 0.0f;
        proxy.uv        = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        proxy.tint      = params->color;
        proxy.tint.a    = params->visibility;
        proxy.texture   = g_sdl.circle_texture;
        proxy.blend     = BLEND_ADDITIVE;
        proxy.layer     = params->layer;
        proxy.glow_override = nullptr;

        u32 tex_index = (g_sdl.circle_texture.id & 0x3FFFFu);
        u32 sort_key  = make_sort_key(proxy.layer, proxy.blend, tex_index);

        batched_sprite* a = &g_sdl.aux_batch[g_sdl.aux_batch_count++];
        a->sprite   = proxy;
        a->sort_key = sort_key;
    }
}

void sdlgpu_backend_draw_planetsurface(struct renderer_backend* backend, const bs_planetsurface_params* params)
{
    (void)backend;
    if (!params) return;
    if (g_sdl.planetsurface_queue_count >= BS_MAX_PLANETSURFACE)
        return; // fixed-size safety net; silently drop extras
    auto& slot = g_sdl.planetsurface_queue[g_sdl.planetsurface_queue_count++];
    slot.active = TRUE;
    slot.params = *params;
}

void sdlgpu_backend_draw_heat_map(struct renderer_backend* backend, const bs_heat_map_params* params)
{
    (void)backend;
    if (!params) return;
    g_sdl.heat_map_params = *params;
    g_sdl.heat_map_set    = TRUE;
}
void sdlgpu_backend_draw_nebula(struct renderer_backend* backend, const bs_nebula_params* params)
{
    (void)backend;
    if (!params) return;
    g_sdl.nebula_params = *params;
    g_sdl.nebula_set    = TRUE;
}

void sdlgpu_backend_draw_sprite(struct renderer_backend* backend, const bs_sprite* sprite)
{
    (void)backend;
    if (!sprite) return;
    if (g_sdl.batch_count >= BS_MAX_SPRITES)
    {
        BS_LOG_WARN("draw_sprite: batch full (%u); dropping sprite.", (u32)BS_MAX_SPRITES);
        return;
    }

    // Resolve the texture to a pool index for the sort key (id 0 / stale => white texture).
    gpu_texture* slot = pool_resolve_texture(sprite->texture);
    u32 tex_index;
    if (slot)
    {
        tex_index = (sprite->texture.id & 0x3FFFFu);
    }
    else
    {
        tex_index = (g_sdl.white_texture.id & 0x3FFFFu);
    }

    u32 sort_key = make_sort_key(sprite->layer, sprite->blend, tex_index);

    batched_sprite* b = &g_sdl.batch[g_sdl.batch_count++];
    b->sprite   = *sprite;
    b->sort_key = sort_key;

    // Aux batch capture: when active, also push to the auxiliary batch for streak processing.
    if (g_sdl.aux_bloom_mode && g_sdl.aux_batch_count < BS_MAX_SPRITES)
    {
        batched_sprite* a = &g_sdl.aux_batch[g_sdl.aux_batch_count++];
        a->sprite   = *sprite;
        a->sort_key = sort_key;
    }
}

void sdlgpu_backend_draw_mapped_sprite(struct renderer_backend* backend, const bs_mapped_sprite* sprite)
{
    (void)backend;
    if (!sprite) return;
    if (g_sdl.mapped_batch_count >= BS_MAX_MAPPED_SPRITES)
    {
        BS_LOG_WARN("draw_mapped_sprite: batch full (%u); dropping sprite.", (u32)BS_MAX_MAPPED_SPRITES);
        return;
    }
    g_sdl.mapped_batch[g_sdl.mapped_batch_count++] = *sprite;
}

void sdlgpu_backend_get_frame_stats(struct renderer_backend* backend, bs_frame_stats* out_stats)
{
    (void)backend;
    if (!out_stats) return;
    *out_stats = g_sdl.last_stats;
}

b8 sdlgpu_backend_set_present_mode(struct renderer_backend* backend, b8 immediate)
{
    (void)backend;
    if (!g_sdl.device || !g_sdl.window) return FALSE;

    SDL_GPUPresentMode mode = immediate ? SDL_GPU_PRESENTMODE_IMMEDIATE : SDL_GPU_PRESENTMODE_VSYNC;

    // IMMEDIATE is optional per SDL; fall back to VSYNC if the driver does not support it.
    if (mode == SDL_GPU_PRESENTMODE_IMMEDIATE &&
        !SDL_WindowSupportsGPUPresentMode(g_sdl.device, g_sdl.window, SDL_GPU_PRESENTMODE_IMMEDIATE))
    {
        BS_LOG_WARN("IMMEDIATE present mode unsupported by this driver; staying on VSYNC.");
        return FALSE;
    }

    if (!SDL_SetGPUSwapchainParameters(g_sdl.device, g_sdl.window,
                                       SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
    {
        BS_LOG_WARN("SDL_SetGPUSwapchainParameters failed: %s", SDL_GetError());
        return FALSE;
    }

    BS_LOG_INFO("Present mode set to %s.", immediate ? "IMMEDIATE" : "VSYNC");
    return TRUE;
}

void sdlgpu_backend_get_present_timing(struct renderer_backend* backend, f32* out_acquire_ms, f32* out_submit_ms)
{
    (void)backend;
    if (out_acquire_ms) *out_acquire_ms = g_sdl.last_acquire_ms;
    if (out_submit_ms)  *out_submit_ms  = g_sdl.last_submit_ms;
}

// =====================================================================================
// Dear ImGui facade (renderer/bs_imgui.h). Implemented HERE because this is the only TU that
// includes <SDL3/...> and the ImGui SDL/SDL_GPU backends, and because it needs direct access
// to the backend globals (g_sdl.device/window/cmd/pass). The per-frame NewFrame/Render/Prepare/
// Record calls are sequenced inside begin_frame/end_frame above; only init/shutdown/event/focus
// are exposed across the SDL-free seam.
// =====================================================================================
b8 bs_imgui_initialize(void)
{
    if (g_sdl.imgui_active) { BS_LOG_WARN("bs_imgui_initialize called twice; ignoring."); return TRUE; }
    if (!g_sdl.device || !g_sdl.window)
    {
        BS_LOG_ERROR("bs_imgui_initialize: GPU device/window not ready (call after backend init).");
        return FALSE;
    }

    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext())
    {
        BS_LOG_ERROR("bs_imgui_initialize: ImGui::CreateContext failed.");
        return FALSE;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = NULL; // don't litter the working dir with imgui.ini

    // Load monospace HUD font from system fonts (Consolas).
    g_sdl.hud_font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\consola.ttf", 14.0f, NULL, io.Fonts->GetGlyphRangesDefault());
    if (!g_sdl.hud_font)
        BS_LOG_WARN("bs_imgui_initialize: Consolas font not found (C:\\Windows\\Fonts\\consola.ttf). HUD will use default font.");

    if (!ImGui_ImplSDL3_InitForSDLGPU(g_sdl.window))
    {
        BS_LOG_ERROR("bs_imgui_initialize: ImGui_ImplSDL3_InitForSDLGPU failed.");
        ImGui::DestroyContext();
        return FALSE;
    }

    ImGui_ImplSDLGPU3_InitInfo init_info;
    SDL_zero(init_info);
    init_info.Device            = g_sdl.device;
    init_info.ColorTargetFormat = get_corrected_swapchain_format(g_sdl.device, g_sdl.window);
    init_info.MSAASamples       = SDL_GPU_SAMPLECOUNT_1;

    if (!ImGui_ImplSDLGPU3_Init(&init_info))
    {
        BS_LOG_ERROR("bs_imgui_initialize: ImGui_ImplSDLGPU3_Init failed.");
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
        return FALSE;
    }

    g_sdl.imgui_active = TRUE;
    BS_LOG_INFO("Dear ImGui initialized (SDL3 + SDL_GPU backends, swapchain format 0x%x).",
        (u32)init_info.ColorTargetFormat);
    return TRUE;
}

void bs_imgui_shutdown(void)
{
    if (!g_sdl.imgui_active) return;

    // The SDL_GPU ImGui backend owns GPU resources on g_sdl.device; drain the GPU before tearing
    // them down, and do it BEFORE the backend destroys the device (renderer.cpp ordering).
    if (g_sdl.device) SDL_WaitForGPUIdle(g_sdl.device);

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();

    g_sdl.imgui_active = FALSE;
    BS_LOG_INFO("Dear ImGui shut down.");
}

void bs_imgui_process_event(const void* sdl_event)
{
    if (!g_sdl.imgui_active || !sdl_event) return;
    ImGui_ImplSDL3_ProcessEvent((const SDL_Event*)sdl_event);
}

b8 bs_imgui_wants_mouse(void)
{
    if (!g_sdl.imgui_active) return FALSE;
    return ImGui::GetIO().WantCaptureMouse ? TRUE : FALSE;
}

b8 bs_imgui_wants_keyboard(void)
{
    if (!g_sdl.imgui_active) return FALSE;
    return ImGui::GetIO().WantCaptureKeyboard ? TRUE : FALSE;
}

void* bs_imgui_get_hud_font(void)
{
    return g_sdl.hud_font;
}

// =====================================================================================
// end_frame: flush the sprite batch.
//   1. sort the CPU batch by (layer, blend, texture)
//   2. build 4 world-space corner vertices per sprite on the CPU
//   3. upload them into the dynamic vertex buffer via a copy pass (BEFORE any render pass)
//   4. acquire the swapchain image, open one render pass
//   5. for each contiguous (blend, texture) run: bind pipeline + sampler/texture, push the
//      view-proj uniform, draw the run's indexed quads
//   6. submit
// =====================================================================================
b8 sdlgpu_backend_end_frame(struct renderer_backend* backend, f32 dt)
{
    (void)backend; (void)dt;

    // ---- Slow-present forensics: per-section CPU timers across end_frame. On machines where
    // Prsnt is large but Acqr/Sbmt are small, the stall hides in one of these sections; a
    // throttled WARN below prints the breakdown so the culprit is identifiable from the log.
    const f64 perf_to_ms = 1000.0 / (f64)SDL_GetPerformanceFrequency();
    u64 ef_t0 = SDL_GetPerformanceCounter();
    f32 tm_imgui_render = 0.0f, tm_build_upload = 0.0f, tm_imgui_prepare = 0.0f,
        tm_record = 0.0f, tm_rml = 0.0f;

    // Finalize ImGui's draw data for this frame. ImGui::Render() is CPU-only (it does not touch
    // the GPU), and it MUST run for every begin_frame that issued NewFrame to keep the pair
    // balanced — including the failed-acquire and minimized early-outs below, where the draw
    // data is simply never recorded into a render pass. The GPU-side upload (PrepareDrawData)
    // and record (RenderDrawData) happen later, only when there is a valid swapchain target.
    ImDrawData* imgui_draw_data = NULL;
    if (g_sdl.imgui_active)
    {
        ImGui::Render();
        imgui_draw_data = ImGui::GetDrawData();
    }
    tm_imgui_render = (f32)((f64)(SDL_GetPerformanceCounter() - ef_t0) * perf_to_ms);

    if (!g_sdl.cmd) return FALSE;

    // Sort the batch for run-friendly draw order.
    if (g_sdl.batch_count > 1)
        SDL_qsort(g_sdl.batch, g_sdl.batch_count, sizeof(batched_sprite), batch_compare);

    // Sort aux batch too (if any).
    u32 aux_count = g_sdl.aux_batch_count;
    if (aux_count > 1)
        SDL_qsort(g_sdl.aux_batch, aux_count, sizeof(batched_sprite), batch_compare);

    // ---- 2 & 3: build vertices and upload (only if we have sprites) ----
    if (g_sdl.batch_count > 0)
    {
        sprite_vertex* verts = (sprite_vertex*)SDL_MapGPUTransferBuffer(g_sdl.device, g_sdl.vtransfer, true);

        // Write aux-batch vertices first (if any), then regular-batch vertices.
        u32 aux_count     = g_sdl.aux_batch_count;
        u32 total_sprites = aux_count + g_sdl.batch_count;

        for (u32 pass = 0; pass < 2; ++pass)
        {
            u32 count  = (pass == 0) ? aux_count : g_sdl.batch_count;
            u32 vbase  = (pass == 0) ? 0 : aux_count;
            batched_sprite* src_sprites = (pass == 0) ? g_sdl.aux_batch : g_sdl.batch;

            for (u32 i = 0; i < count; ++i)
            {
                const bs_sprite* s = &src_sprites[i].sprite;

                f32 ox = s->origin.x * s->size.x;
                f32 oy = s->origin.y * s->size.y;

                Vec2 corners[4];
                corners[0] = Vec2{ -ox,            oy              };
                corners[1] = Vec2{ s->size.x - ox, oy              };
                corners[2] = Vec2{ s->size.x - ox, oy - s->size.y  };
                corners[3] = Vec2{ -ox,            oy - s->size.y  };

                f32 u0 = s->uv.x,            v0 = s->uv.y;
                f32 u1 = s->uv.x + s->uv.w,  v1 = s->uv.y + s->uv.h;
                f32 uvs[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };

                for (u32 c = 0; c < 4; ++c)
                {
                    Vec2 r = vec2_rotate(corners[c], s->rotation);
                    sprite_vertex* vtx = &verts[(vbase + i) * 4u + c];
                    vtx->x = s->position.x + r.x;
                    vtx->y = s->position.y + r.y;
                    vtx->u = uvs[c][0];
                    vtx->v = uvs[c][1];
                    vtx->r  = s->tint.r;
                    vtx->g  = s->tint.g;
                    vtx->b  = s->tint.b;
                    vtx->a  = s->tint.a;
                    vtx->cr = s->custom.r;
                    vtx->cg = s->custom.g;
                    vtx->cb = s->custom.b;
                    vtx->ca = s->custom.a;
                }
            }
        }

        SDL_UnmapGPUTransferBuffer(g_sdl.device, g_sdl.vtransfer);

        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_sdl.cmd);
        SDL_GPUTransferBufferLocation src; SDL_zero(src);
        src.transfer_buffer = g_sdl.vtransfer; src.offset = 0;
        SDL_GPUBufferRegion dst; SDL_zero(dst);
        dst.buffer = g_sdl.vbuffer; dst.offset = 0;
        dst.size   = sizeof(sprite_vertex) * 4u * total_sprites;
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        SDL_EndGPUCopyPass(cp);
    }

    // ---- 2b & 3b: build and upload mapped sprite vertices ----
    if (g_sdl.mapped_batch_count > 0)
    {
        mapped_vertex* mverts = (mapped_vertex*)SDL_MapGPUTransferBuffer(g_sdl.device, g_sdl.mapped_vtransfer, true);
        for (u32 i = 0; i < g_sdl.mapped_batch_count; ++i)
        {
            const bs_mapped_sprite& s = g_sdl.mapped_batch[i];
            f32 ox = s.origin.x * s.size.x;
            f32 oy = s.origin.y * s.size.y;
            Vec2 corners[4] = {
                Vec2{ -ox,            oy             },
                Vec2{ s.size.x - ox,  oy             },
                Vec2{ s.size.x - ox,  oy - s.size.y },
                Vec2{ -ox,            oy - s.size.y }
            };
            f32 u0 = s.uv.x,           v0 = s.uv.y;
            f32 u1 = s.uv.x + s.uv.w,  v1 = s.uv.y + s.uv.h;
            f32 uvs[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };
            for (u32 c = 0; c < 4; ++c)
            {
                Vec2 r = vec2_rotate(corners[c], s.rotation);
                mapped_vertex* vtx = &mverts[i * 4u + c];
                vtx->x = s.position.x + r.x;
                vtx->y = s.position.y + r.y;
                vtx->u = uvs[c][0];
                vtx->v = uvs[c][1];
                vtx->wx = s.position.x;
                vtx->wy = s.position.y;
                vtx->wz = 0.0f;
                vtx->angle = s.rotation;
            }
        }
        SDL_UnmapGPUTransferBuffer(g_sdl.device, g_sdl.mapped_vtransfer);

        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_sdl.cmd);
        SDL_GPUTransferBufferLocation src; SDL_zero(src);
        src.transfer_buffer = g_sdl.mapped_vtransfer; src.offset = 0;
        SDL_GPUBufferRegion dst; SDL_zero(dst);
        dst.buffer = g_sdl.mapped_vbuffer; dst.offset = 0;
        dst.size   = sizeof(mapped_vertex) * 4u * g_sdl.mapped_batch_count;
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        SDL_EndGPUCopyPass(cp);
    }

    // ---- 4: acquire the swapchain image (may be NULL when minimized -> skip drawing) ----
    // Timed separately: under VSYNC this call BLOCKS until the driver frees a swapchain image,
    // so vblank pacing / swapchain starvation shows up here as pure CPU wait, not GPU work.
    u64 acq_t0 = SDL_GetPerformanceCounter();
    tm_build_upload = (f32)((f64)(acq_t0 - ef_t0) * perf_to_ms) - tm_imgui_render;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
            g_sdl.cmd, g_sdl.window, &g_sdl.swapchain_texture, &g_sdl.swap_width, &g_sdl.swap_height))
    {
        BS_LOG_ERROR("end_frame: SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(g_sdl.cmd);
        g_sdl.cmd = NULL;
        return FALSE;
    }
    g_sdl.last_acquire_ms =
        (f32)((f64)(SDL_GetPerformanceCounter() - acq_t0) * 1000.0 / (f64)SDL_GetPerformanceFrequency());

    if (!g_sdl.swapchain_texture)
    {
        // Window minimized; nothing to present. Submit to release the command buffer.
        SDL_SubmitGPUCommandBuffer(g_sdl.cmd);
        g_sdl.cmd = NULL;
        return TRUE;
    }

    // Upload ImGui's vertex/index data for this frame. PrepareDrawData runs its OWN copy pass
    // internally, so it MUST be called here — after the swapchain image is acquired but BEFORE
    // the render pass opens (SDL3 GPU forbids copy passes inside a render pass). Skipped when
    // there are no draws (e.g. nothing built any UI this frame).
    if (g_sdl.imgui_active && imgui_draw_data && imgui_draw_data->TotalVtxCount > 0)
    {
        u64 prep_t0 = SDL_GetPerformanceCounter();
        ImGui_ImplSDLGPU3_PrepareDrawData(imgui_draw_data, g_sdl.cmd);
        tm_imgui_prepare = (f32)((f64)(SDL_GetPerformanceCounter() - prep_t0) * perf_to_ms);
    }
    u64 rec_t0 = SDL_GetPerformanceCounter();

    // ---- Helper: draw a sub-range of a sprite batch into the CURRENT render pass ----
    auto draw_sprite_batch = [&](b8 render_to_offscreen, batched_sprite* sprites, u32 sprite_count, u32 index_offset, u32 batch_start, u32 batch_end) -> u32
    {
        u32 calls = 0;
        if (batch_start >= batch_end || batch_start >= sprite_count) return calls;
        batch_end = (batch_end > sprite_count) ? sprite_count : batch_end;

        Mat4 view_proj = camera2d_view_proj(&g_sdl.camera, (u16)g_sdl.swap_width, (u16)g_sdl.swap_height);

        gpu_lights lit;
        SDL_zero(lit);
        lit.params[0]  = (f32)g_sdl.light_count;
        lit.params[1]  = 0.0f;
        lit.params[2]  = (f32)g_sdl.swap_width;
        lit.params[3]  = (f32)g_sdl.swap_height;
        lit.ambient[0] = g_sdl.light_ambient.r;
        lit.ambient[1] = g_sdl.light_ambient.g;
        lit.ambient[2] = g_sdl.light_ambient.b;
        for (u32 i = 0; i < g_sdl.light_count; ++i)
        {
            const bs_light2d& L = g_sdl.lights[i];
            lit.pos_radius[i][0] = L.position.x;
            lit.pos_radius[i][1] = L.position.y;
            lit.pos_radius[i][2] = L.radius;
            lit.pos_radius[i][3] = L.intensity;
            lit.color[i][0]      = L.color.r;
            lit.color[i][1]      = L.color.g;
            lit.color[i][2]      = L.color.b;
            lit.color[i][3]      = L.enabled ? 1.0f : 0.0f;
        }

        auto fill_glow = [&](const bs_glow_params* gp) {
            const bs_glow_params& p = gp ? *gp : g_sdl.glow_params;
            lit.glow[0][0] = p.intensity;    lit.glow[0][1] = p.falloff;
            lit.glow[0][2] = p.head_mult;    lit.glow[0][3] = p.head_falloff;
            lit.glow[1][0] = p.head_range;   lit.glow[1][1] = p.distort_amp;
            lit.glow[1][2] = p.wave_speed;   lit.glow[1][3] = p.wave_freq;
            lit.glow[2][0] = p.jitter_speed; lit.glow[2][1] = p.jitter_freq;
            lit.glow[2][2] = 0.0f;           lit.glow[2][3] = 0.0f;
            lit.glow[3][0] = p.glow_tint.r;  lit.glow[3][1] = p.glow_tint.g;
            lit.glow[3][2] = p.glow_tint.b;  lit.glow[3][3] = 0.0f;
            lit.glow[4][0] = p.temp_cool.r;  lit.glow[4][1] = p.temp_cool.g;
            lit.glow[4][2] = p.temp_cool.b;  lit.glow[4][3] = 0.0f;
            lit.glow[5][0] = p.temp_warm.r;  lit.glow[5][1] = p.temp_warm.g;
            lit.glow[5][2] = p.temp_warm.b;  lit.glow[5][3] = 0.0f;
            lit.glow[6][0] = p.temp_hot.r;   lit.glow[6][1] = p.temp_hot.g;
            lit.glow[6][2] = p.temp_hot.b;   lit.glow[6][3] = 0.0f;
            lit.glow[7][0] = 0.0f;           lit.glow[7][1] = 0.0f;
            lit.glow[7][2] = 0.0f;           lit.glow[7][3] = 0.0f;
        };
        fill_glow(nullptr); // default to global for first run

        SDL_GPUBufferBinding vbind; SDL_zero(vbind);
        vbind.buffer = g_sdl.vbuffer; vbind.offset = 0;
        SDL_BindGPUVertexBuffers(g_sdl.pass, 0, &vbind, 1);

        SDL_GPUBufferBinding ibind; SDL_zero(ibind);
        ibind.buffer = g_sdl.ibuffer; ibind.offset = 0;
        SDL_BindGPUIndexBuffer(g_sdl.pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

        SDL_GPUGraphicsPipeline** pipe_set = render_to_offscreen ? g_sdl.pipelines_offscreen : g_sdl.pipelines;

        u32 run_start = batch_start;
        while (run_start < batch_end)
        {
            EBlendMode run_blend = sprites[run_start].sprite.blend;
            u32        run_texid = (sprites[run_start].sort_key & 0x3FFFFu);
            const bs_glow_params* run_glow = sprites[run_start].sprite.glow_override;

            u32 run_end = run_start + 1;
            while (run_end < batch_end)
            {
                EBlendMode b = sprites[run_end].sprite.blend;
                u32        t = (sprites[run_end].sort_key & 0x3FFFFu);
                const bs_glow_params* g = sprites[run_end].sprite.glow_override;
                if (b != run_blend || t != run_texid || g != run_glow) break;
                ++run_end;
            }

            u32 run_count = run_end - run_start;

            gpu_texture* slot = &g_sdl.textures[run_texid - 1u];
            SDL_GPUTexture* tex = (slot->in_use && slot->tex) ? slot->tex
                : g_sdl.textures[(g_sdl.white_texture.id & 0x3FFFFu) - 1u].tex;

            SDL_BindGPUGraphicsPipeline(g_sdl.pass, pipe_set[(u32)run_blend]);
            SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, &view_proj, sizeof(Mat4));

            u32 run_layer = (sprites[run_start].sort_key >> 20) & 0xFFFu;
            lit.params[1] = (g_sdl.light_count > 0 && run_layer < g_sdl.light_unlit_layer) ? 1.0f : 0.0f;
            fill_glow(run_glow);
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, &lit, sizeof(gpu_lights));

            SDL_GPUTextureSamplerBinding tsb;
            tsb.texture = tex;  tsb.sampler = g_sdl.sampler;
            SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);

            SDL_DrawGPUIndexedPrimitives(
                g_sdl.pass,
                run_count * 6u,
                1,
                (index_offset + run_start) * 6u,
                0,
                0);

            ++calls;
            run_start = run_end;
        }
        return calls;
    };

    // Helper: draw all queued mapped sprites in the current render pass.
    auto draw_mapped_batch = [&](b8 render_to_offscreen) -> u32
    {
        u32 calls = 0;
        u32 count = g_sdl.mapped_batch_count;
        if (count == 0) return calls;

        Mat4 view_proj = camera2d_view_proj(&g_sdl.camera, (u16)g_sdl.swap_width, (u16)g_sdl.swap_height);

        mapped_light lit;
        SDL_zero(lit);
        // Use the first mapped sprite's light direction for the whole batch (all ships share
        // the same star direction this frame). Intensity 1.0, ambient from the scene floor.
        const bs_mapped_sprite& first = g_sdl.mapped_batch[0];
        lit.light_dir[0] = first.light_dir.x;
        lit.light_dir[1] = first.light_dir.y;
        lit.light_dir[2] = first.light_dir.z;
        lit.light_dir[3] = 1.0f;
        lit.ambient[0]   = g_sdl.light_ambient.r;
        lit.ambient[1]   = g_sdl.light_ambient.g;
        lit.ambient[2]   = g_sdl.light_ambient.b;
        lit.ambient[3]   = 1.0f;
        lit.tuning[0]    = 1.0f; // normal strength
        lit.tuning[1]    = 0.02f; // depth parallax scale
        lit.tuning[2]    = 0.0f;
        lit.tuning[3]    = 0.0f;

        SDL_GPUBufferBinding vbind; SDL_zero(vbind);
        vbind.buffer = g_sdl.mapped_vbuffer; vbind.offset = 0;
        SDL_BindGPUVertexBuffers(g_sdl.pass, 0, &vbind, 1);

        SDL_GPUBufferBinding ibind; SDL_zero(ibind);
        ibind.buffer = g_sdl.mapped_ibuffer; ibind.offset = 0;
        SDL_BindGPUIndexBuffer(g_sdl.pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

        SDL_GPUGraphicsPipeline* pipe = render_to_offscreen ? g_sdl.pipeline_mapped_offscreen : g_sdl.pipeline_mapped;
        SDL_BindGPUGraphicsPipeline(g_sdl.pass, pipe);
        SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, &view_proj, sizeof(Mat4));
        SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, &lit, sizeof(mapped_light));

        // Each mapped sprite is one draw with its own four textures. We can't merge them
        // because each ship uses a different set of maps.
        for (u32 i = 0; i < count; ++i)
        {
            const bs_mapped_sprite& s = g_sdl.mapped_batch[i];
            gpu_texture* diffuse_slot  = pool_resolve_texture(s.diffuse_map);
            gpu_texture* normal_slot   = pool_resolve_texture(s.normal_map);
            gpu_texture* depth_slot    = pool_resolve_texture(s.depth_map);
            gpu_texture* position_slot = pool_resolve_texture(s.position_map);

            SDL_GPUTexture* white_tex = g_sdl.textures[(g_sdl.white_texture.id & 0x3FFFFu) - 1u].tex;
            SDL_GPUTexture* diffuse_tex  = (diffuse_slot  && diffuse_slot->tex)  ? diffuse_slot->tex  : white_tex;
            SDL_GPUTexture* normal_tex   = (normal_slot   && normal_slot->tex)   ? normal_slot->tex   : white_tex;
            SDL_GPUTexture* depth_tex    = (depth_slot    && depth_slot->tex)    ? depth_slot->tex    : white_tex;
            SDL_GPUTexture* position_tex = (position_slot && position_slot->tex) ? position_slot->tex : white_tex;

            SDL_GPUTextureSamplerBinding tsb[4];
            SDL_zero(tsb);
            tsb[0].texture = diffuse_tex;  tsb[0].sampler = g_sdl.sampler;
            tsb[1].texture = normal_tex;   tsb[1].sampler = g_sdl.sampler;
            tsb[2].texture = depth_tex;    tsb[2].sampler = g_sdl.sampler;
            tsb[3].texture = position_tex;   tsb[3].sampler = g_sdl.sampler;
            SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, tsb, 4);

            SDL_DrawGPUIndexedPrimitives(g_sdl.pass, 6, 1, i * 6u, 0, 0);
            ++calls;
        }
        return calls;
    };

    // Find the split between game-world sprites (below threshold -> bloom) and
    // UI/debug overlays (at/above threshold -> draw after composite, bypass bloom).
    u32 bloom_split = g_sdl.batch_count;
    for (u32 i = 0; i < g_sdl.batch_count; ++i)
    {
        u32 layer = (g_sdl.batch[i].sort_key >> 20) & 0xFFFu;
        if (layer >= BS_LAYER_BLOOM_THRESHOLD) { bloom_split = i; break; }
    }

    u32 draw_calls = 0;

    // ---- PASS 0: render the nebula FBM once into the half-resolution target (premultiplied) ----
    // The nebula fragment shader is fill-rate bound; rendering at half resolution cuts its shader
    // invocations ~4x. It is later bilinearly upscaled and composited over the scene/swapchain in
    // whichever main pass runs below. Skipped entirely when no nebula is queued this frame.
    b8 nebula_ready = FALSE;
    if (g_sdl.nebula_set && g_sdl.pipeline_nebula_halfres && g_sdl.nebula_rt)
    {
        SDL_GPUColorTargetInfo neb_target;
        SDL_zero(neb_target);
        neb_target.texture     = g_sdl.nebula_rt;
        neb_target.clear_color = SDL_FColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        neb_target.load_op     = SDL_GPU_LOADOP_CLEAR;
        neb_target.store_op    = SDL_GPU_STOREOP_STORE;

        g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &neb_target, 1, NULL);
        const bs_nebula_params& p = g_sdl.nebula_params;
        SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_nebula_halfres);
        float ubo[40];
        ubo[0]  = p.cam_cell.x;
        ubo[1]  = p.cam_cell.y;
        ubo[2]  = p.cam.zoom;
        ubo[3]  = (float)p.fb_w;
        ubo[4]  = (float)p.fb_h;
        ubo[5]  = p.intensity;
        ubo[6]  = p.dust_intensity;
        ubo[7]  = (float)p.seed;
        ubo[8]  = p.gas_color_a.r; ubo[9]  = p.gas_color_a.g; ubo[10] = p.gas_color_a.b; ubo[11] = p.lod_target;
        ubo[12] = p.gas_color_b.r; ubo[13] = p.gas_color_b.g; ubo[14] = p.gas_color_b.b; ubo[15] = p.parallax;
        ubo[16] = p.gas_color_c.r; ubo[17] = p.gas_color_c.g; ubo[18] = p.gas_color_c.b; ubo[19] = 0.0f;
        ubo[20] = p.dust_color.r;  ubo[21] = p.dust_color.g;  ubo[22] = p.dust_color.b;  ubo[23] = 0.0f;
        ubo[24] = p.gas_brightness_mul;
        ubo[25] = p.highlight_power;
        ubo[26] = p.palette_shift;
        ubo[27] = p.swirl_strength;
        ubo[28] = p.falloff_radius;
        ubo[29] = p.band_strength;
        ubo[30] = p.cam_local.x;
        ubo[31] = p.cam_local.y;
        // biome0 = (strength, scale, hue_spread, zoom_detail); biome1 = (zoom_saturation, _, _, _)
        ubo[32] = p.biome_strength;
        ubo[33] = p.biome_scale;
        ubo[34] = p.biome_hue_spread;
        ubo[35] = p.zoom_detail;
        ubo[36] = p.zoom_saturation;
        ubo[37] = 0.0f;
        ubo[38] = 0.0f;
        ubo[39] = 0.0f;
        SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, ubo, sizeof(ubo));
        SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(g_sdl.pass);
        g_sdl.pass = NULL;
        nebula_ready = TRUE;
    }

    // ---- PASS 0b: render the radiation heat map once into the half-resolution target ------------
    // Same rationale as the nebula: the heat_map fragment shader loops over every source per pixel
    // plus a domain-warp noise, so it is fill-rate bound. Render it at half resolution (premultiply-
    // on-write into a cleared-transparent target), then upscale + composite below. The UBO packing is
    // identical to the old inline full-res draw: viewport spans the full framebuffer, so uv 0..1 across
    // the half-res target maps world positions exactly as before.
    b8 heat_ready = FALSE;
    if (g_sdl.heat_map_set && g_sdl.pipeline_heat_map_halfres && g_sdl.heat_rt)
    {
        SDL_GPUColorTargetInfo heat_target;
        SDL_zero(heat_target);
        heat_target.texture     = g_sdl.heat_rt;
        heat_target.clear_color = SDL_FColor{ 0.0f, 0.0f, 0.0f, 0.0f };
        heat_target.load_op     = SDL_GPU_LOADOP_CLEAR;
        heat_target.store_op    = SDL_GPU_STOREOP_STORE;

        g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &heat_target, 1, NULL);
        const bs_heat_map_params& p = g_sdl.heat_map_params;
        SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_heat_map_halfres);
        float ubo[4 + 4 + 4 + 4 + 4 + BS_MAX_HEAT_SOURCES * 4];
        ubo[0] = p.camera_pos.x; ubo[1] = p.camera_pos.y; ubo[2] = p.viewport_w; ubo[3] = p.viewport_h;
        ubo[4] = p.base_radius;  ubo[5] = p.heat_warp_strength; ubo[6] = p.threshold; ubo[7] = p.intensity;
        ubo[8] = (float)p.source_count; ubo[9] = p.venn_sharpness; ubo[10] = p.heat_signature_radius; ubo[11] = p.color_falloff_power;
        ubo[12] = (float)p.palette; ubo[13] = p.color_low.r; ubo[14] = p.color_low.g; ubo[15] = p.color_low.b;
        ubo[16] = p.color_high.r; ubo[17] = p.color_high.g; ubo[18] = p.color_high.b; ubo[19] = 0.0f;
        for (u32 i = 0; i < p.source_count; ++i) {
            ubo[20 + i*4]   = p.sources[i].x;
            ubo[20 + i*4+1] = p.sources[i].y;
            ubo[20 + i*4+2] = p.is_detector[i] ? 1.0f : 0.0f;
            ubo[20 + i*4+3] = p.emissions[i];
        }
        SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, ubo, sizeof(ubo));
        SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(g_sdl.pass);
        g_sdl.pass = NULL;
        heat_ready = TRUE;
    }

    // Bind + draw the upscale/composite of nebula_rt into the CURRENT pass (premultiplied over).
    auto composite_nebula = [&](SDL_GPUGraphicsPipeline* pipe) {
        if (!nebula_ready || !pipe) return;
        SDL_BindGPUGraphicsPipeline(g_sdl.pass, pipe);
        SDL_GPUTextureSamplerBinding nb; SDL_zero(nb);
        nb.texture = g_sdl.nebula_rt;
        nb.sampler = g_sdl.sampler_linear;
        SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &nb, 1);
        SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
    };

    // Same for the half-res heat map (shares the nebula composite pipelines: premult-over copy).
    auto composite_heat = [&](SDL_GPUGraphicsPipeline* pipe) {
        if (!heat_ready || !pipe) return;
        SDL_BindGPUGraphicsPipeline(g_sdl.pass, pipe);
        SDL_GPUTextureSamplerBinding hb; SDL_zero(hb);
        hb.texture = g_sdl.heat_rt;
        hb.sampler = g_sdl.sampler_linear;
        SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &hb, 1);
        SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
    };

    b8 use_offscreen = g_sdl.scene_rt && g_sdl.bloom_a && g_sdl.bloom_b;
    b8 need_bloom    = g_sdl.bloom_enabled;
    b8 need_streak   = g_sdl.streak_enabled && aux_count > 0;
    // The frosted-glass UI backdrop samples a blurred copy of scene_rt, which is only populated on the
    // offscreen path. Force that path whenever the UI frost is active so the effect works even with
    // bloom/streak disabled (the composite handles zero bloom/streak intensity, same as the streak path).
    b8 need_frost    = g_rml_frost_active();

    if (use_offscreen && (need_bloom || need_streak || need_frost))
    {
        // ---- PASS 1: game-world sprites -> scene_rt -------------------------------------------
        SDL_GPUColorTargetInfo scene_target;
        SDL_zero(scene_target);
        scene_target.texture     = g_sdl.scene_rt;
        scene_target.clear_color = SDL_FColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        scene_target.load_op     = SDL_GPU_LOADOP_CLEAR;
        scene_target.store_op    = SDL_GPU_STOREOP_STORE;

        g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &scene_target, 1, NULL);
        // Draw queued procedural starfield layers behind all sprites.
        for (u32 i = 0; i < g_sdl.starfield_queue_count; ++i)
        {
            if (!g_sdl.starfield_queue[i].active) continue;
            const bs_starfield_params& p = g_sdl.starfield_queue[i].params;
            if (g_sdl.pipeline_starfield_layer) {
                SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_starfield_layer);
                float layer_params[16];
                layer_params[0]  = p.cam_cell.x;
                layer_params[1]  = p.cam_cell.y;
                layer_params[2]  = p.cam.zoom;
                layer_params[3]  = (float)p.fb_w;
                layer_params[4]  = (float)p.fb_h;
                layer_params[5]  = p.density;
                layer_params[6]  = p.size_mul;
                layer_params[7]  = p.brightness_mul;
                layer_params[8]  = (float)p.seed;
                layer_params[9]  = p.cam_local.x;
                layer_params[10] = p.cam_local.y;
                layer_params[11] = p.base_cell;
                layer_params[12] = p.star_rel.x;
                layer_params[13] = p.star_rel.y;
                layer_params[14] = p.dazzle_inner;
                layer_params[15] = p.dazzle_outer;
                float dazzle_params[8] = { p.dazzle_intensity, p.target_px, p.lod_levels, p.lod_factor,
                                           p.parallax_near, p.parallax_falloff, 0.0f, 0.0f };
                SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, layer_params, sizeof(layer_params));
                SDL_PushGPUFragmentUniformData(g_sdl.cmd, 1, dazzle_params, sizeof(dazzle_params));
                SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
            }
        }
        // Composite the half-res nebula (upscaled) in front of the starfield but behind sunburst/sprites.
        composite_nebula(g_sdl.pipeline_nebula_composite);
        // Draw queued sunburst stars behind sprites.
        for (u32 i = 0; i < g_sdl.sunburst_queue_count; ++i)
        {
            if (!g_sdl.sunburst_queue[i].active) continue;
            const bs_sunburst_params& p = g_sdl.sunburst_queue[i].params;
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_sunburst);
            float u[16];
            u[0] = p.screen_pos.x;   u[1] = p.screen_pos.y;   u[2] = p.body_radius;  u[3] = p.glow_radius;
            u[4] = p.color.r;        u[5] = p.color.g;        u[6] = p.color.b;      u[7] = p.time;
            u[8] = 16.0f;            u[9] = 1.0f;             u[10] = 2.5f;          u[11] = p.visibility;
            u[12] = (float)p.fb_w;   u[13] = (float)p.fb_h;   u[14] = 0.0f;          u[15] = 0.0f;
            SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_DrawGPUPrimitives(g_sdl.pass, 6, 1, 0, 0);
        }
        // Draw queued procedural star surfaces (occluding photosphere + corona) behind sprites.
        for (u32 i = 0; i < g_sdl.starsurface_queue_count; ++i)
        {
            if (!g_sdl.starsurface_queue[i].active) continue;
            const bs_starsurface_params& p = g_sdl.starsurface_queue[i].params;
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_starsurface);
            float u[24];
            u[0]  = p.screen_pos.x;  u[1]  = p.screen_pos.y;  u[2]  = p.body_radius;     u[3]  = p.glow_radius;
            u[4]  = p.color.r;       u[5]  = p.color.g;       u[6]  = p.color.b;         u[7]  = p.time;
            u[8]  = p.noise_scale;   u[9]  = p.flow_speed;    u[10] = p.granule_contrast;u[11] = p.visibility;
            u[12] = (float)p.fb_w;   u[13] = (float)p.fb_h;   u[14] = p.hotspot_gain;    u[15] = p.sunspot_density;
            u[16] = p.limb_darkening;u[17] = p.brightness;    u[18] = p.corona_strength; u[19] = p.dark_radius;
            u[20] = 0.0f; u[21] = 0.0f; u[22] = 0.0f; u[23] = 0.0f;
            SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_DrawGPUPrimitives(g_sdl.pass, 6, 1, 0, 0);
        }
        // Draw queued procedural planet surfaces (lit impostor spheres) behind sprites.
        for (u32 i = 0; i < g_sdl.planetsurface_queue_count; ++i)
        {
            if (!g_sdl.planetsurface_queue[i].active) continue;
            const bs_planetsurface_params& p = g_sdl.planetsurface_queue[i].params;
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_planetsurface);
            float u[52];
            u[0]  = p.screen_pos.x;  u[1]  = p.screen_pos.y;  u[2]  = p.body_radius;  u[3]  = p.quad_radius;
            u[4]  = p.base_color.r;  u[5]  = p.base_color.g;  u[6]  = p.base_color.b;  u[7]  = p.time;
            u[8]  = p.light_dir.x;   u[9]  = p.light_dir.y;   u[10] = (float)p.planet_type; u[11] = p.visibility;
            u[12] = (float)p.fb_w;   u[13] = (float)p.fb_h;   u[14] = p.rotation;     u[15] = p.seed;
            u[16] = p.has_atmosphere ? 1.0f : 0.0f; u[17] = p.has_rings ? 1.0f : 0.0f; u[18] = p.cloud_amount; u[19] = p.ring_shadow_elev;
            u[20] = p.star_color.r;  u[21] = p.star_color.g;  u[22] = p.star_color.b;  u[23] = 0.0f;
            u[24] = p.pal_deep.r;    u[25] = p.pal_deep.g;    u[26] = p.pal_deep.b;    u[27] = p.noise_freq;
            u[28] = p.pal_mid.r;     u[29] = p.pal_mid.g;     u[30] = p.pal_mid.b;     u[31] = p.warp_amount;
            u[32] = p.pal_light.r;   u[33] = p.pal_light.g;   u[34] = p.pal_light.b;   u[35] = p.feature_density;
            u[36] = p.pal_accent.r;  u[37] = p.pal_accent.g;  u[38] = p.pal_accent.b;  u[39] = p.band_detail;
            u[40] = p.cloud_tint.r;  u[41] = p.cloud_tint.g;  u[42] = p.cloud_tint.b;  u[43] = p.cap_extent;
            u[44] = p.atmo_tint.r;   u[45] = p.atmo_tint.g;   u[46] = p.atmo_tint.b;   u[47] = p.roughness;
            u[48] = p.anomaly;       u[49] = 0.0f;            u[50] = 0.0f;            u[51] = 0.0f;
            SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_DrawGPUPrimitives(g_sdl.pass, 6, 1, 0, 0);
        }
        // Composite the half-res radiation heat map behind the sprite batch (upscaled, premult-over).
        composite_heat(g_sdl.pipeline_nebula_composite);
        draw_calls = draw_sprite_batch(TRUE, g_sdl.batch, g_sdl.batch_count, aux_count, 0, bloom_split);
        draw_calls += draw_mapped_batch(TRUE);
        SDL_EndGPURenderPass(g_sdl.pass);
        g_sdl.pass = NULL;

        // ---- PASS 1b: aux sprites -> aux_bloom_a (clear black, additive) --------------------
        if (aux_count > 0)
        {
            SDL_GPUColorTargetInfo aux_target;
            SDL_zero(aux_target);
            aux_target.texture     = g_sdl.aux_bloom_a;
            aux_target.clear_color = SDL_FColor{ 0.0f, 0.0f, 0.0f, 1.0f };
            aux_target.load_op     = SDL_GPU_LOADOP_CLEAR;
            aux_target.store_op    = SDL_GPU_STOREOP_STORE;

            g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &aux_target, 1, NULL);
            draw_calls += draw_sprite_batch(TRUE, g_sdl.aux_batch, aux_count, 0, 0, aux_count);
            SDL_EndGPURenderPass(g_sdl.pass);
            g_sdl.pass = NULL;
        }

        // Common fullscreen sampler binding (LINEAR for smooth post-process sampling).
        SDL_GPUTextureSamplerBinding tsb;
        SDL_zero(tsb);
        tsb.sampler = g_sdl.sampler_linear;

        float bloom_params[4];

        // ---- PASS 2-4: main bloom extract + blur (only if bloom is enabled) ------------------
        if (need_bloom)
        {
            SDL_GPUColorTargetInfo bloom_target;
            SDL_zero(bloom_target);
            bloom_target.texture  = g_sdl.bloom_a;
            bloom_target.load_op  = SDL_GPU_LOADOP_DONT_CARE;
            bloom_target.store_op = SDL_GPU_STOREOP_STORE;

            // PASS 2: brightness extract
            g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &bloom_target, 1, NULL);
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_extract);
            tsb.texture = g_sdl.scene_rt;
            SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);
            bloom_params[0] = g_sdl.bloom_threshold;
            bloom_params[1] = 0.0f; bloom_params[2] = 0.0f; bloom_params[3] = 0.0f;
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, bloom_params, sizeof(bloom_params));
            SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(g_sdl.pass);
            g_sdl.pass = NULL;

            // PASS 3: blur horizontal -> bloom_b
            bloom_target.texture = g_sdl.bloom_b;
            g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &bloom_target, 1, NULL);
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_blur_h);
            tsb.texture = g_sdl.bloom_a;
            SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);
            bloom_params[0] = 1.0f / (f32)((g_sdl.bloom_width  / 2) > 1u ? (g_sdl.bloom_width  / 2) : 1u);
            bloom_params[1] = 1.0f / (f32)((g_sdl.bloom_height / 2) > 1u ? (g_sdl.bloom_height / 2) : 1u);
            bloom_params[2] = 0.0f; bloom_params[3] = 0.0f;
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, bloom_params, sizeof(bloom_params));
            SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(g_sdl.pass);
            g_sdl.pass = NULL;

            // PASS 4: blur vertical -> bloom_a
            bloom_target.texture = g_sdl.bloom_a;
            g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &bloom_target, 1, NULL);
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_blur_v);
            tsb.texture = g_sdl.bloom_b;
            SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, bloom_params, sizeof(bloom_params));
            SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(g_sdl.pass);
            g_sdl.pass = NULL;
        }

        // ---- PASS 5-7: aux blur + streak ------------------------------------------------------
        if (aux_count > 0)
        {
            SDL_GPUColorTargetInfo bloom_target;
            SDL_zero(bloom_target);
            bloom_target.load_op  = SDL_GPU_LOADOP_DONT_CARE;
            bloom_target.store_op = SDL_GPU_STOREOP_STORE;

            // PASS 5: aux blur horizontal -> aux_bloom_b
            bloom_target.texture = g_sdl.aux_bloom_b;
            g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &bloom_target, 1, NULL);
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_blur_h);
            tsb.texture = g_sdl.aux_bloom_a;
            SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);
            bloom_params[0] = 1.0f / (f32)((g_sdl.bloom_width  / 2) > 1u ? (g_sdl.bloom_width  / 2) : 1u);
            bloom_params[1] = 1.0f / (f32)((g_sdl.bloom_height / 2) > 1u ? (g_sdl.bloom_height / 2) : 1u);
            bloom_params[2] = 0.0f; bloom_params[3] = 0.0f;
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, bloom_params, sizeof(bloom_params));
            SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(g_sdl.pass);
            g_sdl.pass = NULL;

            // PASS 6: aux blur vertical -> aux_bloom_a
            bloom_target.texture = g_sdl.aux_bloom_a;
            g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &bloom_target, 1, NULL);
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_blur_v);
            tsb.texture = g_sdl.aux_bloom_b;
            SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, bloom_params, sizeof(bloom_params));
            SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(g_sdl.pass);
            g_sdl.pass = NULL;

            // PASS 7: streak -> aux_bloom_b
            if (g_sdl.streak_enabled)
            {
                bloom_target.texture = g_sdl.aux_bloom_b;
                g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &bloom_target, 1, NULL);
                SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_streak);
                tsb.texture = g_sdl.aux_bloom_a;
                SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);
                bloom_params[0] = 1.0f / (f32)((g_sdl.bloom_width  / 2) > 1u ? (g_sdl.bloom_width  / 2) : 1u);
                bloom_params[1] = 1.0f / (f32)((g_sdl.bloom_height / 2) > 1u ? (g_sdl.bloom_height / 2) : 1u);
                bloom_params[2] = g_sdl.streak_angle;
                bloom_params[3] = g_sdl.streak_length;
                SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, bloom_params, sizeof(bloom_params));
                SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(g_sdl.pass);
                g_sdl.pass = NULL;

                // PASS 7b: lens-flare ghosts -> aux_bloom_a
                bloom_target.texture = g_sdl.aux_bloom_a;
                g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &bloom_target, 1, NULL);
                SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_flare);
                tsb.texture = g_sdl.aux_bloom_b;
                SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);
                float flare_source_u = g_sdl.streak_source_set ? g_sdl.streak_source.x / (f32)g_sdl.swap_width : 0.5f;
                float flare_source_v = g_sdl.streak_source_set ? g_sdl.streak_source.y / (f32)g_sdl.swap_height : 0.5f;
                bloom_params[0] = flare_source_u;
                bloom_params[1] = flare_source_v;
                bloom_params[2] = g_sdl.streak_flare_intensity;
                bloom_params[3] = (g_sdl.swap_height > 0)
                                  ? (f32)g_sdl.swap_width / (f32)g_sdl.swap_height : (16.0f / 9.0f);
                SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, bloom_params, sizeof(bloom_params));
                SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(g_sdl.pass);
                g_sdl.pass = NULL;
            }
        }

        // ---- FROST FILL: blurred copy of scene_rt -> ui_backdrop_rt -------------------------
        // Produces the half-res, gaussian-blurred scene sampled by the "backdrop" UI decorator for a
        // real frosted-glass look. Two separable passes (H+V twice) via the existing bloom blur
        // pipelines; bloom_b is reused as scratch (it is free after the bloom ping-pong at this point).
        if (g_rml_frost_active() && g_sdl.ui_backdrop_rt && g_sdl.pipeline_blur_h && g_sdl.pipeline_blur_v)
        {
            const u32 hw = (g_sdl.bloom_width  / 2) > 1u ? (g_sdl.bloom_width  / 2) : 1u;
            const u32 hh = (g_sdl.bloom_height / 2) > 1u ? (g_sdl.bloom_height / 2) : 1u;
            float fp[4]; fp[0] = 1.0f / (f32)hw; fp[1] = 1.0f / (f32)hh; fp[2] = 0.0f; fp[3] = 0.0f;

            SDL_GPUColorTargetInfo ft; SDL_zero(ft);
            ft.load_op = SDL_GPU_LOADOP_DONT_CARE; ft.store_op = SDL_GPU_STOREOP_STORE;
            SDL_GPUTextureSamplerBinding fb; SDL_zero(fb); fb.sampler = g_sdl.sampler_linear;

            struct { SDL_GPUTexture* dst; SDL_GPUTexture* src; SDL_GPUGraphicsPipeline* pipe; } frost_steps[4] = {
                { g_sdl.bloom_b,        g_sdl.scene_rt,       g_sdl.pipeline_blur_h }, // downsample + blur H
                { g_sdl.ui_backdrop_rt, g_sdl.bloom_b,        g_sdl.pipeline_blur_v }, // blur V
                { g_sdl.bloom_b,        g_sdl.ui_backdrop_rt, g_sdl.pipeline_blur_h }, // blur H (2nd iter)
                { g_sdl.ui_backdrop_rt, g_sdl.bloom_b,        g_sdl.pipeline_blur_v }, // blur V (2nd iter)
            };
            for (int fs = 0; fs < 4; ++fs)
            {
                ft.texture = frost_steps[fs].dst;
                g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &ft, 1, NULL);
                SDL_BindGPUGraphicsPipeline(g_sdl.pass, frost_steps[fs].pipe);
                fb.texture = frost_steps[fs].src;
                SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &fb, 1);
                SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, fp, sizeof(fp));
                SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
                SDL_EndGPURenderPass(g_sdl.pass);
                g_sdl.pass = NULL;
            }
        }

        // ---- PASS 8: composite -> swapchain -------------------------------------------------
        SDL_GPUColorTargetInfo swap_target;
        SDL_zero(swap_target);
        swap_target.texture     = g_sdl.swapchain_texture;
        swap_target.clear_color = SDL_FColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        swap_target.load_op     = SDL_GPU_LOADOP_CLEAR;
        swap_target.store_op    = SDL_GPU_STOREOP_STORE;

        g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &swap_target, 1, NULL);
        SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_composite);
        SDL_GPUTextureSamplerBinding tsb2[4];
        SDL_zero(tsb2);
        tsb2[0].texture = g_sdl.scene_rt;     tsb2[0].sampler = g_sdl.sampler_linear;
        tsb2[1].texture = g_sdl.bloom_a;      tsb2[1].sampler = g_sdl.sampler_linear;
        tsb2[2].texture = (aux_count > 0 && g_sdl.streak_enabled) ? g_sdl.aux_bloom_b : g_sdl.aux_bloom_a;
        tsb2[2].sampler = g_sdl.sampler_linear;
        tsb2[3].texture = (aux_count > 0 && g_sdl.streak_enabled) ? g_sdl.aux_bloom_a : g_sdl.aux_bloom_b;
        tsb2[3].sampler = g_sdl.sampler_linear;
        SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, tsb2, 4);
        bloom_params[0] = need_bloom ? g_sdl.bloom_intensity : 0.0f;
        bloom_params[1] = (aux_count > 0) ? g_sdl.streak_intensity : 0.0f;
        bloom_params[2] = (aux_count > 0 && g_sdl.streak_enabled) ? g_sdl.streak_flare_intensity : 0.0f;
        bloom_params[3] = 0.0f;
        SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, bloom_params, sizeof(bloom_params));
        SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);

        // ---- PASS 8b: debug / UI overlays directly on swapchain (bypass bloom) ------------
        if (bloom_split < g_sdl.batch_count)
            draw_calls += draw_sprite_batch(FALSE, g_sdl.batch, g_sdl.batch_count, aux_count, bloom_split, g_sdl.batch_count);

        // RmlUi in-game UI, drawn into the swapchain pass beneath the ImGui editor overlay.
        {
            u64 rml_t0 = SDL_GetPerformanceCounter();
            bs_rml_render_internal();
            tm_rml = (f32)((f64)(SDL_GetPerformanceCounter() - rml_t0) * perf_to_ms);
        }

        // ImGui on top of everything.
        if (g_sdl.imgui_active && imgui_draw_data && imgui_draw_data->TotalVtxCount > 0)
            ImGui_ImplSDLGPU3_RenderDrawData(imgui_draw_data, g_sdl.cmd, g_sdl.pass);

        SDL_EndGPURenderPass(g_sdl.pass);
        g_sdl.pass = NULL;
    }
    else
    {
        // ---- Bloom and streak disabled: single pass directly to swapchain -------------------
        SDL_GPUColorTargetInfo color_target;
        SDL_zero(color_target);
        color_target.texture     = g_sdl.swapchain_texture;
        color_target.clear_color = SDL_FColor{ 0.0f, 0.0f, 0.0f, 1.0f };
        color_target.load_op     = SDL_GPU_LOADOP_CLEAR;
        color_target.store_op    = SDL_GPU_STOREOP_STORE;

        g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &color_target, 1, NULL);
        // Draw queued procedural starfield layers behind all sprites.
        for (u32 i = 0; i < g_sdl.starfield_queue_count; ++i)
        {
            if (!g_sdl.starfield_queue[i].active) continue;
            const bs_starfield_params& p = g_sdl.starfield_queue[i].params;
            if (g_sdl.pipeline_starfield_layer_swapchain) {
                SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_starfield_layer_swapchain);
                float layer_params[16];
                layer_params[0]  = p.cam_cell.x;
                layer_params[1]  = p.cam_cell.y;
                layer_params[2]  = p.cam.zoom;
                layer_params[3]  = (float)p.fb_w;
                layer_params[4]  = (float)p.fb_h;
                layer_params[5]  = p.density;
                layer_params[6]  = p.size_mul;
                layer_params[7]  = p.brightness_mul;
                layer_params[8]  = (float)p.seed;
                layer_params[9]  = p.cam_local.x;
                layer_params[10] = p.cam_local.y;
                layer_params[11] = p.base_cell;
                layer_params[12] = p.star_rel.x;
                layer_params[13] = p.star_rel.y;
                layer_params[14] = p.dazzle_inner;
                layer_params[15] = p.dazzle_outer;
                float dazzle_params[8] = { p.dazzle_intensity, p.target_px, p.lod_levels, p.lod_factor,
                                           p.parallax_near, p.parallax_falloff, 0.0f, 0.0f };
                SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, layer_params, sizeof(layer_params));
                SDL_PushGPUFragmentUniformData(g_sdl.cmd, 1, dazzle_params, sizeof(dazzle_params));
                SDL_DrawGPUPrimitives(g_sdl.pass, 3, 1, 0, 0);
            }
        }
        // Composite the half-res nebula (upscaled) in front of the starfield but behind sunburst/sprites.
        composite_nebula(g_sdl.pipeline_nebula_composite_swapchain);
        // Draw queued sunburst stars behind sprites.
        for (u32 i = 0; i < g_sdl.sunburst_queue_count; ++i)
        {
            if (!g_sdl.sunburst_queue[i].active) continue;
            const bs_sunburst_params& p = g_sdl.sunburst_queue[i].params;
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_sunburst_swapchain);
            float u[16];
            u[0] = p.screen_pos.x;   u[1] = p.screen_pos.y;   u[2] = p.body_radius;  u[3] = p.glow_radius;
            u[4] = p.color.r;        u[5] = p.color.g;        u[6] = p.color.b;      u[7] = p.time;
            u[8] = 16.0f;            u[9] = 1.0f;             u[10] = 2.5f;          u[11] = p.visibility;
            u[12] = (float)p.fb_w;   u[13] = (float)p.fb_h;   u[14] = 0.0f;          u[15] = 0.0f;
            SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_DrawGPUPrimitives(g_sdl.pass, 6, 1, 0, 0);
        }
        // Draw queued procedural star surfaces (occluding photosphere + corona) behind sprites.
        for (u32 i = 0; i < g_sdl.starsurface_queue_count; ++i)
        {
            if (!g_sdl.starsurface_queue[i].active) continue;
            const bs_starsurface_params& p = g_sdl.starsurface_queue[i].params;
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_starsurface_swapchain);
            float u[24];
            u[0]  = p.screen_pos.x;  u[1]  = p.screen_pos.y;  u[2]  = p.body_radius;     u[3]  = p.glow_radius;
            u[4]  = p.color.r;       u[5]  = p.color.g;       u[6]  = p.color.b;         u[7]  = p.time;
            u[8]  = p.noise_scale;   u[9]  = p.flow_speed;    u[10] = p.granule_contrast;u[11] = p.visibility;
            u[12] = (float)p.fb_w;   u[13] = (float)p.fb_h;   u[14] = p.hotspot_gain;    u[15] = p.sunspot_density;
            u[16] = p.limb_darkening;u[17] = p.brightness;    u[18] = p.corona_strength; u[19] = p.dark_radius;
            u[20] = 0.0f; u[21] = 0.0f; u[22] = 0.0f; u[23] = 0.0f;
            SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_DrawGPUPrimitives(g_sdl.pass, 6, 1, 0, 0);
        }
        // Draw queued procedural planet surfaces (lit impostor spheres) behind sprites.
        for (u32 i = 0; i < g_sdl.planetsurface_queue_count; ++i)
        {
            if (!g_sdl.planetsurface_queue[i].active) continue;
            const bs_planetsurface_params& p = g_sdl.planetsurface_queue[i].params;
            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipeline_planetsurface_swapchain);
            float u[52];
            u[0]  = p.screen_pos.x;  u[1]  = p.screen_pos.y;  u[2]  = p.body_radius;  u[3]  = p.quad_radius;
            u[4]  = p.base_color.r;  u[5]  = p.base_color.g;  u[6]  = p.base_color.b;  u[7]  = p.time;
            u[8]  = p.light_dir.x;   u[9]  = p.light_dir.y;   u[10] = (float)p.planet_type; u[11] = p.visibility;
            u[12] = (float)p.fb_w;   u[13] = (float)p.fb_h;   u[14] = p.rotation;     u[15] = p.seed;
            u[16] = p.has_atmosphere ? 1.0f : 0.0f; u[17] = p.has_rings ? 1.0f : 0.0f; u[18] = p.cloud_amount; u[19] = p.ring_shadow_elev;
            u[20] = p.star_color.r;  u[21] = p.star_color.g;  u[22] = p.star_color.b;  u[23] = 0.0f;
            u[24] = p.pal_deep.r;    u[25] = p.pal_deep.g;    u[26] = p.pal_deep.b;    u[27] = p.noise_freq;
            u[28] = p.pal_mid.r;     u[29] = p.pal_mid.g;     u[30] = p.pal_mid.b;     u[31] = p.warp_amount;
            u[32] = p.pal_light.r;   u[33] = p.pal_light.g;   u[34] = p.pal_light.b;   u[35] = p.feature_density;
            u[36] = p.pal_accent.r;  u[37] = p.pal_accent.g;  u[38] = p.pal_accent.b;  u[39] = p.band_detail;
            u[40] = p.cloud_tint.r;  u[41] = p.cloud_tint.g;  u[42] = p.cloud_tint.b;  u[43] = p.cap_extent;
            u[44] = p.atmo_tint.r;   u[45] = p.atmo_tint.g;   u[46] = p.atmo_tint.b;   u[47] = p.roughness;
            u[48] = p.anomaly;       u[49] = 0.0f;            u[50] = 0.0f;            u[51] = 0.0f;
            SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, u, sizeof(u));
            SDL_DrawGPUPrimitives(g_sdl.pass, 6, 1, 0, 0);
        }
        // Composite the half-res radiation heat map behind the sprite batch (upscaled, premult-over).
        composite_heat(g_sdl.pipeline_nebula_composite_swapchain);
        draw_calls = draw_sprite_batch(FALSE, g_sdl.batch, g_sdl.batch_count, aux_count, 0, g_sdl.batch_count);
        draw_calls += draw_mapped_batch(FALSE);

        // RmlUi in-game UI, drawn into the swapchain pass beneath the ImGui editor overlay.
        {
            u64 rml_t0 = SDL_GetPerformanceCounter();
            bs_rml_render_internal();
            tm_rml = (f32)((f64)(SDL_GetPerformanceCounter() - rml_t0) * perf_to_ms);
        }

        if (g_sdl.imgui_active && imgui_draw_data && imgui_draw_data->TotalVtxCount > 0)
            ImGui_ImplSDLGPU3_RenderDrawData(imgui_draw_data, g_sdl.cmd, g_sdl.pass);

        SDL_EndGPURenderPass(g_sdl.pass);
        g_sdl.pass = NULL;
    }

    g_sdl.last_stats.sprite_count = g_sdl.batch_count + aux_count + g_sdl.mapped_batch_count;
    g_sdl.last_stats.draw_calls   = draw_calls;

    u64 sub_t0 = SDL_GetPerformanceCounter();
    tm_record = (f32)((f64)(sub_t0 - rec_t0) * perf_to_ms) - tm_rml;
    // Flush RmlUi's batched buffer uploads FIRST: submission order makes the GPU run the geometry
    // uploads before the main command buffer's draws that reference those buffers.
    rml_upload_flush();
    SDL_SubmitGPUCommandBuffer(g_sdl.cmd);
    g_sdl.last_submit_ms =
        (f32)((f64)(SDL_GetPerformanceCounter() - sub_t0) * 1000.0 / (f64)SDL_GetPerformanceFrequency());

    // Slow-present forensics: when end_frame exceeds the budget, print the per-section breakdown
    // (throttled to ~1 line/second so a persistent stall doesn't flood the log).
    {
        f32 total_ms = (f32)((f64)(SDL_GetPerformanceCounter() - ef_t0) * perf_to_ms);
        if (total_ms > 20.0f)
        {
            static u64 s_last_report = 0;
            u64 now = SDL_GetPerformanceCounter();
            if (s_last_report == 0 || (f64)(now - s_last_report) * perf_to_ms > 1000.0)
            {
                s_last_report = now;
                BS_LOG_WARN("end_frame breakdown (%.1fms total): imgui_render=%.2f build_upload=%.2f "
                            "acquire=%.2f imgui_prepare=%.2f record=%.2f rml=%.2f submit=%.2f "
                            "rml_compiles=%u creates=%u pool_hits=%u",
                            total_ms, tm_imgui_render, tm_build_upload, g_sdl.last_acquire_ms,
                            tm_imgui_prepare, tm_record, tm_rml, g_sdl.last_submit_ms,
                            g_rml_stat_compiles, g_rml_stat_creates, g_rml_stat_pool_hits);
            }
        }
    }
    g_rml_stat_compiles = g_rml_stat_creates = g_rml_stat_pool_hits = 0;
    g_sdl.cmd               = NULL;
    g_sdl.swapchain_texture = NULL;
    g_sdl.aux_bloom_mode  = FALSE;
    g_sdl.aux_batch_count = 0;
    return TRUE;
}

// =====================================================================================
// RmlUi backend (renderer/bs_rml.h). Implemented HERE, alongside the ImGui facade, because this
// is the only TU that includes <SDL3/...> and the RmlUi headers and it needs direct access to the
// backend globals (g_sdl.device/window/cmd/pass/swap_width/height, the texture pool, samplers,
// and the shared white texture). RmlUi provides three application interfaces; we implement the
// SystemInterface (time + logging) and the RenderInterface (SDL3 GPU), and rely on RmlUi's default
// FileInterface (stdio, paths relative to CWD) and default FreeType font engine (we compile
// Source/FontEngineDefault into rmlui.lib and link FreeType).
//
// GPU-upload discipline: RmlUi may call CompileGeometry / GenerateTexture / LoadTexture at any
// time, INCLUDING lazily during Context::Render() while our swapchain render pass (g_sdl.pass) is
// open. SDL3 GPU forbids copy passes inside an active render pass, so every RmlUi upload acquires
// its OWN command buffer and copy pass (mirroring sdlgpu_upload_texture_pixels) — independent of
// the frame's g_sdl.cmd — and SDL synchronizes the cross-command-buffer hazard automatically.
// RenderGeometry, by contrast, records draws directly into the open g_sdl.pass.
// =====================================================================================
namespace {

// Persistent GPU buffers for one compiled RmlUi geometry (retained across frames until released).
struct bs_rml_geometry
{
    SDL_GPUBuffer* vbuf;
    SDL_GPUBuffer* ibuf;
    u32            index_count;
    u32            vbuf_alloc; // pooled allocation size (pow2 bucket), for pool return
    u32            ibuf_alloc;
};

// A compiled RmlUi shader effect. Currently only the "backdrop" frosted-glass shader; the tint is
// captured from the decorator's RCSS at CompileShader time and pushed as a fragment uniform.
struct bs_rml_shader
{
    f32 tint[4]; // rgb = tint colour, a = tint strength [0..1]
};

class BsRmlSystemInterface : public Rml::SystemInterface
{
public:
    double GetElapsedTime() override { return (double)platform_get_absolute_time(); }

    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override
    {
        switch (type)
        {
            case Rml::Log::LT_ERROR:
            case Rml::Log::LT_ASSERT:  BS_LOG_ERROR("[RmlUi] %s", message.c_str()); break;
            case Rml::Log::LT_WARNING: BS_LOG_WARN ("[RmlUi] %s", message.c_str()); break;
            case Rml::Log::LT_INFO:    BS_LOG_INFO ("[RmlUi] %s", message.c_str()); break;
            default:                   BS_LOG_DEBUG("[RmlUi] %s", message.c_str()); break;
        }
        return true; // continue execution (never break into a debugger)
    }
};

class BsRmlRenderInterface : public Rml::RenderInterface
{
public:
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

    // Frosted-glass "backdrop" decorator support (only these shader hooks are implemented; any other
    // shader name returns 0 so unrelated shader/filter decorators simply do not render).
    Rml::CompiledShaderHandle CompileShader(const Rml::String& name, const Rml::Dictionary& parameters) override;
    void RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                      Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseShader(Rml::CompiledShaderHandle shader) override;
};

// All RmlUi backend state. The interface instances live here (static storage) so they outlive
// Rml::Shutdown, which is a hard requirement of SetSystemInterface/SetRenderInterface.
struct bs_rml_state
{
    b8                       active;
    Rml::Context*            context;
    BsRmlSystemInterface     system_iface;
    BsRmlRenderInterface     render_iface;

    SDL_GPUGraphicsPipeline* pipeline;   // premult-alpha UI pipeline targeting the swapchain
    SDL_GPUGraphicsPipeline* pipeline_backdrop; // frosted-glass backdrop pipeline (samples ui_backdrop_rt)
    SDL_GPUTexture*          white_tex;  // shared 1x1 white (untextured geometry)

    // Render-time state, mutated by the render interface during Context::Render().
    Mat4                     transform;        // current element transform (column-major)
    b8                       has_transform;    // FALSE => identity (skip the multiply)
    b8                       scissor_enabled;
    SDL_Rect                 scissor;          // pixel rect, top-left origin

    // Input state.
    b8                       wants_mouse;      // last mouse-move landed on an interactive element
};

bs_rml_state g_rml;

} // anonymous namespace

// Gates the per-frame scene-blur fill for the frosted-glass "backdrop" decorator (see end_frame).
// The in-game HUD panels use the decorator continuously, so filling whenever the UI is active is
// both correct and cheap (four half-res passes).
static b8 g_rml_frost_active(void) { return g_rml.active; }

// -------------------------------------------------------------------------------------
// RmlUi batched buffer uploads + buffer pooling.
//
// RmlUi recompiles the geometry of every CHANGED element each frame (live HUD text = dozens of
// CompileGeometry calls per frame). Two costs must be avoided on driver-overhead-heavy machines:
//   1. One command-buffer submit per upload (~0.25 ms each) -> all uploads for a frame are
//      recorded into ONE shared command buffer / copy pass, submitted right before the frame's
//      main command buffer (submission order guarantees the uploads execute first).
//   2. Creating + destroying ~100 GPU buffers and transfer buffers per frame (driver allocation
//      and residency cost, paid at submit) -> released buffers go into pow2-bucketed pools and
//      are reused. In-flight hazards are handled by SDL_GPU's `cycle` flag on map/upload.
// -------------------------------------------------------------------------------------
#define RML_UPLOAD_MAX_TRANSFERS 1024
static SDL_GPUCommandBuffer*  g_rml_upload_cmd  = NULL;
static SDL_GPUCopyPass*       g_rml_upload_pass = NULL;
struct rml_pending_transfer { SDL_GPUTransferBuffer* tb; u32 size; };
static rml_pending_transfer  g_rml_upload_transfers[RML_UPLOAD_MAX_TRANSFERS];
static u32                   g_rml_upload_transfer_count = 0;

struct rml_pooled_buffer   { SDL_GPUBuffer* buf; u32 size; SDL_GPUBufferUsageFlags usage; };
struct rml_pooled_transfer { SDL_GPUTransferBuffer* tb; u32 size; };
#define RML_BUFFER_POOL_CAP   1024
#define RML_TRANSFER_POOL_CAP 512
static rml_pooled_buffer   g_rml_buffer_pool[RML_BUFFER_POOL_CAP];
static u32                 g_rml_buffer_pool_count = 0;
static rml_pooled_transfer g_rml_transfer_pool[RML_TRANSFER_POOL_CAP];
static u32                 g_rml_transfer_pool_count = 0;

// Round a byte size up to the pool bucket (pow2, min 256) so text quads of slightly varying
// sizes still hit the same pool slots.
static u32 rml_pool_bucket(u32 size)
{
    u32 bucket = 256;
    while (bucket < size) bucket <<= 1;
    return bucket;
}

static void rml_buffer_pool_release(SDL_GPUBuffer* buf, u32 alloc_size, SDL_GPUBufferUsageFlags usage)
{
    if (buf == NULL) return;
    if (alloc_size == 0 || g_rml_buffer_pool_count >= RML_BUFFER_POOL_CAP)
    {
        SDL_ReleaseGPUBuffer(g_sdl.device, buf);
        return;
    }
    g_rml_buffer_pool[g_rml_buffer_pool_count].buf   = buf;
    g_rml_buffer_pool[g_rml_buffer_pool_count].size  = alloc_size;
    g_rml_buffer_pool[g_rml_buffer_pool_count].usage = usage;
    g_rml_buffer_pool_count++;
}

static void rml_transfer_pool_release(SDL_GPUTransferBuffer* tb, u32 alloc_size)
{
    if (tb == NULL) return;
    if (alloc_size == 0 || g_rml_transfer_pool_count >= RML_TRANSFER_POOL_CAP)
    {
        SDL_ReleaseGPUTransferBuffer(g_sdl.device, tb);
        return;
    }
    g_rml_transfer_pool[g_rml_transfer_pool_count].tb   = tb;
    g_rml_transfer_pool[g_rml_transfer_pool_count].size = alloc_size;
    g_rml_transfer_pool_count++;
}

// Free everything held by the pools (shutdown only; the GPU must be idle).
static void rml_pools_destroy(void)
{
    for (u32 i = 0; i < g_rml_buffer_pool_count; ++i)
        SDL_ReleaseGPUBuffer(g_sdl.device, g_rml_buffer_pool[i].buf);
    g_rml_buffer_pool_count = 0;
    for (u32 i = 0; i < g_rml_transfer_pool_count; ++i)
        SDL_ReleaseGPUTransferBuffer(g_sdl.device, g_rml_transfer_pool[i].tb);
    g_rml_transfer_pool_count = 0;
}

static void rml_upload_flush(void)
{
    if (!g_rml_upload_cmd) return;
    SDL_EndGPUCopyPass(g_rml_upload_pass);
    g_rml_upload_pass = NULL;
    SDL_SubmitGPUCommandBuffer(g_rml_upload_cmd);
    g_rml_upload_cmd = NULL;
    // Return transfer buffers to the pool for reuse next frame (mapping with cycle=true makes
    // reuse safe even while this frame's copies are still in flight).
    for (u32 i = 0; i < g_rml_upload_transfer_count; ++i)
        rml_transfer_pool_release(g_rml_upload_transfers[i].tb, g_rml_upload_transfers[i].size);
    g_rml_upload_transfer_count = 0;
}

// Lazily open (or return) the shared upload copy pass. Returns NULL on failure.
static SDL_GPUCopyPass* rml_upload_pass_begin(void)
{
    if (g_rml_upload_transfer_count >= RML_UPLOAD_MAX_TRANSFERS) rml_upload_flush(); // cap safety
    if (!g_rml_upload_cmd)
    {
        g_rml_upload_cmd = SDL_AcquireGPUCommandBuffer(g_sdl.device);
        if (!g_rml_upload_cmd) return NULL;
        g_rml_upload_pass = SDL_BeginGPUCopyPass(g_rml_upload_cmd);
    }
    return g_rml_upload_pass;
}

// Create (or reuse from the pool) a GPU buffer and record its upload into the shared per-frame
// RmlUi upload batch (NOT submitted here) — safe while the frame's render pass is open. The batch
// is flushed in end_frame before the main command buffer is submitted. On success *out_alloc gets
// the pooled allocation size (pass it back to rml_buffer_pool_release). Returns NULL on failure.
static SDL_GPUBuffer* sdlgpu_create_buffer_with_data(SDL_GPUBufferUsageFlags usage,
                                                     const void* data, u32 size, u32* out_alloc,
                                                     const char* ctx)
{
    if (out_alloc) *out_alloc = 0;
    if (size == 0) { BS_LOG_ERROR("%s: zero-size buffer.", ctx); return NULL; }
    const u32 bucket = rml_pool_bucket(size);

    // GPU buffer: pool first, create on miss.
    SDL_GPUBuffer* buf = NULL;
    for (u32 i = 0; i < g_rml_buffer_pool_count; ++i)
    {
        if (g_rml_buffer_pool[i].usage == usage && g_rml_buffer_pool[i].size == bucket)
        {
            buf = g_rml_buffer_pool[i].buf;
            g_rml_buffer_pool[i] = g_rml_buffer_pool[--g_rml_buffer_pool_count];
            g_rml_stat_pool_hits++;
            break;
        }
    }
    if (!buf)
    {
        SDL_GPUBufferCreateInfo binfo;
        SDL_zero(binfo);
        binfo.usage = usage;
        binfo.size  = bucket;
        buf = SDL_CreateGPUBuffer(g_sdl.device, &binfo);
        if (!buf) { BS_LOG_ERROR("%s: SDL_CreateGPUBuffer failed: %s", ctx, SDL_GetError()); return NULL; }
        g_rml_stat_creates++;
    }

    // Transfer buffer: pool first, create on miss.
    SDL_GPUTransferBuffer* tb = NULL;
    for (u32 i = 0; i < g_rml_transfer_pool_count; ++i)
    {
        if (g_rml_transfer_pool[i].size == bucket)
        {
            tb = g_rml_transfer_pool[i].tb;
            g_rml_transfer_pool[i] = g_rml_transfer_pool[--g_rml_transfer_pool_count];
            g_rml_stat_pool_hits++;
            break;
        }
    }
    if (!tb)
    {
        SDL_GPUTransferBufferCreateInfo tinfo;
        SDL_zero(tinfo);
        tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tinfo.size  = bucket;
        tb = SDL_CreateGPUTransferBuffer(g_sdl.device, &tinfo);
        if (!tb) { BS_LOG_ERROR("%s: transfer buffer failed: %s", ctx, SDL_GetError()); rml_buffer_pool_release(buf, bucket, usage); return NULL; }
        g_rml_stat_creates++;
    }

    // cycle=true: if the buffer is still referenced by in-flight commands, SDL swaps in a fresh
    // backing allocation instead of stalling — required for safe pool reuse across frames.
    void* map = SDL_MapGPUTransferBuffer(g_sdl.device, tb, true);
    SDL_memcpy(map, data, size);
    SDL_UnmapGPUTransferBuffer(g_sdl.device, tb);

    SDL_GPUCopyPass* cp = rml_upload_pass_begin();
    if (!cp) { BS_LOG_ERROR("%s: upload command buffer failed: %s", ctx, SDL_GetError()); rml_transfer_pool_release(tb, bucket); rml_buffer_pool_release(buf, bucket, usage); return NULL; }

    SDL_GPUTransferBufferLocation src; SDL_zero(src);
    src.transfer_buffer = tb; src.offset = 0;
    SDL_GPUBufferRegion dst; SDL_zero(dst);
    dst.buffer = buf; dst.offset = 0; dst.size = size;
    SDL_UploadToGPUBuffer(cp, &src, &dst, true);
    g_rml_upload_transfers[g_rml_upload_transfer_count].tb   = tb; // pooled after the batch submit
    g_rml_upload_transfers[g_rml_upload_transfer_count].size = bucket;
    g_rml_upload_transfer_count++;

    if (out_alloc) *out_alloc = bucket;
    return buf;
}

// Create + upload a standalone RGBA8 sampler texture (not pooled). Returns NULL on failure.
static SDL_GPUTexture* sdlgpu_create_rgba_texture(const u8* pixels, u32 width, u32 height, const char* ctx)
{
    SDL_GPUTextureCreateInfo info;
    SDL_zero(info);
    info.type                 = SDL_GPU_TEXTURETYPE_2D;
    info.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width                = width;
    info.height               = height;
    info.layer_count_or_depth = 1;
    info.num_levels           = 1;

    SDL_GPUTexture* tex = SDL_CreateGPUTexture(g_sdl.device, &info);
    if (!tex) { BS_LOG_ERROR("%s: SDL_CreateGPUTexture failed: %s", ctx, SDL_GetError()); return NULL; }

    if (!sdlgpu_upload_texture_pixels(tex, pixels, width, height, ctx))
    {
        SDL_ReleaseGPUTexture(g_sdl.device, tex);
        return NULL;
    }
    return tex;
}

// -------------------------------------------------------------------------------------
// RenderInterface implementation (reopened anonymous namespace so the method definitions see the
// anon-namespace class + g_rml, while freely using the file-scope backend helpers/globals).
// -------------------------------------------------------------------------------------
namespace {

Rml::CompiledGeometryHandle BsRmlRenderInterface::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                                  Rml::Span<const int> indices)
{
    const u32 vsize = (u32)(vertices.size() * sizeof(Rml::Vertex));
    const u32 isize = (u32)(indices.size()  * sizeof(int));
    if (vsize == 0 || isize == 0) return 0;
    g_rml_stat_compiles++;

    u32 valloc = 0, ialloc = 0;
    SDL_GPUBuffer* vbuf = sdlgpu_create_buffer_with_data(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(), vsize, &valloc, "rml CompileGeometry(vbuf)");
    if (!vbuf) return 0;
    SDL_GPUBuffer* ibuf = sdlgpu_create_buffer_with_data(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), isize, &ialloc, "rml CompileGeometry(ibuf)");
    if (!ibuf) { rml_buffer_pool_release(vbuf, valloc, SDL_GPU_BUFFERUSAGE_VERTEX); return 0; }

    bs_rml_geometry* geom = new bs_rml_geometry{ vbuf, ibuf, (u32)indices.size(), valloc, ialloc };
    return (Rml::CompiledGeometryHandle)geom;
}

void BsRmlRenderInterface::RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                                          Rml::TextureHandle texture)
{
    if (!g_sdl.pass || !g_rml.pipeline) return;
    bs_rml_geometry* geom = (bs_rml_geometry*)geometry;
    if (!geom) return;

    SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_rml.pipeline);

    SDL_GPUBufferBinding vb; SDL_zero(vb); vb.buffer = geom->vbuf; vb.offset = 0;
    SDL_BindGPUVertexBuffers(g_sdl.pass, 0, &vb, 1);
    SDL_GPUBufferBinding ib; SDL_zero(ib); ib.buffer = geom->ibuf; ib.offset = 0;
    SDL_BindGPUIndexBuffer(g_sdl.pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    SDL_GPUTexture* tex = texture ? (SDL_GPUTexture*)texture : g_rml.white_tex;
    SDL_GPUTextureSamplerBinding tsb; SDL_zero(tsb);
    tsb.texture = tex; tsb.sampler = g_sdl.sampler_linear;
    SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);

    // Vertex uniform: mvp = project * transform, plus the per-draw pixel translation. project is a
    // top-left-origin orthographic matrix sized to the current swapchain (RmlUi submits geometry in
    // pixels from the top-left). Column-major throughout, matching the HLSL mul(mvp, ...).
    const Mat4 project = mat4_ortho(0.0f, (f32)g_sdl.swap_width, (f32)g_sdl.swap_height, 0.0f, -10000.0f, 10000.0f);
    const Mat4 mvp     = g_rml.has_transform ? mat4_mul(project, g_rml.transform) : project;

    struct { f32 mvp[16]; f32 translation[2]; f32 pad[2]; } ubo;
    SDL_memcpy(ubo.mvp, mvp.data, sizeof(ubo.mvp));
    ubo.translation[0] = translation.x;
    ubo.translation[1] = translation.y;
    ubo.pad[0] = ubo.pad[1] = 0.0f;
    SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, &ubo, sizeof(ubo));

    // Scissor: apply the current region (clamped to the framebuffer) or reset to full.
    SDL_Rect sc;
    if (g_rml.scissor_enabled) sc = g_rml.scissor;
    else { sc.x = 0; sc.y = 0; sc.w = (int)g_sdl.swap_width; sc.h = (int)g_sdl.swap_height; }
    if (sc.x < 0) { sc.w += sc.x; sc.x = 0; }
    if (sc.y < 0) { sc.h += sc.y; sc.y = 0; }
    if (sc.w < 0) sc.w = 0;
    if (sc.h < 0) sc.h = 0;
    if (sc.x + sc.w > (int)g_sdl.swap_width)  sc.w = (int)g_sdl.swap_width  - sc.x;
    if (sc.y + sc.h > (int)g_sdl.swap_height) sc.h = (int)g_sdl.swap_height - sc.y;
    SDL_SetGPUScissor(g_sdl.pass, &sc);

    SDL_DrawGPUIndexedPrimitives(g_sdl.pass, geom->index_count, 1, 0, 0, 0);
}

void BsRmlRenderInterface::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    bs_rml_geometry* geom = (bs_rml_geometry*)geometry;
    if (!geom) return;
    // Returned to the pool for reuse, not destroyed (avoids per-frame create/release churn).
    rml_buffer_pool_release(geom->vbuf, geom->vbuf_alloc, SDL_GPU_BUFFERUSAGE_VERTEX);
    rml_buffer_pool_release(geom->ibuf, geom->ibuf_alloc, SDL_GPU_BUFFERUSAGE_INDEX);
    delete geom;
}

Rml::TextureHandle BsRmlRenderInterface::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
    // stb_image is built with STBI_NO_STDIO (see stb_image_impl.cpp), so decode from a memory
    // buffer rather than a file path. SDL_LoadFile reads the bytes relative to the CWD, matching
    // RmlUi's default FileInterface (which resolves texture paths the same way).
    size_t file_size = 0;
    void*  file_data = SDL_LoadFile(source.c_str(), &file_size);
    if (!file_data)
    {
        BS_LOG_ERROR("rml LoadTexture: failed to read '%s': %s", source.c_str(), SDL_GetError());
        return 0;
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory((const stbi_uc*)file_data, (int)file_size, &w, &h, &comp, 4);
    SDL_free(file_data);
    if (!pixels)
    {
        BS_LOG_ERROR("rml LoadTexture: failed to decode '%s': %s", source.c_str(), stbi_failure_reason());
        return 0;
    }

    // RmlUi expects premultiplied-alpha textures (it multiplies by premultiplied vertex colors).
    const int pixel_count = w * h;
    for (int i = 0; i < pixel_count; ++i)
    {
        u8* p = pixels + i * 4;
        const u32 a = p[3];
        p[0] = (u8)((p[0] * a) / 255u);
        p[1] = (u8)((p[1] * a) / 255u);
        p[2] = (u8)((p[2] * a) / 255u);
    }

    SDL_GPUTexture* tex = sdlgpu_create_rgba_texture(pixels, (u32)w, (u32)h, "rml LoadTexture");
    stbi_image_free(pixels);
    if (!tex) return 0;

    texture_dimensions.x = w;
    texture_dimensions.y = h;
    return (Rml::TextureHandle)tex;
}

Rml::TextureHandle BsRmlRenderInterface::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i source_dimensions)
{
    if (source_dimensions.x <= 0 || source_dimensions.y <= 0) return 0;
    SDL_GPUTexture* tex = sdlgpu_create_rgba_texture((const u8*)source.data(),
                                                     (u32)source_dimensions.x, (u32)source_dimensions.y,
                                                     "rml GenerateTexture");
    return tex ? (Rml::TextureHandle)tex : 0;
}

void BsRmlRenderInterface::ReleaseTexture(Rml::TextureHandle texture)
{
    if (!texture) return;
    // SDL defers the actual destruction until the GPU is finished with the texture, so this is
    // safe to call even mid-frame without a manual SDL_WaitForGPUIdle.
    SDL_ReleaseGPUTexture(g_sdl.device, (SDL_GPUTexture*)texture);
}

void BsRmlRenderInterface::EnableScissorRegion(bool enable)
{
    g_rml.scissor_enabled = enable ? TRUE : FALSE;
}

void BsRmlRenderInterface::SetScissorRegion(Rml::Rectanglei region)
{
    g_rml.scissor.x = region.Left();
    g_rml.scissor.y = region.Top();
    g_rml.scissor.w = region.Width();
    g_rml.scissor.h = region.Height();
}

void BsRmlRenderInterface::SetTransform(const Rml::Matrix4f* transform)
{
    if (!transform) { g_rml.has_transform = FALSE; return; }
    g_rml.has_transform = TRUE;
    // Rml::Matrix4f is column-major by default (Config.h); data() returns 16 floats in column-major
    // order, matching the engine's Mat4 layout (data[col*4+row]) — a direct copy is correct.
    SDL_memcpy(g_rml.transform.data, transform->data(), sizeof(f32) * 16u);
}

// --- Frosted-glass backdrop shader --------------------------------------------------------------
// The custom "backdrop" decorator (below) compiles this "shader" to fill a panel with a tinted,
// blurred copy of the scene behind it. Only "backdrop" is handled; every other name returns 0.
Rml::CompiledShaderHandle BsRmlRenderInterface::CompileShader(const Rml::String& name, const Rml::Dictionary& parameters)
{
    if (name != "backdrop") return 0;

    bs_rml_shader* sh = new bs_rml_shader{};
    // Defaults (dark blue glass) used when the decorator omits a tint.
    sh->tint[0] = 0.05f; sh->tint[1] = 0.09f; sh->tint[2] = 0.15f; sh->tint[3] = 0.55f;

    auto read = [&](const char* key, f32& out) {
        auto it = parameters.find(key);
        if (it != parameters.end()) out = it->second.Get<float>(out);
    };
    read("tr", sh->tint[0]);
    read("tg", sh->tint[1]);
    read("tb", sh->tint[2]);
    read("ta", sh->tint[3]);
    return (Rml::CompiledShaderHandle)sh;
}

void BsRmlRenderInterface::RenderShader(Rml::CompiledShaderHandle shader, Rml::CompiledGeometryHandle geometry,
                                        Rml::Vector2f translation, Rml::TextureHandle /*texture*/)
{
    if (!g_sdl.pass || !g_rml.pipeline_backdrop || !g_sdl.ui_backdrop_rt) return;
    bs_rml_shader*   sh   = (bs_rml_shader*)shader;
    bs_rml_geometry* geom = (bs_rml_geometry*)geometry;
    if (!sh || !geom) return;

    SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_rml.pipeline_backdrop);

    SDL_GPUBufferBinding vb; SDL_zero(vb); vb.buffer = geom->vbuf; vb.offset = 0;
    SDL_BindGPUVertexBuffers(g_sdl.pass, 0, &vb, 1);
    SDL_GPUBufferBinding ib; SDL_zero(ib); ib.buffer = geom->ibuf; ib.offset = 0;
    SDL_BindGPUIndexBuffer(g_sdl.pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // Fragment samples the pre-UI blurred scene in screen space (see ui_backdrop.frag.hlsl).
    SDL_GPUTextureSamplerBinding tsb; SDL_zero(tsb);
    tsb.texture = g_sdl.ui_backdrop_rt; tsb.sampler = g_sdl.sampler_linear;
    SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);

    // Vertex uniform: same ortho * transform + per-draw translation as the normal UI path, so a
    // dragged panel's frost stays screen-correct (translation is live each frame).
    const Mat4 project = mat4_ortho(0.0f, (f32)g_sdl.swap_width, (f32)g_sdl.swap_height, 0.0f, -10000.0f, 10000.0f);
    const Mat4 mvp     = g_rml.has_transform ? mat4_mul(project, g_rml.transform) : project;
    struct { f32 mvp[16]; f32 translation[2]; f32 pad[2]; } vubo;
    SDL_memcpy(vubo.mvp, mvp.data, sizeof(vubo.mvp));
    vubo.translation[0] = translation.x;
    vubo.translation[1] = translation.y;
    vubo.pad[0] = vubo.pad[1] = 0.0f;
    SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, &vubo, sizeof(vubo));

    // Fragment uniform: 1/screen (for screen-space uv) + the tint.
    struct { f32 inv_screen[4]; f32 tint[4]; } fubo;
    fubo.inv_screen[0] = (g_sdl.swap_width  > 0) ? 1.0f / (f32)g_sdl.swap_width  : 0.0f;
    fubo.inv_screen[1] = (g_sdl.swap_height > 0) ? 1.0f / (f32)g_sdl.swap_height : 0.0f;
    fubo.inv_screen[2] = 0.0f; fubo.inv_screen[3] = 0.0f;
    fubo.tint[0] = sh->tint[0]; fubo.tint[1] = sh->tint[1]; fubo.tint[2] = sh->tint[2]; fubo.tint[3] = sh->tint[3];
    SDL_PushGPUFragmentUniformData(g_sdl.cmd, 0, &fubo, sizeof(fubo));

    // Scissor: current region (clamped to the framebuffer) or reset to full.
    SDL_Rect sc;
    if (g_rml.scissor_enabled) sc = g_rml.scissor;
    else { sc.x = 0; sc.y = 0; sc.w = (int)g_sdl.swap_width; sc.h = (int)g_sdl.swap_height; }
    if (sc.x < 0) { sc.w += sc.x; sc.x = 0; }
    if (sc.y < 0) { sc.h += sc.y; sc.y = 0; }
    if (sc.w < 0) sc.w = 0;
    if (sc.h < 0) sc.h = 0;
    if (sc.x + sc.w > (int)g_sdl.swap_width)  sc.w = (int)g_sdl.swap_width  - sc.x;
    if (sc.y + sc.h > (int)g_sdl.swap_height) sc.h = (int)g_sdl.swap_height - sc.y;
    SDL_SetGPUScissor(g_sdl.pass, &sc);

    SDL_DrawGPUIndexedPrimitives(g_sdl.pass, geom->index_count, 1, 0, 0, 0);
}

void BsRmlRenderInterface::ReleaseShader(Rml::CompiledShaderHandle shader)
{
    delete (bs_rml_shader*)shader;
}

} // anonymous namespace

// -------------------------------------------------------------------------------------
// Frosted-glass "backdrop" decorator. Registered with RmlUi so RCSS can request a real scene-blur
// behind a panel:  decorator: backdrop( <tint-color> );
// It builds a rounded-box mesh (respecting border-radius) and binds our custom "backdrop" shader,
// which the render interface draws by sampling the blurred ui_backdrop_rt in screen space. The tint
// colour's alpha channel is the tint STRENGTH (0 = pure blurred scene, 255 = pure tint colour).
// -------------------------------------------------------------------------------------
namespace {

struct BsBackdropElementData
{
    Rml::Geometry       geometry;
    Rml::CompiledShader shader;
};

class BsBackdropDecorator : public Rml::Decorator
{
public:
    bool Initialise(Rml::Colourb in_tint) { tint = in_tint; return true; }

    Rml::DecoratorDataHandle GenerateElementData(Rml::Element* element, Rml::BoxArea paint_area) const override
    {
        Rml::RenderManager* rm = element->GetRenderManager();
        if (!rm) return Rml::Decorator::INVALID_DECORATORDATAHANDLE;

        // Serialize the tint as normalized floats through the shader parameters (see CompileShader).
        Rml::Dictionary params;
        params["tr"] = Rml::Variant(tint.red   / 255.0f);
        params["tg"] = Rml::Variant(tint.green / 255.0f);
        params["tb"] = Rml::Variant(tint.blue  / 255.0f);
        params["ta"] = Rml::Variant(tint.alpha / 255.0f);
        Rml::CompiledShader shader = rm->CompileShader("backdrop", params);
        if (!shader) return Rml::Decorator::INVALID_DECORATORDATAHANDLE;

        const Rml::RenderBox render_box = element->GetRenderBox(paint_area);
        const Rml::ComputedValues& computed = element->GetComputedValues();
        const Rml::byte alpha = Rml::byte(computed.opacity() * 255.0f);

        Rml::Mesh mesh;
        Rml::MeshUtilities::GenerateBackground(mesh, render_box, Rml::ColourbPremultiplied(alpha, alpha));

        BsBackdropElementData* data = new BsBackdropElementData{ rm->MakeGeometry(std::move(mesh)), std::move(shader) };
        return reinterpret_cast<Rml::DecoratorDataHandle>(data);
    }

    void ReleaseElementData(Rml::DecoratorDataHandle handle) const override
    {
        delete reinterpret_cast<BsBackdropElementData*>(handle);
    }

    void RenderElement(Rml::Element* element, Rml::DecoratorDataHandle handle) const override
    {
        BsBackdropElementData* data = reinterpret_cast<BsBackdropElementData*>(handle);
        data->geometry.Render(element->GetAbsoluteOffset(Rml::BoxArea::Border), {}, data->shader);
    }

private:
    Rml::Colourb tint;
};

class BsBackdropDecoratorInstancer : public Rml::DecoratorInstancer
{
public:
    BsBackdropDecoratorInstancer()
    {
        id_tint = RegisterProperty("tint", "#0d1a2a8c").AddParser("color").GetId();
        RegisterShorthand("decorator", "tint", Rml::ShorthandType::FallThrough);
    }

    Rml::SharedPtr<Rml::Decorator> InstanceDecorator(const Rml::String& /*name*/, const Rml::PropertyDictionary& properties,
                                                     const Rml::DecoratorInstancerInterface& /*iface*/) override
    {
        Rml::Colourb tint = properties.GetProperty(id_tint)->Get<Rml::Colourb>();
        auto decorator = Rml::MakeShared<BsBackdropDecorator>();
        if (decorator->Initialise(tint)) return decorator;
        return nullptr;
    }

private:
    Rml::PropertyId id_tint = {};
};

} // anonymous namespace

// -------------------------------------------------------------------------------------
// UI pipeline + init helpers.
// -------------------------------------------------------------------------------------
static b8 bs_rml_create_pipeline(void)
{
    SDL_GPUShader* vs = load_shader(g_sdl.device, "rml", "vert", SDL_GPU_SHADERSTAGE_VERTEX,   0, 1);
    SDL_GPUShader* fs = load_shader(g_sdl.device, "rml", "frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    if (!vs || !fs)
    {
        if (vs) SDL_ReleaseGPUShader(g_sdl.device, vs);
        if (fs) SDL_ReleaseGPUShader(g_sdl.device, fs);
        return FALSE;
    }

    SDL_GPUVertexBufferDescription vbuf_desc;
    SDL_zero(vbuf_desc);
    vbuf_desc.slot       = 0;
    vbuf_desc.pitch      = sizeof(Rml::Vertex);
    vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    // Rml::Vertex: position (float2 @0), colour (premult RGBA8 @8), tex_coord (float2 @12).
    SDL_GPUVertexAttribute attrs[3];
    SDL_zero(attrs);
    attrs[0].location = 0; attrs[0].buffer_slot = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;      attrs[0].offset = offsetof(Rml::Vertex, position);
    attrs[1].location = 1; attrs[1].buffer_slot = 0; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM; attrs[1].offset = offsetof(Rml::Vertex, colour);
    attrs[2].location = 2; attrs[2].buffer_slot = 0; attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;      attrs[2].offset = offsetof(Rml::Vertex, tex_coord);

    // Premultiplied-alpha over blend (RmlUi convention): src=ONE, dst=ONE_MINUS_SRC_ALPHA.
    SDL_GPUColorTargetBlendState blend;
    SDL_zero(blend);
    blend.color_write_mask     = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
                                 SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    blend.enable_blend         = true;
    blend.color_blend_op       = SDL_GPU_BLENDOP_ADD;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op       = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    SDL_GPUColorTargetDescription color_target;
    SDL_zero(color_target);
    color_target.format      = get_corrected_swapchain_format(g_sdl.device, g_sdl.window);
    color_target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader   = vs;
    info.fragment_shader = fs;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    info.vertex_input_state.num_vertex_buffers         = 1;
    info.vertex_input_state.vertex_attributes          = attrs;
    info.vertex_input_state.num_vertex_attributes      = 3;
    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets         = 1;
    info.target_info.has_depth_stencil_target  = false;

    g_rml.pipeline = SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
    SDL_ReleaseGPUShader(g_sdl.device, vs);
    SDL_ReleaseGPUShader(g_sdl.device, fs);

    if (!g_rml.pipeline)
    {
        BS_LOG_FATAL("bs_rml: UI pipeline creation failed: %s", SDL_GetError());
        return FALSE;
    }
    return TRUE;
}

// Frosted-glass backdrop pipeline: same vertex layout / premult-alpha blend / swapchain format as the
// UI pipeline, but with the ui_backdrop fragment shader (samples the blurred scene). Reuses rml.vert.
static b8 bs_rml_create_backdrop_pipeline(void)
{
    SDL_GPUShader* vs = load_shader(g_sdl.device, "rml",         "vert", SDL_GPU_SHADERSTAGE_VERTEX,   0, 1);
    SDL_GPUShader* fs = load_shader(g_sdl.device, "ui_backdrop", "frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 1);
    if (!vs || !fs)
    {
        if (vs) SDL_ReleaseGPUShader(g_sdl.device, vs);
        if (fs) SDL_ReleaseGPUShader(g_sdl.device, fs);
        return FALSE;
    }

    SDL_GPUVertexBufferDescription vbuf_desc;
    SDL_zero(vbuf_desc);
    vbuf_desc.slot       = 0;
    vbuf_desc.pitch      = sizeof(Rml::Vertex);
    vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attrs[3];
    SDL_zero(attrs);
    attrs[0].location = 0; attrs[0].buffer_slot = 0; attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;      attrs[0].offset = offsetof(Rml::Vertex, position);
    attrs[1].location = 1; attrs[1].buffer_slot = 0; attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM; attrs[1].offset = offsetof(Rml::Vertex, colour);
    attrs[2].location = 2; attrs[2].buffer_slot = 0; attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;      attrs[2].offset = offsetof(Rml::Vertex, tex_coord);

    SDL_GPUColorTargetBlendState blend;
    SDL_zero(blend);
    blend.color_write_mask     = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
                                 SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    blend.enable_blend         = true;
    blend.color_blend_op       = SDL_GPU_BLENDOP_ADD;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op       = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    SDL_GPUColorTargetDescription color_target;
    SDL_zero(color_target);
    color_target.format      = get_corrected_swapchain_format(g_sdl.device, g_sdl.window);
    color_target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info;
    SDL_zero(info);
    info.vertex_shader   = vs;
    info.fragment_shader = fs;
    info.primitive_type  = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    info.vertex_input_state.num_vertex_buffers         = 1;
    info.vertex_input_state.vertex_attributes          = attrs;
    info.vertex_input_state.num_vertex_attributes      = 3;
    info.rasterizer_state.fill_mode  = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode  = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets         = 1;
    info.target_info.has_depth_stencil_target  = false;

    g_rml.pipeline_backdrop = SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
    SDL_ReleaseGPUShader(g_sdl.device, vs);
    SDL_ReleaseGPUShader(g_sdl.device, fs);

    if (!g_rml.pipeline_backdrop)
    {
        BS_LOG_ERROR("bs_rml: backdrop pipeline creation failed: %s", SDL_GetError());
        return FALSE;
    }
    return TRUE;
}

// -------------------------------------------------------------------------------------
// Input translation helpers.
// -------------------------------------------------------------------------------------
static int bs_rml_key_modifiers(void)
{
    SDL_Keymod m = SDL_GetModState();
    int r = 0;
    if (m & SDL_KMOD_SHIFT) r |= Rml::Input::KM_SHIFT;
    if (m & SDL_KMOD_CTRL)  r |= Rml::Input::KM_CTRL;
    if (m & SDL_KMOD_ALT)   r |= Rml::Input::KM_ALT;
    if (m & SDL_KMOD_GUI)   r |= Rml::Input::KM_META;
    if (m & SDL_KMOD_CAPS)  r |= Rml::Input::KM_CAPSLOCK;
    if (m & SDL_KMOD_NUM)   r |= Rml::Input::KM_NUMLOCK;
    return r;
}

// Map an SDL scancode to a RmlUi key identifier. Covers letters (for shortcuts) and the navigation
// / editing keys a text field needs; character entry itself flows through SDL_EVENT_TEXT_INPUT.
static Rml::Input::KeyIdentifier bs_rml_scancode_to_key(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z)
        return (Rml::Input::KeyIdentifier)(Rml::Input::KI_A + (sc - SDL_SCANCODE_A));

    switch (sc)
    {
        case SDL_SCANCODE_RETURN:
        case SDL_SCANCODE_KP_ENTER: return Rml::Input::KI_RETURN;
        case SDL_SCANCODE_BACKSPACE: return Rml::Input::KI_BACK;
        case SDL_SCANCODE_DELETE:    return Rml::Input::KI_DELETE;
        case SDL_SCANCODE_TAB:       return Rml::Input::KI_TAB;
        case SDL_SCANCODE_ESCAPE:    return Rml::Input::KI_ESCAPE;
        case SDL_SCANCODE_LEFT:      return Rml::Input::KI_LEFT;
        case SDL_SCANCODE_RIGHT:     return Rml::Input::KI_RIGHT;
        case SDL_SCANCODE_UP:        return Rml::Input::KI_UP;
        case SDL_SCANCODE_DOWN:      return Rml::Input::KI_DOWN;
        case SDL_SCANCODE_HOME:      return Rml::Input::KI_HOME;
        case SDL_SCANCODE_END:       return Rml::Input::KI_END;
        case SDL_SCANCODE_PAGEUP:    return Rml::Input::KI_PRIOR;
        case SDL_SCANCODE_PAGEDOWN:  return Rml::Input::KI_NEXT;
        default:                     return Rml::Input::KI_UNKNOWN;
    }
}

// SDL mouse buttons are 1=left, 2=middle, 3=right; RmlUi wants 0=left, 1=right, 2=middle.
static int bs_rml_mouse_button(Uint8 sdl_button)
{
    switch (sdl_button)
    {
        case SDL_BUTTON_LEFT:   return 0;
        case SDL_BUTTON_RIGHT:  return 1;
        case SDL_BUTTON_MIDDLE: return 2;
        default:                return -1;
    }
}

// Convert logical (window) mouse coordinates to swapchain pixel coordinates (the context uses
// pixel dimensions). On a 1.0x display the scale is 1.0, so this is a no-op there.
static void bs_rml_scale_mouse(float lx, float ly, int* out_x, int* out_y)
{
    int lw = 0, lh = 0, pw = 0, ph = 0;
    SDL_GetWindowSize(g_sdl.window, &lw, &lh);
    SDL_GetWindowSizeInPixels(g_sdl.window, &pw, &ph);
    float sx = (lw > 0) ? ((float)pw / (float)lw) : 1.0f;
    float sy = (lh > 0) ? ((float)ph / (float)lh) : 1.0f;
    *out_x = (int)(lx * sx);
    *out_y = (int)(ly * sy);
}

// -------------------------------------------------------------------------------------
// bs_rml facade (renderer/bs_rml.h).
// -------------------------------------------------------------------------------------
b8 bs_rml_initialize(void)
{
    if (g_rml.active) { BS_LOG_WARN("bs_rml_initialize called twice; ignoring."); return TRUE; }
    if (!g_sdl.device || !g_sdl.window)
    {
        BS_LOG_ERROR("bs_rml_initialize: GPU device/window not ready (call after backend init).");
        return FALSE;
    }

    // Resolve the shared 1x1 white texture used for untextured geometry.
    gpu_texture* wt = pool_resolve_texture(g_sdl.white_texture);
    g_rml.white_tex = wt ? wt->tex : NULL;
    if (!g_rml.white_tex)
    {
        BS_LOG_ERROR("bs_rml_initialize: shared white texture not available.");
        return FALSE;
    }

    if (!bs_rml_create_pipeline()) return FALSE;

    // Frosted-glass backdrop pipeline is optional: if it fails, the "backdrop" decorator's RenderShader
    // no-ops and panels fall back to their solid background-color.
    if (!bs_rml_create_backdrop_pipeline())
        BS_LOG_WARN("bs_rml_initialize: frosted-glass backdrop unavailable (panels stay opaque).");

    Rml::SetSystemInterface(&g_rml.system_iface);
    Rml::SetRenderInterface(&g_rml.render_iface);

    if (!Rml::Initialise())
    {
        BS_LOG_ERROR("bs_rml_initialize: Rml::Initialise failed.");
        SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_rml.pipeline);
        g_rml.pipeline = NULL;
        return FALSE;
    }

    // Register the custom frosted-glass "backdrop" decorator (instancer kept alive for the process).
    {
        static BsBackdropDecoratorInstancer s_backdrop_instancer;
        Rml::Factory::RegisterDecoratorInstancer("backdrop", &s_backdrop_instancer);
    }

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_sdl.window, &pw, &ph);
    if (pw <= 0) pw = 1280;
    if (ph <= 0) ph = 720;

    g_rml.context = Rml::CreateContext("main", Rml::Vector2i(pw, ph));
    if (!g_rml.context)
    {
        BS_LOG_ERROR("bs_rml_initialize: Rml::CreateContext failed.");
        Rml::Shutdown();
        SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_rml.pipeline);
        g_rml.pipeline = NULL;
        return FALSE;
    }

    // Optional in-UI inspector (toggle with F12 via bs_rml_debugger_toggle). Harmless if it fails.
    Rml::Debugger::Initialise(g_rml.context);

    g_rml.transform       = mat4_identity();
    g_rml.has_transform   = FALSE;
    g_rml.scissor_enabled = FALSE;
    g_rml.wants_mouse     = FALSE;
    g_rml.active          = TRUE;

    BS_LOG_INFO("RmlUi initialized (context %dx%d, swapchain format-matched pipeline).", pw, ph);
    return TRUE;
}

void bs_rml_shutdown(void)
{
    if (!g_rml.active) return;

    // Flush any pending batched geometry uploads, then drain the GPU before releasing RmlUi's
    // device-bound resources, and do it BEFORE the backend destroys the device (renderer.cpp
    // ordering). Rml::Shutdown unloads all documents and calls
    // ReleaseGeometry/ReleaseTexture on our render interface, freeing the GPU buffers/textures.
    rml_upload_flush();
    if (g_sdl.device) SDL_WaitForGPUIdle(g_sdl.device);

    // Tear down the HUD data model + document BEFORE Rml::Shutdown so the RemoveDataModel /
    // UnloadDocument calls run while the context is still valid.
    bs_rml_hud_shutdown();

    Rml::Shutdown();
    g_rml.context = NULL;

    // Rml::Shutdown released all geometry into the pools; now actually destroy the pooled buffers.
    rml_pools_destroy();

    if (g_rml.pipeline)
    {
        SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_rml.pipeline);
        g_rml.pipeline = NULL;
    }
    if (g_rml.pipeline_backdrop)
    {
        SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_rml.pipeline_backdrop);
        g_rml.pipeline_backdrop = NULL;
    }

    g_rml.active = FALSE;
    BS_LOG_INFO("RmlUi shut down.");
}

i32 bs_rml_load_fonts(const char* fonts_dir)
{
    if (!g_rml.active || !fonts_dir) return 0;

    i32 loaded = 0;
    const char* patterns[2] = { "*.ttf", "*.otf" };
    for (int pi = 0; pi < 2; ++pi)
    {
        int count = 0;
        char** names = SDL_GlobDirectory(fonts_dir, patterns[pi], (SDL_GlobFlags)0, &count);
        if (!names) continue;
        for (int i = 0; i < count; ++i)
        {
            char full[512];
            SDL_snprintf(full, sizeof(full), "%s/%s", fonts_dir, names[i]);
            if (Rml::LoadFontFace(full)) ++loaded;
            else BS_LOG_WARN("bs_rml_load_fonts: failed to load '%s'.", full);
        }
        SDL_free(names);
    }

    BS_LOG_INFO("bs_rml_load_fonts: loaded %d face(s) from '%s'.", loaded, fonts_dir);
    return loaded;
}

bs_rml_document bs_rml_load_document(const char* rml_path)
{
    bs_rml_document handle;
    handle.ptr = NULL;
    if (!g_rml.active || !g_rml.context || !rml_path) return handle;

    Rml::ElementDocument* doc = g_rml.context->LoadDocument(rml_path);
    if (!doc)
    {
        BS_LOG_ERROR("bs_rml_load_document: failed to load '%s'.", rml_path);
        return handle;
    }
    handle.ptr = doc;
    return handle;
}

void bs_rml_show(bs_rml_document doc, b8 show)
{
    if (!doc.ptr) return;
    Rml::ElementDocument* d = (Rml::ElementDocument*)doc.ptr;
    if (show) d->Show();
    else      d->Hide();
}

void bs_rml_unload_document(bs_rml_document doc)
{
    if (!g_rml.active || !g_rml.context || !doc.ptr) return;
    g_rml.context->UnloadDocument((Rml::ElementDocument*)doc.ptr);
}

void bs_rml_update(void)
{
    if (!g_rml.active || !g_rml.context) return;

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(g_sdl.window, &pw, &ph);
    if (pw > 0 && ph > 0)
        g_rml.context->SetDimensions(Rml::Vector2i(pw, ph));

    g_rml.context->Update();
}

void bs_rml_process_event(const void* sdl_event)
{
    if (!g_rml.active || !g_rml.context || !sdl_event) return;
    const SDL_Event* ev = (const SDL_Event*)sdl_event;
    const int mods = bs_rml_key_modifiers();

    switch (ev->type)
    {
        case SDL_EVENT_MOUSE_MOTION:
        {
            int x = 0, y = 0;
            bs_rml_scale_mouse(ev->motion.x, ev->motion.y, &x, &y);
            // ProcessMouseMove returns TRUE when the mouse is NOT over any interactive element.
            g_rml.wants_mouse = g_rml.context->ProcessMouseMove(x, y, mods) ? FALSE : TRUE;
        } break;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        {
            int b = bs_rml_mouse_button(ev->button.button);
            if (b >= 0)
                g_rml.wants_mouse = g_rml.context->ProcessMouseButtonDown(b, mods) ? FALSE : TRUE;
        } break;

        case SDL_EVENT_MOUSE_BUTTON_UP:
        {
            int b = bs_rml_mouse_button(ev->button.button);
            if (b >= 0)
                g_rml.context->ProcessMouseButtonUp(b, mods);
        } break;

        case SDL_EVENT_MOUSE_WHEEL:
        {
            // SDL wheel.y > 0 = scroll up; RmlUi wheel_delta > 0 = scroll down. Invert.
            g_rml.context->ProcessMouseWheel(-ev->wheel.y, mods);
        } break;

        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        {
            g_rml.context->ProcessMouseLeave();
            g_rml.wants_mouse = FALSE;
        } break;

        case SDL_EVENT_KEY_DOWN:
        {
            Rml::Input::KeyIdentifier key = bs_rml_scancode_to_key(ev->key.scancode);
            if (key != Rml::Input::KI_UNKNOWN)
                g_rml.context->ProcessKeyDown(key, mods);
        } break;

        case SDL_EVENT_KEY_UP:
        {
            Rml::Input::KeyIdentifier key = bs_rml_scancode_to_key(ev->key.scancode);
            if (key != Rml::Input::KI_UNKNOWN)
                g_rml.context->ProcessKeyUp(key, mods);
        } break;

        case SDL_EVENT_TEXT_INPUT:
        {
            if (ev->text.text && ev->text.text[0])
                g_rml.context->ProcessTextInput(Rml::String(ev->text.text));
        } break;

        default: break;
    }
}

b8 bs_rml_wants_mouse(void)
{
    return (g_rml.active && g_rml.wants_mouse) ? TRUE : FALSE;
}

b8 bs_rml_wants_keyboard(void)
{
    if (!g_rml.active || !g_rml.context) return FALSE;
    Rml::Element* focus = g_rml.context->GetFocusElement();
    if (!focus) return FALSE;
    const Rml::String& tag = focus->GetTagName();
    return (tag == "input" || tag == "textarea") ? TRUE : FALSE;
}

void bs_rml_on_resize(u16 width, u16 height)
{
    if (!g_rml.active || !g_rml.context) return;
    g_rml.context->SetDimensions(Rml::Vector2i((int)width, (int)height));
}

void bs_rml_debugger_toggle(void)
{
    if (!g_rml.active || !g_rml.context) return;
    bool vis = !Rml::Debugger::IsVisible();
    Rml::Debugger::SetVisible(vis);
    BS_LOG_INFO("bs_rml_debugger_toggle: RmlUi debugger %s.", vis ? "shown" : "hidden");
}

// -------------------------------------------------------------------------------------
// In-game HUD data model (Phase 4). One "hud" data model backs the whole in-game document; the
// game pushes a POD snapshot each frame (bs_rml_hud_update) and drains click actions (poll).
// -------------------------------------------------------------------------------------

// A repeated action-log line, exposed to RML as { text }.
struct BsRmlHudLog
{
    Rml::String text;
};

// A repeated discovery line, exposed to RML as { text, color } (color is an RCSS colour string).
struct BsRmlHudDisc
{
    Rml::String text;
    Rml::String color;
};

// A repeated fleet-ship weapon row, exposed to RML as { text, glyph, action, selected, empty }.
// `glyph` is the placeholder symbol shown in the Arsenal drag-queue (unused by the fleet panel).
struct BsRmlHudWeapon
{
    Rml::String text;
    Rml::String glyph;
    Rml::String action;
    Rml::String drop;
    bool        selected = false;
    bool        empty    = false;
};

// The engine-side mirror of bs_rml_hud_state, using RmlUi string/array types the data model binds
// to. Populated by bs_rml_hud_update from the game's POD snapshot.
struct BsRmlHudModel
{
    bool                     nav_visible = false;
    Rml::String              nav_sector, nav_system, nav_distance, nav_zone;

    bool                     ship_visible = false;
    Rml::String              ship_speed;

    bool                     time_visible = false;
    Rml::String              time_date;
    int                      time_tier = 1;

    bool                     enc_visible = false;
    Rml::String              enc_name, enc_distance, enc_faction, enc_desc;

    bool                     log_visible = false;
    float                    log_alpha = 1.0f;
    Rml::Vector<BsRmlHudLog> log;

    bool                     disc_visible = false;
    Rml::String              disc_count_label;
    Rml::Vector<BsRmlHudDisc> disc;

    bool                        fleet_visible = false;
    Rml::String                 fleet_name, fleet_faction, fleet_speed, fleet_heading, fleet_health;
    Rml::Vector<BsRmlHudWeapon> fleet_weapon;
    Rml::String                 fleet_mode_label;
    bool                        fleet_mode_enabled = false;

    bool                     jump_visible = false;

    bool                     debug_visible = false;
    Rml::String              debug_text;

    bool                     tip_visible = false;
    Rml::String              tip_left = "0px", tip_top = "0px", tip_text;

    bool                        inspector_btn_visible = false;
    bool                        inspector_visible = false;
    Rml::String                 insp_ship_name;
    Rml::Vector<BsRmlHudWeapon> arsenal_inv;
    Rml::Vector<BsRmlHudWeapon> arsenal_def;
    Rml::Vector<BsRmlHudWeapon> arsenal_hp;

    int                         ui_kit = 1; // 0 = Neon, 1 = Clean, 2 = Minimal (drives body class)

    bool                        station_menu_visible = false;
    Rml::String                 station_menu_left = "0px", station_menu_top = "0px";
    bool                        station_inspector_visible = false;
    Rml::String                 station_insp_title;
    bool                        station_insp_show_dock = false;
    bool                        station_insp_show_market = false;
    bool                        station_insp_show_contracts = false;
    bool                        station_insp_tab2_visible = false;
    Rml::String                 station_insp_dock;
    Rml::String                 station_insp_market;
    Rml::String                 station_insp_contracts;

    bool                        planet_insp_visible = false;
    Rml::String                 planet_insp_title;
    Rml::String                 planet_insp_subtitle;
    Rml::String                 planet_insp_stats;
    Rml::String                 planet_insp_comp;
    Rml::String                 planet_insp_moons;
    Rml::String                 planet_insp_events;
    Rml::String                 planet_insp_hab_w = "0%";
    Rml::String                 planet_insp_haz_w = "0%";
    Rml::String                 planet_insp_hab_label;
    Rml::String                 planet_insp_haz_label;
};

static BsRmlHudModel          g_hud_model;
static Rml::DataModelHandle   g_hud_handle;
static Rml::ElementDocument*  g_hud_doc = nullptr;
static Rml::Vector<Rml::String> g_hud_actions; // pending UI actions (drained by the game)

// Copy a bounded, possibly-unterminated C string into an Rml::String.
static void bs_rml_assign(Rml::String& dst, const char* src, size_t cap)
{
    if (!src) { dst.clear(); return; }
    size_t n = 0;
    while (n < cap && src[n] != '\0') ++n;
    dst.assign(src, n);
}

b8 bs_rml_hud_init(const char* rml_path)
{
    if (!g_rml.active || !g_rml.context)
    {
        BS_LOG_ERROR("bs_rml_hud_init: RmlUi is not initialized.");
        return FALSE;
    }
    if (!rml_path) return FALSE;
    if (g_hud_doc) { BS_LOG_WARN("bs_rml_hud_init called twice; ignoring."); return TRUE; }

    Rml::DataModelConstructor c = g_rml.context->CreateDataModel("hud");
    if (!c)
    {
        BS_LOG_ERROR("bs_rml_hud_init: CreateDataModel('hud') failed.");
        return FALSE;
    }

    // Register the repeated row structs and their arrays before binding them.
    if (auto h = c.RegisterStruct<BsRmlHudLog>())
        h.RegisterMember("text", &BsRmlHudLog::text);
    c.RegisterArray<Rml::Vector<BsRmlHudLog>>();

    if (auto h = c.RegisterStruct<BsRmlHudDisc>())
    {
        h.RegisterMember("text",  &BsRmlHudDisc::text);
        h.RegisterMember("color", &BsRmlHudDisc::color);
    }
    c.RegisterArray<Rml::Vector<BsRmlHudDisc>>();

    if (auto h = c.RegisterStruct<BsRmlHudWeapon>())
    {
        h.RegisterMember("text",     &BsRmlHudWeapon::text);
        h.RegisterMember("glyph",    &BsRmlHudWeapon::glyph);
        h.RegisterMember("action",   &BsRmlHudWeapon::action);
        h.RegisterMember("drop",     &BsRmlHudWeapon::drop);
        h.RegisterMember("selected", &BsRmlHudWeapon::selected);
        h.RegisterMember("empty",    &BsRmlHudWeapon::empty);
    }
    c.RegisterArray<Rml::Vector<BsRmlHudWeapon>>();

    // Scalar bindings.
    c.Bind("nav_visible",  &g_hud_model.nav_visible);
    c.Bind("nav_sector",   &g_hud_model.nav_sector);
    c.Bind("nav_system",   &g_hud_model.nav_system);
    c.Bind("nav_distance", &g_hud_model.nav_distance);
    c.Bind("nav_zone",     &g_hud_model.nav_zone);

    c.Bind("ship_visible", &g_hud_model.ship_visible);
    c.Bind("ship_speed",   &g_hud_model.ship_speed);

    c.Bind("time_visible", &g_hud_model.time_visible);
    c.Bind("time_date",    &g_hud_model.time_date);
    c.Bind("time_tier",    &g_hud_model.time_tier);

    c.Bind("enc_visible",  &g_hud_model.enc_visible);
    c.Bind("enc_name",     &g_hud_model.enc_name);
    c.Bind("enc_distance", &g_hud_model.enc_distance);
    c.Bind("enc_faction",  &g_hud_model.enc_faction);
    c.Bind("enc_desc",     &g_hud_model.enc_desc);

    c.Bind("log_visible",  &g_hud_model.log_visible);
    c.Bind("log_alpha",    &g_hud_model.log_alpha);
    c.Bind("log",          &g_hud_model.log);

    c.Bind("disc_visible",     &g_hud_model.disc_visible);
    c.Bind("disc_count_label", &g_hud_model.disc_count_label);
    c.Bind("disc",             &g_hud_model.disc);

    c.Bind("fleet_visible",      &g_hud_model.fleet_visible);
    c.Bind("fleet_name",         &g_hud_model.fleet_name);
    c.Bind("fleet_faction",      &g_hud_model.fleet_faction);
    c.Bind("fleet_speed",        &g_hud_model.fleet_speed);
    c.Bind("fleet_heading",      &g_hud_model.fleet_heading);
    c.Bind("fleet_health",       &g_hud_model.fleet_health);
    c.Bind("fleet_weapon",       &g_hud_model.fleet_weapon);
    c.Bind("fleet_mode_label",   &g_hud_model.fleet_mode_label);
    c.Bind("fleet_mode_enabled", &g_hud_model.fleet_mode_enabled);

    c.Bind("jump_visible", &g_hud_model.jump_visible);

    c.Bind("debug_visible", &g_hud_model.debug_visible);
    c.Bind("debug_text",    &g_hud_model.debug_text);

    c.Bind("tip_visible", &g_hud_model.tip_visible);
    c.Bind("tip_left",    &g_hud_model.tip_left);
    c.Bind("tip_top",     &g_hud_model.tip_top);
    c.Bind("tip_text",    &g_hud_model.tip_text);

    c.Bind("inspector_btn_visible", &g_hud_model.inspector_btn_visible);
    c.Bind("inspector_visible",     &g_hud_model.inspector_visible);
    c.Bind("insp_ship_name",        &g_hud_model.insp_ship_name);
    c.Bind("arsenal_inv",           &g_hud_model.arsenal_inv);
    c.Bind("arsenal_def",           &g_hud_model.arsenal_def);
    c.Bind("arsenal_hp",            &g_hud_model.arsenal_hp);

    c.Bind("ui_kit", &g_hud_model.ui_kit);

    c.Bind("station_menu_visible",      &g_hud_model.station_menu_visible);
    c.Bind("station_menu_left",         &g_hud_model.station_menu_left);
    c.Bind("station_menu_top",          &g_hud_model.station_menu_top);
    c.Bind("station_inspector_visible", &g_hud_model.station_inspector_visible);
    c.Bind("station_insp_title",        &g_hud_model.station_insp_title);
    c.Bind("station_insp_show_dock",      &g_hud_model.station_insp_show_dock);
    c.Bind("station_insp_show_market",    &g_hud_model.station_insp_show_market);
    c.Bind("station_insp_show_contracts", &g_hud_model.station_insp_show_contracts);
    c.Bind("station_insp_tab2_visible",   &g_hud_model.station_insp_tab2_visible);
    c.Bind("station_insp_dock",           &g_hud_model.station_insp_dock);
    c.Bind("station_insp_market",         &g_hud_model.station_insp_market);
    c.Bind("station_insp_contracts",      &g_hud_model.station_insp_contracts);

    c.Bind("planet_insp_visible",   &g_hud_model.planet_insp_visible);
    c.Bind("planet_insp_title",     &g_hud_model.planet_insp_title);
    c.Bind("planet_insp_subtitle",  &g_hud_model.planet_insp_subtitle);
    c.Bind("planet_insp_stats",     &g_hud_model.planet_insp_stats);
    c.Bind("planet_insp_comp",      &g_hud_model.planet_insp_comp);
    c.Bind("planet_insp_moons",     &g_hud_model.planet_insp_moons);
    c.Bind("planet_insp_events",    &g_hud_model.planet_insp_events);
    c.Bind("planet_insp_hab_w",     &g_hud_model.planet_insp_hab_w);
    c.Bind("planet_insp_haz_w",     &g_hud_model.planet_insp_haz_w);
    c.Bind("planet_insp_hab_label", &g_hud_model.planet_insp_hab_label);
    c.Bind("planet_insp_haz_label", &g_hud_model.planet_insp_haz_label);

    // Click actions from the encounter buttons / discoveries close box enqueue a short string that
    // the game drains with bs_rml_hud_poll_action. The engine never mutates game state directly.
    c.BindEventCallback("hud_action",
        [](Rml::DataModelHandle /*handle*/, Rml::Event& /*event*/, const Rml::VariantList& args)
        {
            if (!args.empty())
            {
                Rml::String a = args[0].Get<Rml::String>();
                if (!a.empty()) g_hud_actions.push_back(a);
            }
        });

    g_hud_handle = c.GetModelHandle();

    g_hud_doc = g_rml.context->LoadDocument(rml_path);
    if (!g_hud_doc)
    {
        BS_LOG_ERROR("bs_rml_hud_init: failed to load HUD document '%s'.", rml_path);
        return FALSE;
    }
    g_hud_doc->Show();

    BS_LOG_INFO("bs_rml_hud_init: HUD document '%s' loaded and shown.", rml_path);
    return TRUE;
}

void bs_rml_hud_shutdown(void)
{
    if (g_rml.context)
    {
        if (g_hud_doc) g_rml.context->UnloadDocument(g_hud_doc);
        g_rml.context->RemoveDataModel("hud");
    }
    g_hud_doc = nullptr;
    g_hud_handle = Rml::DataModelHandle();
    g_hud_actions.clear();
    g_hud_model = BsRmlHudModel();
}

void bs_rml_hud_update(const bs_rml_hud_state* s)
{
    if (!g_rml.active || !s || !g_hud_handle) return;

    g_hud_model.nav_visible = s->nav_visible ? true : false;
    bs_rml_assign(g_hud_model.nav_sector,   s->nav_sector,   sizeof(s->nav_sector));
    bs_rml_assign(g_hud_model.nav_system,   s->nav_system,   sizeof(s->nav_system));
    bs_rml_assign(g_hud_model.nav_distance, s->nav_distance, sizeof(s->nav_distance));
    bs_rml_assign(g_hud_model.nav_zone,     s->nav_zone,     sizeof(s->nav_zone));

    g_hud_model.ship_visible = s->ship_visible ? true : false;
    bs_rml_assign(g_hud_model.ship_speed, s->ship_speed, sizeof(s->ship_speed));

    g_hud_model.time_visible = s->time_visible ? true : false;
    bs_rml_assign(g_hud_model.time_date, s->time_date, sizeof(s->time_date));
    g_hud_model.time_tier = s->time_tier;

    g_hud_model.enc_visible = s->enc_visible ? true : false;
    bs_rml_assign(g_hud_model.enc_name,     s->enc_name,     sizeof(s->enc_name));
    bs_rml_assign(g_hud_model.enc_distance, s->enc_distance, sizeof(s->enc_distance));
    bs_rml_assign(g_hud_model.enc_faction,  s->enc_faction,  sizeof(s->enc_faction));
    bs_rml_assign(g_hud_model.enc_desc,     s->enc_desc,     sizeof(s->enc_desc));

    g_hud_model.log_visible = s->log_visible ? true : false;
    g_hud_model.log_alpha   = s->log_alpha;
    {
        i32 n = s->log_count;
        if (n < 0) n = 0;
        if (n > BS_RML_LOG_MAX) n = BS_RML_LOG_MAX;
        g_hud_model.log.resize((size_t)n);
        for (i32 i = 0; i < n; ++i)
            bs_rml_assign(g_hud_model.log[(size_t)i].text, s->log[i].text, sizeof(s->log[i].text));
    }

    g_hud_model.disc_visible = s->disc_visible ? true : false;
    bs_rml_assign(g_hud_model.disc_count_label, s->disc_count_label, sizeof(s->disc_count_label));
    {
        i32 n = s->disc_count;
        if (n < 0) n = 0;
        if (n > BS_RML_DISC_MAX) n = BS_RML_DISC_MAX;
        g_hud_model.disc.resize((size_t)n);
        for (i32 i = 0; i < n; ++i)
        {
            bs_rml_assign(g_hud_model.disc[(size_t)i].text,  s->disc[i].text,  sizeof(s->disc[i].text));
            bs_rml_assign(g_hud_model.disc[(size_t)i].color, s->disc[i].color, sizeof(s->disc[i].color));
        }
    }

    g_hud_model.fleet_visible = s->fleet_visible ? true : false;
    bs_rml_assign(g_hud_model.fleet_name,    s->fleet_name,    sizeof(s->fleet_name));
    bs_rml_assign(g_hud_model.fleet_faction, s->fleet_faction, sizeof(s->fleet_faction));
    bs_rml_assign(g_hud_model.fleet_speed,   s->fleet_speed,   sizeof(s->fleet_speed));
    bs_rml_assign(g_hud_model.fleet_heading, s->fleet_heading, sizeof(s->fleet_heading));
    bs_rml_assign(g_hud_model.fleet_health,  s->fleet_health,  sizeof(s->fleet_health));
    bs_rml_assign(g_hud_model.fleet_mode_label, s->fleet_mode_label, sizeof(s->fleet_mode_label));
    g_hud_model.fleet_mode_enabled = s->fleet_mode_enabled ? true : false;
    {
        i32 n = s->fleet_weapon_count;
        if (n < 0) n = 0;
        if (n > BS_RML_WEAPON_MAX) n = BS_RML_WEAPON_MAX;
        g_hud_model.fleet_weapon.resize((size_t)n);
        for (i32 i = 0; i < n; ++i)
        {
            bs_rml_assign(g_hud_model.fleet_weapon[(size_t)i].text, s->fleet_weapon[i].text,
                          sizeof(s->fleet_weapon[i].text));
            bs_rml_assign(g_hud_model.fleet_weapon[(size_t)i].action, s->fleet_weapon[i].action,
                          sizeof(s->fleet_weapon[i].action));
            bs_rml_assign(g_hud_model.fleet_weapon[(size_t)i].drop, s->fleet_weapon[i].drop,
                          sizeof(s->fleet_weapon[i].drop));
            g_hud_model.fleet_weapon[(size_t)i].selected = s->fleet_weapon[i].selected ? true : false;
            g_hud_model.fleet_weapon[(size_t)i].empty    = s->fleet_weapon[i].empty ? true : false;
        }
    }

    g_hud_model.jump_visible = s->jump_visible ? true : false;

    g_hud_model.debug_visible = s->debug_visible ? true : false;
    bs_rml_assign(g_hud_model.debug_text, s->debug_text, sizeof(s->debug_text));

    g_hud_model.tip_visible = s->tip_visible ? true : false;
    bs_rml_assign(g_hud_model.tip_left, s->tip_left, sizeof(s->tip_left));
    bs_rml_assign(g_hud_model.tip_top,  s->tip_top,  sizeof(s->tip_top));
    bs_rml_assign(g_hud_model.tip_text, s->tip_text, sizeof(s->tip_text));

    g_hud_model.inspector_btn_visible = s->inspector_btn_visible ? true : false;
    g_hud_model.inspector_visible     = s->inspector_visible ? true : false;
    bs_rml_assign(g_hud_model.insp_ship_name, s->insp_ship_name, sizeof(s->insp_ship_name));
    {
        i32 n = s->arsenal_inv_count;
        if (n < 0) n = 0;
        if (n > BS_RML_WEAPON_MAX) n = BS_RML_WEAPON_MAX;
        g_hud_model.arsenal_inv.resize((size_t)n);
        for (i32 i = 0; i < n; ++i)
        {
            bs_rml_assign(g_hud_model.arsenal_inv[(size_t)i].text, s->arsenal_inv[i].text,
                          sizeof(s->arsenal_inv[i].text));
            bs_rml_assign(g_hud_model.arsenal_inv[(size_t)i].glyph, s->arsenal_inv[i].glyph,
                          sizeof(s->arsenal_inv[i].glyph));
            bs_rml_assign(g_hud_model.arsenal_inv[(size_t)i].action, s->arsenal_inv[i].action,
                          sizeof(s->arsenal_inv[i].action));
            bs_rml_assign(g_hud_model.arsenal_inv[(size_t)i].drop, s->arsenal_inv[i].drop,
                          sizeof(s->arsenal_inv[i].drop));
            g_hud_model.arsenal_inv[(size_t)i].selected = s->arsenal_inv[i].selected ? true : false;
            g_hud_model.arsenal_inv[(size_t)i].empty    = s->arsenal_inv[i].empty ? true : false;
        }
    }
    {
        i32 n = s->arsenal_def_count;
        if (n < 0) n = 0;
        if (n > BS_RML_WEAPON_MAX) n = BS_RML_WEAPON_MAX;
        g_hud_model.arsenal_def.resize((size_t)n);
        for (i32 i = 0; i < n; ++i)
        {
            bs_rml_assign(g_hud_model.arsenal_def[(size_t)i].text, s->arsenal_def[i].text,
                          sizeof(s->arsenal_def[i].text));
            bs_rml_assign(g_hud_model.arsenal_def[(size_t)i].glyph, s->arsenal_def[i].glyph,
                          sizeof(s->arsenal_def[i].glyph));
            bs_rml_assign(g_hud_model.arsenal_def[(size_t)i].action, s->arsenal_def[i].action,
                          sizeof(s->arsenal_def[i].action));
            bs_rml_assign(g_hud_model.arsenal_def[(size_t)i].drop, s->arsenal_def[i].drop,
                          sizeof(s->arsenal_def[i].drop));
            g_hud_model.arsenal_def[(size_t)i].selected = s->arsenal_def[i].selected ? true : false;
            g_hud_model.arsenal_def[(size_t)i].empty    = s->arsenal_def[i].empty ? true : false;
        }
    }
    {
        i32 n = s->arsenal_hp_count;
        if (n < 0) n = 0;
        if (n > BS_RML_WEAPON_MAX) n = BS_RML_WEAPON_MAX;
        g_hud_model.arsenal_hp.resize((size_t)n);
        for (i32 i = 0; i < n; ++i)
        {
            bs_rml_assign(g_hud_model.arsenal_hp[(size_t)i].text, s->arsenal_hp[i].text,
                          sizeof(s->arsenal_hp[i].text));
            bs_rml_assign(g_hud_model.arsenal_hp[(size_t)i].glyph, s->arsenal_hp[i].glyph,
                          sizeof(s->arsenal_hp[i].glyph));
            bs_rml_assign(g_hud_model.arsenal_hp[(size_t)i].action, s->arsenal_hp[i].action,
                          sizeof(s->arsenal_hp[i].action));
            bs_rml_assign(g_hud_model.arsenal_hp[(size_t)i].drop, s->arsenal_hp[i].drop,
                          sizeof(s->arsenal_hp[i].drop));
            g_hud_model.arsenal_hp[(size_t)i].selected = s->arsenal_hp[i].selected ? true : false;
            g_hud_model.arsenal_hp[(size_t)i].empty    = s->arsenal_hp[i].empty ? true : false;
        }
    }

    g_hud_model.ui_kit = s->ui_kit;

    g_hud_model.station_menu_visible = s->station_menu_visible ? true : false;
    bs_rml_assign(g_hud_model.station_menu_left, s->station_menu_left, sizeof(s->station_menu_left));
    bs_rml_assign(g_hud_model.station_menu_top,  s->station_menu_top,  sizeof(s->station_menu_top));
    g_hud_model.station_inspector_visible = s->station_inspector_visible ? true : false;
    bs_rml_assign(g_hud_model.station_insp_title, s->station_insp_title, sizeof(s->station_insp_title));
    g_hud_model.station_insp_show_dock      = s->station_insp_show_dock ? true : false;
    g_hud_model.station_insp_show_market    = s->station_insp_show_market ? true : false;
    g_hud_model.station_insp_show_contracts = s->station_insp_show_contracts ? true : false;
    g_hud_model.station_insp_tab2_visible   = s->station_insp_tab2_visible ? true : false;
    bs_rml_assign(g_hud_model.station_insp_dock,      s->station_insp_dock,      sizeof(s->station_insp_dock));
    bs_rml_assign(g_hud_model.station_insp_market,    s->station_insp_market,    sizeof(s->station_insp_market));
    bs_rml_assign(g_hud_model.station_insp_contracts, s->station_insp_contracts, sizeof(s->station_insp_contracts));

    g_hud_model.planet_insp_visible = s->planet_insp_visible ? true : false;
    bs_rml_assign(g_hud_model.planet_insp_title,     s->planet_insp_title,     sizeof(s->planet_insp_title));
    bs_rml_assign(g_hud_model.planet_insp_subtitle,  s->planet_insp_subtitle,  sizeof(s->planet_insp_subtitle));
    bs_rml_assign(g_hud_model.planet_insp_stats,     s->planet_insp_stats,     sizeof(s->planet_insp_stats));
    bs_rml_assign(g_hud_model.planet_insp_comp,      s->planet_insp_comp,      sizeof(s->planet_insp_comp));
    bs_rml_assign(g_hud_model.planet_insp_moons,     s->planet_insp_moons,     sizeof(s->planet_insp_moons));
    bs_rml_assign(g_hud_model.planet_insp_events,    s->planet_insp_events,    sizeof(s->planet_insp_events));
    bs_rml_assign(g_hud_model.planet_insp_hab_w,     s->planet_insp_hab_w,     sizeof(s->planet_insp_hab_w));
    bs_rml_assign(g_hud_model.planet_insp_haz_w,     s->planet_insp_haz_w,     sizeof(s->planet_insp_haz_w));
    bs_rml_assign(g_hud_model.planet_insp_hab_label, s->planet_insp_hab_label, sizeof(s->planet_insp_hab_label));
    bs_rml_assign(g_hud_model.planet_insp_haz_label, s->planet_insp_haz_label, sizeof(s->planet_insp_haz_label));

    // Poll-style HUD: everything may change each frame, so just dirty the whole model.
    g_hud_handle.DirtyAllVariables();
}

i32 bs_rml_hud_poll_action(char* out_action, i32 cap)
{
    if (!out_action || cap <= 0) return 0;
    out_action[0] = '\0';
    if (g_hud_actions.empty()) return 0;

    Rml::String a = g_hud_actions.front();
    g_hud_actions.erase(g_hud_actions.begin());

    i32 len = (i32)a.size();
    if (len > cap - 1) len = cap - 1;
    for (i32 i = 0; i < len; ++i) out_action[i] = a[(size_t)i];
    out_action[len] = '\0';
    return len;
}

// Internal: render the RmlUi context into the currently open swapchain render pass. Called from
// sdlgpu_backend_end_frame BEFORE the ImGui editor overlay, so game UI sits beneath it. RmlUi may
// leave a tight scissor set from its last draw, so reset it to the full framebuffer afterwards to
// avoid clipping the ImGui overlay that records into the same pass.
static void bs_rml_render_internal(void)
{
    if (!g_rml.active || !g_rml.context || !g_sdl.pass) return;

    g_rml.context->Render();

    SDL_Rect full;
    full.x = 0; full.y = 0;
    full.w = (int)g_sdl.swap_width;
    full.h = (int)g_sdl.swap_height;
    SDL_SetGPUScissor(g_sdl.pass, &full);
}
