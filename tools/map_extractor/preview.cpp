#include "preview.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#ifdef _WIN32
#include <windows.h>
#endif

static void get_exe_dir(char* out, size_t out_size)
{
    out[0] = '\0';
#ifdef _WIN32
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH)
    {
        char* last = strrchr(buf, '\\');
        if (last) *last = '\0';
        strncpy(out, buf, out_size - 1);
    }
#endif
}

// Mirror the engine's mapped-sprite vertex layout.
typedef struct preview_vertex
{
    float x, y;       // world-space position
    float u, v;       // atlas UV
    float wx, wy, wz; // world-space position for lighting
    float angle;      // sprite rotation in radians
} preview_vertex_t;

// Mirror the engine's mapped-sprite fragment UBO (mapped_light in
// renderer_backend_sdlgpu.cpp / LightUBO in mapped_sprite.frag.hlsl). The preview keeps
// the point-light arrays zeroed (tuning[2] = count = 0): star + ambient only.
#define PREVIEW_MAX_LIGHTS 16
typedef struct preview_light
{
    float light_dir[4];  // xyz = direction, w = intensity
    float ambient[4];
    float tuning[4];     // x = normal strength, y = parallax scale, z = point light count
    float pos_radius[PREVIEW_MAX_LIGHTS][4];
    float color[PREVIEW_MAX_LIGHTS][4];
} preview_light_t;

static SDL_GPUShader* load_shader(SDL_GPUDevice* device, const char* name, const char* stage_ext,
                                   SDL_GPUShaderStage stage, Uint32 num_samplers, Uint32 num_ubos)
{
    SDL_GPUShaderFormat fmts = SDL_GetGPUShaderFormats(device);
    const char* subdir = NULL;
    const char* ext = NULL;
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
        SDL_Log("preview: device exposes no shader format we compile for (0x%x).", (unsigned)fmts);
        return NULL;
    }

    char exe_dir[512];
    get_exe_dir(exe_dir, sizeof(exe_dir));

    const char* candidates[4];
    int num_candidates = 0;
    char path0[512], path1[512], path2[512];
    SDL_snprintf(path0, sizeof(path0), "assets/shaders/%s/%s.%s.%s", subdir, name, stage_ext, ext);
    candidates[num_candidates++] = path0;
    if (exe_dir[0])
    {
        SDL_snprintf(path1, sizeof(path1), "%s\\assets\\shaders\\%s\\%s.%s.%s", exe_dir, subdir, name, stage_ext, ext);
        candidates[num_candidates++] = path1;
        SDL_snprintf(path2, sizeof(path2), "%s\\..\\assets\\shaders\\%s\\%s.%s.%s", exe_dir, subdir, name, stage_ext, ext);
        candidates[num_candidates++] = path2;
    }

    size_t code_size = 0;
    void* code = NULL;
    const char* loaded_path = NULL;
    for (int i = 0; i < num_candidates; ++i)
    {
        code = SDL_LoadFile(candidates[i], &code_size);
        if (code)
        {
            loaded_path = candidates[i];
            break;
        }
    }
    if (!code)
    {
        SDL_Log("preview: failed to read shader '%s.%s.%s' from any candidate path.", name, stage_ext, ext);
        return NULL;
    }

    SDL_GPUShaderCreateInfo info = {};
    info.code = (const Uint8*)code;
    info.code_size = code_size;
    info.entrypoint = "main";
    info.format = chosen;
    info.stage = stage;
    info.num_samplers = num_samplers;
    info.num_uniform_buffers = num_ubos;

    SDL_GPUShader* shader = SDL_CreateGPUShader(device, &info);
    SDL_free(code);
    if (!shader)
    {
        SDL_Log("preview: SDL_CreateGPUShader('%s') failed: %s", loaded_path, SDL_GetError());
    }
    return shader;
}

