# TODO — RTS command layer (temporary)

**Delete this file when the two unfinished items below are done.** It is a hand-off note, not
architecture; the permanent record lives in `docs/architecture/sandbox/{RtsControl,FleetControl,
InWorldOverlays}.md`.

Covers the command-overlay work: escorts active by default, the Space overlay with 0.25x
dilation, box/click selection restored, the fleet roster, stances, and the standing-order
simulation.

---

## 1. Standing orders have no input binding — **the sim half exists, the input half does not**

`Fleet::order_escort`, `order_avoid` and `order_rally` are implemented, wired into
`update_autopilot`, and drive through `steering::standoff` / `steering::flee`. **Nothing calls
them.** There is currently no gesture that issues an escort or avoid order, so the behaviour is
unreachable in play despite being live code.

What is needed, in `RtsControls::update` (the module that owns input interpretation — orders are
issued through a `Fleet` method, per the subsystem's extension point):

- **Escort** — right-click a *friendly* hull while the overlay is up. The hover loop already
  distinguishes friendly (`m_hovered_ship_idx`) from hostile (`m_hovered_enemy_idx`), so the
  target resolution exists; only the dispatch is missing.
- **Avoid** — a modified right-click on a hostile (shift-RMB is unclaimed while the overlay is
  up). Note `order_avoid` clears any attack order on the same ship deliberately: running from
  something you are shooting is incoherent.
- **Rally** — a key, no target needed. `order_rally` is `order_escort` aimed at member 0.

Nothing in `Fleet` needs to change for any of these.

## 2. Formation editor — not started

A separate window where fleet ships are boxes the player drags to define a fixed geometry, and
those offsets become the formation the fleet holds.

Design constraints already settled:

- **Self-drawn, like `weapon_hub` and `fleet_roster`.** Not RmlUi (its HUD snapshot carries
  fields for exactly one ship; a roster would mean new `BS_RML_*` capacities and engine-side
  work) and not ImGui (`bs_ui` exposes no drag handle or canvas). Drawing it in screen space with
  quads, lines and bitmap text keeps the whole feature sandbox-side with **no new engine
  exports** — that is the property to preserve.
- **`_update` + `_draw` split with one shared geometry**, for the same reason `weapon_hub`
  documents: a panel whose boxes are laid out twice will eventually drag a different ship from
  the one under the cursor.
- **Offsets consume into `Fleet::order_move`.** It currently computes an automatic centred grid
  (`FORMATION_SPACING_MUL`, cols = ceil(sqrt(n))). The authored offsets should replace that grid
  when a formation has been defined, rotated into the move order's forward axis exactly as the
  auto-grid already is.
- Storage must live in the fixed `FleetShip m_ships[FLEET_MAX_SHIPS]` array or a parallel fixed
  array — **never a growable container.** `CombatEntity` holds raw `Ship*` and the fixed array is
  a documented pointer-stability guarantee.

## 3. Defects to adjust

- **Row 5's stance chip renders differently from rows 1–4** in the roster. Both the flagship and
  every escort are created through `Fleet::add()`, which sets `FLEET_STANCE_AGGRESSIVE`, so the
  cause is not the stored value. Pixel sampling of the capture disagreed with what the image
  shows, so *neither reading is trustworthy yet* — this wants a look at the live panel, not
  another screenshot measurement.
- **The flagship row shows no name** — it renders as `1` where escorts render `2 Escort`. Its
  `Ship::vessel_name` appears to be empty; the escorts' is set at spawn.
- **The HUD speed tier highlights "Pause" at 0.25x.** The tier buttons match on 0/1/3/5/10 and
  0.25 matches none, so the readout claims the game is paused while it is dilated. Either add a
  dilated state to the tier display or have the overlay suppress the tier highlight.

## 4. Smaller follow-ups

- `RtsControl` now includes a `render/` header (`fleet_roster.h`) for `fleet_roster_wants_mouse`.
  That is the **second** inverted `sim/` → `render/` edge in this module, alongside
  `galaxy_pick_planet`. Both are recorded as tech debt; if a third appears it is worth extracting
  a small input-arbitration seam instead.
- The roster's `wants_mouse` is polled by `RtsControls` because the panel is drawn over the world
  rather than in a layer that arbitrates for itself. Any further self-drawn panel will need the
  same treatment — consider a shared "screen panel owns the cursor" helper before the third one.
- Stances are reachable **only** through the roster chips. If the overlay ever gains a keyboard
  layer, stance keys are the obvious first candidates.
