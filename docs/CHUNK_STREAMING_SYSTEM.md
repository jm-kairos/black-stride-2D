# Chunk Streaming System

An in-depth reference for the chunk-based content streaming subsystem. This document is
written for engineers (and AI models) who want to understand the system well enough to
extend it — add entity types, gameplay interaction, persistence, or scale the generator.

**Primary sources**

| Concern | File |
| --- | --- |
| Public API, structs, constants | [sim/chunk_stream.h](../sandbox/source/sim/chunk_stream.h) |
| Streamer object, worker, generation, deltas, update loop | [sim/chunk_stream.cpp](../sandbox/source/sim/chunk_stream.cpp) |
| Draw pass, culling, fade, debug grid | [render/chunk_render.cpp](../sandbox/source/render/chunk_render.cpp) / [render/chunk_render.h](../sandbox/source/render/chunk_render.h) |
| Draw call site (below ships) | [render/scene_renderer.cpp](../sandbox/source/render/scene_renderer.cpp#L32) |
| Init + per-frame update | [game.cpp](../sandbox/source/game.cpp#L476) |
| Editor panel | [ui/editor_ui.cpp](../sandbox/source/ui/editor_ui.cpp#L313) |
| Owning field | [state/game_state.h](../sandbox/source/state/game_state.h#L686) |
| Render-space transform | [core/view_transform.cpp](../sandbox/source/core/view_transform.cpp#L81) |

---

## 1. Overview & design goals

Infinite 2D space is divided into a regular grid of square **chunks**. Chunks near the
player are generated on demand by a **background worker thread** and freed when the player
moves away, so only a small working set is ever resident in memory. Content is fully
**deterministic**: the same chunk coordinate always regenerates identical content from a
hash of the world seed, so the world is stable and reproducible without storing it.

Design goals baked into the implementation:

- **Precision-safe everywhere.** Chunk keys and positions are derived from `HierPos2`
  (integer cell + local float offset), never from absolute floats, so the system works
  arbitrarily far from the galaxy origin without precision loss.
- **No main-thread hitches.** Generation runs off-thread; the main thread only enqueues
  wanted keys and drains finished chunks under a small lock.
- **Deterministic + reproducible.** Regenerating a chunk yields byte-identical content.
- **Persist only player changes.** The world isn't serialized; only *deltas* (things the
  player destroyed) are recorded, as compact per-chunk bitmasks.
- **Content follows the map.** Density is keyed to the orbital zone of the nearest star
  system, so belts are dense with asteroids and deep space is sparse.

---

## 2. Coordinate model

### HierPos2 recap

Absolute galaxy positions use `HierPos2` = `{ GridCell cell (i64 x,y); Vec2 local (f32) }`.
A cell is `BS_HIERPOS_CELL_SIZE` world units on a side; `local` is a canonical offset within
the cell in the range `[-BS_HIERPOS_HALF_CELL, +BS_HIERPOS_HALF_CELL)`. This gives global
reach with local float precision.

### Chunk grid

A chunk spans `CHUNK_CELLS` = `1 << CHUNK_CELL_SHIFT` = **8** cells per edge, so:

```
CHUNK_CELL_SHIFT = 3
CHUNK_CELLS      = 8
CHUNK_SIZE       = BS_HIERPOS_CELL_SIZE * 8   // 131072 world units per edge
```

The chunk key is computed by an **arithmetic right shift** of the HierPos2 cell coordinate,
which is exactly floor-division by 8 for signed integers (correct for negative coords too):

```cpp
ChunkKey chunk_key_from_hierpos(const HierPos2* hp) {
    return ChunkKey{ hp->cell.x >> CHUNK_CELL_SHIFT, hp->cell.y >> CHUNK_CELL_SHIFT };
}
```

`chunk_origin_hierpos(key)` returns the min-corner of a chunk as an exact `HierPos2`. Because
keys are `i64` and derived by integer shift, the grid is precision-safe across the entire
galaxy — there is no float rounding in chunk identity.

For distance/zone math the worker converts a chunk center to exact `f64` world coordinates
(`chunk_center_f64`), which is fine because it only classifies content, not entity identity.

---

## 3. Data structures

All defined in [sim/chunk_stream.h](../sandbox/source/sim/chunk_stream.h).

### `ChunkKey`
```cpp
struct ChunkKey { i64 cx; i64 cy; };
```
Integer chunk coordinate; the unit of loading/unloading and the seed input.

### `ChunkEntity`
```cpp
struct ChunkEntity {
    ChunkEntityType type;
    HierPos2        pos;         // absolute galaxy position (persisted, precision-safe)
    Vec2            render_pos;  // TRANSIENT: recomputed each frame by the render pass
    f32             radius;      // world units
    f32             rotation;    // radians (asteroids tumble)
    f32             spin;        // radians/second
    bs_color        color;
    i32             verts;                              // asteroid silhouette vertex count (5..9)
    f32             vert_jitter[CHUNK_ASTEROID_MAX_VERTS]; // deterministic radial jitter 0.6..1.0
    b8              alive;       // FALSE when removed via a delta
};
```
`render_pos` is scratch — recomputed every frame from `pos` and never persisted.

### `Chunk`
```cpp
struct Chunk {
    ChunkKey    key;
    ChunkEntity entities[CHUNK_MAX_ENTITIES]; // CHUNK_MAX_ENTITIES = 64
    i32         entity_count;
    f32         fade_in;   // 0 -> 1 over ~0.4s after activation (smooth pop-in)
};
```
The **64-entity cap is deliberate**: it lets a single `u64` bitmask represent "which entities
in this chunk have been removed" (see §8). Changing the cap requires changing the delta
representation.

### `ChunkEntityType`
```cpp
enum ChunkEntityType {
    CHUNK_ASTEROID = 0, CHUNK_STATION, CHUNK_ENEMY_MARKER, CHUNK_RESOURCE, CHUNK_DECORATION,
};
```

### `ChunkSystemSnapshot`
```cpp
struct ChunkSystemSnapshot {
    HierPos2 center;
    f32      orbit_radii[5]; // sorted ascending
    i32      orbit_count;
    bs_color star_color;
};
```
The **only** game data the worker thread reads. Snapshotted at init from the galaxy's systems
(orbit radii come from `semi_major_axis`, which is constant after galaxy generation). Because
it's immutable after init, the worker needs no lock to read it.

### `ChunkStreamConfig`
```cpp
struct ChunkStreamConfig {
    b8  enabled;         // master toggle (unloads everything when FALSE)
    i32 load_radius;     // chunks (Chebyshev) around the focus that must be resident
    f32 density_mul;     // global multiplier on per-zone entity counts
    b8  debug_draw_grid; // render chunk borders + counts on LAYER_DEBUG
};
```
Live-editable tunables exposed in the editor panel.

### `ChunkStreamer` (opaque)
Declared opaque in the header; defined in the `.cpp`. Owns the worker `std::thread`, the
`mutex`/`condition_variable`, the resident set, the request/completed queues, the immutable
snapshot array, and the delta store. External code only holds a `ChunkStreamer*` (stored in
`game_state::chunk_streamer`).

### Key constants
```
CHUNK_CELL_SHIFT           3     // 8 cells per chunk edge
CHUNK_SIZE                 131072 world units
CHUNK_MAX_ENTITIES         64    // == bits in the u64 removal mask
MAX_LOADED_CHUNKS          128   // resident hard cap (load radius 3 => 7x7 = 49 wanted)
CHUNK_MAX_SNAPSHOT_SYSTEMS 64
CHUNK_ASTEROID_MAX_VERTS   9
```

---

## 4. Threading model

There is exactly **one worker thread**. The contract is:

- The worker reads **only** the immutable `ChunkSystemSnapshot[]` and the config's
  `density_mul`. It never touches `game_state`, resident chunks, or deltas.
- Main ↔ worker handoff goes through a single `std::mutex` (`mtx`) + `std::condition_variable`
  (`cv`) guarding three queues:

```cpp
std::vector<ChunkKey> requests;   // pending keys, sorted FARTHEST-first (pop_back = nearest)
std::vector<Chunk*>   completed;  // generated, awaiting main-thread activation
ChunkKey              generating; // key currently being generated
b8                    generating_valid;
```

The worker loop:
```cpp
for (;;) {
    // wait for work or quit
    lock; cv.wait(lk, has_requests_or_quit);
    if (quit) return;
    key = requests.back(); requests.pop_back();   // nearest chunk first
    generating = key; generating_valid = TRUE;
    density = cfg.density_mul;
    unlock;

    Chunk* c = generate_chunk(cs, key, density);  // heavy work, no lock held

    lock; completed.push_back(c); generating_valid = FALSE; unlock;
}
```

Two properties fall out of this:

- **Nearest-first generation.** The main thread sorts `requests` farthest-first so the worker
  pops the nearest wanted chunk from the back — the player sees the closest content appear
  first.
- **Generation happens outside the lock**, so the main thread is never blocked on the (heavier)
  generation work; it only ever holds the lock to swap small vectors.

### Determinism vs. threads

The global RNG in the system generator is **not** thread-safe, so the worker uses a private,
instance-seeded `ChunkRng` (splitmix64). Each chunk seeds a fresh RNG, so generation is
independent of thread scheduling and order — fully deterministic.

---

## 5. Determinism & seeding

Each chunk's content is a pure function of `(world_seed, cx, cy)`:

```cpp
static u64 chunk_seed(u64 world_seed, i64 cx, i64 cy) {
    u64 z = world_seed ^ (cx * 0x9e3779b97f4a7c15) ^ (cy * 0xc2b2ae3d27d4eb4f);
    // splitmix64 finalizer
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9;
    z = (z ^ (z >> 27)) * 0x94d049bb133111eb;
    return z ^ (z >> 31);
}
```

`ChunkRng` is a splitmix64 generator with `.f()` (0..1), `.range(lo,hi)`, and `.irange(lo,hi)`
(inclusive int). Because the chunk always regenerates identically, the world doesn't need to
be stored — only the deltas that record what the player changed (§8).

The world seed is currently hard-coded at init: `chunk_stream_init(s, 0x123456789ABCDEF0)`
([game.cpp](../sandbox/source/game.cpp#L476)).

---

## 6. Region classification & content profiles

Before generating, the worker classifies a chunk into a region based on the **nearest system**
(by exact f64 distance) and that system's sorted orbit radii:

```cpp
enum ChunkRegion {
    REGION_DEEP,   // > 1.5x outermost orbit (or no system): near-empty
    REGION_FRINGE, // between outermost orbit and 1.5x it
    REGION_BELT,   // between outermost and second-outermost orbit (asteroid belt)
    REGION_MID,    // middle orbital rings
    REGION_INNER,  // inside the innermost orbit (star glare)
};
```

The zone index is computed outside-in against the ascending `orbit_radii`, matching the
`get_system_zone` semantics used elsewhere. Each region has a content profile (entity type mix
and count ranges), and every count is scaled by `density_mul` (editor tunable):

| Region | Content |
| --- | --- |
| DEEP | 1–3 decorations; ~35% chance of 1 asteroid |
| FRINGE | 1–4 asteroids, 0–3 decorations, ~8% enemy marker |
| BELT | 14–28 asteroids, 2–5 resources, 2–4 decorations |
| MID | 4–10 asteroids, ~25% station, 0–2 enemy markers, 0–2 resources, 2–4 decorations |
| INNER | 0–2 asteroids, 1–3 decorations (sparse, near the star) |

Entity attributes (radius, spin, color, asteroid silhouette) are also drawn from the chunk
RNG in `spawn_entity`, so silhouettes/tumble are deterministic. Decorations are tinted by the
nearest star's color. Positions are chosen as an exact `HierPos2` (random cell within the 8×8
block + canonical local offset) — no float math on absolute coordinates.

---

## 7. Streaming update loop

`chunk_stream_update(game_state*, sim_dt)` runs once per frame on the main thread from
[game.cpp](../sandbox/source/game.cpp#L1181) (wrapped in `PROF_CHUNK_STREAM`). It runs **after**
the camera rebase so the focus is this frame's final position.

**Focus = the player ship's origin, not the camera.** This is deliberate: the free camera and
map panning must not drag the loaded set around.

```cpp
ChunkKey focus = chunk_key_from_hierpos(&s->player_ship().origin);
i32 load_r   = max(1, cfg.load_radius);
i32 unload_r = load_r + 1;  // hysteresis band prevents load/unload thrash at boundaries
```

Four phases:

1. **Drain completed** (under lock). For each finished chunk, discard it if streaming is
   disabled, it's now beyond `unload_r`, it's already resident, or the resident cap is hit;
   otherwise apply any delta, reset `fade_in`, and add it to the resident set.
2. **Unload** any resident chunk beyond `unload_r` (Chebyshev distance). Deltas persist through
   unload.
3. **Rebuild the request queue** (under lock): every key within `load_r` that is not resident
   and not currently generating is enqueued, then sorted **farthest-first** and the worker is
   notified. Rebuilding from scratch each frame keeps the queue consistent with the focus.
4. **Tick** resident chunks: advance `fade_in` (0→1 over 0.4s) and rotate spinning asteroids.

The `load_r` / `unload_r` split (hysteresis) is what prevents a chunk on the boundary from
being repeatedly loaded and unloaded as the player jitters across an edge.

---

## 8. Delta / persistence system

Player-caused changes are stored as compact per-chunk removal bitmasks, kept for the whole
session and **never unloaded**:

```cpp
struct ChunkDelta { ChunkKey key; u64 removed_mask; };
std::vector<ChunkDelta> deltas;
```

The public hook is:
```cpp
void chunk_stream_remove_entity(ChunkStreamer* cs, ChunkKey key, i32 local_idx);
```
`local_idx` is the entity's index in generation order. It sets bit `local_idx` in that chunk's
mask (creating the delta if needed) and, if the chunk is currently resident, immediately marks
that entity `alive = FALSE`. When a chunk is later regenerated, `apply_delta` re-clears the same
bits, so destroyed content stays destroyed. This is the intended hook for mining/destruction
gameplay.

Because the world is deterministic, this delta list is the **entire** save state needed for
chunk content — but note it is **not yet wired to save/load** (see §13). The 64-entity cap
exists precisely so one `u64` covers a whole chunk.

---

## 9. Render pass

`draw_chunk_content(game_state*)` ([render/chunk_render.cpp](../sandbox/source/render/chunk_render.cpp))
is called from `render_scene` **below the ships** on `LAYER_CELESTIAL`
([scene_renderer.cpp](../sandbox/source/render/scene_renderer.cpp#L32)).

For each resident chunk and each alive entity:

1. Recompute `render_pos = render_from_hierpos(s, &en.pos)` — the floating-origin
   transform, so streamed content lines up with everything else rendered through
   the same path.
2. Cull off-screen entities using the **full draw extent** (`2× max(radius, min_r)`; the station
   ring is the largest at 1.6× radius).
3. Fade by sensor visibility × per-chunk `fade_in`; skip if `alpha <= 0.003`.
4. Draw by type:
   - **Asteroid** — closed polygon silhouette: `verts` points evenly around a circle, each
     radius jittered by `vert_jitter`, rotated by the tumble angle, drawn as lines.
   - **Station** — filled quad core + a docking-ring circle.
   - **Enemy marker / Resource** — outlined circles.
   - **Decoration** — small faint quad (fixed world size, star-tinted).

A **zoom-out size floor** (`min_r = 2.5 / zoom`) keeps entities at least a few screen pixels
across so streamed content stays readable when zoomed way out (matching the "2px dot" treatment
of planets on the map look). The cull margin accounts for this floor.

### Debug grid

When `cfg.debug_draw_grid` is on, `draw_debug_grid` outlines each resident chunk (4 corners
routed through `render_from_hierpos`) and labels the center with `(cx,cy) n=<entity_count>` on
`LAYER_DEBUG`.

---

## 10. Editor, debug & profiling

The editor panel ([ui/editor_ui.cpp](../sandbox/source/ui/editor_ui.cpp#L313)) exposes:

- **Streaming enabled** (master toggle; unloads everything when off)
- **Load radius (chunks)** — 1..6
- **Density multiplier** — 0..4
- **Draw chunk grid (debug)**
- Live counters: `Resident`, `In-flight`, `Entities`, `Deltas`

Query functions backing the panel:
```cpp
i32 chunk_stream_resident_count(const ChunkStreamer*);
i32 chunk_stream_inflight_count(const ChunkStreamer*);  // requests + generating + completed
i32 chunk_stream_total_entities(const ChunkStreamer*);
i32 chunk_stream_delta_count(const ChunkStreamer*);
```

Per-frame cost is measured under the `PROF_CHUNK_STREAM` profiler zone ("Chunk streaming",
group `G_UPDATE`; see [profiler.cpp](../sandbox/source/profiler.cpp)).

---

## 11. Lifecycle & integration points

| Stage | Where | Notes |
| --- | --- | --- |
| Init | [game.cpp](../sandbox/source/game.cpp#L476) — `chunk_stream_init(s, seed)` | Must run **after** `galaxy_map_init` (snapshots the systems). Starts the worker. |
| Update | [game.cpp](../sandbox/source/game.cpp#L1181) — `chunk_stream_update(s, sim_dt)` | Runs after the camera rebase; profiled. |
| Render | [scene_renderer.cpp](../sandbox/source/render/scene_renderer.cpp#L32) — `draw_chunk_content(s)` | Below ships on `LAYER_CELESTIAL`. |
| Shutdown | `chunk_stream_shutdown(s)` + `atexit` fallback | Stops/joins the worker, frees resident + completed chunks. |
| State | [game_state.h](../sandbox/source/state/game_state.h#L686) — `ChunkStreamer* chunk_streamer` | The single owning pointer. |
| Module include | [game_modules.h](../sandbox/source/game_modules.h#L29) | Declares the subsystem. |

`chunk_stream_init` snapshots up to `CHUNK_MAX_SNAPSHOT_SYSTEMS` systems, sorting each system's
orbit radii ascending. A file-static `g_chunk_streamer` is registered with `atexit` as a
shutdown fallback because the Game layer has no explicit shutdown hook yet.

---

## 12. Extension guide

### Add a new entity type
1. Add the enum value to `ChunkEntityType` in [chunk_stream.h](../sandbox/source/sim/chunk_stream.h).
2. Add a `case` in `spawn_entity` (chunk_stream.cpp) that sets `radius`, `spin`, `color`, and any
   type-specific fields — using the passed `ChunkRng` only, so it stays deterministic.
3. Reference it in the region profiles in `generate_chunk` (how often / how many).
4. Add a `case` in `draw_entity` (chunk_render.cpp) for its visual, drawing on `LAYER_CELESTIAL`.
5. If it needs new per-entity data, add fields to `ChunkEntity` (watch `struct` size × 64 ×
   `MAX_LOADED_CHUNKS` for memory).

### Add or retune a region
- Adjust thresholds in `classify_region`, or add a new `ChunkRegion` value plus a `switch` case
  in `generate_chunk`. Region classification depends only on the immutable snapshot, so it stays
  thread-safe.

### Make entities interactive (gameplay)
- Entities live in resident chunks; iterate them via `chunk_stream_resident_count` /
  `chunk_stream_resident_at`. Their absolute positions are `en.pos` (HierPos2).
- To permanently remove one (mining, destruction), call `chunk_stream_remove_entity(cs, key,
  local_idx)`. Persistence and re-application on reload are handled for you.
- For collision, compute against `en.pos` in HierPos2 space (use `hierpos_diff`), not screen
  space.

### Add save/load
- The complete content save state is `world_seed` + the `deltas` vector. Add serialization that
  writes/reads `{ world_seed, [ {cx, cy, removed_mask} ... ] }`. On load, set the seed before
  `chunk_stream_init` and restore the deltas; `apply_delta` will re-apply them as chunks stream
  in. Cross-reference [SAVE_LOAD_SYSTEM_PLAN.md](../SAVE_LOAD_SYSTEM_PLAN.md).

### Scale the generator (multiple workers)
- The generation path is already pure (reads only the immutable snapshot + a per-chunk seed), so
  it's safe to run on N workers. You'd need a work-claiming scheme so multiple workers don't pop
  the same key, and to guard `completed` (already mutex-protected). `generating`/
  `generating_valid` would become a per-worker set.

---

## 13. Known limitations & TODO

- **Stale render comment.** [chunk_render.h](../sandbox/source/render/chunk_render.h) says content
  is faded by the "arena weight", but `draw_chunk_content` does **not** currently multiply alpha
  by `view_arena_w`. Either wire the arena-weight fade or update the comment.
- **No save/load wiring.** Deltas are save-ready but not serialized anywhere yet (§12).
- **Static decor only.** Entities have no collision or gameplay interaction; they're visual.
  `chunk_stream_remove_entity` is the only mutation hook.
- **Single worker.** Generation is single-threaded; the design permits more (§12) but it isn't
  implemented.
- **`atexit` shutdown is a stopgap.** The Game layer lacks a real shutdown hook, so a file-static
  pointer + `atexit` guarantees the worker is joined.
- **64-entity cap is load-bearing.** It's coupled to the `u64` removal mask; raising it means
  changing the delta representation.
- **Hard-coded world seed** at init (`0x123456789ABCDEF0`).
