// bloom_blur_h.frag.hlsl — horizontal Gaussian blur pass
// 9-tap separable Gaussian with sigma ≈ 2.0

Texture2D    src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

cbuffer BloomParams : register(b0, space3)
{
    float4 params; // x = texel_size_x, y = texel_size_y, z = unused, w = unused
};

static const float weights[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float2 uv  = input.uv;
    float2 off = float2(params.x, 0.0);
    float4 sum = src_tex.Sample(src_smp, uv) * weights[0];
    for (int i = 1; i < 5; ++i)
    {
        sum += src_tex.Sample(src_smp, uv + off * i) * weights[i];
        sum += src_tex.Sample(src_smp, uv - off * i) * weights[i];
    }
    return sum;
}
