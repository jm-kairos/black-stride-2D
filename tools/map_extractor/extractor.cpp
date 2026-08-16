#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"

#include "extractor.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

extract_params_t extract_params_default(void)
{
    extract_params_t p = {};
    p.alpha_threshold  = 0.05f;
    p.normal_strength  = 1.0f;
    p.depth_contrast   = 1.0f;
    p.normal_algo      = NORMAL_ALGORITHM_SDF_TRUE_EDT;
    p.bevel_px         = 0.0f;  // auto: derived from the art size
    p.dome_amount      = 0.35f;
    p.detail_amp_px    = 2.5f;
    p.detail_radius_px = 10.0f;
    p.ao_strength      = 0.35f;
    p.profile_amp      = 1.0f;
    p.side_nose_dir    = 0;
    return p;
}

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
// Returns the unsigned distance-to-boundary for every pixel.
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

// One horizontal box-blur pass using per-row prefix sums (edge-truncated window).
static void box_blur_h(float* dst, const float* src, int w, int h, int r, float* prefix)
{
    for (int y = 0; y < h; ++y)
    {
        const float* row = &src[y * w];
        prefix[0] = 0.0f;
        for (int x = 0; x < w; ++x)
        {
            prefix[x + 1] = prefix[x] + row[x];
        }
        for (int x = 0; x < w; ++x)
        {
            int lo = x - r; if (lo < 0) lo = 0;
            int hi = x + r; if (hi > w - 1) hi = w - 1;
            dst[y * w + x] = (prefix[hi + 1] - prefix[lo]) / (float)(hi - lo + 1);
        }
    }
}

static void box_blur_v(float* dst, const float* src, int w, int h, int r, float* prefix)
{
    for (int x = 0; x < w; ++x)
    {
        prefix[0] = 0.0f;
        for (int y = 0; y < h; ++y)
        {
            prefix[y + 1] = prefix[y] + src[y * w + x];
        }
        for (int y = 0; y < h; ++y)
        {
            int lo = y - r; if (lo < 0) lo = 0;
            int hi = y + r; if (hi > h - 1) hi = h - 1;
            dst[y * w + x] = (prefix[hi + 1] - prefix[lo]) / (float)(hi - lo + 1);
        }
    }
}

// Gaussian-approximating blur: three iterated box blurs, any radius, O(n) per pass.
// Blurs `data` in place; `tmp` is caller-provided scratch of the same size.
static void blur_approx(float* data, float* tmp, int w, int h, float radius)
{
    if (radius < 0.5f || w < 2 || h < 2) return;
    int r = (int)(radius * 0.5f + 0.5f);
    if (r < 1) r = 1;
    int max_n = (w > h ? w : h) + 1;
    float* prefix = (float*)malloc((size_t)max_n * sizeof(float));
    if (!prefix) return;
    for (int i = 0; i < 3; ++i)
    {
        box_blur_h(tmp, data, w, h, r, prefix);
        box_blur_v(data, tmp, w, h, r, prefix);
    }
    free(prefix);
}

// Flood the hull's rim colors outward so bilinear sampling never pulls black into
// the silhouette. Alpha stays untouched (transparent); only RGB is padded.
static void dilate_diffuse_rgb(unsigned char* rgba, const unsigned char* mask, int w, int h, int passes)
{
    int n = w * h;
    unsigned char* filled = (unsigned char*)malloc((size_t)n);
    unsigned char* next = (unsigned char*)malloc((size_t)n);
    if (!filled || !next)
    {
        free(filled);
        free(next);
        return;
    }
    memcpy(filled, mask, (size_t)n);

    for (int pass = 0; pass < passes; ++pass)
    {
        memcpy(next, filled, (size_t)n);
        int any = 0;
        for (int y = 0; y < h; ++y)
        {
            for (int x = 0; x < w; ++x)
            {
                int i = y * w + x;
                if (filled[i]) continue;
                int sr = 0, sg = 0, sb = 0, cnt = 0;
                for (int dy = -1; dy <= 1; ++dy)
                {
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        if (!dx && !dy) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        int j = ny * w + nx;
                        if (!filled[j]) continue;
                        sr += rgba[j * 4 + 0];
                        sg += rgba[j * 4 + 1];
                        sb += rgba[j * 4 + 2];
                        cnt++;
                    }
                }
                if (cnt > 0)
                {
                    rgba[i * 4 + 0] = (unsigned char)(sr / cnt);
                    rgba[i * 4 + 1] = (unsigned char)(sg / cnt);
                    rgba[i * 4 + 2] = (unsigned char)(sb / cnt);
                    next[i] = 1;
                    any = 1;
                }
            }
        }
        memcpy(filled, next, (size_t)n);
        if (!any) break;
    }

    free(filled);
    free(next);
}

