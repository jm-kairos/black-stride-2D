#pragma once

#include <defines.h>

// =====================================================================================
// Weapon micro-selection hub — a radial selector held open with the MIDDLE MOUSE BUTTON,
// blooming AROUND THE CURSOR at the point the button went down.
//
//        [ N ]          N / E / W are the piloted ship's first three MOUNTED WEAPONS in
//   [ W ][ + ][ E ]     hardpoint order; S is always "All", the default fire-the-active-
//        [All]          group behaviour. A hull carrying more than three weapons reaches
//                       the extras through "All" (or the number-row fire groups).
//
// Press opens, the cursor direction from the hub center highlights a slot, release commits
// it. Picking a weapon sets `Ship::weapon_override`; picking "All" clears it. The hub only
// ever reads and writes that one field — group membership is left untouched, so restoring
// "All" restores exactly the behaviour that was there before.
//
// Anchoring on the press point keeps the eye where it already was and makes the readout read
// correctly: the frozen aim target IS that point, so the hub sits on the thing being shot at
// and every tile answers "can this weapon hit *here*". A press near a screen edge slides the
// whole hub inward so no slot is cropped.
//
// Input state lives on `game_state` (weapon_hub_*); the committed result lives on the Ship.
// Nothing here touches flight control: the hub polls one mouse button and the ship keeps
// flying, turning and firing while it is open.
// =====================================================================================

struct game_state;
struct Ship;

#define WEAPON_HUB_SLOTS 4   // 0 = North, 1 = East, 2 = West, 3 = South ("All")

// Poll BUTTON_MIDDLE and drive the hub for `piloted`: open on press (freezing the target the
// range readout is judged against), track the highlighted slot while held, commit on release.
// The CALLER owns the gating — piloting mode, edit mode, and the two UI cursor-capture checks
// — and must call weapon_hub_close() if a gate closes while the hub is open.
void weapon_hub_update(game_state* s, Ship* piloted);

// Force the hub shut WITHOUT committing the highlighted slot. Safe to call when closed.
void weapon_hub_close(game_state* s);

// Draw the hub (no-op while closed). Called from draw_gameplay_overlays.
void weapon_hub_draw(game_state* s);
