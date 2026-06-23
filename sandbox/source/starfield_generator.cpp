#include "starfield_generator.h"
#include <stdlib.h>
#include <math.h>
// =====================================================================================
// Random-walk star generation (Endless Sky style).
// =====================================================================================
void StarfieldGenerator::generate(std::vector<Star>& out, int count, int fieldSize, u32 seed)
{
    out.clear();
    out.reserve(count);
    // Deterministic LCG for reproducibility.
    auto lcg = [](u32& s) -> u32 {
        s = s * 1103515245u + 12345u;
        return s;
    };
    // Precompute random-step offsets in an annulus.
    // Larger steps → bigger, more coherent clusters with dark voids between.
    static const int MAX_OFF = 120;
    static const int MAX_D   = MAX_OFF * MAX_OFF;  // 14400
    static const int MIN_D   = MAX_D / 9;           // 1600
    std::vector<int> off;
    off.reserve(MAX_OFF * MAX_OFF * 5);
    for (int x = -MAX_OFF; x <= MAX_OFF; ++x)
        for (int y = -MAX_OFF; y <= MAX_OFF; ++y)
        {
            int d = x * x + y * y;
            if (d < MIN_D || d > MAX_D)
                continue;
            off.push_back(x);
            off.push_back(y);
        }
    int widthMod = fieldSize - 1;
    u32 s = seed;
    int x = (int)(lcg(s) % (u32)fieldSize);
    int y = (int)(lcg(s) % (u32)fieldSize);
    for (int i = 0; i < count; ++i)
    {
        // Longer random walk → tighter clusters with real voids.
        for (int j = 0; j < 20; ++j)
        {
            int index = (int)(lcg(s) % (u32)(off.size() >> 1)) << 1;
            x += off[index];
            y += off[index + 1];
            x &= widthMod;
            y &= widthMod;
        }
        // Randomize sub-pixel position.
        u32 random = lcg(s);
        float jitterX = (float)(random & 15) * 0.0625f;
        float jitterY = (float)((random >> 8) & 15) * 0.0625f;
        float fx = (float)x + jitterX;
        float fy = (float)y + jitterY;
        // Power-law brightness: most stars are dim, a few are bright.
        // uniform [0,1] → pow(uniform, 2.5) skews heavily toward 0.
        float u = (float)((random >> 4) & 255) / 255.0f;
        float brightness = powf(u, 2.5f) * 0.90f + 0.05f;
        // Size correlates with brightness (bright stars are bigger).
        float sz = 2.0f + brightness * 4.0f;
        // Color temperature: brighter = bluer, dimmer = redder (Endless Sky style).
        float t = brightness / 0.95f; // 0..1
        float r, g, b;
        if (t > 0.75f) {          // hot / bright → blue-white
            r = 0.70f; g = 0.85f; b = 1.00f;
        } else if (t > 0.55f) {   // warm white
            r = 0.90f; g = 0.92f; b = 1.00f;
        } else if (t > 0.35f) {   // yellow-white
            r = 1.00f; g = 1.00f; b = 0.80f;
        } else if (t > 0.18f) {  // orange
            r = 1.00f; g = 0.70f; b = 0.50f;
        } else {                  // cool red / dim
            r = 1.00f; g = 0.55f; b = 0.35f;
        }
        out.push_back(Star{ fx, fy, sz, brightness, r, g, b });
    }
}
// =====================================================================================
// Build tile index for GPU culling.
// =====================================================================================
void StarfieldGenerator::build_tile_index(const std::vector<Star>& stars,
                                            std::vector<int>& tileIndex,
                                            int& tileCols, int& widthMod,
                                            int fieldSize, int tileSize)
{
    widthMod = fieldSize - 1;
    tileCols = fieldSize / tileSize;
    int totalTiles = tileCols * tileCols;
    tileIndex.clear();
    tileIndex.resize(totalTiles + 1, 0);
    // Count stars per tile.
    for (const auto& star : stars)
    {
        int tx = (int)star.x / tileSize;
        int ty = (int)star.y / tileSize;
        int idx = tx + ty * tileCols;
        tileIndex[idx]++;
    }
    // Prefix sum: tileIndex[i] = first star in tile i.
    // After this, tileIndex[totalTiles] = total star count.
    int running = 0;
    for (int i = 0; i < totalTiles; ++i)
    {
        int count = tileIndex[i];
        tileIndex[i] = running;
        running += count;
    }
    tileIndex[totalTiles] = running;
}
// =====================================================================================
// Pack stars into interleaved vertex buffer (6 vertices per star).
// =====================================================================================
void StarfieldGenerator::pack_vertices(const std::vector<Star>& stars,
                                        const std::vector<int>& tileIndex,
                                        std::vector<float>& outVertices,
                                        int fieldSize, int tileSize)
{
    int tileCols = fieldSize / tileSize;
    int totalStars = (int)stars.size();
    outVertices.resize(totalStars * 6 * 7); // 6 verts, 7 floats each (x,y,size,corner,r,g,b)
    // We need to copy tileIndex since we'll modify it during placement.
    std::vector<int> writeIndex(tileIndex.begin(), tileIndex.end());
    const float PI = 3.14159265358979323846f;
    const float CORNER[6] = {
        0.0f * PI,
        0.5f * PI,
        1.5f * PI,
        0.5f * PI,
        1.5f * PI,
        1.0f * PI
    };
    for (const auto& star : stars)
    {
        int tx = (int)star.x / tileSize;
        int ty = (int)star.y / tileSize;
        int tileIdx = tx + ty * tileCols;
        int vertIdx = writeIndex[tileIdx]++;
        float* dst = &outVertices[vertIdx * 6 * 7];
        // Tile-local position (the vertex shader adds translate = tilePos - cam).
        float localX = star.x - (float)(tx * tileSize);
        float localY = star.y - (float)(ty * tileSize);
        for (int c = 0; c < 6; ++c)
        {
            *dst++ = localX;
            *dst++ = localY;
            *dst++ = star.size;
            *dst++ = CORNER[c];
            *dst++ = star.r;
            *dst++ = star.g;
            *dst++ = star.b;
        }
    }
}
