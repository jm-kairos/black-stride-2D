// godray_composite.frag.hlsl — additive upscale of the blurred god-ray buffer into the HDR
// scene target (ONE/ONE pipeline blend; alpha contribution 0 keeps scene_rt opaque).

Texture2D    src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    return float4(src_tex.Sample(src_smp, input.uv).rgb, 0.0);
}
