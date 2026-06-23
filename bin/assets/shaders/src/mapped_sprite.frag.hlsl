Texture2D diffuse_tex  : register(t0, space2);
Texture2D normal_tex   : register(t1, space2);
Texture2D depth_tex    : register(t2, space2);
Texture2D position_tex : register(t3, space2);
SamplerState sprite_smp : register(s0, space2);

cbuffer LightUBO : register(b0, space3)
{
    float4 light_dir;  // xyz = direction, w = intensity
    float4 ambient;
    float4 tuning;     // x = normal strength, y = depth parallax scale
};

struct PSInput
{
    float4 position  : SV_Position;
    float2 uv        : TEXCOORD0;
    float3 world_pos : TEXCOORD1;
    float  angle     : TEXCOORD2;
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

    float2 L_world = normalize(light_dir.xy);
    float2 L_local = float2(
        L_world.x * cos(input.angle) + L_world.y * sin(input.angle),
       -L_world.x * sin(input.angle) + L_world.y * cos(input.angle)
    );
    float3 L = normalize(float3(L_local, 0.2)); // slight Z component for rim lift

    float NdotL = max(dot(n, L), 0.0);

    float3 ambient_color = ambient.rgb;
    float3 lit = diffuse.rgb * (ambient_color + NdotL * light_dir.w * tuning.x);

    return float4(lit, diffuse.a);
}
