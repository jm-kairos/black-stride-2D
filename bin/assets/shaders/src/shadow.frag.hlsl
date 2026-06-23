// shadow.frag.hlsl — fullscreen shadow mask pass for sprite soft shadows.
// Drawn once per occluding sprite into a half-res R8 shadow mask with additive blending.
// Each pixel accumulates shadow occlusion from all sprites near the dominant light.

// Fragment uniform block (register b0, space3 per SDL3 GPU contract).
cbuffer ShadowParams : register(b0, space3)
{
    float4 u_light;         // xy = world position, z = light radius, w = max shadow distance
    float4 u_sprite;        // xy = world position, z = sprite radius, w = unused
    float4 u_camera;        // xy = camera position, z = zoom, w = rotation (radians)
    float2 u_screen_size;   // width, height of the shadow mask in pixels
    float2 u_pad;
};

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0) : SV_Target0
{
    // position.xy is pixel coordinates in the shadow mask render target.
    float2 screen_px = position.xy;

    // ---- Convert screen pixel -> world space (inverse of camera2d_view_proj) ----
    // NOTE: the shadow mask is half-resolution, but each texel maps to a 2x2 block of
    // screen pixels. We multiply view_pt by 2 so the reconstructed world position matches
    // the full-resolution sprite pass.
    float hw = u_screen_size.x * 0.5;
    float hh = u_screen_size.y * 0.5;
    float2 view_pt = float2(screen_px.x - hw, hh - screen_px.y) * 2.0;

    float z       = u_camera.z;
    float rot     = u_camera.w;
    float c       = cos(rot);
    float s       = sin(rot);
    float2 unscaled   = view_pt / max(z, 0.0001);
    float2 unrotated  = float2(unscaled.x * c - unscaled.y * s,
                               unscaled.x * s + unscaled.y * c);
    float2 world_pos  = u_camera.xy + unrotated;

    // ---- Light / sprite parameters ----
    float2 light_pos  = u_light.xy;
    float2 sprite_pos = u_sprite.xy;
    // u_light.z is the light's effect radius (huge). For soft shadows we need the
    // light SOURCE radius — the apparent angular size of the star. We pass that
    // in u_light.w (virtual radius) instead of the effect radius.
    float  light_r    = u_light.w;
    float  sprite_r   = u_sprite.z;

    float2 to_pixel   = world_pos - light_pos;
    float  pixel_dist = length(to_pixel);

    float2 to_sprite   = sprite_pos - light_pos;
    float  sprite_dist = length(to_sprite);
    if (sprite_dist < 0.0001)
        discard;

    // Only shadow pixels that lie beyond the sprite (away from the light).
    float2 dir_to_sprite = to_sprite / sprite_dist;
    float  along = dot(to_pixel, dir_to_sprite);
    if (along < sprite_dist)
        discard;

    // Angular distance between pixel and shadow center.
    float pixel_angle  = atan2(to_pixel.y, to_pixel.x);
    float sprite_angle = atan2(to_sprite.y, to_sprite.x);
    float delta = abs(pixel_angle - sprite_angle);
    if (delta > 3.14159265)
        delta = 6.2831853 - delta;

    // Umbra = full occlusion, penumbra = partial occlusion.
    float umbra_half    = atan(max(sprite_r - light_r, 0.0) / max(sprite_dist, 0.0001));
    float penumbra_half = atan((sprite_r + light_r)      / max(sprite_dist, 0.0001));

    // Clamp angular width so tiny close sprites don't cast absurdly huge shadows
    // and distant sprites still cast a visible minimum shadow.
    float min_half = 0.0087; // 0.5 degrees in radians
    float max_half = 0.262;  // 15 degrees in radians
    penumbra_half = clamp(penumbra_half, min_half, max_half);
    umbra_half    = min(umbra_half, penumbra_half * 0.5);

    // Shadow strength: 1.0 in umbra, 0.0 outside penumbra.
    float shadow = 1.0 - smoothstep(umbra_half, penumbra_half, delta);

    // Fade near the sprite so the shadow starts just beyond the occluder.
    float start_dist = sprite_dist + sprite_r;
    shadow *= smoothstep(sprite_dist, start_dist, along);

    return float4(shadow, 0.0, 0.0, shadow);
}
