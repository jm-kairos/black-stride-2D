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

#include "core/logger.h"
#include "platform/platform.h"
#include "math/math_utils.h"
#include "renderer/camera2d.h"

#include <SDL3/SDL_gpu.h>
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

#include <stdlib.h> // qsort

using namespace bs_math; // Mat4, mat4_ortho, etc.

// One interleaved sprite vertex: 2D position + UV + RGBA tint. Matches the pipeline vertex
// layout and the HLSL VSInput (TEXCOORD0=position, TEXCOORD1=uv, TEXCOORD2=color).
typedef struct sprite_vertex
{
    f32 x, y;       // world-space position (pre-transformed CPU-side)
    f32 u, v;       // atlas UV
    f32 r, g, b, a; // tint
} sprite_vertex;

// Capacity limits for the per-frame dynamic batch. 16k sprites = 64k verts / 96k indices —
// plenty for a Starsector-like sandbox frame; grows are a future optimization.
#define BS_MAX_SPRITES        16384
#define BS_MAX_BATCH_VERTS    (BS_MAX_SPRITES * 4)
#define BS_MAX_BATCH_INDICES  (BS_MAX_SPRITES * 6)
#define BS_MAX_TEXTURES       1024

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

    // Sprite pipelines: one per EBlendMode, all sharing the sprite vert/frag shaders.
    SDL_GPUGraphicsPipeline* pipelines[BLEND_MODE_COUNT];
    SDL_GPUSampler*          sampler;

    // GPU-resident batch buffers. The index buffer is filled once (quad pattern is fixed);
    // the vertex buffer is re-uploaded each frame from the sorted sprite list.
    SDL_GPUBuffer*         vbuffer;
    SDL_GPUBuffer*         ibuffer;
    SDL_GPUTransferBuffer* vtransfer; // persistent UPLOAD transfer buffer for the vertex stream

    // Texture pool.
    gpu_texture textures[BS_MAX_TEXTURES];
    bs_texture  white_texture; // 1x1 opaque white; lets solid sprites reuse the sprite pipeline

    // Camera (rebuilt into view-proj each frame from the live swapchain size).
    Camera2D camera;

    // CPU-side per-frame batch.
    batched_sprite batch[BS_MAX_SPRITES];
    u32            batch_count;

    // Phase 4: stats snapshotted at the end of end_frame for the previous completed frame.
    bs_frame_stats last_stats;

    // Dear ImGui: TRUE between bs_imgui_initialize and bs_imgui_shutdown. Gates every ImGui
    // call in the frame lifecycle so the engine runs cleanly if ImGui ever fails to init.
    b8 imgui_active;
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

