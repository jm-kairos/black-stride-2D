#include "render/ship_render.h"
#include "game.h"
#include "sim/module.h"          // ModuleDef::type (sensor-dish mount art)
#include "core/view_transform.h" // render_from_hierpos
#include "render/text.h"
#include "core/render_layers.h" // LAYER_UI
#include <renderer/renderer.h>
#include <renderer/camera2d.h>
#include <math.h>  // atan2f, cosf, sinf
#include <stdio.h> // snprintf

using namespace bs_math;

// ---- Ship type emblem (system mode) ----------------------------------------------------
// Low-level draw of a single screen-space-stable emblem. The emblem grows larger and
// more opaque as the camera zooms out, so ship types remain readable at wide zoom levels.
// We explicitly disable glow/bloom on the emblem so it renders as a crisp alpha sprite.
static const bs_glow_params EMBLEM_NO_GLOW = {
    0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
    bs_color{ 1.0f, 1.0f, 1.0f, 1.0f },
    bs_color{ 1.0f, 1.0f, 1.0f, 1.0f },
    bs_color{ 1.0f, 1.0f, 1.0f, 1.0f },
    bs_color{ 1.0f, 1.0f, 1.0f, 1.0f }
};

static void draw_ring_arc(game_state* s, Vec2 center, f32 world_radius, f32 start_angle,
                           f32 sweep_angle, f32 thickness, bs_color color, u32 layer)
{
    if (sweep_angle == 0.0f) return;
    f32 abs_sweep = sweep_angle > 0.0f ? sweep_angle : -sweep_angle;
    u32 segments = (u32)(abs_sweep / (2.0f * BS_PI) * 32.0f);
    if (segments < 4) segments = 4;
    if (segments > 32) segments = 32;
    f32 step = sweep_angle / (f32)(segments - 1);
    Vec2 prev = Vec2{ center.x + world_radius * cosf(start_angle),
                      center.y + world_radius * sinf(start_angle) };
    for (u32 i = 1; i < segments; ++i) {
        f32 a = start_angle + step * (f32)i;
        Vec2 cur = Vec2{ center.x + world_radius * cosf(a),
                         center.y + world_radius * sinf(a) };
        renderer_draw_line(prev, cur, thickness, color, layer);
        prev = cur;
    }
}

