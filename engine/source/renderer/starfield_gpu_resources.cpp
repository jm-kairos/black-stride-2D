#include "starfield_gpu_resources.h"
#include "renderer_types.h"
#include "backend/renderer_backend_sdlgpu.h"
#include <core/logger.h>
#include <math/math_utils.h>

// =====================================================================================
StarfieldGpuResources::StarfieldGpuResources() = default;
StarfieldGpuResources::~StarfieldGpuResources() = default;

// =====================================================================================
b8 StarfieldGpuResources::init(SDL_GPUDevice* device, SDL_GPUShader* vs,
                                SDL_GPUShader* fs, SDL_GPUTextureFormat colorFmt)
{
    device_ = device;
    SDL_GPUGraphicsPipelineCreateInfo pipelineInfo;
    SDL_zero(pipelineInfo);
    pipelineInfo.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    pipelineInfo.vertex_shader = vs;
    pipelineInfo.fragment_shader = fs;

    // Vertex input: interleaved (offset_xy, size, corner, r, g, b)
    SDL_GPUVertexBufferDescription vbufDesc;
    SDL_zero(vbufDesc);
    vbufDesc.slot = 0;
    vbufDesc.pitch = 7 * sizeof(float); // 7 floats per vertex
    vbufDesc.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
    pipelineInfo.vertex_input_state.num_vertex_buffers = 1;
    pipelineInfo.vertex_input_state.vertex_buffer_descriptions = &vbufDesc;

    SDL_GPUVertexAttribute attribs[6];
    SDL_zero(attribs);
    attribs[0].location = 0;  attribs[0].buffer_slot = 0;
    attribs[0].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2;  attribs[0].offset = 0;
    attribs[1].location = 1;  attribs[1].buffer_slot = 0;
    attribs[1].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;    attribs[1].offset = 2 * sizeof(float);
    attribs[2].location = 2;  attribs[2].buffer_slot = 0;
    attribs[2].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT;    attribs[2].offset = 3 * sizeof(float);
    attribs[3].location = 3;  attribs[3].buffer_slot = 0;
    attribs[3].format = SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3;   attribs[3].offset = 4 * sizeof(float);
    pipelineInfo.vertex_input_state.num_vertex_attributes = 4;
    pipelineInfo.vertex_input_state.vertex_attributes = attribs;

    // Target format with additive blending (ONE, ONE — Endless Sky style)
    SDL_GPUColorTargetDescription colorTarget;
    SDL_zero(colorTarget);
    colorTarget.format = colorFmt;
    colorTarget.blend_state.enable_blend = true;
    colorTarget.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    colorTarget.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
    colorTarget.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;

    pipelineInfo.target_info.num_color_targets = 1;
    pipelineInfo.target_info.color_target_descriptions = &colorTarget;
    pipelineInfo.target_info.has_depth_stencil_target = false;

    pipelineInfo.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    pipelineInfo.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    pipelineInfo.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;

    pipelineInfo.depth_stencil_state.enable_depth_test = false;
    pipelineInfo.depth_stencil_state.enable_depth_write = false;
    pipelineInfo.depth_stencil_state.enable_stencil_test = false;

    pipeline_ = SDL_CreateGPUGraphicsPipeline(device, &pipelineInfo);
    if (!pipeline_) {
        BS_LOG_FATAL("StarfieldGpuResources: failed to create pipeline");
        return FALSE;
    }
    return TRUE;
}

// =====================================================================================
void StarfieldGpuResources::shutdown(SDL_GPUDevice* device)
{
    for (auto& lv : layerVbos_) {
        if (lv.buffer) {
            SDL_ReleaseGPUBuffer(device, lv.buffer);
            lv.buffer = nullptr;
        }
    }
    layerVbos_.clear();
    if (pipeline_) {
        SDL_ReleaseGPUGraphicsPipeline(device, pipeline_);
        pipeline_ = nullptr;
    }
    device_ = nullptr;
}

// =====================================================================================
void StarfieldGpuResources::upload_layer(SDL_GPUDevice* device, u32 layerId,
                                          const float* data, u32 floatCount)
{
    if (layerId >= layerVbos_.size())
        layerVbos_.resize(layerId + 1);

    LayerVbo& lv = layerVbos_[layerId];
    if (lv.buffer) {
        SDL_ReleaseGPUBuffer(device, lv.buffer);
        lv.buffer = nullptr;
    }
    if (floatCount == 0) return;

    u32 byteCount = floatCount * sizeof(float);
    SDL_GPUBufferCreateInfo bufferInfo = {};
    bufferInfo.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
    bufferInfo.size = byteCount;
    lv.buffer = SDL_CreateGPUBuffer(device, &bufferInfo);
    if (!lv.buffer) {
        BS_LOG_ERROR("StarfieldGpuResources: failed to create VBO for layer %u", layerId);
        return;
    }

    SDL_GPUTransferBufferCreateInfo transferInfo = {};
    transferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    transferInfo.size = byteCount;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device, &transferInfo);
    if (!transfer) {
        BS_LOG_ERROR("StarfieldGpuResources: failed to create transfer buffer");
        return;
    }

    void* mapped = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (mapped) {
        SDL_memcpy(mapped, data, byteCount);
        SDL_UnmapGPUTransferBuffer(device, transfer);
    }

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
    if (cmd) {
        SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src = { transfer, 0 };
        SDL_GPUBufferRegion dst = { lv.buffer, 0, byteCount };
        SDL_UploadToGPUBuffer(copyPass, &src, &dst, false);
        SDL_EndGPUCopyPass(copyPass);
        SDL_SubmitGPUCommandBuffer(cmd);
    }
    SDL_ReleaseGPUTransferBuffer(device, transfer);

    lv.vertexCount = floatCount / 7; // 7 floats per vertex
}

