#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include "extractor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

image_t image_load(const char* path)
{
    image_t out = {};
    int w = 0, h = 0, channels = 0;
    unsigned char* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels)
    {
        return out;
    }
    out.w = w;
    out.h = h;
    out.rgba = pixels;
    return out;
}

void image_free(image_t* img)
{
    if (img && img->rgba)
    {
        stbi_image_free(img->rgba);
        img->rgba = nullptr;
        img->w = 0;
        img->h = 0;
        img->internal = nullptr;
    }
}

int image_save_png(const char* path, image_t* img)
{
    if (!img || !img->rgba) return 0;
    return stbi_write_png(path, img->w, img->h, 4, img->rgba, img->w * 4);
}

image_t image_alloc(int w, int h)
{
    image_t out = {};
    out.w = w;
    out.h = h;
    out.internal = nullptr;
    out.rgba = (unsigned char*)malloc((size_t)w * h * 4);
    if (out.rgba)
    {
        memset(out.rgba, 0, (size_t)w * h * 4);
    }
    return out;
}

static inline unsigned char f2b(float v)
{
    int i = (int)(v * 255.0f + 0.5f);
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    return (unsigned char)i;
}

static inline float b2f(unsigned char v)
{
    return v / 255.0f;
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// 4-connected / Manhattan chamfer distance transform in 1D.
// Input/output: float array where edges are 0 and non-edges are INF.
static void chamfer_1d(float* d, int n)
{
    for (int i = 1; i < n; ++i)
    {
        float cand = d[i - 1] + 1.0f;
        if (cand < d[i]) d[i] = cand;
    }
    for (int i = n - 2; i >= 0; --i)
    {
        float cand = d[i + 1] + 1.0f;
        if (cand < d[i]) d[i] = cand;
    }
}

// Chamfer distance transform: treat edges as 0, non-edges as INF.
static float* compute_edge_distance_chamfer(const unsigned char* mask, int w, int h)
{
    const float INF = 1e9f;
    float* d = (float*)malloc((size_t)w * h * sizeof(float));
    if (!d) return nullptr;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            int i = y * w + x;
            unsigned char m = mask[i];
            unsigned char edge = 0;
            if (m)
            {
                if (x > 0 && !mask[i - 1]) edge = 1;
                if (x < w - 1 && !mask[i + 1]) edge = 1;
                if (y > 0 && !mask[i - w]) edge = 1;
                if (y < h - 1 && !mask[i + w]) edge = 1;
            }
            d[i] = edge ? 0.0f : INF;
        }
    }

    for (int y = 0; y < h; ++y)
    {
        chamfer_1d(&d[y * w], w);
    }

    float* col = (float*)malloc((size_t)h * sizeof(float));
    for (int x = 0; x < w; ++x)
    {
        for (int y = 0; y < h; ++y)
        {
            col[y] = d[y * w + x];
        }
        chamfer_1d(col, h);
        for (int y = 0; y < h; ++y)
        {
            d[y * w + x] = col[y];
        }
    }
    free(col);

    return d;
}

// Felzenszwalb-Huttenlocher 1D squared Euclidean distance transform.
static void edt_1d(float* f, int* v, float* z, int n)
{
    int k = 0;
    v[0] = 0;
    z[0] = -1e9f;
    z[1] = 1e9f;
    for (int q = 1; q < n; ++q)
    {
        float s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * q - 2.0f * v[k]);
        while (s <= z[k])
        {
            k--;
            s = ((f[q] + (float)q * q) - (f[v[k]] + (float)v[k] * v[k])) / (2.0f * q - 2.0f * v[k]);
        }
        k++;
        v[k] = q;
        z[k] = s;
        z[k + 1] = 1e9f;
    }

    k = 0;
    for (int q = 0; q < n; ++q)
    {
        while (z[k + 1] < q) k++;
        f[q] = (q - v[k]) * (q - v[k]) + f[v[k]];
    }
}

static void compute_edt(float* d, int w, int h)
{
    int max_n = w > h ? w : h;
    int* v = (int*)malloc((size_t)max_n * sizeof(int));
    float* z = (float*)malloc((size_t)(max_n + 1) * sizeof(float));

    for (int y = 0; y < h; ++y)
    {
        edt_1d(&d[y * w], v, z, w);
    }

    float* col = (float*)malloc((size_t)h * sizeof(float));
    for (int x = 0; x < w; ++x)
    {
        for (int y = 0; y < h; ++y)
        {
            col[y] = d[y * w + x];
        }
        edt_1d(col, v, z, h);
        for (int y = 0; y < h; ++y)
        {
            d[y * w + x] = col[y];
        }
    }

    free(col);
    free(v);
    free(z);
}

