# Baked Starfield Texture — Implementation Ready

Replace the removed procedural starfield with a CPU-generated static RGBA texture drawn as a sprite.

## Steps

1. `game.h`: add `bs_texture starfield_tex` and `bs_light2d sun_light` to `game_state`
2. `game.cpp` `game_init`: generate 4096x4096 RGBA starfield texture on CPU, upload via `renderer_create_texture`; init `sun_light`
3. `game.cpp` `game_render`: draw starfield as a `bs_sprite` with UV window matching visible camera area, layer 0; prepend `sun_light` to light array for `renderer_set_lights`
4. Build and test

No new shaders or backend changes.
