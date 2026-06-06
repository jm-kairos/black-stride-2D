// sprite.frag.hlsl — Phase 3 sprite-batch pixel shader.
//
// SDL3 GPU binding contract (DXIL/D3D12 path): fragment sampled textures => register(t[n], space2),
// samplers => register(s[n], space2). These are bound together via SDL_BindGPUFragmentSamplers.
// The pipeline's fragment shader must declare num_samplers = 1 to match.
//
// Samples the bound texture and multiplies by the interpolated per-vertex tint. A solid-white
// 1x1 texture lets untextured/solid sprites reuse this same pipeline (white * tint = tint).

Texture2D    sprite_tex : register(t0, space2);
SamplerState sprite_smp : register(s0, space2);

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float4 color    : TEXCOORD1;
};

float4 main(PSInput input) : SV_Target0
{
    float4 texel = sprite_tex.Sample(sprite_smp, input.uv);
    return texel * input.color;
}
