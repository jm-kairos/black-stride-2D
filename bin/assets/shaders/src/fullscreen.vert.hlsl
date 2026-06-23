// fullscreen.vert.hlsl — fullscreen triangle for post-process passes
// Emits a single triangle covering the entire clip space; no vertex buffer needed.
// SV_VertexID: 0 -> (-1,-1), 1 -> (3,-1), 2 -> (-1,3)
// UVs go 0..2 so the 0..1 subrange is the visible quad.

struct VSOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOutput main(uint id : SV_VertexID)
{
    VSOutput output;
    float2 uv = float2(id % 2, id / 2) * 2.0;
    output.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    output.uv       = float2(uv.x, 1.0 - uv.y);
    return output;
}
