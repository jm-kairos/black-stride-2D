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
    NORMAL_ALGORITHM_SDF_CHAMFER = 0, // beveled hull from a 2-pass 1D chamfer SDF
    NORMAL_ALGORITHM_SDF_TRUE_EDT,    // beveled hull from a Felzenszwalb-Huttenlocher Euclidean SDF
    NORMAL_ALGORITHM_ALPHA_GRADIENT,  // height straight from the alpha channel
    NORMAL_ALGORITHM_GAUSSIAN_HEIGHT, // height from a Gaussian-blurred alpha channel
    NORMAL_ALGORITHM_COUNT
} normal_algorithm_t;

// All extraction tunables in one place so the GUI, the worker thread and the headless
// modes stay in sync. Pixel-unit fields scale with the source art resolution.
typedef struct
{
    float alpha_threshold;   // 0..1: alpha above this counts as hull
    float normal_strength;   // slope multiplier for the normal map (1 = geometric slope)
    float depth_contrast;    // pow() applied to the bevel profile
    normal_algorithm_t normal_algo;
    float bevel_px;          // rounded-bevel width in pixels; <= 0 picks one from the art size
    float dome_amount;       // 0..1: large-scale height rise from the rim toward the hull spine
    float detail_amp_px;     // relief amplitude (in pixels) recovered from painted luminance
    float detail_radius_px;  // band-pass radius separating panel detail from painted lighting
    float ao_strength;       // 0..1: cavity darkening baked into the diffuse
    float profile_amp;       // multiplier on the side-view elevation profile (1 = true scale)
    int side_nose_dir;       // side-view nose direction: 0 = auto-detect, -1 = left, +1 = right
} extract_params_t;

extract_params_t extract_params_default(void);

image_t image_load(const char* path);
void image_free(image_t* img);
int image_save_png(const char* path, image_t* img);

image_t image_alloc(int w, int h);

// Generate the four maps from a single top-down RGBA input, plus an optional side-view
// image of the same hull (side_view.rgba == NULL disables it). The whole pipeline derives
// one height field (rounded silhouette bevel + optional dome + side-view dorsal elevation
// profile + luminance-recovered surface detail); the normal map is the analytic gradient
// of that field and the depth map is its normalization, so lighting and parallax agree.
// The side view contributes a per-station elevation curve read off its top silhouette
// (command towers, raised decks), mapped along the top-down ship's length and converted
// to top-down pixel units by the length ratio of the two silhouettes.
extracted_maps_t extract_maps(image_t input, image_t side_view, const extract_params_t* params);

void extracted_maps_free(extracted_maps_t* maps);