static void draw_one_ship_emblem(game_state* s, bs_texture tex, f32 zoom,
                                  bs_math::Vec2 origin, bs_math::Vec2 velocity,
                                  i32 count, i32 selected_count) {
    if (!tex.id) return;
    // Interpolate screen-space size and opacity between the widest (0.04f) and close-up (1.0f) zooms.
    const f32 ZOOM_WIDE = 0.04f;
    const f32 ZOOM_CLOSE = 1.0f;
    f32 t = (zoom - ZOOM_WIDE) / (ZOOM_CLOSE - ZOOM_WIDE);
    t = clampf(t, 0.0f, 1.0f);
    f32 screen_size = 64.0f - (64.0f - 24.0f) * t; // 64 px at wide zoom, 24 px at close zoom
    f32 alpha       = 1.0f - (1.0f - 0.3f) * t;    // 1.0 alpha at wide zoom, 0.3 at close zoom
    f32 safe_zoom   = zoom > 0.0001f ? zoom : 0.0001f;
    f32 world_size  = screen_size / safe_zoom;
    // renderer_draw_circle/line take thickness in SCREEN PIXELS and divide by zoom internally,
    // so we pass a constant screen thickness rather than a world-space thickness.
    f32 line_screen_thickness = 1.0f;
    bs_color ring_color   = bs_color{ 0.20f, 1.00f, 0.20f, alpha }; // fluorescent green
    bs_color vector_color = bs_color{ 0.00f, 1.00f, 1.00f, alpha }; // cyan
    // Velocity direction (reused for emblem rotation and the vector overlay).
    f32 speed = vec2_length(velocity);
    Vec2 vel_dir = Vec2{ 0.0f, 0.0f };
    f32 emblem_rotation = 0.0f;
    if (speed > 0.001f) {
        vel_dir = vec2_scale(velocity, 1.0f / speed);
        // The sprite texture is authored with the emblem pointing up (+Y at rotation 0).
        // Rotate it by the angle from +Y to the velocity direction so it points along the vector.
        emblem_rotation = atan2f(-vel_dir.x, vel_dir.y);
    }
    // ---- Emblem sprite.
    bs_sprite sp{};
    sp.position = origin;
    sp.size     = Vec2{ world_size, world_size };
    sp.origin   = Vec2{ 0.5f, 0.5f };
    sp.rotation = emblem_rotation; // align with movement direction
    sp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    sp.tint     = bs_color{ 1.0f, 1.0f, 1.0f, alpha };
    sp.texture  = tex;
    sp.blend    = BLEND_ALPHA;
    sp.layer    = LAYER_UI;
    sp.glow_override = &EMBLEM_NO_GLOW;
    sp.custom   = bs_color{ 1.0f, 0.0f, 0.0f, 0.0f };
    renderer_draw_sprite(&sp);
    // ---- Surrounding ring.
    f32 ring_screen_radius = screen_size * 0.55f; // just outside the emblem half-size
    f32 ring_world_radius  = ring_screen_radius / safe_zoom;
    bs_color dim_ring_color = bs_color{ 0.20f, 1.00f, 0.20f, alpha * 0.35f };
    b8 is_fused = (count > 1);
    if (is_fused) {
        // Fused cluster: always show the dim ring, then overlay the selected fraction in green.
        renderer_draw_circle(origin, ring_world_radius, 32, line_screen_thickness, dim_ring_color, LAYER_UI);
        if (selected_count > 0) {
            f32 selected_fraction = (f32)selected_count / (f32)count;
            f32 start_angle = BS_PI * 0.5f + s->camera_state.camera.rotation;
            f32 sweep_angle = 2.0f * BS_PI * selected_fraction;
            draw_ring_arc(s, origin, ring_world_radius, start_angle, sweep_angle,
                          line_screen_thickness, ring_color, LAYER_UI);
        }
    } else if (selected_count > 0) {
        // Single ship: full green ring only when selected.
        renderer_draw_circle(origin, ring_world_radius, 32, line_screen_thickness, ring_color, LAYER_UI);
    }
    // ---- Velocity vector (direction of movement, length coupled to speed).
    if (speed > 0.001f) {
        f32 speed_ratio = clampf(speed / SHIP_MAX_SPEED, 0.0f, 1.0f);
        f32 vector_screen_length = 12.0f + (100.0f - 12.0f) * speed_ratio;
        f32 vector_world_length  = vector_screen_length / safe_zoom;
        Vec2 end = vec2_add(origin, vec2_scale(vel_dir, vector_world_length));
        renderer_draw_line(origin, end, line_screen_thickness, vector_color, LAYER_UI);
    }
    // ---- Fusion count badge (top-right corner of the emblem, only for fused clusters).
    if (count > 1) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%d", count);
        Vec2 screen_pos = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, origin);
        f32 badge_x = screen_pos.x + screen_size * 0.5f;
        f32 badge_y = screen_pos.y - screen_size * 0.5f;
        text_draw(buf, badge_x, badge_y, 1.0f, vector_color, &s->camera_state.camera, s->fb_width, s->fb_height, LAYER_UI);
    }
}

static i32 emblem_find(i32 parent[], i32 x)
{
    while (parent[x] != x) {
        parent[x] = parent[parent[x]];
        x = parent[x];
    }
    return x;
}

static void emblem_union(i32 parent[], i32 a, i32 b)
{
    i32 ra = emblem_find(parent, a);
    i32 rb = emblem_find(parent, b);
    if (ra != rb) parent[ra] = rb;
}

