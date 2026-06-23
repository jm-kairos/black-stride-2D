// bloom_composite.frag.hlsl — composite scene + bloom + aux streak back to swapchain

Texture2D    scene_tex     : register(t0, space2);
Texture2D    bloom_tex     : register(t1, space2);
Texture2D    aux_streak_tex : register(t2, space2);
Texture2D    aux_flare_tex : register(t3, space2);
SamplerState smp           : register(s0, space2);

cbuffer BloomParams : register(b0, space3)
{
    float4 params; // x = bloom intensity, y = aux streak intensity, z = flare intensity, w = unused
};

struct PSInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    float2 uv     = input.uv;
    float4 scene  = scene_tex.Sample(smp, uv);
    float4 bloom  = bloom_tex.Sample(smp, uv);
    float4 streak = aux_streak_tex.Sample(smp, uv);
    float4 flare  = aux_flare_tex.Sample(smp, uv);
    return float4(scene.rgb + bloom.rgb * params.x + streak.rgb * params.y + flare.rgb * params.z, scene.a);
}
