// sunburst.frag.hlsl — procedural Endless-Sky-style sunburst star
// Radial rays + FBM turbulence + warm colour gradation + tiny intense core.
// Drawn additively on top of the scene (pipeline uses ONE/ONE blend).

cbuffer SunburstParams : register(b0, space3)
{
    float4 center_glow; // xy = centre (pixels), z = body radius (pixels), w = glow radius (pixels)
    float4 color_time; // xyz = star colour tint, w = elapsed time
    float4 params;     // x = ray_count, y = ray_intensity, z = falloff, w = visibility
    float4 fb_size;    // xy = framebuffer size (pixels)
};

struct PSInput
{
    float4 position : SV_Position;
    float2 local_pos : TEXCOORD0;
};

// --- hash / noise / fbm (same family as nebula.frag) -----------------------------
float hash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float noise(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + float2(1.0, 0.0));
    float c = hash(i + float2(0.0, 1.0));
    float d = hash(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}

float fbm(float2 p)
{
    float val = 0.0;
    float amp = 0.5;
    float freq = 1.0;
    for (int i = 0; i < 2; ++i)
    {
        val += amp * noise(p * freq);
        amp *= 0.5;
        freq *= 2.0;
    }
    return val;
}

// ----------------------------------------------------------------------------------
float4 main(PSInput input) : SV_Target0
{
    float2 lp = input.local_pos;
    float dist = length(lp);

    float body_r = center_glow.z;
    float glow_r = center_glow.w;
    float time   = color_time.w;
    float3 star_col = color_time.xyz;

    float ray_count     = params.x;
    float ray_intensity = params.y;
    float falloff       = params.z;
    float visibility    = params.w;

    float angle = atan2(lp.y, lp.x);

    // Hard cutoff at disc edge (smoothstep to avoid sharp aliasing)
    float edge = 1.0 - smoothstep(0.92, 1.0, dist);
    if (edge < 0.001) return float4(0,0,0,1);

    // --- Tiny ultra-bright core (white-hot photosphere) ---
    float body_norm = max(body_r / glow_r, 0.001);
    float core_dist = dist / body_norm;
    float core = exp2(-16.0 * core_dist * core_dist);

    // NOTE: radial spikes are no longer drawn on this quad. They are rendered as a
    // screen-space lens effect in bloom_flare.frag.hlsl so they behave like a true
    // lens artifact instead of translating rigidly with the star.

    // --- Organic turbulence (fire-like) ---
    float turb = fbm(float2(dist * 8.0, angle * 2.0) + time * 0.1);

    // --- Distance falloff ---
    float glow = exp(-dist * falloff * 2.5);

    // --- Combine intensity ---
    float intensity = glow * (1.0 + turb * 0.15) + core * 2.0;
    intensity *= edge;

    // --- Warm colour gradation: white-hot centre -> yellow -> orange -> red edge ---
    float t = dist;
    float3 white  = float3(1.00, 1.00, 0.98);
    float3 yellow = float3(1.00, 0.85, 0.35);
    float3 orange = float3(1.00, 0.50, 0.12);
    float3 red    = float3(0.75, 0.15, 0.04);

    float3 col = white;
    col = lerp(col, yellow, smoothstep(0.10, 0.35, t));
    col = lerp(col, orange, smoothstep(0.35, 0.60, t));
    col = lerp(col, red,    smoothstep(0.60, 0.90, t));

    // Blend in the star's intrinsic colour tint
    col = lerp(col, star_col, 0.25);

    // Visibility fade (sensor range, etc.)
    intensity *= visibility;

    return float4(col * intensity, 1.0);
}
