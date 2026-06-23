#include "travel.h"
using namespace bs_math;
// Ease function: maps raw progress [0,1] to eased progress [0,1].
static f32 travel_ease(f32 t, TravelEaseMode mode) {
    switch (mode) {
    case TRAVEL_EASE_SMOOTHSTEP:
        return t * t * (3.0f - 2.0f * t);
    case TRAVEL_EASE_QUAD_IN_OUT: {
        if (t < 0.5f) return 2.0f * t * t;
        f32 u = t - 1.0f;
        return 1.0f - 2.0f * u * u;
    }
    default:
    case TRAVEL_EASE_LINEAR:
        return t;
    }
}
void travel_init(TravelState* ts, Vec2 ship_origin, Vec2 dest_world) {
    if (!ts) return;
    ts->origin      = hierpos_from_vec2(ship_origin, BS_HIERPOS_CELL_SIZE);
    ts->destination = hierpos_from_vec2(dest_world,  BS_HIERPOS_CELL_SIZE);
    ts->current     = ts->origin;
    ts->progress    = 0.0f;
    ts->speed       = 0.15f;
    ts->active      = TRUE;
    ts->paused      = FALSE;
    ts->ease_mode   = TRAVEL_EASE_LINEAR;
    hierpos_to_f64(&ts->current, BS_HIERPOS_CELL_SIZE, &ts->world_x, &ts->world_y);
}
void travel_update(TravelState* ts, f32 dt) {
    if (!ts || !ts->active || ts->paused) return;
    ts->progress += ts->speed * dt;
    if (ts->progress >= 1.0f) {
        ts->progress = 1.0f;
        ts->current  = ts->destination;
        ts->active   = FALSE;
    } else {
        f32 eased = travel_ease(ts->progress, ts->ease_mode);
        ts->current = hierpos_lerp(&ts->origin, &ts->destination, eased, BS_HIERPOS_CELL_SIZE);
    }
    hierpos_to_f64(&ts->current, BS_HIERPOS_CELL_SIZE, &ts->world_x, &ts->world_y);
}
void travel_reset(TravelState* ts) {
    if (!ts) return;
    ts->progress = 0.0f;
    ts->current  = ts->origin;
    ts->active   = TRUE;
    hierpos_to_f64(&ts->current, BS_HIERPOS_CELL_SIZE, &ts->world_x, &ts->world_y);
}
void travel_set_destination(TravelState* ts, Vec2 dest_world) {
    if (!ts) return;
    ts->destination = hierpos_from_vec2(dest_world, BS_HIERPOS_CELL_SIZE);
}