static SDL_GPUGraphicsPipeline* create_mapped_pipeline(SDL_GPUDevice* device, SDL_GPUShader* vs, SDL_GPUShader* fs,
                                                        SDL_GPUTextureFormat color_fmt)
{
    SDL_GPUVertexBufferDescription vbuf_desc = {};
    vbuf_desc.slot = 0;
    vbuf_desc.pitch = sizeof(preview_vertex_t);
    vbuf_desc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;

    SDL_GPUVertexAttribute attrs[4] = {};
    attrs[0].location = 0; attrs[0].buffer_slot = 0;
    attrs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[0].offset = offsetof(preview_vertex_t, x);
    attrs[1].location = 1; attrs[1].buffer_slot = 0;
    attrs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;
    attrs[1].offset = offsetof(preview_vertex_t, u);
    attrs[2].location = 2; attrs[2].buffer_slot = 0;
    attrs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;
    attrs[2].offset = offsetof(preview_vertex_t, wx);
    attrs[3].location = 3; attrs[3].buffer_slot = 0;
    attrs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;
    attrs[3].offset = offsetof(preview_vertex_t, angle);

    SDL_GPUColorTargetBlendState blend = {};
    blend.color_write_mask = SDL_GPU_COLORCOMPONENT_R | SDL_GPU_COLORCOMPONENT_G |
                             SDL_GPU_COLORCOMPONENT_B | SDL_GPU_COLORCOMPONENT_A;
    blend.enable_blend = true;
    blend.color_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
    blend.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    blend.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    blend.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;

    SDL_GPUColorTargetDescription color_target = {};
    color_target.format = color_fmt;
    color_target.blend_state = blend;

    SDL_GPUGraphicsPipelineCreateInfo info = {};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.vertex_input_state.vertex_buffer_descriptions = &vbuf_desc;
    info.vertex_input_state.num_vertex_buffers = 1;
    info.vertex_input_state.vertex_attributes = attrs;
    info.vertex_input_state.num_vertex_attributes = 4;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.target_info.color_target_descriptions = &color_target;
    info.target_info.num_color_targets = 1;
    info.target_info.has_depth_stencil_target = false;

    SDL_GPUGraphicsPipeline* pipe = SDL_CreateGPUGraphicsPipeline(device, &info);
    if (!pipe)
    {
        SDL_Log("preview: create pipeline failed: %s", SDL_GetError());
    }
    return pipe;
}

int preview_init(preview_context_t* ctx, SDL_GPUDevice* device)
{
    if (!ctx || !device) return 0;
    memset(ctx, 0, sizeof(*ctx));
    ctx->device = device;

    ctx->vs = load_shader(device, "mapped_sprite", "vert", SDL_GPU_SHADERSTAGE_VERTEX, 0, 1);
    ctx->fs = load_shader(device, "mapped_sprite", "frag", SDL_GPU_SHADERSTAGE_FRAGMENT, 4, 1);
    if (!ctx->vs || !ctx->fs)
    {
        preview_shutdown(ctx);
        return 0;
    }

    // The preview always renders to an R8G8B8A8_UNORM offscreen target, so the
    // pipeline must match that format regardless of the swapchain format.
    SDL_GPUTextureFormat color_fmt = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;

    ctx->pipeline = create_mapped_pipeline(device, ctx->vs, ctx->fs, color_fmt);
    if (!ctx->pipeline)
    {
        preview_shutdown(ctx);
        return 0;
    }

    SDL_GPUSamplerCreateInfo sinfo = {};
    sinfo.min_filter = SDL_GPU_FILTER_LINEAR;
    sinfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    sinfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sinfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    sinfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ctx->sampler = SDL_CreateGPUSampler(device, &sinfo);
    if (!ctx->sampler)
    {
        preview_shutdown(ctx);
        return 0;
    }

    SDL_GPUBufferCreateInfo vinfo = {};
    vinfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    vinfo.size = sizeof(preview_vertex_t) * 4;
    ctx->vbuffer = SDL_CreateGPUBuffer(device, &vinfo);

    SDL_GPUBufferCreateInfo iinfo = {};
    iinfo.usage = SDL_GPU_BUFFERUSAGE_INDEX;
    iinfo.size = sizeof(Uint16) * 6;
    ctx->ibuffer = SDL_CreateGPUBuffer(device, &iinfo);

    if (!ctx->vbuffer || !ctx->ibuffer)
    {
        preview_shutdown(ctx);
        return 0;
    }

    // Upload the fixed index buffer once.
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferCreateInfo tinfo = {};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = sizeof(Uint16) * 6;
    SDL_GPUTransferBuffer* tbuf = SDL_CreateGPUTransferBuffer(device, &tinfo);
    Uint16* idx = (Uint16*)SDL_MapGPUTransferBuffer(device, tbuf, false);
    idx[0] = 0; idx[1] = 1; idx[2] = 2;
    idx[3] = 2; idx[4] = 3; idx[5] = 0;
    SDL_UnmapGPUTransferBuffer(device, tbuf);
    SDL_GPUBufferRegion dst = {};
    dst.buffer = ctx->ibuffer; dst.offset = 0; dst.size = sizeof(Uint16) * 6;
    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = tbuf; src.offset = 0;
    SDL_UploadToGPUBuffer(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device, tbuf);

    ctx->initialized = 1;
    return 1;
}