// Pearson correlation of two same-length series.
static float correlate(const float* a, const float* b, int n)
{
    float ma = 0.0f, mb = 0.0f;
    for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= (float)n; mb /= (float)n;
    float num = 0.0f, da = 0.0f, db = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        num += (a[i] - ma) * (b[i] - mb);
        da += (a[i] - ma) * (a[i] - ma);
        db += (b[i] - mb) * (b[i] - mb);
    }
    float den = sqrtf(da * db);
    return den > 1e-6f ? num / den : 0.0f;
}

// Dorsal elevation profile from a side-view image: for each station along the ship's
// length, how far the top silhouette rises above the lowest deck line (command towers,
// raised hubs). Returns a per-top-down-row elevation array in TOP-DOWN pixel units,
// aligned to the top-down mask's row bounding box [by0..by1], or NULL if unusable.
static float* build_side_profile(image_t side, float alpha_threshold, int nose_dir,
                                 const unsigned char* td_mask, int td_w, int td_h,
                                 int by0, int by1)
{
    if (!side.rgba || side.w < 8 || side.h < 2 || by1 <= by0) return nullptr;
    int sw = side.w, sh = side.h;

    // Top/bottom silhouette per column.
    int* top_y = (int*)malloc((size_t)sw * sizeof(int));
    int* bot_y = (int*)malloc((size_t)sw * sizeof(int));
    if (!top_y || !bot_y) { free(top_y); free(bot_y); return nullptr; }
    int sx0 = -1, sx1 = -1;
    for (int x = 0; x < sw; ++x)
    {
        top_y[x] = -1; bot_y[x] = -1;
        for (int y = 0; y < sh; ++y)
        {
            if (b2f(side.rgba[(y * sw + x) * 4 + 3]) > alpha_threshold) { top_y[x] = y; break; }
        }
        for (int y = sh - 1; y >= 0; --y)
        {
            if (b2f(side.rgba[(y * sw + x) * 4 + 3]) > alpha_threshold) { bot_y[x] = y; break; }
        }
        if (top_y[x] >= 0)
        {
            if (sx0 < 0) sx0 = x;
            sx1 = x;
        }
    }
    if (sx0 < 0 || sx1 - sx0 < 8) { free(top_y); free(bot_y); return nullptr; }

    // Bridge gaps (separated pods) by linear interpolation between valid neighbors.
    for (int x = sx0; x <= sx1; ++x)
    {
        if (top_y[x] >= 0) continue;
        int l = x - 1;
        int r = x + 1;
        while (r <= sx1 && top_y[r] < 0) r++;
        if (l >= sx0 && r <= sx1)
        {
            float t = (float)(x - l) / (float)(r - l);
            top_y[x] = (int)(top_y[l] + t * (top_y[r] - top_y[l]) + 0.5f);
            bot_y[x] = (int)(bot_y[l] + t * (bot_y[r] - bot_y[l]) + 0.5f);
        }
    }

    // Elevation above the lowest top-deck line, smoothed to kill AA noise.
    int span = sx1 - sx0 + 1;
    float* elev = (float*)malloc((size_t)span * sizeof(float));
    float* elev_tmp = (float*)malloc((size_t)span * sizeof(float));
    if (!elev || !elev_tmp) { free(top_y); free(bot_y); free(elev); free(elev_tmp); return nullptr; }
    int baseline = 0;
    for (int x = sx0; x <= sx1; ++x)
    {
        if (top_y[x] > baseline) baseline = top_y[x];
    }
    for (int x = sx0; x <= sx1; ++x)
    {
        elev[x - sx0] = (float)(baseline - top_y[x]);
    }
    {
        int r = span / 256; if (r < 2) r = 2;
        for (int it = 0; it < 3; ++it)
        {
            for (int i = 0; i < span; ++i)
            {
                int lo = i - r; if (lo < 0) lo = 0;
                int hi = i + r; if (hi > span - 1) hi = span - 1;
                float sum = 0.0f;
                for (int j = lo; j <= hi; ++j) sum += elev[j];
                elev_tmp[i] = sum / (float)(hi - lo + 1);
            }
            memcpy(elev, elev_tmp, (size_t)span * sizeof(float));
        }
    }

    // Nose direction: auto-detect by correlating the side thickness profile against the
    // top-down width profile (a tapered bow is thin in both views).
    bool nose_right;
    if (nose_dir > 0) nose_right = true;
    else if (nose_dir < 0) nose_right = false;
    else
    {
        const int N = 64;
        float tw[N], svf[N], svr[N];
        for (int i = 0; i < N; ++i)
        {
            int y = by0 + (int)((float)i * (by1 - by0) / (N - 1) + 0.5f);
            int cnt = 0;
            for (int x = 0; x < td_w; ++x)
            {
                if (td_mask[y * td_w + x]) cnt++;
            }
            tw[i] = (float)cnt;

            int sx = sx0 + (int)((float)i * (sx1 - sx0) / (N - 1) + 0.5f);
            float th = (top_y[sx] >= 0) ? (float)(bot_y[sx] - top_y[sx]) : 0.0f;
            svf[i] = th;                 // nose at side-left
            svr[N - 1 - i] = th;         // nose at side-right
        }
        nose_right = correlate(tw, svr, N) > correlate(tw, svf, N);
    }

    // Resample to one elevation per top-down row, converting side px -> top-down px by
    // the silhouette length ratio.
    int rows = by1 - by0 + 1;
    float scale = (float)rows / (float)span;
    float* profile = (float*)malloc((size_t)td_h * sizeof(float));
    if (!profile) { free(top_y); free(bot_y); free(elev); free(elev_tmp); return nullptr; }
    memset(profile, 0, (size_t)td_h * sizeof(float));
    float max_elev = 0.0f;
    int max_row = by0;
    for (int y = by0; y <= by1; ++y)
    {
        float t = (float)(y - by0) / (float)(rows - 1);
        float fx = nose_right ? ((float)span - 1.0f) * (1.0f - t) : ((float)span - 1.0f) * t;
        int i0 = (int)fx;
        int i1 = i0 + 1 < span ? i0 + 1 : i0;
        float f = fx - (float)i0;
        float e = (elev[i0] * (1.0f - f) + elev[i1] * f) * scale;
        profile[y] = e;
        if (e > max_elev) { max_elev = e; max_row = y; }
    }

    printf("side profile: nose %s, side span %d px, scale %.2f, max elevation %.0f td-px at %.0f%% from nose\n",
           nose_right ? "right" : "left", span, scale, max_elev,
           100.0f * (float)(max_row - by0) / (float)(rows - 1));

    free(top_y);
    free(bot_y);
    free(elev);
    free(elev_tmp);
    return profile;
}