// Cluster combat-mode emblems by ship type when their screen-space rings overlap.
// Ships of the same type whose rings overlap are fused into a single emblem at the
// cluster centroid with the average velocity vector.
void draw_fleet_emblems(game_state* s)
{
    // Fleet emblems are a galaxy-map affordance. Fade them in by map weight (1 - arena weight)
    // so they appear smoothly as the view crosses from arena to map instead of popping at the
    // discrete render-mode boundary. map_w == 0 (deep arena) draws nothing, matching the original.
    f32 map_w = 1.0f - s->view_arena_w;
    if (map_w <= 0.0f) return;
    i32 count = s->fleet_state.fleet.count();
    if (count <= 0) return;

    // Shared zoom-dependent screen-space ring radius.
    const f32 ZOOM_WIDE = 0.04f;
    const f32 ZOOM_CLOSE = 1.0f;
    f32 t = (s->camera_state.camera.zoom - ZOOM_WIDE) / (ZOOM_CLOSE - ZOOM_WIDE);
    t = clampf(t, 0.0f, 1.0f);
    f32 screen_size = 64.0f - (64.0f - 24.0f) * t;
    f32 ring_screen_radius = screen_size * 0.55f;
    f32 merge_distance_sq = (2.0f * ring_screen_radius) * (2.0f * ring_screen_radius);

    // Project fleet ships to screen space.
    Vec2 screen_pos[FLEET_MAX_SHIPS];
    for (i32 i = 0; i < count; ++i) {
        Vec2 rp = render_from_hierpos(s, &s->fleet_state.fleet.at(i).ship.origin);
        screen_pos[i] = camera2d_world_to_screen(&s->camera_state.camera, s->fb_width, s->fb_height, rp);
    }

    // Cluster by ship type using union-find; two ships fuse if their rings overlap.
    i32 parent[FLEET_MAX_SHIPS];
    for (i32 i = 0; i < count; ++i) parent[i] = i;

    for (i32 i = 0; i < count; ++i) {
        const FleetShip& fi = s->fleet_state.fleet.at(i);
        for (i32 j = i + 1; j < count; ++j) {
            const FleetShip& fj = s->fleet_state.fleet.at(j);
            if (fi.ship_type != fj.ship_type) continue;
            Vec2 d = vec2_sub(screen_pos[i], screen_pos[j]);
            f32 dist_sq = d.x * d.x + d.y * d.y;
            if (dist_sq < merge_distance_sq) {
                emblem_union(parent, i, j);
            }
        }
    }

    // Resolve each cluster root and draw a single emblem per cluster.
    renderer_set_draw_alpha(map_w);
    for (i32 i = 0; i < count; ++i) {
        if (emblem_find(parent, i) != i) continue; // not a root

        i32 members[FLEET_MAX_SHIPS];
        i32 member_count = 0;
        for (i32 j = 0; j < count; ++j) {
            if (emblem_find(parent, j) == i) members[member_count++] = j;
        }
        if (member_count == 0) continue;

        const FleetShip& representative = s->fleet_state.fleet.at(members[0]);
        ShipType type = representative.ship_type;
        if (type != SHIP_TYPE_DRONE && type != SHIP_TYPE_EXTRACTOR) continue;
        bs_texture tex = (type == SHIP_TYPE_DRONE) ? s->render.emblem_drone : s->render.emblem_extractor;

        Vec2 origin = Vec2{ 0.0f, 0.0f };
        Vec2 velocity = Vec2{ 0.0f, 0.0f };
        i32 selected_count = 0;
        for (i32 k = 0; k < member_count; ++k) {
            const FleetShip& fs = s->fleet_state.fleet.at(members[k]);
            origin = vec2_add(origin, render_from_hierpos(s, &fs.ship.origin));
            velocity = vec2_add(velocity, fs.flight.velocity);
            if (fs.selected) ++selected_count;
        }
        f32 inv = 1.0f / (f32)member_count;
        origin = vec2_scale(origin, inv);
        velocity = vec2_scale(velocity, inv);

        draw_one_ship_emblem(s, tex, s->camera_state.camera.zoom, origin, velocity, member_count, selected_count);
    }
    renderer_set_draw_alpha(1.0f);
}

