// starfield.vert.hlsl — per-star quad transform (Endless Sky style)
// Each star is 6 vertices (2 triangles) with interleaved attributes:
//   offset (float2) — star position within the wrapping field
//   size   (float)  — star radius
//   corner (float)  — angle encoding which corner of the quad
//
// SDL3 GPU D3D12 convention: vertex semantics use TEXCOORDn;
// vertex uniform buffers use register(b[n], space1).

cbuffer StarfieldTransform : register(b0, space1)
{
    float4 p0; // scale_x, scale_y, translate_x, translate_y
    float4 p1; // rotate[0], rotate[1], rotate[2], rotate[3]
    float4 p2; // elongation, brightness, 0, 0
};

struct VSInput
{
    float2 offset : TEXCOORD0;
    float  size   : TEXCOORD1;
    float  corner : TEXCOORD2;
    float3 color  : TEXCOORD3;
};

struct VSOutput
{
    float4 position : SV_Position;
    float2 coord    : TEXCOORD0;
    float  alpha    : TEXCOORD1;
    float3 color    : TEXCOORD2;
};

VSOutput main(VSInput input)
{
    VSOutput output;

    // Build unit vector from corner angle (diamond shape).
    float2 coord = float2(sin(input.corner), cos(input.corner));

    // Elongate along the blur direction (Y after rotation).
    float2 elongated = float2(coord.x * input.size,
                               coord.y * (input.size + p2.x));

    // Apply rotation, translation, and screen scale.
    float2x2 rot = float2x2(p1.x, p1.y, p1.z, p1.w);
    float2 pos = mul(rot, elongated) + p0.zw + input.offset;
    output.position = float4(pos * p0.xy, 0.0, 1.0);

    output.coord = coord;
    output.color = input.color;
    // drawSize = brightness * (4 / (4 + elongation)) * size * 0.2 + 0.05
    float drawSize = p2.y * (4.0 / (4.0 + p2.x)) * input.size * 0.2 + 0.05;
    output.alpha = min(1.0, drawSize);

    return output;
}
