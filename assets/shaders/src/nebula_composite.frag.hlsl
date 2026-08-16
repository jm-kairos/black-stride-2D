// nebula_composite.frag.hlsl — upscale + composite the half-resolution nebula target.
// The nebula is rendered once into a half-size render target (premultiplied alpha) to cut the
// expensive per-pixel FBM cost to a quarter, then this pass bilinearly upscales it back to full
// resolution. The owning pipeline uses premultiplied-over blending (src=ONE, dst=1-SRC_ALPHA),
// so this shader simply returns the sampled premultiplied RGBA unchanged.

Texture2D    src_tex : register(t0, space2);
SamplerState src_smp : register(s0, space2);

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    return src_tex.Sample(src_smp, input.uv);
}
