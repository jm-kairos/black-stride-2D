#pragma once
#include <defines.h>
#include <math/bs_hierpos.h>

struct game_state;
struct EditSelection;
struct Ship;
enum EditDragMode : int;

// In-world entity editor: pick/drag lights and ships, plus the transform gizmo geometry.

// Edit-mode input: left-click selects an entity or gizmo handle under the cursor and begins a
// drag; holding repositions/rotates; releasing ends the drag. Gated on bs_imgui_wants_mouse.
void update_edit_mode(game_state* s);

// World position of an edit selection, or {0,0} for EDIT_NONE. Used by the gizmo renderer.
bs_math::HierPos2 edit_entity_position(const game_state* s, EditSelection sel);

// Which gizmo part (if any) is under the cursor for the current selection. Used by the gizmo
// renderer for hover feedback so the highlight matches update_edit_mode's hit-test.
EditDragMode edit_pick_gizmo(const game_state* s, bs_math::HierPos2 cursor);

// Gizmo geometry helpers (world space). Sizes are target screen pixels * zoom_inv so the
// gizmo stays a constant on-screen size at every zoom level.
f32 gizmo_axis_len(f32 zoom_inv);
f32 gizmo_ring_radius_ship(const Ship* ship, f32 zoom_inv);
f32 gizmo_ring_radius_light(f32 zoom_inv);
f32 gizmo_arrow_size(f32 zoom_inv);
