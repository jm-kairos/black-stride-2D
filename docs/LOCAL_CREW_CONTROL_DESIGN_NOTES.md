# Black Stride — `mode::local` Crew Control Redesign (DESIGN SLICE / scratch)

> **Status: DESIGN DISCUSSION ONLY — no code written, spec skill NOT yet patched.**
> This parks the agreed design direction so it can be picked up cold. The pivot below
> changes one of the prototype's seven "done" features (direct WASD crew movement) and
> brushes the spec's future-phase line ("AI behaviors / autonomous navigation"), so it is
> a deliberate scope graduation that needs the forks in §5 answered **before any code**.
> Nothing here is built. (Mirrors `docs/MAP_GENERATION_DESIGN_NOTES.md` in spirit.)
>
> Context when written: the prototype proved its pillar (zoom-driven `mode::local` ↔
> `mode::global`) and the ship is now a true rigid body — crew + collision live in
> **ship-local space** and ride the ship's full pose (origin AND angle). This doc is about
> graduating `mode::local` from placeholder direct-control to the intended Kenshi model.

---

## 1. The pivot in one line

**Replace direct WASD crew control with select-and-order + pathfinding** (Kenshi / colony-sim
model): left-click selects the crew member, right-click on a walkable tile issues a move
order, and the crew **navigates autonomously** along an A\* path through the ship interior.
Starts with one crew member; the model must not preclude many.

This is not a detour from the vision — it IS the vision. The spec's north stars are
"squad-based simulation… **Kenshi**." Kenshi has no direct control; you select and order.
Direct WASD was always prototype scaffolding (the cheapest thing that proved the zoom pillar).

---

## 2. Why this is the right direction (the non-obvious argument)

The headline reason is **not** "it matches the reference game." It's that indirect control is
what makes the two modes *coexist* instead of being two disconnected control schemes:

- With **direct** WASD you cannot drive a character and fly the ship at the same time. The
  moment you zoom out to pilot, the crew is inert — and we just spent a rigid-body refactor
  making the ship keep moving/coasting. "Ship coasts while you're zoomed in" has no actor.
- With **indirect** control you *order* a crew member to a destination, then zoom out and fly
  the ship — and because the crew executes its path autonomously, **it keeps walking its route
  inside the moving, rotating hull while you pilot**. Assign work below decks, then handle the
  helm. That emergent "ship as a living vessel" behavior is only possible with select-and-order.

That composition — indirect control + rigid-body crew + keep-simulating-both-modes — is the
real prize, and it falls out for free from work already on disk.

---

## 3. Architectural synergy — why this is cheaper than it looks

Two things that are normally the hard part are **already solved** in the current code:

1. **The tile grid is already a navmesh.** `ship_tile_is_solid(ship, col, row)` (ship.h) marks
   blocked tiles; floor/door are walkable. Grid **A\*** drops straight onto it. Ship-sized
   grids (the current map is 13×17 ≈ 221 tiles) make the search cost negligible.

2. **Crew already lives in ship-local space** (rigid-body refactor). A path is just a list of
   **ship-local tile centers** (`ship_tile_center_local`). The crew follows it in ship-local
   space, and under ship translation *and* rotation the path stays valid with **zero extra
   work** — same reason the crew rides the rigid body today. Pathfinding inherits that for free.

3. **The picking round-trip is half-built.** `camera2d_screen_to_world(cam, w, h, screen_px)`
   already exists (camera2d.{h,cpp}) and already accounts for `camera.rotation`. Since local
   mode sets `camera.rotation = ship.angle * (1 - roof_alpha)` (cancels heading when fully
   zoomed in), a screen click maps cleanly through this exact chain:
   ```
   screen_px --camera2d_screen_to_world--> world
              --ship_world_to_local-------> ship-local
              --ship_local_to_tile--------> (col,row)   then ship_tile_is_solid() to validate
   ```
   Every helper in that chain already exists. The pick **must** traverse the same transform
   the renderer uses, or clicks land on the wrong tile after the ship has rotated (classic bug).

So what's genuinely NEW is small: an A\* over the grid, a "follow this path" steering behavior,
selection/order input, and a free-roam camera rule. Everything else is reuse.

---

## 4. Trade-offs I actually worry about (in priority order)

1. **Camera decoupling — the big one; it touches the proven pillar.** Today the local camera
   *follows the crew* (`cam_local = ship_local_to_world(ship, crew.position)`, game.cpp ~L282),
   and the seamless zoom transition relies on ONE smoothly-lerped target (crew → ship origin).
   Select-and-order wants a **free-roaming** local camera (look around the ship independent of
   the crew, RimWorld/Kenshi style). That breaks the single-target assumption. New rule needed:
   e.g. zoom-out eases the camera toward the ship centroid (so the global hand-off still
   glides); zoom-in leaves the camera where the player parked it (or snaps to selected crew on
   a tap). Solvable, but it's the one piece that reaches into the working zoom machinery — treat
   with care, verify the cross-fade still glides.

2. **WASD repurposes, raising a consistency question.** With a free camera, the natural home for
   WASD in local is **camera panning** (RTS convention). Note the result: WASD *flies the ship*
   in global and *pans the camera* in local. Mode-contextual controls are common and fine, but
   make it a deliberate decision, not drift. (Alternative: middle-drag / edge-scroll to pan,
   leave WASD unbound in local.)

