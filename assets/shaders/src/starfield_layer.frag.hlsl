// starfield_layer.frag.hlsl — procedural virtual-quadtree starfield (single fullscreen draw)
// Drawn fullscreen as a post-process, additive-blended against black. No texture sampling;
// stars are hashed from world-space grid cells so they never flicker during camera movement.
//
// The camera centre is supplied as a HierPos2 split (integer cell + local offset) so world
// coordinates stay precise billions of units from the origin. Several LOD levels are accumulated
// per pixel: each level's cells are `lod_factor` times coarser than the last, and a triangular
// on-screen-size window (centred on `target_px`) keeps the field continuous across the entire
// zoom range (arena AND galaxy map). Coarser levels scroll slower (parallax) for apparent depth.
//
// Uniform layout (b0, space3):
//   params0 : cam_cell_x, cam_cell_y, zoom, fb_width
//   params1 : fb_height, density, size_mul, brightness_mul
//   params2 : seed, cam_local_x, cam_local_y, base_cell
//   params3 : star_rel_x, star_rel_y, dazzle_inner_radius, dazzle_outer_radius
// Uniform layout (b1, space3):
//   params4 : dazzle_intensity, target_px, lod_levels, lod_factor
//   params5 : parallax_near, parallax_falloff, 0, 0

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

cbuffer StarfieldLayerParams : register(b0, space3)
{
    float4 params0; // cam_cell_x, cam_cell_y, zoom, fb_w
    float4 params1; // fb_h, density, size_mul, brightness_mul
    float4 params2; // seed, cam_local_x, cam_local_y, base_cell
    float4 params3; // star_rel_x, star_rel_y, dazzle_inner, dazzle_outer
};

cbuffer StarfieldDazzleParams : register(b1, space3)
{
    float4 params4; // dazzle_intensity, target_px, lod_levels, lod_factor
    float4 params5; // parallax_near, parallax_falloff, 0, 0
};

// Compile-time ceiling for the per-pixel LOD loop; the runtime uses `lod_levels`.
#define BS_MAX_LOD    6
// Cells per axis in each LOD level's repeating tile.
#define BS_TILE_CELLS 64.0

// Fast hash (scalar variant).
float hash1(float n)
{
    return frac(sin(n * 127.1) * 43758.5453);
}

