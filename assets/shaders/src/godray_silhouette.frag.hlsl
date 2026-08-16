// godray_silhouette.frag.hlsl — sprite silhouette into the god-ray occlusion buffer.
// Opaque art blocks the sun; only texture alpha (times sprite tint alpha) matters here.
// Input signature must match sprite.vert.hlsl's VSOutput (the pass reuses that vertex shader
// and the already-uploaded sprite vertex stream).

Texture2D    sprite_tex : register(t0, space2);
SamplerState sprite_smp : register(s0, space2);

struct PSInput
{
    float4 position  : SV_Position;
    float2 uv        : TEXCOORD0;
    float4 color     : TEXCOORD1;
    float2 world_pos : TEXCOORD2;
    float4 custom    : TEXCOORD3;
};

float4 main(PSInput input) : SV_Target0
{
    float a = sprite_tex.Sample(sprite_smp, input.uv).a * input.color.a;
    return float4(0.0, 0.0, 0.0, a);
}
