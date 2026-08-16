#pragma once
#include <defines.h>

struct game_state;

// DEBUG: HierPos2 cell-grid overlay toggle. Set from the COORDINATE SPACE editor panel; read
// by game_render to gate draw_hierpos_cell_grid. Off by default.
extern b8 g_debug_cell_grid;

// DEBUG: HierPos2 cell-grid overlay. Visualizes the hierarchical coordinate lattice the
// simulation lives on (cell boundaries around the flagship, a parity checkerboard, per-cell
// labels, and a HUD readout). Every line is routed through render_from_hierpos so a broken
// floating-origin rebase makes the grid shear against the ships. Off by default; toggled
// from the COORDINATE SPACE editor panel.
void draw_hierpos_cell_grid(const game_state* s);