float hash2(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float2 hash22(float2 p, float seed)
{
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    float2 h = frac((p3.xx + p3.yz) * p3.zy);
    h.x += seed * 0.6180339887;
    h = frac(h);
    return h;
}

float3 hash_color(float2 p, float seed)
{
    float h = hash2(p + float2(seed, seed * 0.37));
    // Continuous stellar colour temperature: hot blue-white -> neutral -> warm amber.
    // Bias toward white so coloured stars are the minority, like a real field.
    float t = 0.5 + (h - 0.5) * 0.85;
    float3 cool  = float3(0.62, 0.74, 1.00); // hot blue-white
    float3 white = float3(1.00, 0.98, 0.95); // neutral
    float3 warm  = float3(1.00, 0.80, 0.55); // cool amber
    return (t < 0.5) ? lerp(cool, white, saturate(t * 2.0))
                     : lerp(white, warm, saturate((t - 0.5) * 2.0));
}

// Precision-safe reduction of the (parallax-scaled) camera-centre coordinate modulo a LOD tile,
// using the exact integer HierPos cell so precision never degrades far from the origin. Mirrors
// the nebula_layer reduce: the huge integer term is reduced BEFORE multiplying back up.
//   cell  : integer HierPos cell index along this axis
//   local : local offset within the cell (world units)
//   hier  : HierPos cell size in world units (base_cell * 256)
//   tile  : LOD tile period in world units (cell_size * BS_TILE_CELLS)
//   p     : parallax factor (0..1); 1 == world-locked, <1 == distant (scrolls slower)
float reduce_axis(float cell, float local, float hier, float tile, float p)
{
    float M    = tile / hier;              // tile period expressed in HierPos cells
    float B    = p * cell;                 // small magnitude -> stays exact in f32
    float Bmod = B - floor(B / M) * M;     // in [0, M)
    float r    = Bmod * hier + p * local;  // back to world units
    r -= floor(r / tile) * tile;           // final wrap into [0, tile)
    return r;
}

float4 main(PSInput input) : SV_Target0
{
    float2 uv   = input.uv;
    float2 cam_cell = params0.xy;
    float  zoom = params0.z;
    float  fb_w = params0.w;
    float  fb_h = params1.x;
    float  density        = params1.y;
    float  size_mul       = params1.z;
    float  brightness_mul = params1.w;
    // The raw seed can be a large u32 like 0xDEADBEEF. Reduce it to a small
    // fractional value so all hash operations keep float precision.
    float  seed      = frac(params2.x * 0.1031);
    float2 cam_local = params2.yz;
    float  base_cell = max(params2.w, 1.0);

    float  target_px        = max(params4.y, 1.0);
    int    lod_levels       = (int)clamp(params4.z, 1.0, (float)BS_MAX_LOD);
    float  lod_factor       = max(params4.w, 1.0001);
    float  parallax_near    = params5.x;
    float  parallax_falloff = params5.y;

    // HierPos cell size in world units (fixed relationship: base_cell * 256 == HierPos cell).
    float  hier = base_cell * 256.0;

    // Screen-space offset of this pixel from the camera centre, in world units at current zoom.
    float2 pix_off = (uv - 0.5) * float2(fb_w, fb_h) / max(zoom, 1e-4);

    float3 accum   = float3(0.0, 0.0, 0.0);
    float  accum_a = 0.0;

    [loop]
    for (int lvl = 0; lvl < lod_levels; ++lvl)
    {
        float cell_size = base_cell * pow(lod_factor, (float)lvl);
        float parallax  = parallax_near * pow(parallax_falloff, (float)lvl);
        float tile      = cell_size * BS_TILE_CELLS;

        // Triangular LOD window centred where this level's cells cover ~target_px on screen.
        // At any zoom ~2 adjacent levels overlap and their weights sum to ~1 -> continuous field.
        float on_px  = cell_size * zoom;
        float lodpos = log(on_px / target_px) / log(lod_factor);
        float weight = saturate(1.0 - abs(lodpos));
        if (weight <= 0.0)
            continue;

        // Precision-safe wrapped camera centre for this level, then add the pixel offset.
        float2 camw = float2(
            reduce_axis(cam_cell.x, cam_local.x, hier, tile, parallax),
            reduce_axis(cam_cell.y, cam_local.y, hier, tile, parallax));
        float2 wrap_pos = camw + pix_off;
        wrap_pos -= floor(wrap_pos / tile) * tile; // into [0, tile)

        int2 cell       = int2(floor(wrap_pos / cell_size));
        int  tile_cells = (int)BS_TILE_CELLS;

        // Per-level seed so each LOD draws a distinct star set.
        float lseed = seed + (float)lvl * 0.17;

        float3 laccum   = float3(0.0, 0.0, 0.0);
        float  laccum_a = 0.0;

        // Search neighbouring cells for nearby stars.
        [loop]
        for (int dy = -2; dy <= 2; ++dy)
        {
            [loop]
            for (int dx = -2; dx <= 2; ++dx)
            {
                int2 neighbor = cell + int2(dx, dy);
                // Wrap cell indices for tiling.
                int2 wrapped_cell = int2(
                    (neighbor.x % tile_cells + tile_cells) % tile_cells,
                    (neighbor.y % tile_cells + tile_cells) % tile_cells);
                float2 cell_id = float2(wrapped_cell);

                // Hash cell to determine if it contains a star.
                float presence = hash2(cell_id + float2(lseed * 100.0, lseed * 200.0));
                if (presence > density)
                    continue;

                // Hash star offset within cell, in world units.
                float2 star_offset = hash22(cell_id, lseed);
                float2 star_pos    = (cell_id + star_offset) * cell_size;

                // Handle wrapping for neighbours across the tile boundary.
                float2 delta = star_pos - wrap_pos;
                if (delta.x >  tile * 0.5) delta.x -= tile;
                if (delta.x < -tile * 0.5) delta.x += tile;
                if (delta.y >  tile * 0.5) delta.y -= tile;
                if (delta.y < -tile * 0.5) delta.y += tile;

                // Star size scales with the level's cell size so on-screen dots stay consistent.
                float size_scale = cell_size / base_cell;
                float star_size  = (16.0 + hash1(cell_id.x * 31.0 + cell_id.y * 57.0 + lseed) * 24.0)
                                   * size_scale * size_mul;

                float dist_sq = dot(delta, delta);
                float reach   = star_size * 4.0;
                if (dist_sq > reach * reach)
                    continue;

                // Brightness: heavily skewed — most stars very dim, a few bright standouts.
                float b_hash     = hash1(cell_id.x * 13.0 + cell_id.y * 97.0 + lseed * 0.5);
                float brightness = (0.04 + pow(b_hash, 3.0) * 0.55) * brightness_mul;

                // Two-lobe profile: a crisp bright core plus a soft surrounding halo so each
                // star reads as a real point of light instead of a flat blob.
                float r2    = dist_sq / (star_size * star_size);
                float core  = exp(-r2 * 4.0);
                float halo  = exp(-r2 * 0.6);
                float shape = core + 0.4 * halo;

                // Diffraction sparkle on the rare brightest stars: a thin 4-point cross.
                if (b_hash > 0.85)
                {
                    float spike_len = star_size * 3.0;
                    float spike_w   = star_size * 0.10;
                    float sh = exp(-(delta.y * delta.y) / (spike_w * spike_w)) * exp(-abs(delta.x) / spike_len);
                    float sv = exp(-(delta.x * delta.x) / (spike_w * spike_w)) * exp(-abs(delta.y) / spike_len);
                    shape += (sh + sv) * 0.6 * smoothstep(0.85, 0.98, b_hash);
                }

                float  intensity = shape * brightness;
                float3 col       = hash_color(cell_id, lseed);
                laccum   += col * intensity;
                laccum_a += intensity;
            }
        }

        accum   += laccum   * weight;
        accum_a += laccum_a * weight;
    }

    // Clamp and gamma-correct slightly.
    accum   = saturate(accum);
    accum_a = saturate(accum_a);

    // Star dazzle: suppress faint stars near the bright central star. `star_rel` is the hero
    // star's render-space offset from the camera centre — the same frame as `pix_off`.
    float2 star_rel         = params3.xy;
    float  dazzle_inner     = params3.z;
    float  dazzle_outer     = params3.w;
    float  dazzle_intensity = params4.x;
    if (dazzle_outer > dazzle_inner && dazzle_intensity > 0.0)
    {
        float d = distance(pix_off, star_rel);
        float f = 1.0 - smoothstep(dazzle_inner, dazzle_outer, d); // 1 near star, 0 far away
        float suppress = 1.0 - dazzle_intensity * f;
        accum   *= suppress;
        accum_a *= suppress;
    }

    return float4(accum * 0.8, accum_a);
}