// =====================================================================================
void StarfieldGpuResources::draw(SDL_GPUCommandBuffer* cmd,
                                SDL_GPURenderPass* pass,
                                const bs_starfield_params& params,
                                const bs_math::Vec2& blur,
                                float brightness)
{
    if (!pipeline_) return;
    u32 layerId = params.layer_id;
    if (layerId >= layerVbos_.size()) return;
    const LayerVbo& lv = layerVbos_[layerId];
    if (!lv.buffer || lv.vertexCount == 0) return;

    if (!params.layer_data) return;
    const bs_starfield_layer_data& ld = *params.layer_data;

    SDL_BindGPUGraphicsPipeline(pass, pipeline_);
    SDL_GPUBufferBinding binding = { lv.buffer, 0 };
    SDL_BindGPUVertexBuffers(pass, 0, &binding, 1);

    float zoom = params.cam.zoom;
    float fb_w = (float)params.fb_w;
    float fb_h = (float)params.fb_h;
    if (fb_w < 1.0f) fb_w = 1280.0f;
    if (fb_h < 1.0f) fb_h = 720.0f;

    // 3-pass fullness: each pass gets a slightly different zoom and brightness.
    float passMult = powf((float)(params.pass_index + 1), 0.2f);
    zoom *= passMult;
    brightness *= powf(0.5f, (float)params.pass_index);

    // Clamp zoom so stars stay visible at extreme zoom-out (Endless Sky style).
    static const float MIN_ZOOM = 0.15f;
    if (zoom < MIN_ZOOM) zoom = MIN_ZOOM;

    float baseZoom = 2.0f * zoom;
    float scaleX = baseZoom / fb_w;
    float scaleY = -baseZoom / fb_h;

    float blurLen = sqrtf(blur.x * blur.x + blur.y * blur.y);
    float rot00, rot01, rot10, rot11;
    if (blurLen > 0.001f) {
        float ux = blur.x / blurLen;
        float uy = blur.y / blurLen;
        rot00 = uy;  rot01 = -ux;
        rot10 = ux;  rot11 = uy;
    } else {
        rot00 = 1.0f; rot01 = 0.0f;
        rot10 = 0.0f; rot11 = 1.0f;
    }

    float elongation = blurLen * zoom;
    if (brightness > 1.0f) brightness = 1.0f;

    float uniforms[12];
    uniforms[0] = scaleX;   uniforms[1] = scaleY;
    uniforms[2] = 0.0f;     uniforms[3] = 0.0f;
    uniforms[4] = rot00;    uniforms[5] = rot01;
    uniforms[6] = rot10;    uniforms[7] = rot11;
    uniforms[8] = elongation; uniforms[9] = brightness;
    uniforms[10] = 0.0f;    uniforms[11] = 0.0f;

    // The starfield repeats every (widthMod+1) units. Wrap camera into that
    // range so tile culling works at galaxy-scale coordinates.
    float fieldSize = (float)(ld.widthMod + 1);
    float wrapX = fmodf(params.cam.position.x, fieldSize);
    float wrapY = fmodf(params.cam.position.y, fieldSize);
    if (wrapX < 0.0f) wrapX += fieldSize;
    if (wrapY < 0.0f) wrapY += fieldSize;

    float borderX = fabsf(blur.x) + 1.0f;
    float borderY = fabsf(blur.y) + 1.0f;
    float halfW = fb_w * 0.5f;
    float halfH = fb_h * 0.5f;
    float left   = -halfW - borderX;
    float right  =  halfW + borderX;
    float top    = -halfH - borderY;
    float bottom =  halfH + borderY;

    // Compute visible tile bounds using the WRAPPED position.
    int minX = (int)(wrapX + left / zoom);
    int minY = (int)(wrapY + top / zoom);
    int maxX = (int)(wrapX + right / zoom);
    int maxY = (int)(wrapY + bottom / zoom);

    static const int TILE_SIZE = 256;
    minX &= ~(TILE_SIZE - 1);
    minY &= ~(TILE_SIZE - 1);

    for (int gy = minY; gy < maxY; gy += TILE_SIZE)
    {
        for (int gx = minX; gx < maxX; gx += TILE_SIZE)
        {
            // Offset is relative to wrapped position so stars land on screen.
            float offX = (float)gx - wrapX;
            float offY = (float)gy - wrapY;
            uniforms[2] = offX;
            uniforms[3] = offY;
            SDL_PushGPUVertexUniformData(cmd, 0, uniforms, sizeof(uniforms));

            // Wrap tile index into the repeating field.
            int idx = ((gx & ld.widthMod) / TILE_SIZE)
                    + ((gy & ld.widthMod) / TILE_SIZE) * ld.tileCols;
            if (idx < 0 || idx >= ld.tileCols * ld.tileCols) continue;

            int first = ld.tileIndex[idx];
            int count = ld.tileIndex[idx + 1] - first;
            if (count <= 0) continue;

            SDL_DrawGPUPrimitives(pass, 6 * count, 1, 6 * first, 0);
        }
    }
}
