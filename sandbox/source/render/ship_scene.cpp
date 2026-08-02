#include "render/ship_scene.h"
#include "game.h"
#include "sim/ship.h"                 // ship_local_dir
#include "core/view_transform.h"    // render_from_hierpos
#include "sim/celestial_parallax.h" // celestial_center_render
#include "render/debug_overlay.h"   // g_debug_cell_grid, draw_hierpos_cell_grid
#include "render/ship_render.h"     // draw_collider_outline, draw_fleet_emblems
#include "core/cursor_world.h"      // mouse_true_hierpos (ship-side drag tether)
#include "core/render_layers.h"   // LAYER_SHIP / LAYER_UI
#include "render/text.h"            // text_draw (weapon-group digits)

#include <renderer/renderer.h>
#include <renderer/camera2d.h>      // camera2d_world_to_screen (weapon-group digits)
#include <math.h>

using namespace bs_math;

// ---- Engine exhaust tuning -------------------------------------------------------------
// Jet length is proportional to the hull so a drone's plume and a cruiser's plume both read
// correctly. Fractions match the legacy 16+48-unit jet on the original ~41-unit drone hull.
static const f32 EXHAUST_BASE_FRAC   = 0.40f;  // idle jet length, x hull length
static const f32 EXHAUST_EXTRA_FRAC  = 1.15f;  // extra length at full throttle, x hull length
static const f32 EXHAUST_FLICKER_HZ1 = 30.0f;
static const f32 EXHAUST_FLICKER_HZ2 = 47.3f;
static const f32 EXHAUST_JITTER_AMP  = 0.06f;

static const bs_color COLLIDER_COLOR = bs_color{ 1.00f, 0.18f, 0.85f, 1.0f };

// Mirror of arsenal_drop_on_slot's type/size gates (game.cpp): can the armed arsenal drag
// (kind/src, see game_state.pending_weapon_drag_kind) land on hardpoint dst? Drives the
// green/red world-box feedback while a drag is airborne.
static b8 arsenal_drag_fits(const Ship& fs, i32 kind, i32 src, i32 dst) {
    const HardpointDef& dhp = fs.hardpoints[dst];
    switch (kind) {
        case 0: case 1: return hardpoint_accepts(&dhp, MODULE_TYPE_WEAPON);
        case 2: case 3: return hardpoint_accepts(&dhp, MODULE_TYPE_DEFENSE);
        case 4: return (src >= 0 && src < fs.module_stash_count)
                           ? hardpoint_fits_module(&dhp, fs.module_stash[src]) : FALSE;
        case 5: return (src >= 0 && src < fs.hardpoint_count && fs.module_mounts[src])
                           ? hardpoint_fits_module(&dhp, fs.module_mounts[src]) : FALSE;
        default: return FALSE;
    }
}

// Weapon-group digits: under each mounted weapon's hardpoint box, print the group numbers
// the weapon belongs to. Members of the ACTIVE fire group render bright (they fire on
// click), the rest dim. Screen-anchored bitmap text so the labels stay readable at any zoom.
static void draw_weapon_group_digits(game_state* s, const Ship* ship) {
    const Camera2D* cam = &s->camera_state.camera;
    f32 zoom = cam->zoom > 1.0e-6f ? cam->zoom : 1.0e-6f;
    const f32 scale = 1.5f;                          // 12 px glyphs
    for (i32 h = 0; h < ship->hardpoint_count; ++h) {
        if (!ship->mounts[h]) continue;
        Vec2 center = vec2_add(ship->render_pos,
                               ship_local_dir(ship, ship->hardpoints[h].pos_local));
        Vec2 sc = camera2d_world_to_screen(cam, s->fb_width, s->fb_height, center);
        f32 e   = hardpoint_half_extent(ship->hardpoints[h].size) * 1.25f * zoom;
        f32 step = text_width("0", scale) + 3.0f;
        f32 x = sc.x - ((f32)SHIP_WEAPON_GROUPS * step - 3.0f) * 0.5f;
        f32 y = sc.y + e + 4.0f;
        for (i32 g = 0; g < SHIP_WEAPON_GROUPS; ++g, x += step) {
            if (!((ship->mount_groups[h] >> g) & 1)) continue;
            char d[2] = { (char)('1' + g), '\0' };
            bs_color col = (g == ship->active_group)
                               ? bs_color{ 0.35f, 1.00f, 0.55f, 0.95f }    // active group: green
                               : bs_color{ 0.75f, 0.85f, 1.00f, 0.55f };   // other groups: dim
            text_draw(d, x, y, scale, col, cam, s->fb_width, s->fb_height, LAYER_GIZMO);
        }
    }
}

