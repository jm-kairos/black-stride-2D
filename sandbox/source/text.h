#pragma once

#include <defines.h>
#include <renderer/renderer_types.h>

// =====================================================================================
// text — a minimal screen-anchored bitmap-text layer for the prototype HUD.
//
// Bakes the embedded 8x8 font (font8x8.h) into a single RGBA8 GPU atlas at init, then draws
// strings as one alpha-blended textured quad per glyph. Text is authored in SCREEN space
// (origin top-left, +y DOWN, pixels) and stays pinned to the screen at any camera pan, zoom,
// or rotation: each glyph quad is placed in world space so the live view-projection maps it
// back to the requested screen pixel, axis-aligned and pixel-sized. This is the same
// camera-cancel trick the upcoming UI panels use, proven here on plain text first.
//
// Monospace: every glyph advances 8*scale screen pixels; '\n' starts a new line 8*scale down.
// Only the printable basic-Latin range (0x20..0x7E) is encoded; other bytes advance blank.
//
// Lifecycle: call text_init() once AFTER renderer_initialize(); text_shutdown() at teardown.
// =====================================================================================

// Bake the font atlas and upload it as a GPU texture. Safe to call once. Returns FALSE if the
// renderer is down or the atlas upload fails (text_draw then becomes a no-op).
b8 text_init(void);

// Release the atlas texture. After this, text_draw is a no-op until text_init is called again.
void text_shutdown(void);

// Screen-space width (pixels) of the longest line in `str` at `scale` (scale 1 => 8px glyphs).
f32 text_width(const char* str, f32 scale);

// Screen-space height (pixels) of `str` at `scale`: line_count * 8 * scale.
f32 text_height(const char* str, f32 scale);

// Draw `str` with its top-left at screen pixel (x, y) — origin top-left, +y DOWN — tinted
// `color`, on render `layer` (higher = on top). `cam` plus the framebuffer size are the LIVE
// values for this frame; text_draw inverts them so the result is pinned to the screen
// regardless of how the camera is panned/zoomed/rotated. Emits one quad per non-blank glyph.
void text_draw(const char* str, f32 x, f32 y, f32 scale, bs_color color,
               const Camera2D* cam, u16 fb_width, u16 fb_height, u32 layer);
