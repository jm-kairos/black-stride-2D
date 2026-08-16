#pragma once

#include <defines.h>
#include <SDL3/SDL_gpu.h>
#include <vector>

struct bs_starfield_params;
namespace bs_math { struct Vec2; }

// =====================================================================================
// SDL GPU resources for the Endless-Sky-style starfield VBO pipeline.
// Owns the graphics pipeline and per-layer vertex buffers. Draws visible tiles per frame.
// =====================================================================================

class StarfieldGpuResources {
public:
    StarfieldGpuResources();
    ~StarfieldGpuResources();

    // Create the graphics pipeline. `colorFmt` must match the render target format.
    b8 init(SDL_GPUDevice* device, SDL_GPUShader* vs, SDL_GPUShader* fs,
            SDL_GPUTextureFormat colorFmt);

    // Release GPU resources (pipeline + all layer VBOs).
    void shutdown(SDL_GPUDevice* device);

    // Upload or update vertex data for a specific layer.
    // `layerId` is the parallax layer id (e.g. 0, 1).
    void upload_layer(SDL_GPUDevice* device, u32 layerId,
                      const float* data, u32 floatCount);

    // Draw all visible tiles for a single starfield layer pass.
    void draw(SDL_GPUCommandBuffer* cmd,
              SDL_GPURenderPass* pass,
              const bs_starfield_params& params,
              const bs_math::Vec2& blur,
              float brightness);

    b8 has_pipeline() const { return pipeline_ != nullptr; }

private:
    struct LayerVbo {
        SDL_GPUBuffer* buffer = nullptr;
        u32 vertexCount = 0; // total vertices (floatCount / 4)
    };

    SDL_GPUGraphicsPipeline* pipeline_ = nullptr;
    SDL_GPUDevice*           device_ = nullptr;
    std::vector<LayerVbo>    layerVbos_;
};
