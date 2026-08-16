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

    // Soft knee above the threshold, normalised over a FIXED width rather than over
    // (1.0 - threshold).
    //
    // The old normaliser assumed an LDR source where luma could never exceed 1.0, so
    // (1.0 - threshold) was the remaining headroom. That assumption broke twice over. At the
    // shipped threshold of 1.2 the numerator was always negative and clamped to zero, so bloom
    // contributed NOTHING -- the pass ran every frame and produced a black texture. And the
    // divisor's max(..., 0.0001) guard meant that the moment an HDR source could exceed 1.2,
    // the same expression would divide by 0.0001 and multiply the scene by ~10,000.
    //
    // A fixed knee has neither failure: contribution ramps 0..1 over one stop above the
    // threshold and saturates there, so the threshold stays a pure "how hot before it blooms"
    // control at any value, LDR or HDR.
    const float KNEE = 1.0;
    float contrib = saturate((luma - threshold) / KNEE);
    return float4(c.rgb * contrib, 1.0);
}
