#include "text.h"
#include "font8x8.h"

#include <core/logger.h>
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
#include <math/math_utils.h>

using namespace bs_math;

// =====================================================================================
// Atlas geometry. The 95 printable glyphs are packed into a padded grid: each 8x8 glyph sits
// in a 10x10 cell with a 1px TRANSPARENT guard band on every side. The guard matters because
// the sampler is NEAREST + CLAMP_TO_EDGE (see the SDL GPU backend): when a glyph is drawn at a
// non-integer scale, sampling can land just outside the 8x8 footprint, and the guard makes that
// neighbor a transparent texel instead of an adjacent glyph. UVs address only the 8x8 interior.
// =====================================================================================
static const i32 GLYPH      = 8;                       // glyph footprint (px)
static const i32 PAD        = 1;                        // transparent guard band per side (px)
static const i32 CELL       = GLYPH + 2 * PAD;          // 10
static const i32 ATLAS_COLS = 16;                       // cells per row
static const i32 ATLAS_ROWS = (FONT8X8_COUNT + ATLAS_COLS - 1) / ATLAS_COLS; // 6 (>= 95 cells)
static const i32 ATLAS_W    = ATLAS_COLS * CELL;        // 160
static const i32 ATLAS_H    = ATLAS_ROWS * CELL;        // 60

// Module state. Lives for the program; text_draw is a no-op until text_init succeeds.
static struct {
    bs_texture atlas;
    b8         ready;
} g_text = { { BS_INVALID_HANDLE }, FALSE };

b8 text_init(void)
{
    if (g_text.ready) return TRUE;

    // Bake the atlas into static storage (38KB; not on the stack). RGB is white everywhere and
    // only alpha carries the glyph shape, so the tint controls the on-screen color and there is
    // no dark fringe even if the sampler ever interpolates. Empty/guard texels are fully clear.
    static u8 pixels[ATLAS_H * ATLAS_W * 4];
    for (i32 i = 0; i < ATLAS_H * ATLAS_W; ++i)
    {
        pixels[i * 4 + 0] = 255;
        pixels[i * 4 + 1] = 255;
        pixels[i * 4 + 2] = 255;
        pixels[i * 4 + 3] = 0;   // transparent by default
    }

    for (i32 gi = 0; gi < FONT8X8_COUNT; ++gi)
    {
        i32 cell_col = gi % ATLAS_COLS;
        i32 cell_row = gi / ATLAS_COLS;
        i32 px = cell_col * CELL + PAD;  // glyph's top-left texel in the atlas
        i32 py = cell_row * CELL + PAD;

        for (i32 r = 0; r < GLYPH; ++r)
        {
            u8 row_bits = g_font8x8[gi][r];
            for (i32 c = 0; c < GLYPH; ++c)
            {
                // font8x8 convention: bit 0 (LSB) is the LEFTMOST column.
                if (row_bits & (1u << c))
                {
                    i32 idx = ((py + r) * ATLAS_W + (px + c)) * 4;
                    pixels[idx + 3] = 255; // opaque ink
                }
            }
        }
    }

    g_text.atlas = renderer_create_texture(pixels, (u32)ATLAS_W, (u32)ATLAS_H);
    if (g_text.atlas.id == BS_INVALID_HANDLE)
    {
        BS_LOG_ERROR("text_init: failed to create font atlas texture.");
        return FALSE;
    }

    g_text.ready = TRUE;
    BS_LOG_INFO("text_init: font atlas ready (%dx%d, %d glyphs).", ATLAS_W, ATLAS_H, FONT8X8_COUNT);
    return TRUE;
}

void text_shutdown(void)
{
    if (g_text.atlas.id != BS_INVALID_HANDLE)
    {
        renderer_destroy_texture(g_text.atlas);
        g_text.atlas = bs_texture{ BS_INVALID_HANDLE };
    }
    g_text.ready = FALSE;
}

// Longest run of characters before a '\n' or the terminator, in glyph cells.
static i32 longest_line_cells(const char* str)
{
    i32 best = 0, cur = 0;
    for (const char* p = str; *p; ++p)
    {
        if (*p == '\n') { if (cur > best) best = cur; cur = 0; }
        else            { ++cur; }
    }
    if (cur > best) best = cur;
    return best;
}

f32 text_width(const char* str, f32 scale)
{
    if (!str || !*str) return 0.0f;
    return (f32)longest_line_cells(str) * (f32)GLYPH * scale;
}

f32 text_height(const char* str, f32 scale)
{
    if (!str || !*str) return 0.0f;
    i32 lines = 1;
    for (const char* p = str; *p; ++p) if (*p == '\n') ++lines;
    return (f32)lines * (f32)GLYPH * scale;
}

void text_draw(const char* str, f32 x, f32 y, f32 scale, bs_color color,
               const Camera2D* cam, u16 fb_width, u16 fb_height, u32 layer)
{
    if (!g_text.ready || !str || !cam || scale <= 0.0f || cam->zoom <= 0.0f) return;

    const f32 cell_px = (f32)GLYPH * scale;            // screen advance per glyph / line
    const f32 world_sz = cell_px / cam->zoom;          // world size that renders as cell_px on screen

    // UV footprint of one glyph's 8x8 interior in the atlas (top-left origin, y-down).
    const f32 uw = (f32)GLYPH / (f32)ATLAS_W;
    const f32 uh = (f32)GLYPH / (f32)ATLAS_H;

    f32 cursor_x = x;
    f32 cursor_y = y;

    for (const char* p = str; *p; ++p)
    {
        u8 uc = (u8)*p;
        if (uc == '\n') { cursor_x = x; cursor_y += cell_px; continue; }

        // Draw only printable, non-space glyphs; everything else just advances a blank cell.
        if (uc > (u8)' ' && uc >= (u8)FONT8X8_FIRST && uc <= (u8)FONT8X8_LAST)
        {
            i32 gi = (i32)uc - FONT8X8_FIRST;
            i32 cell_col = gi % ATLAS_COLS;
            i32 cell_row = gi / ATLAS_COLS;
            f32 u0 = (f32)(cell_col * CELL + PAD) / (f32)ATLAS_W;
            f32 v0 = (f32)(cell_row * CELL + PAD) / (f32)ATLAS_H;

            // Camera-cancel placement: pin the glyph's top-left to this exact screen pixel by
            // projecting it into world space, then null the view's rotation (sprite.rotation =
            // cam.rotation) and scale (size = screen_px / zoom). The live view-projection maps
            // the quad straight back to an axis-aligned, pixel-sized rectangle on screen, so the
            // text stays put under any pan / zoom / ship-heading rotation.
            Vec2 pivot = camera2d_screen_to_world(cam, fb_width, fb_height, Vec2{ cursor_x, cursor_y });

            bs_sprite q{};
            q.position = pivot;
            q.size     = Vec2{ world_sz, world_sz };
            q.origin   = Vec2{ 0.0f, 0.0f };   // pivot == glyph top-left
            q.rotation = cam->rotation;        // cancel the view rotation -> screen-upright
            q.uv       = bs_rect{ u0, v0, uw, uh };
            q.tint     = color;
            q.texture  = g_text.atlas;
            q.blend    = BLEND_ALPHA;
            q.layer    = layer;
            renderer_draw_sprite(&q);
        }

        cursor_x += cell_px;
    }
}
