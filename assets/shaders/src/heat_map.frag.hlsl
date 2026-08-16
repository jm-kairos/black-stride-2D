// heat_map.frag.hlsl — procedural radiation heat map (single-pass fullscreen shader)
// Evaluates the same scalar field the CPU heat map used and maps it to a rainbow gradient.
// A mild domain warp breaks the perfect circular symmetry; the CPU provides the fluid trail geometry.

#define MAX_HEAT_SOURCES 256

cbuffer HeatMapParams : register(b0, space3)
{
    float4 camera;      // x = world_pos.x, y = world_pos.y, z = world_viewport_width, w = world_viewport_height
    float4 params;      // x = base_detection_radius, y = heat_warp_strength, z = threshold, w = intensity
    float4 extra;       // x = source_count, y = venn_sharpness, z = heat_signature_radius, w = color_falloff_power
    float4 palette;     // x = palette index, yzw = color_low.rgb
    float4 custom_colors; // xyz = color_high.rgb, w = reserved
    float4 sources[MAX_HEAT_SOURCES]; // xy = position, z = detector flag (1 = player sensor), w = emission
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// 1D hash for a 2D point (returns value in [0,1]).
float hash12(float2 p)
{
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

// Smooth bilinear value noise; produces a continuous low-frequency field.
float smooth_noise2(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);
    float a = hash12(i + float2(0,0));
    float b = hash12(i + float2(1,0));
    float c = hash12(i + float2(0,1));
    float d = hash12(i + float2(1,1));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

float4 main(PSInput input) : SV_Target0
{
    // Convert screen UV to world position. UV y is inverted because screen space has y down.
    float2 world_pos = camera.xy + (input.uv - 0.5) * float2(camera.z, -camera.w);

    float base_radius = params.x;
    float base_r2 = base_radius * base_radius;
    float heat_signature_radius = extra.z;
    float heat_signature_r2 = heat_signature_radius * heat_signature_radius;
    float warp_strength = params.y;
    float threshold = params.z;
    float intensity = params.w;
    int source_count = (int)extra.x;

    // Smooth, low-frequency domain warp for organic (non-grainy) irregular boundaries.
    float warp = (smooth_noise2(world_pos * 0.005) - 0.5) * warp_strength;
    float2 warped_pos = world_pos + warp;

    // Detector mask: union of player sensor circles (z > 0.5).
    // Weighted visual field: enemy/projectile heat emitters (z <= 0.5).
    float soft_mask = 0.0;
    float hard_mask = 0.0;
    float value_weighted = 0.0;
    float inner_r = base_radius * 0.95;
    float inner_r2 = inner_r * inner_r;
    for (int i = 0; i < source_count; ++i) {
        float2 d = warped_pos - sources[i].xy;
        float dist2 = dot(d, d) + 0.0001;
        if (sources[i].z > 0.5) {
            float falloff = base_r2 / dist2;
            soft_mask += falloff;
            // Hard mask uses unwarped distance for a geometric circle boundary.
            float2 hd = world_pos - sources[i].xy;
            float hdist2 = dot(hd, hd) + 0.0001;
            hard_mask += 1.0 - smoothstep(inner_r2, base_r2, hdist2);
        } else {
            float falloff = heat_signature_r2 / dist2;
            float emission = sources[i].w;
            value_weighted += emission * emission * falloff;
        }
    }
    soft_mask = saturate(soft_mask);
    hard_mask = saturate(hard_mask);
    float detector_mask = lerp(soft_mask, hard_mask, extra.y);
    value_weighted *= detector_mask;

    // Map the weighted field to a gradient coordinate, then apply the falloff power.
    float t = saturate(value_weighted / 3.0);
    t = pow(t, extra.w);

    float3 color;
    int palette_idx = (int)palette.x;
    if (palette_idx == 0) {
        // Rainbow: blue -> cyan -> green -> yellow -> red.
        float r, g, b;
        if (t < 0.25) {
            r = 0.0;       g = t * 4.0;       b = 1.0;
        } else if (t < 0.5) {
            r = 0.0;       g = 1.0;           b = 1.0 - (t - 0.25) * 4.0;
        } else if (t < 0.75) {
            r = (t - 0.5) * 4.0; g = 1.0;   b = 0.0;
        } else {
            r = 1.0;       g = 1.0 - (t - 0.75) * 4.0; b = 0.0;
        }
        color = float3(r, g, b);
    } else if (palette_idx == 1) {
        // Thermal: black -> red -> white.
        color = t < 0.5 ? float3(t * 2.0, 0.0, 0.0)
                        : float3(1.0, (t - 0.5) * 2.0, (t - 0.5) * 2.0);
    } else if (palette_idx == 2) {
        // Blackbody: black -> red -> orange -> yellow -> white.
        if (t < 0.33) {
            color = float3(t * 3.0, 0.0, 0.0);
        } else if (t < 0.66) {
            color = float3(1.0, (t - 0.33) * 3.0, 0.0);
        } else {
            color = float3(1.0, 1.0, (t - 0.66) * 3.0);
        }
    } else {
        // Custom: lerp between the two authored colors.
        color = lerp(palette.yzw, custom_colors.xyz, t);
    }

    float alpha = saturate(value_weighted / threshold) * intensity;
    return float4(color, alpha);
}
