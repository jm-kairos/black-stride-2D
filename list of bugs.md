# List of Bugs

## Bug 1: Aux bloom mode toggled per-star → batch reset, no streak accumulation
**Date:** 2026-06-17  
**Area:** Star FX rendering  
**Symptom:** Anamorphic streak effect had no visual effect on stars.  
**Root cause:** `renderer_set_aux_bloom_mode(TRUE/FALSE)` was called inside `StarFxSystem::draw_star()` for each individual star. Every call reset `aux_batch_count` back to 0, so only the last star in the loop was ever captured into the auxiliary batch. The streak pass needs all stars accumulated to produce a visible blur.

**Fix:** Moved the aux bloom mode toggle to wrap the *entire* star drawing loop in `game.cpp`:
```cpp
renderer_set_aux_bloom_mode(TRUE);   // before the star loop
// ... draw all stars ...
renderer_set_aux_bloom_mode(FALSE);  // after the loop
```

---

## Bug 2: Streak pass gated behind `bloom_enabled` flag
**Date:** 2026-06-17  
**Area:** SDL GPU renderer frame graph  
**Symptom:** Enabling "Streaks enabled" in STAR FX panel had no effect when HDR BLOOM was unchecked.  
**Root cause:** The entire offscreen frame graph (scene RT, aux sprite render, blur, streak, composite) was only entered if `g_sdl.bloom_enabled == TRUE`. The streak pass was nested inside this block, so it could never run independently.

**Fix:** Decoupled the offscreen pass decision:
```cpp
b8 need_bloom  = g_sdl.bloom_enabled;
b8 need_streak = g_sdl.streak_enabled && aux_count > 0;
if (use_offscreen && (need_bloom || need_streak)) { /* run offscreen passes */ }
```
Bloom extract/blur passes are skipped when only streak is active; composite sets bloom intensity to 0 in that case.

---

## Bug 3: `aux_batch_count` overloaded as both sentinel and actual count
**Date:** 2026-06-17  
**Area:** SDL GPU renderer backend (`renderer_backend_sdlgpu.cpp`)  
**Symptom:** Streak effect still invisible even after fixing Bug 1 and Bug 2.  
**Root cause:** `aux_batch_count` served two incompatible roles:
- Value `<= BS_MAX_SPRITES` → "capture is active, count = value"
- Value `BS_MAX_SPRITES + 1` → "capture is disabled"

`renderer_set_aux_bloom_mode(FALSE)` set `aux_batch_count = BS_MAX_SPRITES + 1`. This happened at the *end* of the star drawing loop, **before** `renderer_end_frame()` ran. By the time `end_frame()` checked `aux_batch_count`, it was already the "disabled" sentinel, yielding `aux_count = 0`. The streak pass was skipped because it thought no aux sprites existed.

**Fix:** Introduced an explicit `b8 aux_bloom_mode` flag separate from `aux_batch_count`:
- `aux_bloom_mode` → tracks whether capture is currently active
- `aux_batch_count` → actual number of captured sprites (no sentinel value)

`set_aux_bloom_mode(TRUE)` sets flag = true and resets count = 0.  
`set_aux_bloom_mode(FALSE)` sets flag = false but **preserves** the count.  
`draw_sprite()` only copies to the aux batch when the flag is true.  
`end_frame()` submits the aux sprites, then resets both flag and count.

---

## Bug 4: Shaders not recompiled / staged after edits
**Date:** 2026-06-17  
**Area:** Build pipeline (`build-all.bat`)  
**Symptom:** Shader code changes had no visual effect even though source files were edited.  
**Root cause:** `build-all.bat` only copies the `assets/` tree into `bin/assets/` but does **not** compile HLSL shaders. The game loads pre-compiled DXIL/SPIR-V blobs from `bin/assets/shaders/dxil/` and `bin/assets/shaders/spirv/`. If those binaries are stale, source edits are silently ignored.

**Fix:** Manually ran shader compilation scripts and copied the resulting `.dxil`/`.spv` files into `bin/assets/shaders/`.  
**Future improvement:** Integrate shader compilation into `build-all.bat` so it is automatic.

---

## Bug 5: Star size / streak length discontinuity during zoom-in (floating-origin rebase)
**Date:** 2026-06-17  
**Area:** Star FX rendering (`game.cpp` system view loop)  
**Symptom:** Stars abruptly changed size and streak length when zooming in, even though no zoom key was pressed. The jump happened at a consistent zoom threshold.

**Root cause:** `dist_scale` was computed from `vec2_length(sys_pos)`, where `sys_pos` is the *world-space compressed* position relative to `camera_hierpos`. When `galaxy_camera_rebase()` folds accumulated `camera.position` into `camera_hierpos` (which happens once `compression_factor(zoom) >= 1.0`), every star's `sys_pos` changes discontinuously. The star's actual *screen* position never moves, but the world-space coordinate jumps, so `dist_from_center` jumps, and `total_scale` pops.

**Fix:** Replaced world-space distance with screen-space pixel distance, which is invariant under floating-origin shifts:
```cpp
Vec2 star_screen = camera2d_world_to_screen(&s->camera, s->fb_width, s->fb_height, star_pos);
Vec2 screen_center = Vec2{ (f32)s->fb_width * 0.5f, (f32)s->fb_height * 0.5f };
f32 dist_from_center = vec2_length(vec2_sub(star_screen, screen_center));
```
Retuned `STAR_DIST_SCALE_FACTOR` from `0.00001f` (world units) to `0.001f` (pixels) to match the new coordinate space.
