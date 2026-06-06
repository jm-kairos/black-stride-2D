// quad.frag.hlsl — Phase 2 first-quad pixel shader.
// No resources bound; just passes the interpolated vertex color through. Phase 3 will add
// a sampled texture (t[0],space2) + sampler (s[0],space2) for sprites.

struct PSInput
{
    float4 position : SV_Position;
    float4 color    : TEXCOORD0;
};

float4 main(PSInput input) : SV_Target0
{
    return input.color;
}
