// nebula_layer.frag.hlsl — procedural alpha-blended nebula/dust cloud layer
// Shadertoy-inspired: cosine palette, domain rotation/warping, radial/band falloff,
// and FBM with lacunarity 2.2. Keeps editor colors and exposes palette-shift,
// swirl, falloff, and band controls.

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

cbuffer NebulaLayerParams : register(b0, space3)
{
    float4 params0;       // cam_cell_x, cam_cell_y, zoom, fb_w
    float4 params1;       // fb_h, intensity, dust_intensity, seed
    float4 gas_color_a;   // deep base gas (rgb, unused)
    float4 gas_color_b;   // mid gas (rgb, unused)
    float4 gas_color_c;   // bright highlight core (rgb, unused)
    float4 dust_color_in; // dark dust silhouette (rgb, unused)
    float4 tunables;      // gas_brightness_mul, highlight_power, palette_shift, swirl_strength
    float4 falloff;       // falloff_radius, band_strength, cam_local_x, cam_local_y
    float4 biome0;        // biome_strength, biome_scale, biome_hue_spread, zoom_detail
    float4 biome1;        // zoom_saturation, unused, unused, unused
};

// HierPos2 cell size (world units per integer cell). Matches BS_HIERPOS_CELL_SIZE.
static const float HIER_CELL = 16384.0;

// Reduce one axis of the camera-center world coordinate modulo the LOD tiling period, using the
// exact integer cell so precision never degrades billions of units from the origin.
//   p : parallax factor (0..1); 1 == world-locked, <1 == distant (scrolls slower). p==1 is
//       bit-identical to the original integer path.
float reduce_axis(float cell, float local, float m, float p)
{
    float B      = p * cell;
    float Bmod   = B - floor(B / m) * m;
    float period = m * HIER_CELL;
    float r = Bmod * HIER_CELL + p * local;
    r -= floor(r / period) * period;
    return r;
}

