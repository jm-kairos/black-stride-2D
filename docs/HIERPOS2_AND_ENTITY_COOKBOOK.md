# HierPos2 Coordinate System & Entity Cookbook

This document has two parts:

1. **The HierPos2 coordinate system** — how Black Stride represents world positions
   so that gameplay stays precise billions of units from the origin.
2. **A cookbook** — practical, copy-pasteable recipes for instantiating entities
   (ships, combat targets, map blips) in the sandbox. Written for game designers.

Everything here is grounded in the actual code:
[`bs_hierpos.h`](../engine/source/math/bs_hierpos.h),
[`bs_hierpos.cpp`](../engine/source/math/bs_hierpos.cpp),
[`ship.h`](../sandbox/source/ship.h),
[`fleet.h`](../sandbox/source/fleet.h) / [`fleet.cpp`](../sandbox/source/fleet.cpp),
[`game.h`](../sandbox/source/game.h),
and the spawn code in `game_init` ([`game.cpp`](../sandbox/source/game.cpp)).

---

# Part 1 — The HierPos2 Coordinate System

## Why it exists

A 32-bit `float` has ~7 significant decimal digits. Near the origin that's fine
(sub-millimetre precision), but Black Stride is a space game where ships travel
*billions* of world units from `{0,0}`. At a coordinate like `x = 2,000,000,000`,
a single float step is roughly **128 units** — objects jitter, physics explodes,
and rendering snaps to a grid.

`HierPos2` (hierarchical position, 2D) solves this by **never storing a large
absolute float**. It splits a world position into a coarse integer grid cell plus
a small local offset, so the number the float actually holds is always tiny.

## The data model

Defined in [`bs_hierpos.h`](../engine/source/math/bs_hierpos.h), namespace `bs_math`:

```cpp
struct GridCell { i64 x; i64 y; };              // which cell (64-bit integer — effectively unlimited range)
struct HierPos2 { GridCell cell; Vec2 local; }; // cell index + local float offset inside that cell
```

- `BS_HIERPOS_CELL_SIZE = 16384.0f` — each cell is 16384 x 16384 world units.
- `BS_HIERPOS_HALF_CELL = 8192.0f`.

The exact world position is conceptually:

```
world = cell * 16384 + local
```

Because `cell` is an `i64`, it can index a galaxy that is astronomically large,
while `local` — the only *float* — is always kept in the range `[-8192, +8192]`.
Float precision at that magnitude is well under a thousandth of a unit, everywhere
in the world.

## The canonical-form invariant

A `HierPos2` is **canonical** when `local` is in `[-8192, +8192)`. Whenever you do
arithmetic that could push `local` outside that band (e.g. adding velocity), you
must *re-canonicalize*: the overflow rolls into the integer `cell`, and `local`
snaps back into range. The helpers below do this for you — you rarely call
`hierpos_normalize` directly.

The engine self-verifies this with `bs_hierpos_selftest()` (round-trips a suite of
coordinates and asserts exact recovery), and the in-game `coord_diag` module
asserts `|local| <= 8193` at runtime.

## The core operations

All live in [`bs_hierpos.cpp`](../engine/source/math/bs_hierpos.cpp). Every function
takes a `cell_size`, but there are convenience overloads that bind
`BS_HIERPOS_CELL_SIZE` for you — **prefer those**.

| Function | Purpose |
|---|---|
| `hierpos_from_vec2(Vec2 world)` | Convert an ordinary world-space point into a canonical `HierPos2`. Your entry point when authoring a position. |
| `hierpos_to_vec2(const HierPos2* hp)` | Collapse back to a `Vec2`. **Only safe near the origin** — the whole point is to avoid this far away. Fine for camera-relative math. |
| `hierpos_to_f64(hp, cs, &x, &y)` | Exact, lossless world coordinates as `double`. Use when you truly need the absolute position. |
| `hierpos_diff(const HierPos2* a, const HierPos2* b)` | The workhorse. Computes `a - b` as a small `Vec2`. **Precision-safe** when `a` and `b` are near each other, regardless of how far both are from the origin. This is how everything is rendered and how distances are measured. |
| `hierpos_add_vec2(const HierPos2* hp, Vec2 d)` | Add a small float delta (velocity * dt, a nudge) and re-canonicalize. Your per-frame movement primitive. |
| `hierpos_add_f64(hp, dx, dy, cs)` | Same, but with `double` deltas for large jumps. |
| `hierpos_lerp(a, b, t)` | Precision-safe interpolation between two positions (camera transitions, tweens). |
| `hierpos_normalize(hp)` | Force a `HierPos2` back into canonical form. Rarely needed — the `add`/`diff` helpers already do it. |

## Floating-origin rendering