// Draw a ship's collider polygon outline (world-space corners) on LAYER_DEBUG.
void draw_collider_outline(const Ship* ship, bs_color color, f32 thickness) {
    if (!ship || ship->collider_count <= 0) return;
    Vec2 k[SHIP_MAX_COLLIDER_VERTS];
    ship_collider_corners(ship, k);  // corners are ship-origin-relative (world units)
    for (i32 i = 0; i < ship->collider_count; ++i) {
        Vec2 a = vec2_add(ship->render_pos, k[i]);
        Vec2 b = vec2_add(ship->render_pos, k[(i + 1) % ship->collider_count]);
        renderer_draw_line(a, b, thickness, color, LAYER_DEBUG);
    }
}

// Colour per accepted module kind (primary bit wins if a slot accepts several).
static bs_color hardpoint_color(u32 accepts) {
    if (accepts & MODULE_TYPE_WEAPON)  return bs_color{ 1.00f, 0.45f, 0.15f, 1.0f }; // orange
    if (accepts & MODULE_TYPE_DEFENSE) return bs_color{ 0.20f, 0.85f, 1.00f, 1.0f }; // cyan
    if (accepts & MODULE_TYPE_SENSOR)  return bs_color{ 0.30f, 1.00f, 0.40f, 1.0f }; // green
    if (accepts & MODULE_TYPE_ENGINE)  return bs_color{ 1.00f, 0.90f, 0.30f, 1.0f }; // yellow
    return bs_color{ 0.65f, 0.65f, 0.65f, 1.0f };                                    // utility/grey
}

void draw_hardpoint_overlay(const Ship* ship, f32 thickness) {
    if (!ship || ship->hardpoint_count <= 0) return;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        const HardpointDef& hp = ship->hardpoints[i];
        bs_color  color = hardpoint_color(hp.accepts);
        f32       e     = hardpoint_half_extent(hp.size);
        // Box corners in ship-local space, rotated by the mount's facing.
        const Vec2 offsets[4] = { { -e, -e }, { e, -e }, { e, e }, { -e, e } };
        Vec2 w[4];
        for (i32 c = 0; c < 4; ++c) {
            Vec2 lc = vec2_add(hp.pos_local, vec2_rotate(offsets[c], hp.facing));
            w[c]    = vec2_add(ship->render_pos, ship_local_dir(ship, lc));
        }
        for (i32 c = 0; c < 4; ++c)
            renderer_draw_line(w[c], w[(c + 1) % 4], thickness, color, LAYER_DEBUG);
        // Facing tick: from the slot's center out past the box's forward edge.
        Vec2 tip_local = vec2_add(hp.pos_local, vec2_rotate(Vec2{ 0.0f, e * 1.8f }, hp.facing));
        Vec2 c0  = vec2_add(ship->render_pos, ship_local_dir(ship, hp.pos_local));
        Vec2 tip = vec2_add(ship->render_pos, ship_local_dir(ship, tip_local));
        renderer_draw_line(c0, tip, thickness, color, LAYER_DEBUG);
        // Traverse arc: boundary rays at facing +/- arc/2 (skipped for full-circle turrets
        // and fixed zero-arc slots).
        if (hp.arc > 0.001f && hp.arc < 2.0f * BS_PI - 0.001f) {
            bs_color arc_color = color;
            arc_color.a *= 0.55f;
            for (i32 sgn = -1; sgn <= 1; sgn += 2) {
                f32 edge = hp.facing + (f32)sgn * hp.arc * 0.5f;
                Vec2 el  = vec2_add(hp.pos_local, vec2_rotate(Vec2{ 0.0f, e * 2.6f }, edge));
                Vec2 ew  = vec2_add(ship->render_pos, ship_local_dir(ship, el));
                renderer_draw_line(c0, ew, thickness, arc_color, LAYER_DEBUG);
            }
        }
    }
}