float hash2(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// Periodic value noise tiling at integer period P (per axis). Wrapping the integer lattice makes
// the field seamless across the reduce_axis tiling boundary: when the camera center crosses a
// period (as zoom-to-cursor pans it) the sample coordinate wraps by exactly one tile and the
// nebula is identical across the seam — so it never pops in/out.
float pvalue(float2 x, float2 P)
{
    float2 i = floor(x);
    float2 f = frac(x);
    f = f * f * (3.0 - 2.0 * f);
    float2 i0 = i - floor(i / P) * P;   // wrap corner (i)   into [0,P)
    float2 i1 = i0 + 1.0;
    i1 = i1 - floor(i1 / P) * P;        // wrap corner (i+1)  into [0,P) across the seam
    float a = hash2(float2(i0.x, i0.y));
    float b = hash2(float2(i1.x, i0.y));
    float c = hash2(float2(i0.x, i1.y));
    float d = hash2(float2(i1.x, i1.y));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

// Tileable fBM (lacunarity 2). `period` is the wrap period of `x` and doubles every octave. There
// is intentionally NO per-octave rotation (a rotation does not map the integer lattice onto itself
// and would break tiling); the domain warp in nebula_level supplies the organic, non-gridded look.
float pfbm(float2 x, float2 period, int octaves)
{
    float v = 0.0;
    float a = 0.75;
    float amp = 0.0;
    float2 pp = x;
    float2 per = period;
    for (int i = 0; i < octaves; ++i)
    {
        v   += a * pvalue(pp, per);
        amp += a;
        pp  *= 2.0;
        per *= 2.0;
        a   *= 0.5;
    }
    return v / amp;
}

// Cosine palette used in the reference: a + b * cos(2pi * (c*x + d)).
float cosine_palette(float a, float b, float c, float d, float x)
{
    return a + b * cos(6.28318 * (c * x + d));
}

// Rotate an RGB colour's hue by `a` radians about the luma diagonal (Rodrigues rotation). Roughly
// luminance-preserving, so biome hue shifts recolour a region without changing its brightness.
float3 hue_rotate(float3 c, float a)
{
    const float3 k = float3(0.57735027, 0.57735027, 0.57735027);
    float ca = cos(a);
    return c * ca + cross(k, c) * sin(a) + k * dot(k, c) * (1.0 - ca);
}

// Tileable ridged noise for sharp, billowy tendrils.
float pridge(float2 x, float2 period, int octaves)
{
    float v = 0.0;
    float a = 0.5;
    float2 pp = x;
    float2 per = period;
    for (int i = 0; i < octaves; ++i)
    {
        float n = pvalue(pp, per);
        v   += a * (1.0 - abs(n - 0.5) * 2.0);
        pp  *= 2.0;
        per *= 2.0;
        a   *= 0.5;
    }
    return v;
}

// Evaluate the full nebula at one world-anchored noise coordinate `p` (in noise units). Returns
// straight (non-premultiplied) colour in .rgb and coverage/opacity in .a. main() calls this once
// per LOD level and cross-fades the results, so the pattern is FIXED in world space at each scale
// (only the blend weights depend on zoom) — the camera flies past clouds instead of remapping them.
//
// Structure: a low-frequency REGION field decides where nebulae live (big localized clouds with
// empty gaps) and drives a coherent per-region hue; a warped high-frequency DETAIL field gives the
// wispy filaments inside those regions. `tile` is the wrap period of `p` (== NEB_SPAN); every noise
// call below tiles at that period so the field is seamless across the reduce_axis boundary.
float4 nebula_level(float2 p, float tile, float4 biome, float zoom_close)
{
    float intensity          = params1.y;
    float dust_intensity     = params1.z;
    float gas_brightness_mul = tunables.x;
    float highlight_power    = tunables.y;
    float palette_shift      = tunables.z;
    float warp_strength      = tunables.w;   // "Warp strength": domain-warp amount
    float region_density     = falloff.x;    // "Region density": 0..1 how much of space holds gas
    float filament_sharpness = falloff.y;    // "Filament sharpness": 0..1 bright-filament contrast

    // Galaxy-wide biome modulation. biome = (hue, density, brightness, dust) low-freq macro fields
    // (0..1); biome_strength scales the whole effect (0 == uniform, original look).
    float biome_strength = saturate(biome0.x);
    float hue_spread     = biome0.z;
    float zoom_detail    = saturate(biome0.w);
    float zoom_sat       = saturate(biome1.x);
    float b_hue    = biome.x;
    float b_dense  = biome.y;
    float b_bright = biome.z;
    float b_dust   = biome.w;

    // Density: some regions become dense clouds, others sparse voids.
    region_density = saturate(region_density * lerp(1.0, 0.3 + b_dense * 1.6, biome_strength));
    // Brightness: gas luminance varies by region.
    gas_brightness_mul *= lerp(1.0, 0.55 + b_bright * 0.9, biome_strength);
    // Dust-vs-gas balance: dusty regions vs glowing-gas regions (-1 gassy .. +1 dusty).
    float dust_bias = (b_dust - 0.5) * 2.0 * biome_strength;
    float gas_gain  = saturate(1.0 - 0.5 * dust_bias);
    float dust_gain = saturate(1.0 + 0.9 * dust_bias);

    // Feature "scale"/character: a decorrelated combo of the biome fields varies structure turbulence
    // (warp) and filament fineness per region. This only touches the warp OFFSET and a pow() exponent,
    // NOT any tiling frequency, so it stays seamless (recommendation A). Centred so strength 0 == 1.0.
    float b_feat = frac((b_hue + b_dust) * 1.37 + b_bright * 0.53);
    warp_strength      *= 1.0 + (b_feat - 0.5) * 0.8 * biome_strength;
    filament_sharpness  = saturate(filament_sharpness * (1.0 + (b_feat - 0.5) * 0.6 * biome_strength));

    float3 gas_a = gas_color_a.rgb;
    float3 gas_b = gas_color_b.rgb;
    float3 gas_c = gas_color_c.rgb;
    float3 dust_color = dust_color_in.rgb;

    // Wrap periods. p tiles at `tile`; detail space (p*DETAIL) tiles at tile*DETAIL. DETAIL is an
    // integer so every derived period stays integral and the lattice wrap is exact.
    const float DETAIL = 22.0;
    float2 P0 = float2(tile, tile);        // region-field period
    float2 dp = p * DETAIL;                // detail-space coordinate (fine structure on screen)
    float2 Pd = P0 * DETAIL;               // detail-space period

    // Low-frequency region field: big, localized nebulae separated by empty space.
    float region = pfbm(p + 11.0, P0, 4);

    // Presence keeps most of space sparse; region_density widens where gas appears.
    float pres_lo   = lerp(0.62, 0.28, saturate(region_density));
    float presence  = smoothstep(pres_lo, pres_lo + 0.30, region);

    // Domain warp in detail space for swirling tendrils. The warp field is periodic, so warping
    // preserves the tiling of the cloud/detail/filament noises evaluated on `dp`.
    float2 q = float2(pfbm(dp * 0.5 + float2(1.0, 9.0), Pd * 0.5, 4),
                      pfbm(dp * 0.5 + float2(8.0, 2.0), Pd * 0.5, 4));
    dp += warp_strength * (q - 0.5) * 2.0;

    // Cloud body, mid detail and ridged filaments, all in warped detail space.
    float clouds    = pfbm(dp, Pd, 6);
    float detail    = pfbm(dp * 2.0 + 20.0, Pd * 2.0, 3);
    float filaments = pridge(dp * 2.0, Pd * 2.0, 4);

    // Density field gated by the large-scale region presence.
    float density_field = clouds * presence;

    // Component masks, calibrated to the field's ~0.5-centred distribution.
    float gas_mask  = smoothstep(0.28, 0.60, density_field);
    float dust_mask = smoothstep(0.14, 0.34, density_field) * (1.0 - gas_mask);
    float fil_pow   = 1.0 + filament_sharpness * 5.0;
    float highlight_mask = smoothstep(0.46, 0.70, density_field) * pow(saturate(filaments), fil_pow);

    // Zoom-reactive detail: as the camera gets closer (zoom_close -> 1) push finer filament/detail
    // contrast so zooming in reveals richer wispy structure instead of a flat far-away wash.
    float detail_boost = zoom_close * zoom_detail;
    highlight_mask *= (1.0 + detail_boost * 1.2);
    float detail_mod = lerp(0.75 + 0.5 * detail, 0.5 + 0.9 * detail, detail_boost);

    // Coherent per-region colour (low-freq), with a cosine-palette overlay. The biome hue field
    // rotates the palette phase so different galaxy regions take on distinct colour families; the
    // palette's blend weight rises with biome_strength so the recolour is clearly visible.
    float hue_t = saturate(region * 1.15 - 0.05 + palette_shift);
    float hue_p = hue_t + (b_hue - 0.5) * hue_spread * biome_strength;
    float3 palette_color;
    palette_color.r = cosine_palette(0.5, -1.0815, 0.7984,  0.0,    hue_p);
    palette_color.g = cosine_palette(0.5,  0.6584, 0.9084,  0.2684, hue_p);
    palette_color.b = cosine_palette(0.5, -0.2015, 0.3184, -0.0015, hue_p);
    palette_color = saturate(palette_color);

    float3 gas_color = lerp(gas_a, gas_b, saturate(hue_t * 1.4));
    gas_color = lerp(gas_color, gas_c, saturate(hue_t * 1.6 - 0.6));
    gas_color = lerp(gas_color, palette_color, 0.22 + 0.45 * biome_strength);
    gas_color *= gas_brightness_mul;
    // Internal luminance variation so the gas is not a flat wash.
    gas_color *= detail_mod;

    // Push bright filament cores toward a hot, whiter colour.
    float3 core_color = saturate((gas_c + 0.30) * 1.6);
    gas_color = lerp(gas_color, core_color, saturate(highlight_mask * highlight_power));

    // Composite: gas + dark dust lanes, plus an additive core glow on the filaments. gas_gain/
    // dust_gain shift the balance per biome (dusty regions vs glowing-gas regions).
    float3 color = gas_color * gas_mask * gas_gain + dust_color * dust_mask * dust_gain;
    color += gas_c * highlight_mask * highlight_power * 0.5;

    // Region hue shift: rotate the whole cloud's hue by the biome field so neighbouring regions read
    // as clearly distinct colour families (up to ~±40deg at full strength/spread), luma-preserving.
    color = hue_rotate(color, (b_hue - 0.5) * hue_spread * biome_strength * 2.2);

    // Zoom-reactive saturation + contrast: close-up nebulae read as richer/punchier.
    float sat_boost = zoom_close * zoom_sat;
    float lum = dot(color, float3(0.299, 0.587, 0.114));
    color = lerp(float3(lum, lum, lum), color, 1.0 + sat_boost * 1.2);
    color = (color - 0.5) * (1.0 + sat_boost * 0.4) + 0.5;
    color = saturate(color);

    // Opacity: gas fairly transparent, dust darkens more, bright cores slightly more opaque.
    float gas_alpha       = gas_mask * gas_gain * intensity * 0.6;
    float dust_alpha      = dust_mask * dust_gain * dust_intensity * 0.75;
    float highlight_alpha = highlight_mask * highlight_power * 0.25;
    float alpha = saturate(gas_alpha + dust_alpha + highlight_alpha);

    return float4(saturate(color), alpha);
}

float4 main(PSInput input) : SV_Target0
{
    float2 uv = input.uv;
    float2 cam_cell = float2(params0.x, params0.y);
    float zoom  = params0.z;
    float fb_w  = params0.w;
    float fb_h  = params1.x;
    float seed = frac(params1.w * 0.1031);
    float2 cam_local         = float2(falloff.z, falloff.w);

    // NEB_TARGET = world units per noise unit at the finest LOD level (feature-scale knob).
    float NEB_TARGET = max(gas_color_a.w, 1.0);
    const float NEB_SPAN   = 128.0;   // noise units spanning one tiling period
    const float NEB_FACTOR = 4.0;     // world-scale ratio between adjacent LOD levels
    const int   NEB_SLOTS  = 4;       // LOD levels cross-faded per pixel. MUST be >=4 so the top/
                                      // bottom slot always enters/leaves at weight 0 (the weight
                                      // window smoothstep(2,0,|dL|) spans ~2 levels each side of
                                      // lod_f); fewer slots pop a half-weight level in on every
                                      // integer crossing. wsum stays ~2 => no brightness pulsing.
    float neb_parallax   = clamp(gas_color_b.w, 0.0, 1.0);
    float NEB_CELLS_BASE = NEB_SPAN * NEB_TARGET / HIER_CELL; // HierPos2 cells per period at level 0

    // Per-pixel world offset from the camera center (physical screen->world; the ONLY zoom term).
    float2 offset = (uv - 0.5) * float2(fb_w, fb_h) / max(zoom, 1e-9);

    // ---- Macro "biome" field ----------------------------------------------------------------
    // A very low-frequency field sampled from this pixel's galaxy position modulates nebula colour
    // family / density / brightness / dust balance so different regions look distinct. Sampled ONCE
    // (not per LOD level): it is a property of the LOCATION, not the detail scale. Precision-safe via
    // reduce_axis with a huge wrap period so it never visibly repeats in normal play. The per-pixel
    // `offset` means biomes vary ACROSS the screen at galaxy zoom yet stay ~uniform when zoomed in.
    float biome_scale = max(biome0.y, 1.0);
    const float M_BIOME = 16384.0; // cells per biome wrap period (~2.7e8 world units)
    float2 bwrap = float2(reduce_axis(cam_cell.x, cam_local.x, M_BIOME, neb_parallax),
                          reduce_axis(cam_cell.y, cam_local.y, M_BIOME, neb_parallax)) + offset;
    // pvalue/pfbm tile ONLY at an INTEGER lattice period; a fractional period puts adjacent cells on
    // mismatched corners and seams the field. So quantize the noise period to a whole number and back
    // out the effective scale from it (region size snaps imperceptibly). bc then wraps at exactly
    // P_biome when bwrap wraps at M_BIOME*HIER_CELL -> the biome is continuous while panning.
    float biome_units  = M_BIOME * HIER_CELL;                 // world units per biome wrap
    float P_biome      = max(round(biome_units / biome_scale), 4.0); // integer noise period
    float2 bc          = bwrap * (P_biome / biome_units);     // wraps at exactly P_biome
    float2 Pbiome      = float2(P_biome, P_biome);
    float4 biome = float4(pfbm(bc + float2(37.0, 11.0), Pbiome, 3),
                          pfbm(bc + float2( 5.0, 71.0), Pbiome, 3),
                          pfbm(bc + float2(53.0, 29.0), Pbiome, 3),
                          pfbm(bc + float2(17.0, 91.0), Pbiome, 3));

    // Zoom-closeness: 0 far (galaxy map) -> 1 near (arena). Drives the zoom-reactive detail/sat.
    float zoom_close = smoothstep(0.06, 1.0, zoom);

    // Continuous LOD index. Level L has world feature scale NEB_TARGET*NEB_FACTOR^L; picking
    // L ~ -log_factor(zoom) keeps the on-screen feature size roughly constant across the whole
    // zoom range (arena AND galaxy map). CRUCIALLY every level is FIXED in world space and its
    // reduction modulus m_L is zoom-INDEPENDENT, so zoom only cross-fades neighbouring levels
    // (mip style) — no discontinuous coordinate remap, so nebulae no longer pop in and out.
    float logf  = log(NEB_FACTOR);
    float lod_f = clamp(-log(max(zoom, 1e-12)) / logf, -4.0, 12.0);
    int   base_level = (int)floor(lod_f) - 1;

    float4 accum = float4(0.0, 0.0, 0.0, 0.0);
    float  wsum  = 0.0;

    [loop]
    for (int k = 0; k < NEB_SLOTS; ++k)
    {
        float fL = (float)(base_level + k);
        float w  = smoothstep(2.0, 0.0, abs(fL - lod_f));
        if (w <= 0.0) continue;

        float scale = pow(NEB_FACTOR, fL);
        float cs_L  = NEB_TARGET     * scale;  // world units per noise unit at this level
        float m_L   = NEB_CELLS_BASE * scale;  // HierPos2 cells per tiling period (zoom-independent)

        float2 wl = float2(reduce_axis(cam_cell.x, cam_local.x, m_L, neb_parallax),
                           reduce_axis(cam_cell.y, cam_local.y, m_L, neb_parallax)) + offset;
        float2 pN = wl / cs_L + seed * 2.0;

        accum += nebula_level(pN, NEB_SPAN, biome, zoom_close) * w;
        wsum  += w;
    }

    if (wsum > 0.0) accum /= wsum;

    return float4(saturate(accum.rgb), saturate(accum.a));
}
