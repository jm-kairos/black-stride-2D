# Black Stride — Map Generation Design Notes (DRAFT / scratch)

> **Status: DESIGN DISCUSSION ONLY — nothing here is built or committed to.**
> Space travel / the map is a **future phase**, explicitly out of scope for prototype-1
> (see `blackstride-prototype-spec`). This file parks the design thinking so we can resume
> when map generation comes up again. No code until a slice is chosen and forks are answered.
>
> Context when written: the prototype proved its single pillar — the zoom-driven
> `mode::local` ↔ `mode::global` switch (interior tilemap ↔ inertial-flight ship in open
> space). This doc is about giving that "open space" actual *content* and a galaxy above it.

---

## 1. The core reframe

Map generation's real job is **giving the empty global-mode plane structure**, not building a
separate map screen. Today `mode::global` flies the ship over an infinite empty plane + debug
grid. Map-gen fills that plane with content and adds coarser scales above it.

**The elegant move: extend the proven zoom-mode pillar to MORE scales** instead of inventing a
parallel map UI. Same hysteresis-latched zoom thresholds, same `roof_alpha`-style cross-fade,
same camera-target lerp.

```
mode::local    (BUILT)  inside the ship, tile interior
mode::global   (BUILT)  ship flying in open space
mode::system   (NEW)    zoom out -> ship becomes a dot; star + planets + belts + jump points appear
mode::cluster  (NEW)    zoom out -> the whole system becomes one node in a graph of systems (hyperspace)
```

Each coarser scale renders the finer one as a single abstracted glyph — exactly how `global`
already draws the whole interior as one roof silhouette. Camera target lerps
crew -> ship -> system-center -> cluster as you zoom out.

---

## 2. THE critical technical boundary (most important decision)

- **`local <-> global <-> system` can share ONE continuous world-coordinate space.**
  The star sits at the world origin; ship + planets + belts share the same f32 plane.
  Pure zoom + cross-fade, identical to what's built today.

- **`system <-> cluster` MUST be a coordinate-space SWITCH, not a literal zoom.**
  f32 precision dies past ~100k units. You cannot hold planets AND a light-years-wide galaxy
  in the same plane. So the cluster/hyperspace map is its **own coordinate space**, and
  crossing that boundary is a context switch with a cross-fade (not continuous zoom).

- **That boundary is exactly where TRAVEL lives:** engage a jump point -> fade out to the
  cluster map -> move along a lane / fly hyperspace -> fade into the destination system's space.
  This mirrors Starsector precisely (continuous system-space contained in continuous
  hyperspace), which is the stated reference game.

---

## 3. Generation approach: layered + deterministic, seed-derived

**Spine = hierarchical, seed-based, LAZY generation.** One architectural commitment that pays
off everywhere.

- **Master seed -> per-system seed -> per-body seed** via a hash (`splitmix64` / PCG).
  NOTE: the engine has **no PRNG yet** — this is a small new `math/` or `core/` utility to add.
- **Lazy:** don't generate N systems up front. The cluster graph is cheap (points + edges +
  a seed per node); a system's *contents* materialize from its seed only on first visit.
- **Save/load payoff (deferred in spec, but DECIDE NOW):** if generation is deterministic from
  `seed + a small delta of player-caused changes`, you persist *the seed + deltas*, not a
  serialized galaxy. Committing to determinism early is cheap; retrofitting it later is brutal.
  **Strong recommendation: bake in determinism from the very first map slice.**

### Cluster pipeline (top-level map)
1. **Point distribution** — where systems sit. NOT pure random (want clumps + voids).
   - Option A: **Poisson-disc / blue-noise** (even-but-organic spacing).
   - Option B: **density field with a few peaks** — fits "a cluster of star systems" literally;
     gives arms/voids that feel like a real stellar neighborhood.