void draw_hardpoint_highlight(const Ship* ship, i32 hp_index, f32 time) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return;
    const HardpointDef& hp = ship->hardpoints[hp_index];
    f32 e     = hardpoint_half_extent(hp.size) * 1.35f;
    f32 pulse = 0.60f + 0.40f * sinf(time * 6.0f);
    bs_color c = bs_color{ 1.0f, 1.0f, 1.0f, pulse };
    const Vec2 offsets[4] = { { -e, -e }, { e, -e }, { e, e }, { -e, e } };
    Vec2 w[4];
    for (i32 i = 0; i < 4; ++i) {
        Vec2 lc = vec2_add(hp.pos_local, vec2_rotate(offsets[i], hp.facing));
        w[i]    = vec2_add(ship->render_pos, ship_local_dir(ship, lc));
    }
    for (i32 i = 0; i < 4; ++i)
        renderer_draw_line(w[i], w[(i + 1) % 4], 2.5f, c, LAYER_GIZMO);
}

void draw_hardpoint_drag_feedback(const Ship* ship, i32 hp_index, b8 fits, f32 time) {
    if (!ship || hp_index < 0 || hp_index >= ship->hardpoint_count) return;
    const HardpointDef& hp = ship->hardpoints[hp_index];
    f32      e = hardpoint_half_extent(hp.size) * 1.25f; // sits just outside the base overlay box
    f32      thickness;
    bs_color c;
    if (fits) {
        f32 pulse = 0.55f + 0.45f * sinf(time * 5.0f);
        c         = bs_color{ 0.25f, 1.00f, 0.40f, pulse };
        thickness = 2.5f;
    } else {
        c         = bs_color{ 1.00f, 0.22f, 0.18f, 0.35f };
        thickness = 1.5f;
    }
    const Vec2 offsets[4] = { { -e, -e }, { e, -e }, { e, e }, { -e, e } };
    Vec2 w[4];
    for (i32 i = 0; i < 4; ++i) {
        Vec2 lc = vec2_add(hp.pos_local, vec2_rotate(offsets[i], hp.facing));
        w[i]    = vec2_add(ship->render_pos, ship_local_dir(ship, lc));
    }
    for (i32 i = 0; i < 4; ++i)
        renderer_draw_line(w[i], w[(i + 1) % 4], thickness, c, LAYER_GIZMO);
}

// ---- Procedural mount art (Phase 5) ------------------------------------------------------
// No turret PNGs exist, so mounts are drawn as flat-shaded rectangles straight from the
// sprite batch. All extents are WORLD units (they scale with the hull under zoom), unlike
// renderer_draw_line's screen-pixel thickness.

static const u32 LAYER_MOUNT_ART = LAYER_SHIP + 1; // just above the hull sprite

// Solid rotated rectangle (white texture). custom.z >= 0.5 marks it self-emissive so the
// galaxy-map star light can't wash the dark steel out to white (same fix as the hull).
static void draw_solid_rect(Vec2 center, Vec2 size, f32 rotation, bs_color color) {
    bs_sprite sp{};
    sp.position = center;
    sp.size     = size;
    sp.origin   = Vec2{ 0.5f, 0.5f };
    sp.rotation = rotation;
    sp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
    sp.tint     = color;
    sp.custom   = bs_color{ 0.0f, 0.0f, 1.0f, 0.0f };
    sp.texture  = bs_texture{ BS_INVALID_HANDLE };
    sp.blend    = BLEND_ALPHA;
    sp.layer    = LAYER_MOUNT_ART;
    renderer_draw_sprite(&sp);
}

