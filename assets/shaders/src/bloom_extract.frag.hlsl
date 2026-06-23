// bloom_extract.frag.hlsl — brightness threshold pass for HDR bloom
// Samples the scene texture and outputs pixels above a luminance threshold.

Texture2D    src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

cbuffer BloomParams : register(b0, space3)
{
    float4 params; // x = threshold, y = intensity, z = unused, w = unused
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float4 c = src_tex.Sample(src_smp, input.uv);
    float luma = dot(c.rgb, float3(0.299, 0.587, 0.114));
    float threshold = params.x;
    float contrib = max(0.0, luma - threshold) / max(1.0 - threshold, 0.0001);
    return float4(c.rgb * contrib, 1.0);
}
