// bloom_streak.frag.hlsl — chromatic anamorphic streak blur pass
// Samples the vertically-blurred bloom texture along a configurable axis
// with RGB channel-specific offsets for cinematic chromatic aberration.

Texture2D    src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

cbuffer BloomParams : register(b0, space3)
{
    float4 params; // x = texel_size_x, y = texel_size_y, z = streak_angle, w = streak_len
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// 17-tap Gaussian approximation (center + 8 symmetric pairs)
static const float weights[9] = {
    0.103140,
    0.100010,
    0.091000,
    0.077900,
    0.062700,
    0.047300,
    0.033500,
    0.022300,
    0.014000
};

float4 main(PSInput input) : SV_Target0
{
    float2 axis = float2(cos(params.z), sin(params.z));
    float2 off  = axis * float2(params.x, params.y) * params.w;

    // Chromatic aberration: fixed pixel shift so the fringing stays subtle even at
    // very long streak lengths instead of separating into red/green/blue bands.
    float2 chromatic = axis * float2(params.x, params.y) * 3.0f;
    float2 offR = off + chromatic;
    float2 offG = off;
    float2 offB = off - chromatic;

    float2 uv = input.uv;
    float4 center = src_tex.Sample(src_smp, uv);

    float sumR = center.r * weights[0];
    float sumG = center.g * weights[0];
    float sumB = center.b * weights[0];

    for (int i = 1; i < 9; ++i)
    {
        sumR += (src_tex.Sample(src_smp, uv + offR * i).r
               + src_tex.Sample(src_smp, uv - offR * i).r) * weights[i];

        sumG += (src_tex.Sample(src_smp, uv + offG * i).g
               + src_tex.Sample(src_smp, uv - offG * i).g) * weights[i];

        sumB += (src_tex.Sample(src_smp, uv + offB * i).b
               + src_tex.Sample(src_smp, uv - offB * i).b) * weights[i];
    }

    return float4(sumR, sumG, sumB, center.a);
}