static void draw_ship_visual_ex(const Ship* ship, f32 alpha, bs_math::Vec3 light_dir,
                                bs_color tint, EBlendMode blend, bs_color custom) {
    if (!ship || alpha <= 0.001f) return;
    for (i32 i = 0; i < ship->visual.layer_count; ++i) {
        const VisualLayer& vl = ship->visual.layers[i];
        if (vl.kind == VIS_LAYER_SPRITE) {
            if (!vl.texture.id) continue;
            bs_sprite sp{};
            sp.position = vec2_add(ship->render_pos, ship_local_dir(ship, vl.offset_local));
            sp.size     = ship->visual.size_local;
            sp.origin   = Vec2{ 0.5f, 0.5f };
            sp.rotation = ship->angle;
            sp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
            sp.tint          = tint;
            sp.tint.a       *= alpha;
            sp.texture       = vl.texture;
            sp.blend         = blend;
            sp.layer         = LAYER_SHIP + vl.z;
            // Hull art is plain sprite art. In sprite.frag.hlsl custom.x/custom.w gate the
            // exhaust-style effects (heat distortion, tail->head temperature gradient, head
            // bloom, radial glow), so zero them here or the SHIP GLOW parameters warp and
            // tint the hull. custom.z >= 0.5 flags the sprite self-emissive so the map-look
            // star light + bright ambient can't wash it out to white at galaxy zoom.
            sp.custom        = bs_color{ 0.0f, custom.g, 1.0f, 0.0f };
            renderer_draw_sprite(&sp);
        } else if (vl.kind == VIS_LAYER_MAPPED) {
            if (!vl.texture.id || !vl.normal_map.id || !vl.depth_map.id || !vl.position_map.id) continue;
            bs_mapped_sprite mp{};
            mp.position = vec2_add(ship->render_pos, ship_local_dir(ship, vl.offset_local));
            mp.size     = ship->visual.size_local;
            mp.origin   = Vec2{ 0.5f, 0.5f };
            mp.rotation = ship->angle;
            mp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
            mp.tint     = tint;
            mp.tint.a  *= alpha;
            mp.diffuse_map  = vl.texture;
            mp.normal_map   = vl.normal_map;
            mp.depth_map    = vl.depth_map;
            mp.position_map = vl.position_map;
            mp.light_dir = light_dir;
            mp.layer    = LAYER_SHIP + vl.z;
            renderer_draw_mapped_sprite(&mp);
        }
    }
}

static inline void draw_ship_visual(const Ship* ship, f32 alpha, bs_math::Vec3 light_dir) {
    draw_ship_visual_ex(ship, alpha, light_dir,
                        bs_color{ 1.0f, 1.0f, 1.0f, 1.0f }, BLEND_ALPHA,
                        bs_color{ 1.0f, 0.0f, 0.0f, 0.0f });
}

