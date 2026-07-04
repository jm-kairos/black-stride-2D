#pragma once
#include <stdint.h>

typedef struct
{
    int w, h;
    unsigned char* rgba; // w * h * 4, top-left origin
    void* internal;      // optional GPU texture (SDL_GPUTexture*) used by the preview
} image_t;

typedef struct
{
    image_t diffuse;
    image_t normal;
    image_t depth;
    image_t position;
} extracted_maps_t;

typedef enum
{
    NORMAL_ALGORITHM_SDF_CHAMFER = 0, // current 2-pass 1D chamfer SDF
    NORMAL_ALGORITHM_SDF_TRUE_EDT,    // Felzenszwalb-Huttenlocher Euclidean SDF
    NORMAL_ALGORITHM_ALPHA_GRADIENT,  // gradient of the alpha channel
    NORMAL_ALGORITHM_GAUSSIAN_HEIGHT, // Gaussian-blurred height field
    NORMAL_ALGORITHM_COUNT
} normal_algorithm_t;

image_t image_load(const char* path);
void image_free(image_t* img);
int image_save_png(const char* path, image_t* img);

image_t image_alloc(int w, int h);

// Generate the four maps from a single RGBA input.
// alpha_threshold: pixels with alpha > threshold are considered inside (0..1).
// normal_strength: 0..2, scales the Z component of the derived normal.
// depth_contrast: 0..4, steepens the SDF->depth curve.
extracted_maps_t extract_maps(image_t input, float alpha_threshold, float normal_strength, float depth_contrast,
                             normal_algorithm_t normal_algo);

void extracted_maps_free(extracted_maps_t* maps);
