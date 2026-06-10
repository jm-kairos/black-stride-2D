// Standalone check: decode each installed ship texture with the ENGINE'S OWN
// vendored stb_image.h, forcing 4-channel RGBA exactly like renderer_load_texture.
// If this prints OK 4x, the engine's decode step cannot fail on these files.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#include <cstdio>

int main() {
    const char* files[] = {
        "assets/textures/wall_texture.png",
        "assets/textures/hull_texture.png",
        "assets/textures/window_texture.png",
        "assets/textures/door_texture.png",
    };
    int fail = 0;
    for (int i = 0; i < 4; ++i) {
        int w = 0, h = 0, ch = 0;
        stbi_uc* px = stbi_load(files[i], &w, &h, &ch, 4);
        if (!px || w <= 0 || h <= 0) {
            printf("FAIL  %-34s  (%s)\n", files[i], stbi_failure_reason());
            fail = 1;
        } else {
            // sample mean luma so we also confirm pixels are sane (not all-black)
            long long sum = 0;
            for (int p = 0; p < w*h; ++p)
                sum += (px[p*4]*299 + px[p*4+1]*587 + px[p*4+2]*114) / 1000;
            printf("OK    %-34s  %dx%d ch=%d meanLuma=%lld\n",
                   files[i], w, h, ch, sum / (w*h));
            stbi_image_free(px);
        }
    }
    printf(fail ? "\nRESULT: DECODE FAILURE\n" : "\nRESULT: ALL TEXTURES DECODE OK\n");
    return fail;
}
