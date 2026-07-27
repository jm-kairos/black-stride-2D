#pragma once
#include <defines.h>
#include <vector>
// =====================================================================================
// Endless-Sky-style starfield generation.
// Generates stars using a random walk (creates natural clusters/voids),
// sorts them into tiles for GPU culling, and packs them into interleaved
// vertex buffer data.
// =====================================================================================
struct Star {
    float x;          // position in wrapping field [0, fieldSize)
    float y;
    float size;       // 1.25..2.5 (world units)
    float brightness; // 0.05..0.85
    float r, g, b;    // color temperature (blue-white for bright, orange-red for dim)
};
class StarfieldGenerator {
public:
    static constexpr int FIELD_SIZE = 4096;
    static constexpr int TILE_SIZE  = 256;
    static constexpr int STARS_PER_LAYER = 3000;
    // Random-walk generation (creates natural clusters/voids).
    // `out` is cleared and filled with `count` stars.
    static void generate(std::vector<Star>& out, int count, int fieldSize, u32 seed);
    // Sort stars into tiles and build tile index for GPU culling.
    // tileIndex[i] = first star index in tile i.
    // tileIndex.back() = total star count.
    static void build_tile_index(const std::vector<Star>& stars,
                                  std::vector<int>& tileIndex,
                                  int& tileCols, int& widthMod,
                                  int fieldSize = FIELD_SIZE,
                                  int tileSize  = TILE_SIZE);
    // Pack stars into interleaved vertex buffer data (6 verts per star).
    // Vertex format per vertex: offset_x, offset_y, size, corner_angle, r, g, b
    static void pack_vertices(const std::vector<Star>& stars,
                               const std::vector<int>& tileIndex,
                               std::vector<float>& outVertices,
                               int fieldSize = FIELD_SIZE,
                               int tileSize  = TILE_SIZE);
};