// True signed Euclidean distance transform.
// Returns a signed distance array: positive inside, negative outside.
static float* compute_edge_distance_true_edt(const unsigned char* mask, int w, int h)
{
    const float INF = 1e9f;
    int n = w * h;
    float* inside = (float*)malloc((size_t)n * sizeof(float));
    float* outside = (float*)malloc((size_t)n * sizeof(float));
    if (!inside || !outside)
    {
        free(inside);
        free(outside);
        return nullptr;
    }

    // Inside EDT: 0 at boundary pixels, INF elsewhere inside.
    for (int i = 0; i < n; ++i)
    {
        unsigned char m = mask[i];
        int x = i % w;
        int y = i / w;
        unsigned char edge = 0;
        if (m)
        {
            if (x > 0 && !mask[i - 1]) edge = 1;
            if (x < w - 1 && !mask[i + 1]) edge = 1;
            if (y > 0 && !mask[i - w]) edge = 1;
            if (y < h - 1 && !mask[i + w]) edge = 1;
        }
        inside[i] = (m && edge) ? 0.0f : INF;
    }

    // Outside EDT: 0 at boundary pixels, INF elsewhere outside.
    for (int i = 0; i < n; ++i)
    {
        unsigned char m = mask[i];
        int x = i % w;
        int y = i / w;
        unsigned char edge = 0;
        if (!m)
        {
            if (x > 0 && mask[i - 1]) edge = 1;
            if (x < w - 1 && mask[i + 1]) edge = 1;
            if (y > 0 && mask[i - w]) edge = 1;
            if (y < h - 1 && mask[i + w]) edge = 1;
        }
        outside[i] = (!m && edge) ? 0.0f : INF;
    }

    compute_edt(inside, w, h);
    compute_edt(outside, w, h);

    float* d = (float*)malloc((size_t)n * sizeof(float));
    for (int i = 0; i < n; ++i)
    {
        d[i] = mask[i] ? sqrtf(inside[i]) : -sqrtf(outside[i]);
    }

    free(inside);
    free(outside);
    return d;
}

static void blur_float(float* dst, const float* src, int w, int h)
{
    float kernel[5] = { 0.06136f, 0.24477f, 0.38774f, 0.24477f, 0.06136f };
    float* temp = (float*)malloc((size_t)w * h * sizeof(float));

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0f;
            for (int k = 0; k < 5; ++k)
            {
                int xk = x + k - 2;
                if (xk < 0) xk = 0;
                if (xk >= w) xk = w - 1;
                sum += src[y * w + xk] * kernel[k];
            }
            temp[y * w + x] = sum;
        }
    }

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0f;
            for (int k = 0; k < 5; ++k)
            {
                int yk = y + k - 2;
                if (yk < 0) yk = 0;
                if (yk >= h) yk = h - 1;
                sum += temp[yk * w + x] * kernel[k];
            }
            dst[y * w + x] = sum;
        }
    }

    free(temp);
}

static void compute_sdf_normal(const unsigned char* mask, const float* dist, const float* smooth_dist, int w, int h, int x, int y, int i, float normal_strength, float* nx, float* ny, float* nz)
{
    float dx = 0.0f, dy = 0.0f;
    if (mask[i])
    {
        const float* d = smooth_dist ? smooth_dist : dist;
        float sdf = d[i];
        float left  = (x > 0) ? (mask[i - 1] ? d[i - 1] : -d[i - 1]) : sdf;
        float right = (x < w - 1) ? (mask[i + 1] ? d[i + 1] : -d[i + 1]) : sdf;
        float up    = (y > 0) ? (mask[i - w] ? d[i - w] : -d[i - w]) : sdf;
        float down  = (y < h - 1) ? (mask[i + w] ? d[i + w] : -d[i + w]) : sdf;
        dx = (right - left) * 0.5f;
        dy = (down - up) * 0.5f;
    }
    *nx = -dx;
    *ny = -dy;
    *nz = normal_strength;
    float len = sqrtf((*nx) * (*nx) + (*ny) * (*ny) + (*nz) * (*nz));
    if (len > 0.0f)
    {
        *nx /= len; *ny /= len; *nz /= len;
    }
}

