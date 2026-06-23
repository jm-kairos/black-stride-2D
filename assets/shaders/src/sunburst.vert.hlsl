// sunburst.vert.hlsl — bounding quad for procedural sunburst star
// Generates a 6-vertex quad (2 triangles) from SV_VertexID; no VBO needed.
// local_pos goes from -1..1 across the glow radius, 0 at center.

cbuffer SunburstParams : register(b0, space1)
{
    float4 center_glow; // xy = center in pixels, z = body radius (pixels), w = glow radius (pixels)
    float4 color_time;  // xyz = star color tint, w = elapsed time
    float4 params;      // x = ray_count, y = ray_intensity, z = falloff, w = visibility
    float4 fb_size;     // xy = framebuffer width/height in pixels
};

struct VSOutput
{
    float4 position : SV_Position;
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

    float2 center   = center_glow.xy;
    float  glow_r   = center_glow.w;
    float2 fb       = fb_size.xy;

    // Pixel position on the bounding quad
    float2 pixel_pos = center + off * glow_r;

    // Convert to clip space (x: -1..1 left-right, y: -1..1 bottom-top)
    output.position = float4(
        pixel_pos.x / fb.x * 2.0 - 1.0,
        1.0 - pixel_pos.y / fb.y * 2.0,
        0.0, 1.0
    );

    // Normalised local coords: 0 at centre, 1 at glow edge
    output.local_pos = off;

    return output;
}