2. **Connectivity** — the robust well-worn recipe:
   **Delaunay triangulate the points -> take the Minimum Spanning Tree** (guarantees no
   unreachable system) **-> add back ~15-30% of remaining Delaunay edges** (loops + alternate
   routes). Connected but interesting.
3. **Attributes** — per-system rolls from `rng_hash(seed, i)`: star type/color, richness,
   danger, faction. Optional spatial second pass (Voronoi / flood-fill over the points) for
   coherent faction territories.

### System pipeline (lazy, per-system seed)
- Star type from a weighted table (M-dwarfs common, O/B rare)
- Orbital rings (Titius-Bode-ish or jittered radii)
- Bodies by zone: rocky inner / gas outer / ice far + moons + belts
- Stations at interesting points
- Jump points placed from the cluster edges (one per outgoing lane)

---

## 4. How it fits the engine (conventions to honor)

- **POD structs + free functions** (`worldgen_*`, `cluster_*`, `system_*`) — C-with-structs,
  no OOP, matching the codebase.
- **Data-vs-code ethos** (like `ship.tmap`): the seed + gen *parameters* are authored data;
  the graph is the generated artifact. Keep an authoring seam open.
- **Memory:** cluster graph is long-lived -> `bs_memory_allocator(MEMORY_TAG_GAME)`;
  Delaunay / temp scratch -> frame or temp arena (never keep an arena pointer across frames).
- **Rendering needs NOTHING new for a first pass:** systems = quads/circles, lanes = existing
  `draw_line`, all on the sprite path with sensible new layers. Pure reuse.
- PRNG utility is the only genuinely new low-level primitive required.

---

## 5. Open forks — MUST be answered before building (they change the architecture)

1. **Travel model — continuous or graph?** *(FOUNDATIONAL — determines everything below.)*
   Starsector-style (fly inside systems AND fly continuous hyperspace between them) vs.
   FTL-style (pick a node, jump, no between-space). The existing inertial-flight global mode
   strongly implies **continuous**.
2. **Scale / count** — ~10 systems? ~50? ~200? Drives whether lazy gen matters + graph weight.
3. **Procedural vs. authored vs. hybrid** — the two references pull opposite ways (Starsector
   procedural sector; Kenshi handcrafted world). Fully seed-procedural, or procedural skeleton
   + a few authored set-piece systems?
4. **Static vs. living** — fixed once generated, or evolving (factions, economies)? Prototype
   answer is surely static, but keep the data model from precluding dynamism.
5. **Player verb at the cluster scale** — just navigation, or selection/orders/strategic layer?
   (Spec left "global-mode interactions" open; the map hosts whatever that becomes.)

---

## 6. Suggested incremental slices (mirror how the prototype was scoped: one transition at a time, screenshot-verified)

- **Slice 1 — `mode::system`:** zoom out past global; ship shrinks to a dot; a SINGLE
  deterministically-generated star system (star + a few ringed planets + jump-point markers)
  appears in the **same world space**. Reuse zoom/hysteresis/cross-fade. **No cluster graph,
  no travel yet** — just prove "zoom out reveals a generated system around the ship," exactly
  as the prototype proved "zoom out reveals the roof."
- **Slice 2 —** add the cluster graph + the `system <-> cluster` coordinate-space switch.
- **Slice 3 —** add travel along lanes (the space-switch + fade is the travel act).
- **Slice 4 —** add attributes / factions (per-system rolls + optional territory pass).

---

## 7. Resume checklist (when we pick this up again)

- [ ] Answer fork #1 (continuous vs. graph) and #2 (rough size) — minimum to proceed.
- [ ] Decide determinism commitment (recommended: YES, from slice 1).
- [ ] Promote this to a real design doc + patch `blackstride-prototype-spec` to open the next
      phase BEFORE any code.
- [ ] Add the PRNG utility (`rng_hash` / splitmix64 or PCG) — the one new low-level primitive.
- [ ] Build Slice 1 via the `blackstride-build-verify` loop; verify with the time/zoom harness.