static void compute_alpha_gradient_normal(const unsigned char* rgba, int w, int h, int x, int y, int i, float normal_strength, float* nx, float* ny, float* nz)
{
    float a = b2f(rgba[i * 4 + 3]);
    float left = (x > 0) ? b2f(rgba[(i - 1) * 4 + 3]) : a;
    float right = (x < w - 1) ? b2f(rgba[(i + 1) * 4 + 3]) : a;
    float up = (y > 0) ? b2f(rgba[(i - w) * 4 + 3]) : a;
    float down = (y < h - 1) ? b2f(rgba[(i + w) * 4 + 3]) : a;
    float dx = (right - left) * 0.5f;
    float dy = (down - up) * 0.5f;
    *nx = -dx;
    *ny = -dy;
    *nz = normal_strength;
    float len = sqrtf((*nx) * (*nx) + (*ny) * (*ny) + (*nz) * (*nz));
    if (len > 0.0f)
    {
        *nx /= len; *ny /= len; *nz /= len;
    }
}

static void gaussian_blur_alpha(const unsigned char* rgba, float* dst, int w, int h)
{
    float kernel[5] = { 0.06136f, 0.24477f, 0.38774f, 0.24477f, 0.06136f };
    float* temp = (float*)malloc((size_t)w * h * sizeof(float));

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0f;
            for (int k = 0; k < 5; ++k)
            {
                int xk = x + k - 2;
                if (xk < 0) xk = 0;
                if (xk >= w) xk = w - 1;
                sum += b2f(rgba[(y * w + xk) * 4 + 3]) * kernel[k];
            }
            temp[y * w + x] = sum;
        }
    }

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            float sum = 0.0f;
            for (int k = 0; k < 5; ++k)
            {
                int yk = y + k - 2;
                if (yk < 0) yk = 0;
                if (yk >= h) yk = h - 1;
                sum += temp[yk * w + x] * kernel[k];
            }
            dst[y * w + x] = sum;
        }
    }

    free(temp);
}

static void compute_gaussian_height_normal(const float* height, int w, int h, int x, int y, int i, float normal_strength, float* nx, float* ny, float* nz)
{
    float hval = height[i];
    float left = (x > 0) ? height[i - 1] : hval;
    float right = (x < w - 1) ? height[i + 1] : hval;
    float up = (y > 0) ? height[i - w] : hval;
    float down = (y < h - 1) ? height[i + w] : hval;
    float dx = (right - left) * 0.5f;
    float dy = (down - up) * 0.5f;
    *nx = -dx;
    *ny = -dy;
    *nz = normal_strength;
    float len = sqrtf((*nx) * (*nx) + (*ny) * (*ny) + (*nz) * (*nz));
    if (len > 0.0f)
    {
        *nx /= len; *ny /= len; *nz /= len;
    }
}

