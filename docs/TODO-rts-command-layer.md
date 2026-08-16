# TODO — RTS command layer (temporary)

**Delete this file when the two open items below are done.** It is a hand-off note, not
architecture; the permanent record lives in `docs/architecture/sandbox/{RtsControl,FleetControl,
InWorldOverlays}.md` and `docs/architecture/engine/UiFacade.md`.

Covers the command-overlay work: escorts active by default, the Space overlay with 0.25x
dilation, box/click selection, the fleet roster, stances, standing orders, the minimum-separation
pass, and the roster's migration to the RML HUD.

---

## 1. Formation editor — not started

A separate window where fleet ships are boxes the player drags to define a fixed geometry, and
those offsets become the formation the fleet holds.

Design constraints, **one of them now reopened**:

- ~~Self-drawn, like `weapon_hub` and `fleet_roster`~~ — the roster is an **RML HUD panel** now
  and the `BS_RML_*` capacity objection no longer applies. RML (drag events exist — the Modules
  bay uses them) vs self-drawn (weapon_hub pattern; needs its own cursor arbitration again) is
  an open decision; bring a recommendation before starting.
- **`_update` + `_draw` split with one shared geometry** if self-drawn, for the reason
  `weapon_hub` documents.
- **Offsets consume into `Fleet::order_move`**, replacing the automatic centred grid
  (`FORMATION_SPACING_MUL`) when a formation is defined, rotated into the move order's forward
  axis exactly as the auto-grid already is. Authored offsets must respect the minimum-separation
  ring (`FLEET_MIN_SEPARATION_MUL`, sum of both hulls' bounding radii) or the post-pass will
  fight the formation.
- Storage in the fixed `FleetShip m_ships[FLEET_MAX_SHIPS]` array or a parallel fixed array —
  **never a growable container** (documented pointer-stability guarantee).

## 2. Shift-RMB avoid vs LSHIFT alt-movement toggle — decision pending

`game.cpp` (~2273) toggles the alternative mouse-follow movement scheme on every LSHIFT press
while the camera is attached, so a shift-RMB avoid issued mid-pilot also flips the flight
controls (shift-clicks on the roster trigger it too). Reproduced live, twice. Options:
(a) gate the toggle on `!command_overlay_active` — one line, consistent with the overlay
already claiming LMB for its duration; or (b) move avoid to Ctrl-RMB (Ctrl is only used for
Ctrl+X). Recommendation on file: (a). Needs a user decision before changing either binding.

## Done since the last revision of this note

- **Standing-order bindings** (RMB escort / shift-RMB avoid under the overlay, F rally) —
  implemented in `RtsControls::update`, verified live end-to-end.
- **Minimum fleet separation** — `steering::separation` + a velocity-only post-pass in
  `Fleet::update_autopilot`; bounding circles never overlap; verified live (rally converges to
  a spread ring, no stacking). Includes the escort/avoid **double-integration fix**
  (`steering::control_face`; `FleetShip::simulate` is the single pose integrator).
- **Fleet roster → RML HUD** — rows/chips in `bs_rml_hud_state` (`BS_RML_ROSTER_MAX`),
  markup in `assets/ui/hud.rml`, actions `"fsel:N"`/`"fstance:N:S"` in `game_push_hud`'s drain;
  `render/fleet_roster.*` deleted; the inverted `sim/`→`render/` roster edge is gone.
- **Defects**: flagship named at spawn ("Flagship"); the HUD tier readout no longer claims
  "Pause" at 0.25x (a dilated 0<ts<1 highlights no tier); the row-5 stance-chip rendering oddity
  is moot — that renderer is deleted, and all five RML rows render identically.