// Draw the enemy ship based on the dedicated ship sensor range.
// Inside the range: real sprite. Outside: the OutSensorDetectionFX subsystem draws the effect.
static void draw_enemy_ship_sensor(game_state* s) {
    if (!s) return;
    Ship* enemy = &s->fleet_state.enemy_ship;
    Ship* player = &s->player_ship();
    Vec2 delta = hierpos_diff(&enemy->origin, &player->origin, BS_HIERPOS_CELL_SIZE);
    f32 dist = vec2_length(delta);
    f32 range = s->ship_sensor_range;
    f32 vis = sensor_visibility_from_dist(dist, range);
    // Light direction: use the fleet-wide star direction, or default if star is unknown.
    bs_math::Vec3 default_light_dir = bs_math::Vec3{ 0.0f, -1.0f, 0.2f };
    bs_math::Vec3 light_dir = default_light_dir;
    bs_math::Vec2 to_star = vec2_sub(s->star_pos, enemy->render_pos);
    f32 star_dist = vec2_length(to_star);
    if (star_dist > 0.001f) {
        bs_math::Vec2 d = vec2_scale(to_star, 1.0f / star_dist);
        light_dir = bs_math::Vec3{ d.x, d.y, 0.2f };
    }
    // Real sprite, visible inside the sensor range.
    if (vis > 0.001f) {
        draw_ship_visual(enemy, vis, light_dir);
        draw_ship_mounts(enemy, s->elapsed_time, vis);
    }
    // Interference effect is rendered by the dedicated subsystem.
    s->out_sensor_fx.render(s->elapsed_time, enemy, player, range, s->camera_state.camera.zoom);
}

// Draw a multi-layer additive engine-exhaust jet behind the ship.
// Layers: white-hot core, orange body, red halo.  Per-frame flicker makes it feel turbulent.
// custom.x gates heat distortion, colour temp, and radial glow in sprite.frag.hlsl.
static void draw_engine_exhaust(const Ship* ship, bs_texture exhaust_tex,
                                const bs_glow_params* glow,
                                f32 speed_ratio, f32 alpha, f32 time) {
    if (!ship || alpha <= 0.001f) return;
    // Flicker: two out-of-phase sines for organic turbulence.
    f32 flicker = sinf(time * EXHAUST_FLICKER_HZ1) * 0.5f
                + sinf(time * EXHAUST_FLICKER_HZ2) * 0.25f;
    f32 jitter = 1.0f + flicker * EXHAUST_JITTER_AMP;
    Vec2 fwd = vec2_rotate(Vec2{ 0.0f, 1.0f }, ship->angle);
    f32 hull_len = ship->visual.size_local.y;
    f32 rear_offset = hull_len * 0.5f + 2.0f;
    Vec2 rear = vec2_sub(ship->render_pos, vec2_scale(fwd, rear_offset * jitter));
    f32 base_h = hull_len * (EXHAUST_BASE_FRAC + speed_ratio * EXHAUST_EXTRA_FRAC);
    f32 base_w = ship->visual.size_local.x * 0.35f;
    // Three layers: core (white, smallest, highest glow), body (orange), halo (red, largest).
    // The soft radial-gradient texture (exhaust_tex) provides natural tapered falloff.
    struct ExhaustLayer { f32 size_mul; f32 glow_mul; bs_color tint; };
    static const ExhaustLayer layers[3] = {
        { 0.35f, 1.5f, bs_color{ 1.00f, 0.95f, 0.85f, 0.90f } }, // core
        { 0.60f, 1.0f, bs_color{ 1.00f, 0.55f, 0.15f, 0.80f } }, // body
        { 0.90f, 0.6f, bs_color{ 1.00f, 0.25f, 0.05f, 0.60f } }, // halo
    };
    for (i32 i = 0; i < 3; ++i) {
        const ExhaustLayer& L = layers[i];
        bs_sprite sp{};
        sp.position = rear;
        sp.size     = Vec2{
            base_w * L.size_mul * jitter * 0.6f,  // taller-than-wide jet
            base_h * L.size_mul * jitter
        };
        sp.origin   = Vec2{ 0.5f, 0.5f };
        sp.rotation = ship->angle;
        sp.uv       = bs_rect{ 0.0f, 0.0f, 1.0f, 1.0f };
        sp.tint     = L.tint;
        sp.tint.a  *= alpha * speed_ratio;      // invisible when stationary
        sp.custom   = bs_color{ speed_ratio * L.glow_mul, 0.0f, 0.0f, 0.0f };
        sp.texture  = exhaust_tex;
        sp.blend         = BLEND_ADDITIVE;
        sp.layer           = LAYER_SHIP;
        sp.glow_override   = glow;
        renderer_draw_sprite(&sp);
    }
}

