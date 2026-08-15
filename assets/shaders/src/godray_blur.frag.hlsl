// godray_blur.frag.hlsl — radial god-ray march: smear the occlusion buffer's surviving sun
// light away from the sun. Sampling off the texture edge is safe: the post sampler clamps to
// edge, so an off-screen sun still produces coherent shafts entering the frame.

Texture2D    src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

cbuffer GodrayBlur : register(b0, space3)
{
    float4 sun;  // x,y = sun centre in uv (top-left origin), z = density, w = per-tap decay
    float4 look; // x = exposure, y = intensity, z,w unused
};

#define NUM_SAMPLES 64

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float2 delta = (input.uv - sun.xy) * (sun.z / NUM_SAMPLES);
    float2 coord = input.uv;
    float  illum = 1.0;
    float3 acc   = 0.0;
    [loop]
    for (int i = 0; i < NUM_SAMPLES; ++i)
    {
        acc   += src_tex.Sample(src_smp, coord).rgb * illum;
        coord -= delta;
        illum *= sun.w;
    }
    acc *= (look.x / NUM_SAMPLES) * look.y;
    return float4(acc, 1.0);
}
