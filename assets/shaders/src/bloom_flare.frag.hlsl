// bloom_flare.frag.hlsl — lens-flare ghost pass
// Samples the streak output and creates a rich, soft, chromatic chain of ghost orbs
// along the line from the bright source through the screen center.

Texture2D    src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

cbuffer BloomParams : register(b0, space3)
{
    float4 params; // x = source_u, y = source_v, z = flare_intensity, w = aspect ratio (w/h)
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float2 uv = input.uv;
    float2 center = float2(0.5, 0.5);
    float2 source = params.xy;
    float aspect = params.w > 0.001 ? params.w : (16.0 / 9.0);

    float4 result = float4(0,0,0,0);

    // Direction and distance from the bright source to the optical (screen) centre. The
    // orb chain spreads along this line, so it collapses onto a centred star and lengthens
    // as the star drifts off-centre (true lens-flare behaviour).
    float2 flare_dir  = center - source;
    float  center_off = length(float2(flare_dir.x * aspect, flare_dir.y));
    // Ease the whole chain in as the star leaves the centre so orbs never pile on the star.
    float  chain_fade = smoothstep(0.05, 0.30, center_off);

    // Procedural warm orb chain (image-2 style). scale: 0 = source, 1 = centre, >1 beyond.
    float scales[7]  = { -0.30, 0.28, 0.52, 0.78, 1.05, 1.35, 1.70 };
    float weights[7] = {  0.10, 0.22, 0.16, 0.32, 0.16, 0.24, 0.12 };
    float radii[7]   = {  0.05, 0.035, 0.11, 0.022, 0.15, 0.045, 0.09 };

    for (int i = 0; i < 7; ++i)
    {
        float2 orb_center = source + flare_dir * scales[i];

        // Aspect-corrected vector from this pixel to the orb centre (keeps orbs circular).
        float2 d = uv - orb_center;
        d.x *= aspect;
        float dr = length(d) / radii[i];

        // Per-channel soft core gives a gentle chromatic fringe at the orb edge.
        float core_r = exp2(-4.0 * (dr * 0.96) * (dr * 0.96));
        float core_g = exp2(-4.0 * (dr       ) * (dr       ));
        float core_b = exp2(-4.0 * (dr * 1.06) * (dr * 1.06));
        // Faint outer ring.
        float ring   = exp2(-4.0 * (dr / 2.2) * (dr / 2.2)) * (1.0 - core_g);
        float3 mask  = saturate(float3(core_r, core_g, core_b) + ring * 0.4);

        // Warm grade along the chain: warm-white -> gold -> amber.
        float t   = saturate(scales[i] / 1.70);
        float3 col = lerp(float3(1.00, 0.92, 0.70), float3(1.00, 0.68, 0.32),
                          smoothstep(0.0, 0.5, t));
        col = lerp(col, float3(0.95, 0.42, 0.18), smoothstep(0.5, 1.0, t));

        result.rgb += col * mask * weights[i];
    }
    result.rgb *= chain_fade;

    // --- Lens diffraction spikes emanating from the source, oriented toward screen center ---
    float2 sd = uv - source;
    sd.x *= aspect;                       // aspect-correct so spikes are radially even
    float sdist = length(sd);
    float sangle = atan2(sd.y, sd.x);

    // Axis from the source toward the screen center (aspect-corrected).
    float2 axis = center - source;
    axis.x *= aspect;
    float axis_angle = atan2(axis.y, axis.x);

    // Tint the spikes with the star's colour sampled at the source, with a warm floor.
    float3 spike_col = src_tex.Sample(src_smp, source).rgb;
    spike_col = max(spike_col, float3(0.45, 0.38, 0.28));

    float pi  = 3.14159265359;
    float pi2 = 6.28318530718;
    int spike_count = 6;
    float spikes = 0.0;
    for (int s = 0; s < spike_count; ++s)
    {
        float fs   = (float)s;
        float seed = frac(sin(fs * 12.9898) * 43758.5453);
        float base = fs * (pi2 / (float)spike_count) + axis_angle;
        float jit  = (seed - 0.5) * 0.4;          // small fixed angular offset
        float wdt  = 0.04 + 0.05 * frac(seed * 1.7);
        float len  = 0.40 + 0.40 * frac(seed * 2.3);
        float brt  = 0.5  + 0.5  * frac(seed * 3.1);

        float a  = base + jit;
        float da = sangle - a;
        da = frac(da / pi2 + 0.5) * pi2 - pi;     // wrap to [-pi, pi]

        float ang = 1.0 - saturate(abs(da) / wdt);
        ang = pow(ang, 3.0);

        float radial = exp(-sdist / len) * (1.0 - smoothstep(len, len * 1.6, sdist));
        spikes += ang * radial * brt;
    }

    result.rgb += spike_col * spikes * 0.8;

    return result * params.z;
}
