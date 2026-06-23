cbuffer UBO : register(b0, space1)
{
    float4x4 view_proj;
};

struct VSInput
{
    float2 position : TEXCOORD0;
    float2 uv       : TEXCOORD1;
    float3 world_pos : TEXCOORD2;
    float  angle    : TEXCOORD3;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float3 world_pos : TEXCOORD1;
    float  angle    : TEXCOORD2;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    output.position = mul(view_proj, float4(input.position, 0.0, 1.0));
    output.uv       = input.uv;
    output.world_pos = input.world_pos;
    output.angle    = input.angle;
    return output;
}