Because you can't hand a billion-unit float to the GPU, rendering is done
**relative to the camera**. The camera has its own `s->camera_hierpos`. To draw
anything, you take the difference between the object and the camera:

```cpp
Vec2 render_pos = hierpos_diff(&entity.origin, &s->camera_hierpos); // small, precise, GPU-safe
```

This is exactly what `render_from_hierpos(s, &hp)` does. The camera is effectively
always at the render origin, and the world slides underneath it — hence "floating
origin." Entities cache this each frame in a transient `render_pos` field (see
`Ship::render_pos`, `CombatEntity::render_pos`), which is recomputed every frame
and must **never** be persisted.

## Rules of thumb

- **Store** every persistent world position as a `HierPos2` (`origin`, `position`,
  `galaxy_pos`, `move_target`, `camera_hierpos`).
- **Measure distance / direction** with `hierpos_diff` — never by converting both
  to `Vec2` and subtracting.
- **Move** with `hierpos_add_vec2(&hp, velocity * dt)`.
- **Author** a position with `hierpos_from_vec2({x, y})`.
- **Never** call `hierpos_to_vec2` on something far from the camera and do math
  with the result — that reintroduces the precision loss the system exists to prevent.
- **Never** store `render_pos` — it's valid for the current frame only.

---

# Part 2 — Cookbook: Instantiating Entities in the Sandbox

This is the practical guide for adding things to the world. All of these patterns
come straight from `game_init` in [`game.cpp`](../sandbox/source/game.cpp).

## Where things live

| Type | File | Meaning |
|---|---|---|
| `Ship` | [`ship.h`](../sandbox/source/ship.h) | A rigid-body vessel: pose (`origin` + `angle`), visual sprite layers, collider, weapons. Loaded from a `.ship` file. |
| `FleetShip` | [`fleet.h`](../sandbox/source/fleet.h) | A **player-controlled** ship: a `Ship` + flight dynamics + selection/order state. Held by `s->fleet`. |
| `CombatEntity` | [`game.h`](../sandbox/source/game.h) | A lightweight hittable target in the flat `s->combat_entities[]` array. Any ship that can be shot must be registered here. |
| `MapEntity` | [`game.h`](../sandbox/source/game.h) | A blip on the galaxy map, in `s->map_entities[]`. |