// Gun turret: base ring fixed to the hull, housing + barrel(s) rotated by the live turret
// aim (Ship.mount_aim). The point-defense variant is smaller with twin barrels.
static void draw_turret(const Ship* ship, i32 hp_index, b8 is_pd, f32 alpha) {
    const HardpointDef& hp = ship->hardpoints[hp_index];
    f32  e   = hardpoint_half_extent(hp.size);
    f32  sc  = ship->world_scale;
    Vec2 c   = vec2_add(ship->render_pos, ship_local_dir(ship, hp.pos_local));
    f32  aim = ship->angle + ship->mount_aim[hp_index]; // world aim angle
    Vec2 fwd = vec2_rotate(Vec2{ 0.0f, 1.0f }, aim);    // aim forward
    Vec2 rgt = vec2_rotate(Vec2{ 1.0f, 0.0f }, aim);    // aim right

    bs_color base_c    = bs_color{ 0.12f, 0.13f, 0.15f, 0.95f * alpha };
    bs_color housing_c = bs_color{ 0.24f, 0.26f, 0.30f, alpha };
    bs_color barrel_c  = bs_color{ 0.34f, 0.37f, 0.42f, alpha };
    bs_color muzzle_c  = bs_color{ 0.09f, 0.10f, 0.11f, alpha };

    if (is_pd) {
        // Base plate (hull-fixed), compact housing, twin barrels.
        draw_solid_rect(c, Vec2{ 1.25f * e * sc, 1.25f * e * sc }, ship->angle, base_c);
        draw_solid_rect(vec2_add(c, vec2_scale(fwd, 0.10f * e * sc)),
                        Vec2{ 0.80f * e * sc, 0.90f * e * sc }, aim, housing_c);
        for (i32 sgn = -1; sgn <= 1; sgn += 2) {
            Vec2 bc = vec2_add(c, vec2_add(vec2_scale(fwd, 0.95f * e * sc),
                                           vec2_scale(rgt, (f32)sgn * 0.22f * e * sc)));
            draw_solid_rect(bc, Vec2{ 0.14f * e * sc, 1.10f * e * sc }, aim, barrel_c);
        }
        return;
    }
    // Main gun: base plate, housing, single heavy barrel with a dark muzzle block.
    draw_solid_rect(c, Vec2{ 1.60f * e * sc, 1.60f * e * sc }, ship->angle, base_c);
    draw_solid_rect(vec2_add(c, vec2_scale(fwd, 0.15f * e * sc)),
                    Vec2{ 1.00f * e * sc, 1.30f * e * sc }, aim, housing_c);
    draw_solid_rect(vec2_add(c, vec2_scale(fwd, 1.15f * e * sc)),
                    Vec2{ 0.28f * e * sc, 1.90f * e * sc }, aim, barrel_c);
    draw_solid_rect(vec2_add(c, vec2_scale(fwd, 2.00f * e * sc)),
                    Vec2{ 0.38f * e * sc, 0.30f * e * sc }, aim, muzzle_c);
}

// Sensor module: hull-fixed base plate with a continuously spinning radar bar + hub.
static void draw_radar_dish(const Ship* ship, i32 hp_index, f32 time, f32 alpha) {
    const HardpointDef& hp = ship->hardpoints[hp_index];
    f32  e    = hardpoint_half_extent(hp.size);
    f32  sc   = ship->world_scale;
    Vec2 c    = vec2_add(ship->render_pos, ship_local_dir(ship, hp.pos_local));
    f32  spin = ship->angle + hp.facing + time * 1.8f;

    draw_solid_rect(c, Vec2{ 1.40f * e * sc, 1.40f * e * sc }, ship->angle,
                    bs_color{ 0.12f, 0.13f, 0.15f, 0.95f * alpha });
    draw_solid_rect(c, Vec2{ 0.18f * e * sc, 2.40f * e * sc }, spin,
                    bs_color{ 0.45f, 0.70f, 0.52f, alpha });
    draw_solid_rect(c, Vec2{ 0.45f * e * sc, 0.45f * e * sc }, spin,
                    bs_color{ 0.26f, 0.30f, 0.28f, alpha });
}

void draw_ship_mounts(const Ship* ship, f32 time, f32 alpha) {
    if (!ship || alpha <= 0.001f) return;
    for (i32 i = 0; i < ship->hardpoint_count; ++i) {
        if (ship->mounts[i]) {
            draw_turret(ship, i, FALSE, alpha);
        } else if (ship->point_defense_mount == i) {
            draw_turret(ship, i, TRUE, alpha);
        } else if (ship->module_mounts[i] && (ship->module_mounts[i]->type & MODULE_TYPE_SENSOR)) {
            draw_radar_dish(ship, i, time, alpha);
        }
    }
}

