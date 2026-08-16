// rml.vert.hlsl — RmlUi UI vertex shader.
//
// Mirrors the sprite shader's SDL3 GPU binding contract (vertex UBO => register(b0, space1)).
// RmlUi submits geometry in pixel coordinates with the origin at the TOP-LEFT of the context.
// The backend uploads mvp = project * transform (both column-major) and the per-draw translation,
// following the RmlUi transform convention: frag_pos = project * transform * (vertex + translation).
//
// Vertex layout matches Rml::Vertex (Include/RmlUi/Core/Vertex.h):
//   float2 position (offset 0), UBYTE4_NORM colour (offset 8), float2 tex_coord (offset 12).

cbuffer UBO : register(b0, space1)
{
    float4x4 mvp;         // column-major project * transform; engine uploads Mat4 directly.
    float2   translation; // pixel translation applied to vertex positions before transform.
    float2   _pad;
};

struct VSInput
{
    float2 position : TEXCOORD0; // pixel position (top-left origin)
    float4 color    : TEXCOORD1; // premultiplied-alpha sRGB, UBYTE4_NORM -> [0,1]
    float2 uv       : TEXCOORD2; // texture coordinate
};

struct VSOutput
{
    float4 position : SV_Position;
    float4 color    : TEXCOORD0;
    float2 uv       : TEXCOORD1;
};

VSOutput main(VSInput input)
{
    VSOutput output;
    float2 p         = input.position + translation;
    output.position  = mul(mvp, float4(p, 0.0f, 1.0f));
    output.color     = input.color;
    output.uv        = input.uv;
    return output;
}
