#pragma once

#include <defines.h>

struct game_state;

// =====================================================================================
// Fleet roster — the second selection surface, and the only place stance is set.
//
// Split into _update (input) and _draw (presentation) for the same reason weapon_hub is:
// the row hit-test and the row layout must be ONE geometry, or the panel selects a
// different ship from the one under the cursor. `game_update` owns the gating and calls
// _update; the overlay dispatcher calls _draw.
//
// Self-drawn in screen space (quads + lines + bitmap text) rather than routed through a UI
// toolkit. That is a deliberate boundary decision: the RmlUi HUD snapshot carries fields for
// exactly ONE ship and adding a roster to it would mean new `bs_rml_*` capacities and
// engine-side work, while the exported ImGui vocabulary has no row/chip widget. Drawing it the
// way weapon_hub already does keeps the whole feature sandbox-side with no new exports.
// =====================================================================================

// Read input: row clicks select, chip clicks set stance on the current selection. Call from
// game_update, gated on the UI not owning the cursor. No-op while the roster is hidden.
void fleet_roster_update(game_state* s);

// Submit the panel. Costs nothing when hidden.
void fleet_roster_draw(const game_state* s);

// TRUE when the cursor is over the panel, so the world beneath it does not also react to the
// click. The roster is drawn over the world, not in a UI layer that arbitrates for itself.
b8 fleet_roster_wants_mouse(const game_state* s);
