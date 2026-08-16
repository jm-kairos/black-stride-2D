#pragma once
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "extractor.h"

// Lightweight renderer that replicates the engine's mapped-sprite pipeline
// so the map_extractor can verify the generated 4 maps under directional light.

typedef struct preview_context
{
    SDL_GPUDevice*           device;
    SDL_GPUGraphicsPipeline* pipeline;
    SDL_GPUShader*           vs;
    SDL_GPUShader*           fs;
    SDL_GPUSampler*          sampler;
    SDL_GPUBuffer*           vbuffer;
    SDL_GPUBuffer*           ibuffer;
    int                      initialized;
} preview_context_t;

// Initialize/shutdown the preview pipeline for a given GPU device.
// Returns 0 on failure, 1 on success.
int preview_init(preview_context_t* ctx, SDL_GPUDevice* device);
void preview_shutdown(preview_context_t* ctx);

// Render the four maps to a CPU image using the engine's mapped-sprite shaders.
// The output image is RGBA8, sized out_w x out_h, and must be freed with image_free.
// star_angle is in radians (0 = light from +X, CCW).
image_t preview_render_to_image(preview_context_t* ctx,
                                const extracted_maps_t* maps,
                                float star_angle,
                                int out_w, int out_h);

// Render the four maps to a GPU texture for ImGui display.
// The returned texture must be released with SDL_ReleaseGPUTexture.
SDL_GPUTexture* preview_render_to_texture(preview_context_t* ctx,
                                          const extracted_maps_t* maps,
                                          float star_angle,
                                          int out_w, int out_h,
                                          SDL_GPUCommandBuffer* cmd);

// Render the four maps to an existing GPU texture. Useful when the GUI reuses
// the same target texture every frame (e.g. while auto-rotating the star).
void preview_render(preview_context_t* ctx,
                    SDL_GPUTexture* target,
                    const extracted_maps_t* maps,
                    float star_angle,
                    int out_w, int out_h,
                    SDL_GPUCommandBuffer* cmd);
