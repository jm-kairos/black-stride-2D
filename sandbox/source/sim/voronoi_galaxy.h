#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>
#include <renderer/renderer_types.h>
// =====================================================================================
// Voronoi Galaxy Map
// =====================================================================================
// Wraps JCash's jc_voronoi.h to partition the galaxy into star-system territories.
// Uses Fortune's sweep-line algorithm (O(N log N)) for ~50 star systems.
#define VORONOI_MAX_SITES 64
// Render layer constants (must match game.cpp)
static const u32 VORONOI_LAYER_CELESTIAL = 2;
static const u32 VORONOI_LAYER_UI        = 50;
struct StarSystem;   // forward declared to avoid circular include with game.h
struct game_state;   // forward declared
struct GalaxyVEdge {
    bs_math::Vec2 p0, p1;       // world-space endpoints
    i32 site_0, site_1;         // adjacent site indices (-1 for border)
    b8 is_border;               // true if edge touches the bounding box
};
struct GalaxyDEdge {
    i32 site_a, site_b;         // connected site indices in Delaunay dual
};
struct GalaxyVCell {
    bs_math::Vec2 center;       // site position (star galaxy position)
    bs_math::Vec2 verts[32];    // polygon vertices (counter-clockwise)
    i32 vert_count;
    bs_math::Vec2 bbox_min;
    bs_math::Vec2 bbox_max;
};
struct GalaxyVoronoi {
    GalaxyVEdge     edges[512];
    i32             edge_count;
    GalaxyDEdge     delaunay_edges[512];
    i32             delaunay_edge_count;
    GalaxyVCell     cells[VORONOI_MAX_SITES];
    i32             num_sites;
    bs_math::Vec2   bbox_min;
    bs_math::Vec2   bbox_max;
    // ---- Hover effect state (written by voronoi_cell_hover_effect.cpp) ----
    i32             hovered_cell;    // -1 = none, else site index
    f32             hover_head_dist; // arc-length distance of bright head along cell perimeter
};
// Generate Voronoi diagram from star system positions.
// Must be called after systems[] and system_count are initialized.
void generate_galaxy_voronoi(const StarSystem* systems, i32 system_count,
                             GalaxyVoronoi* out);
// Find which system's Voronoi cell contains the given galaxy position.
// Returns -1 if no cell contains the point (should not happen with valid bbox).
i32 find_system_by_cell(const bs_math::HierPos2* pos,
                        const GalaxyVoronoi* v,
                        const StarSystem* systems);
// Draw all Voronoi cell edges as wireframe lines.
void draw_voronoi_edges(const GalaxyVoronoi* v, const game_state* s,
                        bs_color edge_col, f32 thickness);
// Draw Delaunay dual edges (lane connectivity) between adjacent systems.
void draw_delaunay_lanes(const GalaxyVoronoi* v, const StarSystem* systems,
                         const game_state* s, bs_color lane_col, f32 thickness);
