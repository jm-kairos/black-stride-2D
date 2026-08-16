// star_surface.vert.hlsl — bounding quad for the procedural real-time star surface.
// Generates a 6-vertex quad (2 triangles) from SV_VertexID; no VBO needed.
// local_pos goes from -1..1 across the OUTER glow radius (quad half-size).

cbuffer StarSurfaceParams : register(b0, space1)
{
    float4 a; // xy = centre in pixels, z = body(disc) radius px, w = outer glow radius px
    float4 b; // xyz = star colour tint, w = elapsed time
    float4 c; // x = noise_scale, y = flow_speed, z = granule_contrast, w = visibility
    float4 d; // xy = framebuffer size px, z = hotspot_gain, w = sunspot_density
    float4 e; // x = limb_darkening, y = brightness, z = corona_strength, w = reserved
    float4 f; // reserved
};

struct VSOutput
{
    float4 position  : SV_Position;
    float2 local_pos : TEXCOORD0;
};

static const float2 offsets[6] = {
    float2(-1, -1), float2( 1, -1), float2(-1,  1),  // tri 1
    float2(-1,  1), float2( 1, -1), float2( 1,  1)   // tri 2
};

VSOutput main(uint id : SV_VertexID)
{
    VSOutput output;
    float2 off = offsets[id];

    float2 center = a.xy;
    float  glow_r = a.w;
    float2 fb     = d.xy;

    float2 pixel_pos = center + off * glow_r;

    output.position = float4(
        pixel_pos.x / fb.x * 2.0 - 1.0,
        1.0 - pixel_pos.y / fb.y * 2.0,
        0.0, 1.0
    );

    output.local_pos = off; // -1..1 across the outer glow radius
    return output;
}