void preview_shutdown(preview_context_t* ctx)
{
    if (!ctx) return;
    if (ctx->pipeline) SDL_ReleaseGPUGraphicsPipeline(ctx->device, ctx->pipeline);
    if (ctx->vs) SDL_ReleaseGPUShader(ctx->device, ctx->vs);
    if (ctx->fs) SDL_ReleaseGPUShader(ctx->device, ctx->fs);
    if (ctx->sampler) SDL_ReleaseGPUSampler(ctx->device, ctx->sampler);
    if (ctx->vbuffer) SDL_ReleaseGPUBuffer(ctx->device, ctx->vbuffer);
    if (ctx->ibuffer) SDL_ReleaseGPUBuffer(ctx->device, ctx->ibuffer);
    memset(ctx, 0, sizeof(*ctx));
}

// Build a simple orthographic view-proj matrix (column-major, z in [0,1]).
static void make_ortho(float* m, float left, float right, float bottom, float top, float znear, float zfar)
{
    float rl = right - left;
    float tb = top - bottom;
    float fn = zfar - znear;
    memset(m, 0, sizeof(float) * 16);
    m[0]  = 2.0f / rl;
    m[5]  = 2.0f / tb;
    m[10] = 1.0f / fn;
    m[12] = -(right + left) / rl;
    m[13] = -(top + bottom) / tb;
    m[14] = -znear / fn;
    m[15] = 1.0f;
}

static void build_quad(preview_vertex_t* verts, float w, float h)
{
    float hw = w * 0.5f;
    float hh = h * 0.5f;
    verts[0].x = -hw; verts[0].y = -hh; verts[0].u = 0.0f; verts[0].v = 1.0f;
    verts[1].x =  hw; verts[1].y = -hh; verts[1].u = 1.0f; verts[1].v = 1.0f;
    verts[2].x =  hw; verts[2].y =  hh; verts[2].u = 1.0f; verts[2].v = 0.0f;
    verts[3].x = -hw; verts[3].y =  hh; verts[3].u = 0.0f; verts[3].v = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        verts[i].wx = verts[i].x;
        verts[i].wy = verts[i].y;
        verts[i].wz = 0.0f;
        verts[i].angle = 0.0f;
    }
}