void draw_ship_scene(game_state* s) {
    // Transform dynamic world positions into render space: shift relative to the camera_hierpos
    // anchor. The persistent HierPos2 simulation state is never mutated by rendering.

    // Star position drives ship lighting/visuals. On the arena side of the blend band the
    // parallax-background pass already set s->star_pos; here we only cover the deep galaxy-map side
    // (view_arena_w == 0), where that pass is skipped. This keeps s->star_pos defined across the
    // whole zoom range without a mode branch.
    if (s->view_arena_w <= 0.0f) {
        if (s->galaxy.current_system >= 0 && s->galaxy.current_system < s->galaxy.system_count) {
            const StarSystem& ss = s->galaxy.systems[s->galaxy.current_system];
            // Depth parallax (precision-safe via hierpos_diff), matching the drawn star so
            // ship lighting tracks the parallaxed star on the deep galaxy-map side too.
            Vec2 sys_rel = celestial_center_render(s, &ss.galaxy_center, &s->celestial_anchor, s->render.depth_star);
            s->star_pos = vec2_add(sys_rel, ss.star.position);
        } else {
            s->star_pos = bs_math::Vec2{ 0.0f, 0.0f };
        }
    }

    // Compute each entity's transient render-space position (precision-safe: subtracts the
    // camera_hierpos anchor in i64 cell space). The persistent HierPos2 simulation state is never
    // mutated by rendering.
    for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {
        s->fleet_state.fleet.at(i).ship.render_pos = render_from_hierpos(s, &s->fleet_state.fleet.at(i).ship.origin);
    }
    s->fleet_state.enemy_ship.render_pos = render_from_hierpos(s, &s->fleet_state.enemy_ship.origin);
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        CombatEntity* ce = &s->combat_entities[i];
        if (ce->active && !ce->ship) {
            ce->render_pos = render_from_hierpos(s, &ce->position);
        }
    }

    // ---- DEBUG: HierPos2 cell-grid overlay (editor-panel toggle) ------------------------
    if (g_debug_cell_grid) draw_hierpos_cell_grid(s);

    // Directional star light: from each ship toward the current star. For a distant star
    // all ships get roughly the same direction, but we compute it per ship to stay correct.
    bs_math::Vec3 default_light_dir = bs_math::Vec3{ 0.0f, -1.0f, 0.2f };
    // Compute a fleet-wide light direction from the player ship to the star as a fallback.
    bs_math::Vec2 to_star = vec2_sub(s->star_pos, s->player_ship().render_pos);
    f32 star_dist = vec2_length(to_star);
    bs_math::Vec3 fleet_light_dir = default_light_dir;
    if (star_dist > 0.001f) {
        bs_math::Vec2 d = vec2_scale(to_star, 1.0f / star_dist);
        fleet_light_dir = bs_math::Vec3{ d.x, d.y, 0.2f };
    }

    // Render each fleet ship with its own per-ship speed ratio.
    s->profiler.begin(PROF_SHIPS);
    for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {
        const FleetShip& fs = s->fleet_state.fleet.at(i);
        const Ship* ship = &fs.ship;
        f32 ship_speed = vec2_length(fs.flight.velocity);
        f32 ship_speed_ratio = clampf(ship_speed / ship->motion.max_speed, 0.0f, 1.0f);

        bs_math::Vec2 ship_to_star = vec2_sub(s->star_pos, ship->render_pos);
        f32 dist = vec2_length(ship_to_star);
        bs_math::Vec3 light_dir = fleet_light_dir;
        if (dist > 0.001f) {
            bs_math::Vec2 d = vec2_scale(ship_to_star, 1.0f / dist);
            light_dir = bs_math::Vec3{ d.x, d.y, 0.2f };
        }

        draw_ship_visual(ship, 1.0f, light_dir);
        draw_ship_mounts(ship, s->elapsed_time, 1.0f);
        draw_engine_exhaust(ship, s->render.exhaust_texture, &s->render.exhaust_glow,
                            ship_speed_ratio, 1.0f, s->elapsed_time);
        // ---- DEBUG collider overlay.
        draw_collider_outline(ship, COLLIDER_COLOR, 1.5f);
        // Hardpoint skeleton: only on the flagship while its inspector is open (the loadout
        // surface). During an airborne arsenal drag each box also gets fit feedback - pulsing
        // green where the dragged item can land, dim red where the slot rejects it. The
        // hardpoint editor draws its own overlay (editor_ui.cpp).
        if (s->show_flagship_inspector && ship == &s->player_ship()) {
            draw_hardpoint_overlay(ship, 1.5f);
            draw_weapon_group_digits(s, ship);
            if (s->pending_weapon_drag >= 0) {
                for (i32 h = 0; h < ship->hardpoint_count; ++h)
                    draw_hardpoint_drag_feedback(ship, h,
                        arsenal_drag_fits(*ship, s->pending_weapon_drag_kind,
                                          s->pending_weapon_drag, h),
                        s->elapsed_time);
            }
            // Ship-side pick-up carry cue: pulse the source box white and tether it to the
            // cursor so the lifted item visibly rides the mouse.
            if (s->world_module_drag && s->pending_weapon_drag >= 0 &&
                s->pending_weapon_drag < ship->hardpoint_count) {
                i32 src = s->pending_weapon_drag;
                draw_hardpoint_highlight(ship, src, s->elapsed_time);
                Vec2 c0 = vec2_add(ship->render_pos,
                                   ship_local_dir(ship, ship->hardpoints[src].pos_local));
                bs_math::HierPos2 mw = mouse_true_hierpos(s);
                Vec2 cur = render_from_hierpos(s, &mw);
                renderer_draw_line(c0, cur, 1.5f, bs_color{ 1.0f, 1.0f, 1.0f, 0.65f },
                                   LAYER_GIZMO);
            }
        }
    }
    // ---- NPC AI ships (General Ship AI): moving, civ-coloured hulls ---------------------
    for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {
        NpcShip& n = s->npc_ships[i];
        if (!n.active) continue;
        n.ship.render_pos = render_from_hierpos(s, &n.ship.origin);
        if (!n.discovered) continue;     // generic "unidentified" marker is drawn in gameplay_overlays
        bs_color tint = (n.faction >= 0 && n.faction < s->galaxy.civ_count)
                          ? s->galaxy.civs[n.faction].color
                          : bs_color{ 0.95f, 0.35f, 0.30f, 1.0f };
        tint.a = 1.0f;
        bs_math::Vec2 to_star = vec2_sub(s->star_pos, n.ship.render_pos);
        f32 sd = vec2_length(to_star);
        bs_math::Vec3 ld = fleet_light_dir;
        if (sd > 0.001f) { bs_math::Vec2 d = vec2_scale(to_star, 1.0f / sd); ld = bs_math::Vec3{ d.x, d.y, 0.2f }; }
        draw_ship_visual_ex(&n.ship, 1.0f, ld, tint, BLEND_ALPHA, bs_color{ 1.0f, 0.0f, 0.0f, 0.0f });
        draw_engine_exhaust(&n.ship, s->render.exhaust_texture, &s->render.exhaust_glow,
                            clampf(vec2_length(n.flight.velocity) / n.ship.motion.max_speed, 0.0f, 1.0f),
                            1.0f, s->elapsed_time);
    }
    // Enemy ship rendering based on the dedicated ship sensor range.
    // Inside the range: real sprite. Outside: flickering interference silhouette.
    draw_enemy_ship_sensor(s);
    s->profiler.end(PROF_SHIPS);

    // ---- Combat entities (non-ship targets draw as quads) -------------------------------
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        const CombatEntity* ce = &s->combat_entities[i];
        if (!ce->active || ce->ship) continue; // ships are drawn above
        renderer_draw_quad(ce->render_pos,
                           Vec2{ ce->radius * 2.0f, ce->radius * 2.0f },
                           ce->tint, LAYER_UI);
    }

    // ---- Ship type emblem + velocity overlay (combat mode now lives in system mode).
    // Emblems are clustered by type so overlapping emblems at low zoom fuse into one.
    draw_fleet_emblems(s);
}