extracted_maps_t extract_maps(image_t input, float alpha_threshold, float normal_strength, float depth_contrast,
                             normal_algorithm_t normal_algo)
{
    extracted_maps_t out = {};
    if (!input.rgba || input.w <= 0 || input.h <= 0)
    {
        return out;
    }

    int w = input.w;
    int h = input.h;
    int n = w * h;

    unsigned char* mask = (unsigned char*)malloc((size_t)n);
    if (!mask)
    {
        return out;
    }

    for (int i = 0; i < n; ++i)
    {
        float a = b2f(input.rgba[i * 4 + 3]);
        mask[i] = (a > alpha_threshold) ? 255 : 0;
    }

    float* dist = nullptr;
    if (normal_algo == NORMAL_ALGORITHM_SDF_CHAMFER)
    {
        dist = compute_edge_distance_chamfer(mask, w, h);
    }
    else
    {
        dist = compute_edge_distance_true_edt(mask, w, h);
    }

    if (!dist)
    {
        free(mask);
        return out;
    }

    // Find max inside distance for depth normalization
    float max_inside = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        if (mask[i])
        {
            if (dist[i] > max_inside) max_inside = dist[i];
        }
    }
    if (max_inside < 1.0f) max_inside = 1.0f;

    float* height = nullptr;
    if (normal_algo == NORMAL_ALGORITHM_GAUSSIAN_HEIGHT)
    {
        height = (float*)malloc((size_t)n * sizeof(float));
        if (height)
        {
            gaussian_blur_alpha(input.rgba, height, w, h);
        }
    }

    float* smooth_dist = nullptr;
    if (normal_algo == NORMAL_ALGORITHM_SDF_TRUE_EDT)
    {
        smooth_dist = (float*)malloc((size_t)n * sizeof(float));
        float* temp = (float*)malloc((size_t)n * sizeof(float));
        if (smooth_dist && temp)
        {
            blur_float(temp, dist, w, h);
            blur_float(smooth_dist, temp, w, h);
        }
        free(temp);
    }

    out.diffuse = image_alloc(w, h);
    out.normal = image_alloc(w, h);
    out.depth = image_alloc(w, h);
    out.position = image_alloc(w, h);

    if (!out.diffuse.rgba || !out.normal.rgba || !out.depth.rgba || !out.position.rgba)
    {
        free(mask);
        free(dist);
        free(height);
        free(smooth_dist);
        extracted_maps_free(&out);
        return out;
    }

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            int i = y * w + x;
            int oi = i * 4;
            unsigned char m = mask[i];

            // Diffuse: keep original where inside, clear outside
            out.diffuse.rgba[oi + 0] = m ? input.rgba[oi + 0] : 0;
            out.diffuse.rgba[oi + 1] = m ? input.rgba[oi + 1] : 0;
            out.diffuse.rgba[oi + 2] = m ? input.rgba[oi + 2] : 0;
            out.diffuse.rgba[oi + 3] = m ? 255 : 0;

            // SDF with sign: positive inside, negative outside
            float sdf = dist[i];

            // Normal
            float nx = 0.0f, ny = 0.0f, nz = 0.0f;
            switch (normal_algo)
            {
            case NORMAL_ALGORITHM_SDF_CHAMFER:
                compute_sdf_normal(mask, dist, nullptr, w, h, x, y, i, normal_strength, &nx, &ny, &nz);
                break;
            case NORMAL_ALGORITHM_SDF_TRUE_EDT:
                compute_sdf_normal(mask, dist, smooth_dist, w, h, x, y, i, normal_strength, &nx, &ny, &nz);
                break;
            case NORMAL_ALGORITHM_ALPHA_GRADIENT:
                compute_alpha_gradient_normal(input.rgba, w, h, x, y, i, normal_strength, &nx, &ny, &nz);
                break;
            case NORMAL_ALGORITHM_GAUSSIAN_HEIGHT:
                compute_gaussian_height_normal(height, w, h, x, y, i, normal_strength, &nx, &ny, &nz);
                break;
            default:
                compute_sdf_normal(mask, dist, nullptr, w, h, x, y, i, normal_strength, &nx, &ny, &nz);
                break;
            }
            out.normal.rgba[oi + 0] = f2b((nx + 1.0f) * 0.5f);
            out.normal.rgba[oi + 1] = f2b((ny + 1.0f) * 0.5f);
            out.normal.rgba[oi + 2] = f2b((nz + 1.0f) * 0.5f);
            out.normal.rgba[oi + 3] = 255;

            // Depth: smoothstep from edge (0) to max interior (1)
            float t = 0.0f;
            if (sdf > 0.0f)
            {
                t = sdf / max_inside;
                t = clampf(t, 0.0f, 1.0f);
                t = powf(t, depth_contrast);
            }
            unsigned char dval = f2b(t);
            out.depth.rgba[oi + 0] = dval;
            out.depth.rgba[oi + 1] = dval;
            out.depth.rgba[oi + 2] = dval;
            out.depth.rgba[oi + 3] = 255;

            // Position: local UV + depth
            float px = (w > 1) ? (float)x / (float)(w - 1) : 0.5f;
            float py = (h > 1) ? (float)y / (float)(h - 1) : 0.5f;
            out.position.rgba[oi + 0] = f2b(px);
            out.position.rgba[oi + 1] = f2b(py);
            out.position.rgba[oi + 2] = dval;
            out.position.rgba[oi + 3] = 255;
        }
    }

    free(mask);
    free(dist);
    free(height);
    free(smooth_dist);
    return out;
}

void extracted_maps_free(extracted_maps_t* maps)
{
    if (!maps) return;
    image_free(&maps->diffuse);
    image_free(&maps->normal);
    image_free(&maps->depth);
    image_free(&maps->position);
}