static void upload_vertices(preview_context_t* ctx, SDL_GPUCommandBuffer* cmd, const preview_vertex_t* verts)
{
    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferCreateInfo tinfo = {};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tinfo.size = sizeof(preview_vertex_t) * 4;
    SDL_GPUTransferBuffer* tbuf = SDL_CreateGPUTransferBuffer(ctx->device, &tinfo);
    void* dst = SDL_MapGPUTransferBuffer(ctx->device, tbuf, false);
    memcpy(dst, verts, sizeof(preview_vertex_t) * 4);
    SDL_UnmapGPUTransferBuffer(ctx->device, tbuf);
    SDL_GPUBufferRegion dst_r = {};
    dst_r.buffer = ctx->vbuffer; dst_r.offset = 0; dst_r.size = sizeof(preview_vertex_t) * 4;
    SDL_GPUTransferBufferLocation src = {};
    src.transfer_buffer = tbuf; src.offset = 0;
    SDL_UploadToGPUBuffer(cp, &src, &dst_r, false);
    SDL_EndGPUCopyPass(cp);
    SDL_ReleaseGPUTransferBuffer(ctx->device, tbuf);
}

static SDL_GPUTexture* create_target_texture(SDL_GPUDevice* device, int w, int h, Uint32 extra_usage)
{
    SDL_GPUTextureCreateInfo info = {};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | extra_usage;
    info.width = (Uint32)w;
    info.height = (Uint32)h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device, &info);
}

void preview_render(preview_context_t* ctx, SDL_GPUTexture* target,
                    const extracted_maps_t* maps,
                    float star_angle, int out_w, int out_h,
                    SDL_GPUCommandBuffer* cmd)
{
    float aspect = (maps->diffuse.w > 0 && maps->diffuse.h > 0)
        ? (float)maps->diffuse.w / (float)maps->diffuse.h : 1.0f;
    float world_h = (float)out_h;
    float world_w = world_h * aspect;

    preview_vertex_t verts[4];
    build_quad(verts, world_w, world_h);
    upload_vertices(ctx, cmd, verts);

    float view_proj[16];
    make_ortho(view_proj, -world_w * 0.5f, world_w * 0.5f, -world_h * 0.5f, world_h * 0.5f, 0.0f, 1.0f);

    preview_light_t lit = {};
    lit.light_dir[0] = cosf(star_angle);
    lit.light_dir[1] = sinf(star_angle);
    lit.light_dir[2] = 0.2f;
    lit.light_dir[3] = 1.0f;
    lit.ambient[0] = 0.1f; lit.ambient[1] = 0.1f; lit.ambient[2] = 0.1f; lit.ambient[3] = 1.0f;
    lit.tuning[0] = 1.0f;
    lit.tuning[1] = 0.02f;
    lit.tuning[2] = 0.0f;
    lit.tuning[3] = 0.0f;

    SDL_GPUColorTargetInfo target_info = {};
    target_info.texture = target;
    target_info.clear_color = (SDL_FColor){ 0.05f, 0.05f, 0.05f, 1.0f };
    target_info.load_op = SDL_GPU_LOADOP_CLEAR;
    target_info.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &target_info, 1, NULL);
    SDL_BindGPUGraphicsPipeline(pass, ctx->pipeline);

    SDL_GPUBufferBinding vbind = {};
    vbind.buffer = ctx->vbuffer; vbind.offset = 0;
    SDL_BindGPUVertexBuffers(pass, 0, &vbind, 1);

    SDL_GPUBufferBinding ibind = {};
    ibind.buffer = ctx->ibuffer; ibind.offset = 0;
    SDL_BindGPUIndexBuffer(pass, &ibind, SDL_GPU_INDEXELEMENTSIZE_16BIT);

    SDL_PushGPUVertexUniformData(cmd, 0, view_proj, sizeof(view_proj));
    SDL_PushGPUFragmentUniformData(cmd, 0, &lit, sizeof(lit));

    SDL_GPUTextureSamplerBinding binds[4] = {};
    binds[0].texture = (SDL_GPUTexture*)maps->diffuse.internal;  // filled by create_texture
    binds[0].sampler = ctx->sampler;
    binds[1].texture = (SDL_GPUTexture*)maps->normal.internal;
    binds[1].sampler = ctx->sampler;
    binds[2].texture = (SDL_GPUTexture*)maps->depth.internal;
    binds[2].sampler = ctx->sampler;
    binds[3].texture = (SDL_GPUTexture*)maps->position.internal;
    binds[3].sampler = ctx->sampler;
    SDL_BindGPUFragmentSamplers(pass, 0, binds, 4);

    SDL_DrawGPUIndexedPrimitives(pass, 6, 1, 0, 0, 0);
    SDL_EndGPURenderPass(pass);
}

