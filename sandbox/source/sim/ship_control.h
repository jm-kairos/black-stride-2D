#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>

struct game_state;
struct FleetShip;

// Piloted-ship control + collision resolution.
//
// control_ship_global: pilot input (WASD heading-relative thrust, Q/E strafe, C brake, A/D or
// mouse-follow turn) -> flight velocities. Mutates ONLY velocities, never the pose (that is
// simulate_ship's job). Returns TRUE if a turn is actively commanded this frame so the simulator
// skips auto-stabilizing the spin.
b8 control_ship_global(game_state* s, FleetShip* pf, f32 dt);

// Push each fleet ship out of the immovable enemy hull (SAT MTV) and cancel the inward
// velocity component so ships slide along the hull instead of sticking or phasing through.
void resolve_ship_collision(game_state* s);

// World-space origin of the ship the player is currently controlling (piloted ship, defaulting
// to the flagship). Used for camera follow.
bs_math::HierPos2 piloted_ship_origin(game_state* s);