3. **Robotic paths.** Raw grid A\* gives staircase motion. For the first slice, continuous
   steering toward grid-center waypoints (the Kenshi/RimWorld approach — smooth motion
   *following* a grid path, reusing the existing accel/arrive feel) looks fine. Defer
   string-pulling / funnel smoothing.

4. **Multi-crew crowding — defer, don't wall off.** One crew member = no crew-crew collision, so
   none of the hard colony-sim problems (corridor blocking, local avoidance, mutual repathing)
   bite yet. The trap is building selection/movement so single-unit-specific that crew #2 forces
   a rewrite. Build the **selection abstraction** now (a `selected` index/handle, even for one
   unit); defer crowd avoidance entirely.

5. **Collision demotes to a safety net.** Nice simplification: if the path only routes through
   walkable tiles, the existing per-axis AABB-vs-tile sweep (`crew_blocked`, game.cpp) rarely
   fires — it just catches edge cases. Pathfinding becomes the primary navigation.

---

## 5. Forks to call BEFORE any code (these change the architecture)

Proposed **defaults** are my recommendations — confirm, or override individually.

| # | Fork | Options | **Proposed default** |
|---|------|---------|----------------------|
| 1 | **Input scheme** | left-select/right-move · click-select then click-move | **left-select, right-move** (genre standard; unambiguous) |
| 2 | **Local camera** | free-roam w/ WASD-pan · follow-selected-crew | **free-roam + WASD-pan** (enables the §2 coexistence win) |
| 3 | **Movement feel** | continuous steering along waypoints · discrete tile-hop | **continuous steering** (smooth; reuses accel/arrive) |
| 4 | **Diagonals** | 4-connected · 8-connected w/ corner-cut rules | **4-connected** (simplest; fine for tight ship corridors) |
| 5 | **Selection feedback** | how minimal for the prototype | **highlight selected + destination marker** (no text/UI) |

> Fork 1 + 2 are the ones I asked to lock last time — they gate everything. 3–5 are cheap to
> change later; the defaults keep the first slice small.

---

## 6. The first slice (scoped like every prior step: one transition, screenshot-verified)

> **Slice: replace WASD-crew with click-to-move.**
> Left-click selects the (single) crew member; right-click on a walkable tile runs A\* in
> ship-local space; the crew steers along the path. Local camera goes free with WASD-pan.

**Verification (the money shot):** order a move and watch the crew route *around* an interior
wall, *through a door*, to the target tile; THEN zoom out, fly + turn the ship, zoom back in —
the crew is still walking its path inside the moved/rotated hull. That single test proves the
indirect-control + rigid-body + keep-simulating composition all at once.

### Concrete touch-list (what changes vs. what's reused)
**New (sandbox side):**
- `pathfind_astar(ship, start_tile, goal_tile, out_path[])` — grid A\*, ship-local, 4-connected,
  walkable = `!ship_tile_is_solid`. Likely a new `nav.{h,cpp}` or a section of game.cpp.
- `Crew` gains a path: `Vec2 path[N]` (ship-local waypoints) + `i32 path_len, path_idx`.
- A `selected` flag/handle on game_state (even for one crew — the multi-crew seam).
- Steering: replace `read_wasd_dir()`/`update_crew_local()` order-source with "steer toward
  `path[path_idx]`, advance on arrival" (keep the existing accel/friction/`crew_blocked` sweep
  as the safety net).
- Input: left-click = pick (the §3 screen→tile chain, select if it hits the crew's tile);
  right-click = order (pick target tile, validate walkable, run A\*). Edge-trigger via
  `input_is_button_down(BUTTON_LEFT/RIGHT)` && `!input_was_button_down(...)`.
- Render: highlight selected crew + a destination marker quad (existing sprite/quad path; pick
  layers above `LAYER_CREW=5`, below `LAYER_ROOF=10`).

**Changed:**
- Local camera follow (game.cpp ~L279–285): crew-follow → free-roam target; keep the
  `roof_alpha` lerp toward `ship.origin` on zoom-out so the global hand-off still glides.
- WASD in local: crew-move → camera-pan (or unbind, per fork 2).

**Reused as-is (do NOT touch):** `ship_world_to_local`, `ship_local_to_tile`,
`ship_tile_center_local`, `ship_tile_is_solid`, `camera2d_screen_to_world`, the rigid-body
pose, the zoom/hysteresis/cross-fade machinery, global-mode flight.

---

## 7. Scope honesty

- This **changes prototype feature #3** (direct WASD crew movement) and touches the spec's
  future-phase "AI behaviors / autonomous navigation." That's the intended graduation, but it
  means **`blackstride-prototype-spec` must be patched to the new control model BEFORE building**
  — not after. (Not done yet; no code until forks called.)
- Still firmly OUT for this slice: multiple crew, crew-crew avoidance, room system, jobs/tasks,
  any RPG layer. Single crew, single order, single path.

---

## 8. Resume checklist (when we pick this up)

- [ ] Answer forks #1 and #2 (gating); accept/adjust #3–#5 defaults.
- [ ] Patch `blackstride-prototype-spec`: local control = select + pathfinding (was WASD-direct).
- [ ] Implement the §6 touch-list; build via `blackstride-build-verify` loop.
- [ ] Run the §6 verification: route around a wall through a door, then global turn + zoom-back
      shows the crew still walking inside the rotated hull.
- [ ] Only then consider: path smoothing, selection polish, crew #2 (separate future slice).
