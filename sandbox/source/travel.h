#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>
enum TravelEaseMode {
    TRAVEL_EASE_LINEAR = 0,
    TRAVEL_EASE_SMOOTHSTEP,
    TRAVEL_EASE_QUAD_IN_OUT,
    TRAVEL_EASE_COUNT
};
struct TravelState {
    bs_math::HierPos2 origin;
    bs_math::HierPos2 destination;
    bs_math::HierPos2 current;
    f32 progress;              // 0..1 from origin to destination
    f32 speed;                 // fraction per second
    b8  active;
    b8  paused;
    TravelEaseMode ease_mode;  // how progress maps to position
    // Cached high-precision world-space position for diagnostics.
    f64 world_x;
    f64 world_y;
};
void travel_init(TravelState* ts, bs_math::Vec2 ship_origin, bs_math::Vec2 dest_world);
void travel_update(TravelState* ts, f32 dt);
void travel_reset(TravelState* ts);
void travel_set_destination(TravelState* ts, bs_math::Vec2 dest_world);
