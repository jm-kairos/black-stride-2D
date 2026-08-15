// godray_mapped_silhouette.frag.hlsl — mapped-hull silhouette into the god-ray occlusion buffer.
// Diffuse alpha blocks the sun outright; the depth (height-field) map lets LOW, thin
// superstructure transmit a fraction of it, so light leaks through deck gaps and greebles
// while tall hull blocks stay opaque.
// The godray pass binds only two of the four maps: t0 = diffuse, t1 = depth.
// Input signature must match mapped_sprite.vert.hlsl's VSOutput.

Texture2D    diffuse_tex : register(t0, space2);
Texture2D    depth_tex   : register(t1, space2);
SamplerState smp         : register(s0, space2);

cbuffer GodrayOccluder : register(b0, space3)
{
    float4 tuning; // x = transmission (0 = alpha-only block, 1 = full height-driven leak)
};

struct PSInput
{
    float4 position  : SV_Position;
    float2 uv        : TEXCOORD0;
    float3 world_pos : TEXCOORD1;
    float  angle     : TEXCOORD2;
    float2 world_xy  : TEXCOORD3;
};

float4 main(PSInput input) : SV_Target0
{
    float a = diffuse_tex.Sample(smp, input.uv).a;
    // The extractor compresses the height field to 0.25..0.75 (see MAP_EXTRACTOR_TOOL.md);
    // renormalize so the lowest deck is 0 and the tallest block is 1.
    float h     = saturate((depth_tex.Sample(smp, input.uv).r - 0.25) / 0.5);
    float block = 1.0 - tuning.x * (1.0 - h);
    return float4(0.0, 0.0, 0.0, a * block);
}