static b8 create_pipelines(SDL_GPUShader* vs, SDL_GPUShader* fs)
{
    SDL_GPUTextureFormat swap_fmt =
        SDL_GetGPUSwapchainTextureFormat(g_sdl.device, g_sdl.window);

    // Vertex buffer: one interleaved stream, per-vertex.
    SDL_GPUVertexBufferDescription vbuf_desc;
    SDL_zero(vbuf_desc);
    vbuf_desc.slot               = 0;
    vbuf_desc.pitch              = sizeof(sprite_vertex);
    vbuf_desc.input_rate         = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    vbuf_desc.instance_step_rate = 0;

    // Attributes: position (loc 0, float2), uv (loc 1, float2), color (loc 2, float4).
    SDL_GPUVertexAttribute attrs[3];
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

    for (u32 m = 0; m < BLEND_MODE_COUNT; ++m)
    {
        SDL_GPUColorTargetBlendState blend;
        fill_blend_state((EBlendMode)m, &blend);

        SDL_GPUColorTargetDescription color_target;
        SDL_zero(color_target);
        color_target.format      = swap_fmt;
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

        g_sdl.pipelines[m] = SDL_CreateGPUGraphicsPipeline(g_sdl.device, &info);
        if (!g_sdl.pipelines[m])
        {
            BS_LOG_FATAL("create_pipelines: pipeline %u failed: %s", m, SDL_GetError());
            return FALSE;
        }
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

    // Load sprite shaders: vertex has 1 uniform buffer (view_proj), fragment has 1 sampler.
    SDL_GPUShader* vs = load_shader(g_sdl.device, "sprite", "vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    SDL_GPUShader* fs = load_shader(g_sdl.device, "sprite", "frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    if (!vs || !fs) return FALSE;

    if (!create_pipelines(vs, fs)) return FALSE;

    // Shaders are baked into the pipelines; release the standalone handles.
    SDL_ReleaseGPUShader(g_sdl.device, vs);
    SDL_ReleaseGPUShader(g_sdl.device, fs);

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

    if (!create_batch_resources()) return FALSE;
    if (!create_white_texture())   return FALSE;

    BS_LOG_INFO("SDL GPU backend initialized (Phase 3: sprites).");
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

    if (g_sdl.sampler)   SDL_ReleaseGPUSampler(g_sdl.device, g_sdl.sampler);
    if (g_sdl.vtransfer) SDL_ReleaseGPUTransferBuffer(g_sdl.device, g_sdl.vtransfer);
    if (g_sdl.vbuffer)   SDL_ReleaseGPUBuffer(g_sdl.device, g_sdl.vbuffer);
    if (g_sdl.ibuffer)   SDL_ReleaseGPUBuffer(g_sdl.device, g_sdl.ibuffer);

    for (u32 m = 0; m < BLEND_MODE_COUNT; ++m)
        if (g_sdl.pipelines[m]) SDL_ReleaseGPUGraphicsPipeline(g_sdl.device, g_sdl.pipelines[m]);

    SDL_ReleaseWindowFromGPUDevice(g_sdl.device, g_sdl.window);
    SDL_DestroyGPUDevice(g_sdl.device);
    SDL_zero(g_sdl);

    BS_LOG_INFO("SDL GPU backend shut down.");
}

void sdlgpu_backend_on_resize(struct renderer_backend* backend, u16 width, u16 height)
{
    (void)backend;
    // The swapchain auto-resizes; view-proj is rebuilt each frame from the live size. Nothing to
    // do here beyond logging. Kept as a hook for future render-target recreation.
    BS_LOG_DEBUG("sdlgpu_backend_on_resize: %ux%u.", (u32)width, (u32)height);
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

    u32 byte_count = width * height * 4u;

    SDL_GPUTransferBufferCreateInfo tinfo;
    SDL_zero(tinfo);
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size  = byte_count;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(g_sdl.device, &tinfo);
    if (!tb) { BS_LOG_ERROR("create_texture: transfer buffer failed: %s", SDL_GetError()); SDL_ReleaseGPUTexture(g_sdl.device, tex); return invalid; }

    void* map = SDL_MapGPUTransferBuffer(g_sdl.device, tb, false);
    SDL_memcpy(map, pixels, byte_count);
    SDL_UnmapGPUTransferBuffer(g_sdl.device, tb);

    SDL_GPUCommandBuffer* up = SDL_AcquireGPUCommandBuffer(g_sdl.device);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(up);
    SDL_GPUTextureTransferInfo src; SDL_zero(src);
    src.transfer_buffer = tb; src.offset = 0; src.pixels_per_row = width; src.rows_per_layer = height;
    SDL_GPUTextureRegion dst; SDL_zero(dst);
    dst.texture = tex; dst.w = width; dst.h = height; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(up);
    SDL_ReleaseGPUTransferBuffer(g_sdl.device, tb);

    bs_texture handle = pool_alloc_texture(tex, width, height);
    if (handle.id == BS_INVALID_HANDLE) { SDL_ReleaseGPUTexture(g_sdl.device, tex); return invalid; }

    BS_LOG_DEBUG("create_texture: %ux%u -> id 0x%x.", width, height, handle.id);
    return handle;
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

    batched_sprite* b = &g_sdl.batch[g_sdl.batch_count++];
    b->sprite   = *sprite;
    b->sort_key = make_sort_key(sprite->layer, sprite->blend, tex_index);
}

void sdlgpu_backend_get_frame_stats(struct renderer_backend* backend, bs_frame_stats* out_stats)
{
    (void)backend;
    if (!out_stats) return;
    *out_stats = g_sdl.last_stats;
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

    if (!ImGui_ImplSDL3_InitForSDLGPU(g_sdl.window))
    {
        BS_LOG_ERROR("bs_imgui_initialize: ImGui_ImplSDL3_InitForSDLGPU failed.");
        ImGui::DestroyContext();
        return FALSE;
    }

    ImGui_ImplSDLGPU3_InitInfo init_info;
    SDL_zero(init_info);
    init_info.Device            = g_sdl.device;
    init_info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(g_sdl.device, g_sdl.window);
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

    if (!g_sdl.cmd) return FALSE;

    // Sort the batch for run-friendly draw order.
    if (g_sdl.batch_count > 1)
        SDL_qsort(g_sdl.batch, g_sdl.batch_count, sizeof(batched_sprite), batch_compare);

    // ---- 2 & 3: build vertices and upload (only if we have sprites) ----
    if (g_sdl.batch_count > 0)
    {
        sprite_vertex* verts = (sprite_vertex*)SDL_MapGPUTransferBuffer(g_sdl.device, g_sdl.vtransfer, true);

        for (u32 i = 0; i < g_sdl.batch_count; ++i)
        {
            const bs_sprite* s = &g_sdl.batch[i].sprite;

            // Local-space corners relative to the pivot (origin is normalized within the quad).
            f32 ox = s->origin.x * s->size.x;
            f32 oy = s->origin.y * s->size.y;

            Vec2 corners[4];
            // World is y-up (origin at screen center). "Top" corners therefore sit at
            // POSITIVE local y. origin.y is measured from the texture top (y-down UV space),
            // so distance pivot->top = oy (up, +) and pivot->bottom = size.y-oy (down, -).
            // Pairing top corners with v0 (texture top) keeps the image upright on screen.
            corners[0] = Vec2{ -ox,            oy              }; // top-left
            corners[1] = Vec2{ s->size.x - ox, oy              }; // top-right
            corners[2] = Vec2{ s->size.x - ox, oy - s->size.y  }; // bottom-right
            corners[3] = Vec2{ -ox,            oy - s->size.y  }; // bottom-left

            // UVs from the atlas sub-rect (top-left origin, y-down in texture space).
            f32 u0 = s->uv.x,            v0 = s->uv.y;
            f32 u1 = s->uv.x + s->uv.w,  v1 = s->uv.y + s->uv.h;
            f32 uvs[4][2] = { { u0, v0 }, { u1, v0 }, { u1, v1 }, { u0, v1 } };

            for (u32 c = 0; c < 4; ++c)
            {
                Vec2 r = vec2_rotate(corners[c], s->rotation);
                sprite_vertex* vtx = &verts[i * 4u + c];
                vtx->x = s->position.x + r.x;
                vtx->y = s->position.y + r.y;
                vtx->u = uvs[c][0];
                vtx->v = uvs[c][1];
                vtx->r = s->tint.r;
                vtx->g = s->tint.g;
                vtx->b = s->tint.b;
                vtx->a = s->tint.a;
            }
        }

        SDL_UnmapGPUTransferBuffer(g_sdl.device, g_sdl.vtransfer);

        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(g_sdl.cmd);
        SDL_GPUTransferBufferLocation src; SDL_zero(src);
        src.transfer_buffer = g_sdl.vtransfer; src.offset = 0;
        SDL_GPUBufferRegion dst; SDL_zero(dst);
        dst.buffer = g_sdl.vbuffer; dst.offset = 0;
        dst.size   = sizeof(sprite_vertex) * 4u * g_sdl.batch_count;
        SDL_UploadToGPUBuffer(cp, &src, &dst, true);
        SDL_EndGPUCopyPass(cp);
    }

    // ---- 4: acquire the swapchain image (may be NULL when minimized -> skip drawing) ----
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
            g_sdl.cmd, g_sdl.window, &g_sdl.swapchain_texture, &g_sdl.swap_width, &g_sdl.swap_height))
    {
        BS_LOG_ERROR("end_frame: SDL_WaitAndAcquireGPUSwapchainTexture failed: %s", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(g_sdl.cmd);
        g_sdl.cmd = NULL;
        return FALSE;
    }

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
        ImGui_ImplSDLGPU3_PrepareDrawData(imgui_draw_data, g_sdl.cmd);
    }

    SDL_GPUColorTargetInfo color_target;
    SDL_zero(color_target);
    color_target.texture     = g_sdl.swapchain_texture;
    color_target.clear_color = g_sdl.clear_color;
    color_target.load_op     = SDL_GPU_LOADOP_CLEAR;
    color_target.store_op    = SDL_GPU_STOREOP_STORE;

    g_sdl.pass = SDL_BeginGPURenderPass(g_sdl.cmd, &color_target, 1, NULL);

    u32 draw_calls = 0;
    if (g_sdl.batch_count > 0)
    {
        // Rebuild the view-proj from the LIVE swapchain size so it stays resize-correct.
        Mat4 view_proj = camera2d_view_proj(&g_sdl.camera, (u16)g_sdl.swap_width, (u16)g_sdl.swap_height);

        // Bind the shared index buffer once; the vertex buffer too.
        SDL_GPUBufferBinding vbind; SDL_zero(vbind);
        vbind.buffer = g_sdl.vbuffer; vbind.offset = 0;
        SDL_BindGPUVertexBuffers(g_sdl.pass, 0, &vbind, 1);

        SDL_GPUBufferBinding ibind; SDL_zero(ibind);
        ibind.buffer = g_sdl.ibuffer; ibind.offset = 0;
        SDL_BindGPUIndexBuffer(g_sdl.pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

        // Walk contiguous runs sharing the same (blend, texture).
        u32 run_start = 0;
        while (run_start < g_sdl.batch_count)
        {
            EBlendMode run_blend = g_sdl.batch[run_start].sprite.blend;
            u32        run_texid = (g_sdl.batch[run_start].sort_key & 0x3FFFFu);

            u32 run_end = run_start + 1;
            while (run_end < g_sdl.batch_count)
            {
                EBlendMode b = g_sdl.batch[run_end].sprite.blend;
                u32        t = (g_sdl.batch[run_end].sort_key & 0x3FFFFu);
                if (b != run_blend || t != run_texid) break;
                ++run_end;
            }

            u32 run_count = run_end - run_start;

            // Resolve the texture for this run (fallback to white).
            gpu_texture* slot = &g_sdl.textures[run_texid - 1u];
            SDL_GPUTexture* tex = (slot->in_use && slot->tex) ? slot->tex
                : g_sdl.textures[(g_sdl.white_texture.id & 0x3FFFFu) - 1u].tex;

            SDL_BindGPUGraphicsPipeline(g_sdl.pass, g_sdl.pipelines[(u32)run_blend]);
            SDL_PushGPUVertexUniformData(g_sdl.cmd, 0, &view_proj, sizeof(Mat4));

            SDL_GPUTextureSamplerBinding tsb; SDL_zero(tsb);
            tsb.texture = tex; tsb.sampler = g_sdl.sampler;
            SDL_BindGPUFragmentSamplers(g_sdl.pass, 0, &tsb, 1);

            // 6 indices per sprite; first_index points into the shared index buffer.
            SDL_DrawGPUIndexedPrimitives(
                g_sdl.pass,
                run_count * 6u,  // num_indices
                1,               // num_instances
                run_start * 6u,  // first_index
                0,               // vertex_offset
                0);              // first_instance

            ++draw_calls;
            run_start = run_end;
        }
    }

    // Snapshot stats for the frame we are about to submit (read via renderer_get_frame_stats).
    g_sdl.last_stats.sprite_count = g_sdl.batch_count;
    g_sdl.last_stats.draw_calls   = draw_calls;

    // Record ImGui's draw commands LAST, inside the same render pass, so the UI composites on
    // top of the game scene. PrepareDrawData (above) already uploaded the geometry. Guarded on
    // a non-empty draw list so an idle-UI frame skips the bind entirely.
    if (g_sdl.imgui_active && imgui_draw_data && imgui_draw_data->TotalVtxCount > 0)
    {
        ImGui_ImplSDLGPU3_RenderDrawData(imgui_draw_data, g_sdl.cmd, g_sdl.pass);
    }

    SDL_EndGPURenderPass(g_sdl.pass);
    g_sdl.pass = NULL;

    SDL_SubmitGPUCommandBuffer(g_sdl.cmd);
    g_sdl.cmd               = NULL;
    g_sdl.swapchain_texture = NULL;
    return TRUE;
}
