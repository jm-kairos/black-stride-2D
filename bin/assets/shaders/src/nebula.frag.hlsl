// nebula.frag.hlsl — procedural nebula background via FBM noise
// Drawn fullscreen into scene_rt before sprite batching.
// Samples the half-res shadow mask to darken the background behind sprite shadows.

Texture2D    shadow_tex : register(t0, space2);
SamplerState shadow_smp : register(s0, space2);

cbuffer BloomParams : register(b0, space3)
{
    float4 params; // x = time, y = unused, z = unused, w = unused
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

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
    for (int i = 0; i < 5; ++i)
    {
        val += amp * noise(p * freq);
        amp *= 0.5;
        freq *= 2.0;
    }
    return val;
}

float4 main(PSInput input) : SV_Target0
{
    float2 uv = input.uv;
    float t = params.x * 0.02;

    float2 p = uv * 3.0 + float2(t * 0.3, t * 0.1);

    float n  = fbm(p);
    float n2 = fbm(p * 1.5 + float2(5.2, 1.3) + t * 0.05);

    float3 deep   = float3(0.02, 0.01, 0.05);
    float3 mid    = float3(0.15, 0.05, 0.20);
    float3 bright = float3(0.40, 0.15, 0.35);
    float3 hot    = float3(0.60, 0.25, 0.30);

    float3 col = deep;
    col = lerp(col, mid,    smoothstep(0.2, 0.4, n));
    col = lerp(col, bright, smoothstep(0.4, 0.6, n2));
    col = lerp(col, hot,    smoothstep(0.6, 0.8, n * n2));

    float brightness = 0.3 + 0.7 * n;
    col *= brightness;

    // Darken the nebula where sprite shadows fall.
    float shadow = saturate(shadow_tex.Sample(shadow_smp, uv).r);
    col *= 1.0 - shadow * 0.85;

    return float4(col, 1.0);
}