Key rule: **a ship is only *simulated*/*rendered* because it's a `FleetShip` or
`s->enemy_ship`, and only *shootable* because it's also registered as a
`CombatEntity`.** These are two separate registrations.

---

## Recipe 1 — Spawn a player-controlled fleet ship

`s->fleet.add()` appends a zero-initialized slot (up to `FLEET_MAX_SHIPS = 8`) and
returns it for you to fill. Member 0 is the flagship.

```cpp
{
    FleetShip& fs = s->fleet.add();

    // Load the hull art + collider + footprint from a .ship file.
    if (!ship_load(&fs.ship, "assets/ships/ship/ship.ship")) {
        BS_LOG_ERROR("failed to load ship");
    } else {
        fs.ship_type = SHIP_TYPE_DRONE;

        // Place it: author a plain world point, convert to floating-origin coords.
        fs.ship.origin      = hierpos_from_vec2(Vec2{ 24000.0f, 1500.0f }, BS_HIERPOS_CELL_SIZE);
        fs.ship.angle       = 0.0f;                    // heading, radians CCW
        fs.ship.faction     = VESSEL_FEDERATION;
        fs.ship.vessel_name = "Escort";
        fs.ship.glow        = s->glow_params;          // shared bloom preset
        fs.ship.radiation_emission = 0.05f;            // heat visible to sensors (0..1)

        // Weapons: clear the array, then create + assign.
        for (i32 w = 0; w < SHIP_MAX_WEAPONS; ++w) fs.ship.weapons[w] = nullptr;
        fs.ship.weapons[0]        = weapon_create_ballistic_cannon(fs.ship.faction);
        fs.ship.weapon_count      = 1;
        fs.ship.active_weapon_idx = 0;
    }
}
```

> `ship_load` always resets `origin` to `{0,0}`, so **set `origin` after loading**,
> never before.

---

## Recipe 2 — Spawn a hostile / NPC ship and make it shootable

A non-fleet ship (like the raider) is a bare `Ship` you own somewhere (here
`s->enemy_ship`). You place it the same way, then **register a `CombatEntity`** so
projectiles can hit it.

```cpp
// 1) The ship itself.
if (ship_load(&s->enemy_ship, "assets/enemy_ship.ship")) {
    s->enemy_ship.origin      = hierpos_from_vec2(Vec2{ 10000.0f, 0.0f }, BS_HIERPOS_CELL_SIZE);
    s->enemy_ship.angle       = 2.36f;
    s->enemy_ship.faction     = VESSEL_PIRATE;
    s->enemy_ship.vessel_name = "Raider-class Interceptor";
    s->enemy_ship.glow        = s->glow_params;
    s->enemy_ship.radiation_emission = 1.0f;

    for (i32 i = 0; i < SHIP_MAX_WEAPONS; ++i) s->enemy_ship.weapons[i] = nullptr;
    s->enemy_ship.weapons[0]        = weapon_create_ballistic_cannon(s->enemy_ship.faction);
    s->enemy_ship.weapon_count      = 1;
    s->enemy_ship.active_weapon_idx = 0;
}

// 2) Register it as a hittable combat entity.
if (s->combat_entity_count < MAX_COMBAT_ENTITIES) {
    CombatEntity* ce = &s->combat_entities[s->combat_entity_count++];
    ce->active   = TRUE;
    ce->position = s->enemy_ship.origin;                 // HierPos2 — same coords
    ce->velocity = Vec2{ 0.0f, 0.0f };
    ce->radius   = ship_bounding_radius(&s->enemy_ship); // derived from the collider
    ce->faction  = s->enemy_ship.faction;
    ce->hp       = 100.0f;
    ce->ship     = &s->enemy_ship;                       // link back to the Ship
    ce->tint     = bs_color{ 1.0f, 0.3f, 0.3f, 1.0f };
    ce->radiation_emission = s->enemy_ship.radiation_emission;
    ce->is_drone = FALSE;                                // TRUE only for friendly drones
}
```

> `ce->ship` must point at a **stable** `Ship`. Fleet ships live in a fixed array
> precisely so these pointers never dangle — don't register a `CombatEntity` for a
> ship stored in a temporary.

---

## Recipe 3 — Placing something far from the origin

The whole point of HierPos2. You author in ordinary coordinates and let the helper
canonicalize — the cell index absorbs the magnitude:

```cpp
// Two billion units out — still precise.
ship.origin = hierpos_from_vec2(Vec2{ 2.0e9f, -3.5e8f }, BS_HIERPOS_CELL_SIZE);

// Relative placement: 500 units to the right of another entity, precisely.
HierPos2 anchor = other.origin;
ship.origin = hierpos_add_vec2(&anchor, Vec2{ 500.0f, 0.0f });
```

---

## Recipe 4 — Add a galaxy-map blip

Map entities are synced into `s->map_entities[]` (cap `MAX_MAP_ENTITIES = 16`). The
player blip is seeded at init and rebuilt each frame:

```cpp
if (s->map_entity_count < MAX_MAP_ENTITIES) {
    s->map_entities[s->map_entity_count++] = MapEntity{
        ship.origin,                          // HierPos2 galaxy_pos
        bs_color{ 1.0f, 1.0f, 1.0f, 1.0f },   // colour
        12.0f,                                // radius
        TRUE,                                 // animated outline (reserved for player)
        "Player Ship"                         // hover label
    };
}
```

---

## Recipe 5 — Move an entity each frame

In your update code, integrate position in floating-origin space and refresh the
transient render position:

```cpp
// dt = frame delta seconds; velocity is world-space Vec2.
ship.origin = hierpos_add_vec2(&ship.origin, vec2_scale(velocity, dt));

// Recompute the GPU-safe render position (do NOT persist this).
ship.render_pos = hierpos_diff(&ship.origin, &s->camera_hierpos, BS_HIERPOS_CELL_SIZE);
```

Distance/aim checks use `hierpos_diff`, never `to_vec2`:

```cpp
Vec2  to_target = hierpos_diff(&target.origin, &shooter.origin); // precise everywhere
f32   range     = vec2_length(to_target);
```

---

## Common gotchas

- **Set `origin`/`position` *after* `ship_load`** — the loader zeroes it.
- **Register twice for a shootable ship**: once as a `Ship` (owned by fleet or a
  member) *and* once as a `CombatEntity`. Rendering and hit-detection are separate
  systems.
- **Clear the `weapons[]` array before assigning** (`= nullptr` for all
  `SHIP_MAX_WEAPONS`), then set `weapon_count` and `active_weapon_idx` to match.
- **Never store `render_pos`** — it's frame-local; recompute it from `hierpos_diff`
  against `camera_hierpos` every frame.
- **Respect the caps**: `FLEET_MAX_SHIPS = 8`, `MAX_COMBAT_ENTITIES = 32`,
  `MAX_MAP_ENTITIES = 16`. `fleet.add()` silently returns the flagship if full; the
  arrays need an explicit bounds check as shown.
- **`radiation_emission = 0`** makes an entity invisible to the heat/sensor
  detector — set it deliberately.