extracted_maps_t extract_maps(image_t input, image_t side_view, const extract_params_t* params)
{
    extracted_maps_t out = {};
    if (!input.rgba || input.w <= 0 || input.h <= 0)
    {
        return out;
    }

    extract_params_t p = params ? *params : extract_params_default();

    int w = input.w;
    int h = input.h;
    int n = w * h;
    int min_dim = w < h ? w : h;

    unsigned char* mask = (unsigned char*)malloc((size_t)n);
    if (!mask)
    {
        return out;
    }

    for (int i = 0; i < n; ++i)
    {
        float a = b2f(input.rgba[i * 4 + 3]);
        mask[i] = (a > p.alpha_threshold) ? 255 : 0;
    }

    // Signed distance field: positive inside the hull, negative outside. The chamfer
    // variant returns an unsigned distance for every pixel, so sign it here; the true
    // EDT already carries the sign.
    float* sdf = nullptr;
    if (p.normal_algo == NORMAL_ALGORITHM_SDF_CHAMFER)
    {
        sdf = compute_edge_distance_chamfer(mask, w, h);
        if (sdf)
        {
            for (int i = 0; i < n; ++i)
            {
                if (!mask[i]) sdf[i] = -sdf[i];
            }
        }
    }
    else
    {
        sdf = compute_edge_distance_true_edt(mask, w, h);
    }

    if (!sdf)
    {
        free(mask);
        return out;
    }

    float max_inside = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        if (mask[i] && sdf[i] > max_inside) max_inside = sdf[i];
    }
    if (max_inside < 1.0f) max_inside = 1.0f;

    // Row bounding box of the hull in the top-down art (nose..stern span).
    int by0 = -1, by1 = -1;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            if (mask[y * w + x]) { if (by0 < 0) by0 = y; by1 = y; break; }
        }
    }

    float bevel = (p.bevel_px > 0.0f) ? p.bevel_px : clampf((float)min_dim * 0.06f, 4.0f, 48.0f);
    if (bevel > max_inside) bevel = max_inside;

    float* height = (float*)malloc((size_t)n * sizeof(float)); // in pixel units
    float* tmp = (float*)malloc((size_t)n * sizeof(float));
    if (!height || !tmp)
    {
        free(mask);
        free(sdf);
        free(height);
        free(tmp);
        return out;
    }

    // ---- Base height: how the hull rises from the silhouette ----
    if (p.normal_algo == NORMAL_ALGORITHM_ALPHA_GRADIENT)
    {
        for (int i = 0; i < n; ++i)
        {
            height[i] = b2f(input.rgba[i * 4 + 3]) * bevel;
        }
        blur_approx(height, tmp, w, h, 1.5f);
    }
    else if (p.normal_algo == NORMAL_ALGORITHM_GAUSSIAN_HEIGHT)
    {
        for (int i = 0; i < n; ++i)
        {
            height[i] = b2f(input.rgba[i * 4 + 3]) * bevel;
        }
        blur_approx(height, tmp, w, h, bevel);
    }
    else
    {
        // Rounded bevel: a quarter-ellipse profile over `bevel` pixels from the rim,
        // then a flat top with an optional gentle dome toward the hull spine. This
        // reads as plated hull instead of the old edge-to-spine cone.
        for (int i = 0; i < n; ++i)
        {
            float d = sdf[i];
            float t = clampf(d / bevel, 0.0f, 1.0f);
            float rim = 1.0f - t;
            float prof = sqrtf(clampf(1.0f - rim * rim, 0.0f, 1.0f));
            prof = powf(prof, p.depth_contrast);
            float dome = 0.0f;
            if (d > 0.0f)
            {
                float dt = clampf(d / max_inside, 0.0f, 1.0f);
                dome = p.dome_amount * dt * (2.0f - dt); // ease-out rise to the spine
            }
            height[i] = (prof + dome) * bevel;
        }
        // Soften the crease where the bevel meets the top so the shoulder highlight
        // rolls off instead of snapping.
        blur_approx(height, tmp, w, h, bevel * 0.3f);
    }

    // ---- Side-view dorsal elevation: real Z data from the hull's side silhouette ----
    // Raised superstructures (command hub, bridge tower) lift the height field at their
    // stations along the length. The side view is a 1D curve, so shape it laterally as a
    // spine-centered ridge: full elevation over the central ~45% of the local beam,
    // walls falling to zero toward the rims. Without the lateral shaping the profile has
    // only along-length slopes and vanishes under abeam (side-on) star light; the ridge
    // walls give it left/right-facing normals that read from every light direction. It
    // also keeps wide outboard structures (wing pods) at deck level.
    if (p.profile_amp > 0.001f && by0 >= 0)
    {
        float* profile = build_side_profile(side_view, p.alpha_threshold, p.side_nose_dir,
                                            mask, w, h, by0, by1);
        if (profile)
        {
            // Per-row beam center/half-width, smoothed along the length so the ridge
            // doesn't develop seams where the beam steps (wing roots).
            float* row_cx = (float*)malloc((size_t)h * sizeof(float));
            float* row_hw = (float*)malloc((size_t)h * sizeof(float));
            float* row_tmp = (float*)malloc((size_t)h * sizeof(float));
            if (row_cx && row_hw && row_tmp)
            {
                for (int y = 0; y < h; ++y)
                {
                    int rx0 = -1, rx1 = -1;
                    for (int x = 0; x < w; ++x)
                    {
                        if (mask[y * w + x]) { if (rx0 < 0) rx0 = x; rx1 = x; }
                    }
                    if (rx0 < 0 && y > 0) { row_cx[y] = row_cx[y - 1]; row_hw[y] = row_hw[y - 1]; }
                    else if (rx0 < 0)     { row_cx[y] = 0.5f * (float)w; row_hw[y] = 1.0f; }
                    else
                    {
                        row_cx[y] = 0.5f * (float)(rx0 + rx1);
                        row_hw[y] = 0.5f * (float)(rx1 - rx0);
                        if (row_hw[y] < 1.0f) row_hw[y] = 1.0f;
                    }
                }
                const int r = 6;
                for (int it = 0; it < 3; ++it)
                {
                    for (int pass = 0; pass < 2; ++pass)
                    {
                        float* src = pass ? row_hw : row_cx;
                        for (int y = 0; y < h; ++y)
                        {
                            int lo = y - r < 0 ? 0 : y - r;
                            int hi = y + r > h - 1 ? h - 1 : y + r;
                            float sum = 0.0f;
                            for (int j = lo; j <= hi; ++j) sum += src[j];
                            row_tmp[y] = sum / (float)(hi - lo + 1);
                        }
                        memcpy(src, row_tmp, (size_t)h * sizeof(float));
                    }
                }

                for (int y = 0; y < h; ++y)
                {
                    float e = profile[y] * p.profile_amp;
                    if (e <= 0.0f) continue;
                    for (int x = 0; x < w; ++x)
                    {
                        int i = y * w + x;
                        if (!mask[i]) continue;
                        float centrality = 1.0f - fabsf((float)x - row_cx[y]) / row_hw[y];
                        // Vaulted cross-section: quarter-ellipse from rim to crest. A flat
                        // plateau would give the tallest structure the same normal as the
                        // deck (both light as (0,0,1)); keeping curvature at the crest lets
                        // its light-facing half brighten and its far half shade, so the
                        // highest zone actually reads as highest under any star bearing.
                        float rim = 1.0f - clampf(centrality / 0.9f, 0.0f, 1.0f);
                        float ridge = sqrtf(clampf(1.0f - rim * rim, 0.0f, 1.0f));
                        float fade = clampf(sdf[i] / (bevel * 0.6f), 0.0f, 1.0f);
                        height[i] += e * ridge * fade;
                    }
                }
            }
            free(row_cx);
            free(row_hw);
            free(row_tmp);
            free(profile);
        }
    }

    // ---- Surface detail: recover painted panel relief from luminance ----
    // Band-pass the mask-weighted luminance: features smaller than detail_radius_px
    // survive; broad painted lighting (which would double-light in the shader) is
    // rejected. The same band-pass drives the cavity AO bake below.
    float* bandpass = nullptr;
    if (p.detail_amp_px > 0.001f || p.ao_strength > 0.001f)
    {
        float* lum_f = (float*)malloc((size_t)n * sizeof(float));
        float* wgt_f = (float*)malloc((size_t)n * sizeof(float));
        float* lum_c = (float*)malloc((size_t)n * sizeof(float));
        float* wgt_c = (float*)malloc((size_t)n * sizeof(float));
        bandpass = (float*)malloc((size_t)n * sizeof(float));
        if (lum_f && wgt_f && lum_c && wgt_c && bandpass)
        {
            for (int i = 0; i < n; ++i)
            {
                float l = 0.2126f * b2f(input.rgba[i * 4 + 0]) +
                          0.7152f * b2f(input.rgba[i * 4 + 1]) +
                          0.0722f * b2f(input.rgba[i * 4 + 2]);
                float m = mask[i] ? 1.0f : 0.0f;
                lum_f[i] = l * m;
                wgt_f[i] = m;
            }
            memcpy(lum_c, lum_f, (size_t)n * sizeof(float));
            memcpy(wgt_c, wgt_f, (size_t)n * sizeof(float));

            blur_approx(lum_f, tmp, w, h, 1.2f);
            blur_approx(wgt_f, tmp, w, h, 1.2f);
            blur_approx(lum_c, tmp, w, h, p.detail_radius_px);
            blur_approx(wgt_c, tmp, w, h, p.detail_radius_px);

            for (int i = 0; i < n; ++i)
            {
                if (!mask[i]) { bandpass[i] = 0.0f; continue; }
                float fine = lum_f[i] / (wgt_f[i] > 1e-4f ? wgt_f[i] : 1e-4f);
                float coarse = lum_c[i] / (wgt_c[i] > 1e-4f ? wgt_c[i] : 1e-4f);
                bandpass[i] = fine - coarse;
            }

            if (p.detail_amp_px > 0.001f)
            {
                for (int i = 0; i < n; ++i)
                {
                    if (!mask[i]) continue;
                    // Fade the detail out across the bevel so the silhouette stays clean.
                    float fade = clampf(sdf[i] / (bevel * 0.6f), 0.0f, 1.0f);
                    height[i] += bandpass[i] * p.detail_amp_px * fade;
                }
            }
        }
        else
        {
            free(bandpass);
            bandpass = nullptr;
        }
        free(lum_f);
        free(wgt_f);
        free(lum_c);
        free(wgt_c);
    }

    out.diffuse = image_alloc(w, h);
    out.normal = image_alloc(w, h);
    out.depth = image_alloc(w, h);
    out.position = image_alloc(w, h);

    if (!out.diffuse.rgba || !out.normal.rgba || !out.depth.rgba || !out.position.rgba)
    {
        free(mask);
        free(sdf);
        free(height);
        free(tmp);
        free(bandpass);
        extracted_maps_free(&out);
        return out;
    }

    float max_height = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        if (mask[i] && height[i] > max_height) max_height = height[i];
    }
    if (max_height < 1e-3f) max_height = 1e-3f;

    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            int i = y * w + x;
            int oi = i * 4;
            unsigned char m = mask[i];

            float h01 = clampf(height[i] / max_height, 0.0f, 1.0f);

            // Diffuse: original color, original (antialiased) alpha inside the hull.
            // Cavity AO: recessed painted detail reads as occluded panel lines.
            // Altitude gain: sky-visibility shading — raised structures (command hub)
            // catch more light than low decks, so elevation reads at any star bearing.
            float ao = 1.0f;
            if (m && bandpass && p.ao_strength > 0.001f)
            {
                float fade = clampf(sdf[i] / (bevel * 0.6f), 0.0f, 1.0f);
                float cav = clampf(-bandpass[i] * 6.0f, 0.0f, 1.0f) * fade;
                ao = 1.0f - p.ao_strength * cav;
            }
            float alt = 0.85f + 0.30f * h01;
            out.diffuse.rgba[oi + 0] = m ? f2b(b2f(input.rgba[oi + 0]) * ao * alt) : 0;
            out.diffuse.rgba[oi + 1] = m ? f2b(b2f(input.rgba[oi + 1]) * ao * alt) : 0;
            out.diffuse.rgba[oi + 2] = m ? f2b(b2f(input.rgba[oi + 2]) * ao * alt) : 0;
            out.diffuse.rgba[oi + 3] = m ? input.rgba[oi + 3] : 0;

            // Normal: analytic gradient of the height field. Texture row 0 maps to
            // ship-local +Y in the engine's mapped quad, so the G channel gets
            // +dH/drow (n = (-dH/dx, -dH/dy_local, 1), y_local = -row).
            float hl = (x > 0)     ? height[i - 1] : height[i];
            float hr = (x < w - 1) ? height[i + 1] : height[i];
            float hu = (y > 0)     ? height[i - w] : height[i];
            float hd = (y < h - 1) ? height[i + w] : height[i];
            float dx = (hr - hl) * 0.5f;
            float dyrow = (hd - hu) * 0.5f;
            float nx = -dx * p.normal_strength;
            float ny = dyrow * p.normal_strength;
            float nz = 1.0f;
            float len = sqrtf(nx * nx + ny * ny + nz * nz);
            nx /= len; ny /= len; nz /= len;
            out.normal.rgba[oi + 0] = f2b((nx + 1.0f) * 0.5f);
            out.normal.rgba[oi + 1] = f2b((ny + 1.0f) * 0.5f);
            out.normal.rgba[oi + 2] = f2b((nz + 1.0f) * 0.5f);
            out.normal.rgba[oi + 3] = 255;

            // Depth: the same height field, normalized, then compressed to 0.25..0.75.
            // The shader offsets UVs by (depth - 0.5) * 0.02 in normalized UV space, so
            // on tall art the full 0..1 range smears texels ~30 px along the light axis;
            // half range keeps the 2.5D parallax pop subtle while the normal map still
            // carries the full geometric slopes for lighting.
            unsigned char dval = m ? f2b(0.25f + 0.5f * h01) : 0;
            out.depth.rgba[oi + 0] = dval;
            out.depth.rgba[oi + 1] = dval;
            out.depth.rgba[oi + 2] = dval;
            out.depth.rgba[oi + 3] = 255;

            // Position: local UV + depth.
            float px = (w > 1) ? (float)x / (float)(w - 1) : 0.5f;
            float py = (h > 1) ? (float)y / (float)(h - 1) : 0.5f;
            out.position.rgba[oi + 0] = f2b(px);
            out.position.rgba[oi + 1] = f2b(py);
            out.position.rgba[oi + 2] = dval;
            out.position.rgba[oi + 3] = 255;
        }
    }

    // Pad rim colors outward so linear filtering never blends black into the edge.
    dilate_diffuse_rgb(out.diffuse.rgba, mask, w, h, 12);

    free(mask);
    free(sdf);
    free(height);
    free(tmp);
    free(bandpass);
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