SDL_GPUTexture* preview_render_to_texture(preview_context_t* ctx,
                                          const extracted_maps_t* maps,
                                          float star_angle,
                                          int out_w, int out_h,
                                          SDL_GPUCommandBuffer* cmd)
{
    if (!ctx || !ctx->initialized || !maps || !cmd) return NULL;
    if (!maps->diffuse.internal || !maps->normal.internal || !maps->depth.internal || !maps->position.internal)
        return NULL;

    SDL_GPUTexture* tex = create_target_texture(ctx->device, out_w, out_h, SDL_GPU_TEXTUREUSAGE_SAMPLER);
    if (!tex) return NULL;

    preview_render(ctx, tex, maps, star_angle, out_w, out_h, cmd);
    return tex;
}

image_t preview_render_to_image(preview_context_t* ctx,
                                const extracted_maps_t* maps,
                                float star_angle,
                                int out_w, int out_h)
{
    image_t out = {};
    if (!ctx || !ctx->initialized || !maps) return out;
    if (!maps->diffuse.internal || !maps->normal.internal || !maps->depth.internal || !maps->position.internal)
        return out;

    SDL_GPUTexture* tex = create_target_texture(ctx->device, out_w, out_h, 0);
    if (!tex) return out;

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(ctx->device);
    preview_render(ctx, tex, maps, star_angle, out_w, out_h, cmd);

    SDL_GPUTransferBufferCreateInfo tinfo = {};
    tinfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tinfo.size = (Uint32)(out_w * out_h * 4);
    SDL_GPUTransferBuffer* tbuf = SDL_CreateGPUTransferBuffer(ctx->device, &tinfo);
    if (!tbuf)
    {
        SDL_ReleaseGPUTexture(ctx->device, tex);
        SDL_SubmitGPUCommandBuffer(cmd);
        return out;
    }

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src = {};
    src.texture = tex;
    src.w = (Uint32)out_w; src.h = (Uint32)out_h; src.d = 1;
    SDL_GPUTextureTransferInfo dst = {};
    dst.transfer_buffer = tbuf;
    dst.offset = 0;
    dst.pixels_per_row = (Uint32)out_w;
    dst.rows_per_layer = (Uint32)out_h;
    SDL_DownloadFromGPUTexture(cp, &src, &dst);
    SDL_EndGPUCopyPass(cp);

    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (!fence)
    {
        SDL_ReleaseGPUTransferBuffer(ctx->device, tbuf);
        SDL_ReleaseGPUTexture(ctx->device, tex);
        return out;
    }

    SDL_WaitForGPUFences(ctx->device, true, &fence, 1);
    SDL_ReleaseGPUFence(ctx->device, fence);

    void* pixels = SDL_MapGPUTransferBuffer(ctx->device, tbuf, false);
    if (pixels)
    {
        out = image_alloc(out_w, out_h);
        if (out.rgba)
        {
            memcpy(out.rgba, pixels, (size_t)out_w * out_h * 4);
        }
        SDL_UnmapGPUTransferBuffer(ctx->device, tbuf);
    }

    SDL_ReleaseGPUTransferBuffer(ctx->device, tbuf);
    SDL_ReleaseGPUTexture(ctx->device, tex);
    return out;
}
