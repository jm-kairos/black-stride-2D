Texture2D diffuse_tex  : register(t0, space2);
Texture2D normal_tex   : register(t1, space2);
Texture2D depth_tex    : register(t2, space2);
Texture2D position_tex : register(t3, space2);
SamplerState sprite_smp : register(s0, space2);

// Movable 2D point lights, shared with the sprite pass (same world space, same falloff).
// MUST equal BS_MAX_LIGHTS in sprite.frag.hlsl / BS_BACKEND_MAX_LIGHTS in the backend.
#define BS_MAX_LIGHTS 16

// MUST be kept in sync with `mapped_light` in renderer_backend_sdlgpu.cpp and
// `preview_light_t` in tools/map_extractor/preview.cpp.
cbuffer LightUBO : register(b0, space3)
{
    float4 light_dir;  // xyz = star direction, w = intensity
    float4 ambient;
    float4 tuning;     // x = normal strength, y = depth parallax scale, z = point light count
    float4 light_pos_radius[BS_MAX_LIGHTS]; // xy = world position, z = radius, w = intensity
    float4 light_color[BS_MAX_LIGHTS];      // rgb = light color, w = per-light enabled (0 = off)
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
    float2 uv = input.uv;

    // Depth-driven parallax: sample depth and offset UV slightly toward the light.
    float depth = depth_tex.Sample(sprite_smp, uv).r;
    float2 parallax_dir = normalize(light_dir.xy);
    uv += parallax_dir * (depth - 0.5) * tuning.y;

    float4 diffuse = diffuse_tex.Sample(sprite_smp, uv);
    float3 normal = normal_tex.Sample(sprite_smp, uv).rgb;
    float3 position = position_tex.Sample(sprite_smp, uv).rgb;

    // Decode normal from [0,1] to [-1,1]. Assume the normal map is in tangent space
    // with the ship pointing along +Y in texture space. Rotate the light direction
    // into sprite-local space using the sprite's world angle.
    normal = normal * 2.0 - 1.0;
    float3 n = normalize(float3(normal.xy, normal.z));

    float ca = cos(input.angle);
    float sa = sin(input.angle);

    float2 L_world = normalize(light_dir.xy);
    float2 L_local = float2(
        L_world.x * ca + L_world.y * sa,
       -L_world.x * sa + L_world.y * ca
    );
    float3 L = normalize(float3(L_local, 0.2)); // slight Z component for rim lift

    float NdotL = max(dot(n, L), 0.0);

    // Star (directional) light + ambient floor.
    float3 acc = ambient.rgb + NdotL * light_dir.w * tuning.x;

    // Dynamic 2D point lights (thruster burns, muzzle flashes, editor lights): per-pixel
    // diffuse against the normal map. Same position space and radius falloff as the sprite
    // shader so one light list drives both passes; direction is rotated into ship-local
    // space exactly like the star. Lights float slightly above the hull plane (z lift) so
    // flat decks still catch some spill instead of only the rims.
    int count = (int)tuning.z;
    [loop]
    for (int i = 0; i < count; ++i)
    {
        if (light_color[i].w < 0.5) continue;

        float2 to_light = light_pos_radius[i].xy - input.world_xy;
        float dist      = length(to_light);
        float radius    = max(light_pos_radius[i].z, 0.0001);

        float direct = saturate(1.0 - dist / radius);
        direct *= direct;
        if (direct <= 0.0) continue;

        float2 Pw = (dist > 0.0001) ? to_light / dist : float2(0.0, 1.0);
        float2 Pl = float2(
            Pw.x * ca + Pw.y * sa,
           -Pw.x * sa + Pw.y * ca
        );
        float3 Lp = normalize(float3(Pl, 0.35));
        float ndl = max(dot(n, Lp), 0.0);

        acc += light_color[i].rgb * (direct * light_pos_radius[i].w * ndl);
    }

    float3 lit = diffuse.rgb * acc;

    return float4(lit, diffuse.a);
}
