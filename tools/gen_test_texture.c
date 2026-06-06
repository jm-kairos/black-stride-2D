// One-off asset generator: writes a 64x64 RGBA test texture whose four quadrants are
// distinctly colored (with an asymmetric mark) so any UV flip / orientation bug is obvious.
//   top-left  = red      top-right = green
//   bot-left  = blue     bot-right = yellow
// A white border frames it; a black bar in the top-left quadrant breaks symmetry.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <string.h>

int main(void)
{
    const int W = 64, H = 64;
    static unsigned char px[64 * 64 * 4];

    for (int y = 0; y < H; ++y)
    {
        for (int x = 0; x < W; ++x)
        {
            unsigned char r = 0, g = 0, b = 0, a = 255;
            int left = x < W / 2;
            int top  = y < H / 2;

            if (top && left)       { r = 220; g = 30;  b = 30;  } // red
            else if (top && !left) { r = 30;  g = 200; b = 30;  } // green
            else if (!top && left) { r = 40;  g = 60;  b = 220; } // blue
            else                   { r = 230; g = 210; b = 30;  } // yellow

            // White border (2px).
            if (x < 2 || x >= W - 2 || y < 2 || y >= H - 2) { r = g = b = 255; }

            // Black asymmetry bar: a horizontal stripe across the top-left red quadrant only.
            if (top && left && y >= 12 && y < 18 && x >= 6 && x < 26) { r = g = b = 0; }

            int i = (y * W + x) * 4;
            px[i + 0] = r; px[i + 1] = g; px[i + 2] = b; px[i + 3] = a;
        }
    }

    int ok = stbi_write_png("test_sprite.png", W, H, 4, px, W * 4);
    return ok ? 0 : 1;
}
