#include "core/coord_diag.h"

#ifdef BS_DEBUG

#include "game.h"
#include "sim/fleet.h"
#include "sim/projectile.h"
#include <math/bs_hierpos.h>
#include <cstdio>
#include <cmath>

using namespace bs_math;

// ---- Module state -------------------------------------------------------------------
static b8  g_enabled       = FALSE;
static f32 g_accum         = 0.0f;
static i32 g_frame         = 0;
static i32 g_last_viol     = 0;

// Dump cadence and invariant thresholds.
static constexpr f32 DIAG_INTERVAL      = 0.25f;   // seconds between dumps (~4 Hz)
static constexpr f32 INV_LOCAL_BOUND    = BS_HIERPOS_HALF_CELL + 1.0f; // canonical local must fold here
static constexpr f32 INV_RENDER_BOUND   = 5.0e6f;  // camera-relative render offset sanity ceiling

static FILE* diag_open() {
    // Append so a full run accumulates; the header notes when a session starts.
    FILE* f = nullptr;
    if (fopen_s(&f, "coord_diag.txt", "a") != 0) {
        return nullptr;
    }
    return f;
}

// Check a single HierPos2 against the invariants, logging any violation.
// enforce_render: when FALSE the camera is intentionally decoupled from the fleet
// (free-camera roam or galaxy-map view), so a large camera-relative offset is expected
// and INV3 is logged for reference only -- not counted as a violation.
static i32 diag_check(FILE* f, const char* label,
                      const HierPos2* p, const HierPos2* cam, b8 enforce_render) {
    i32 viol = 0;

    // INV1: canonical local component stays within +/- half a cell.
    if (fabsf(p->local.x) > INV_LOCAL_BOUND || fabsf(p->local.y) > INV_LOCAL_BOUND) {
        fprintf(f, "  [INV1 FAIL] %-10s local out of range: (%.3f, %.3f)\n",
                label, p->local.x, p->local.y);
        ++viol;
    }

    // INV3: camera-relative render offset must be small (proves floating-origin rebase works;
    // a huge value would mean an absolute-f32 position leaked into the render path).
    // Only enforced while the camera tracks the fleet; a detached camera can legitimately
    // sit far from off-screen entities without any precision loss.
    Vec2 rel = hierpos_diff(p, cam, BS_HIERPOS_CELL_SIZE);
    if (enforce_render && (fabsf(rel.x) > INV_RENDER_BOUND || fabsf(rel.y) > INV_RENDER_BOUND)) {
        fprintf(f, "  [INV3 FAIL] %-10s render offset huge: (%.1f, %.1f)\n",
                label, rel.x, rel.y);
        ++viol;
    }

    f64 wx, wy;
    hierpos_to_f64(p, BS_HIERPOS_CELL_SIZE, &wx, &wy);
    fprintf(f, "  %-10s cell=(%lld, %lld) local=(%.2f, %.2f) world=(%.1f, %.1f) rel_cam=(%.2f, %.2f)\n",
            label, (long long)p->cell.x, (long long)p->cell.y,
            p->local.x, p->local.y, wx, wy, rel.x, rel.y);
    return viol;
}

void coord_diag_set_enabled(b8 enabled) { g_enabled = enabled; }
b8   coord_diag_is_enabled(void)        { return g_enabled; }
i32  coord_diag_last_violations(void)   { return g_last_viol; }

void coord_diag_update(const game_state* s, f32 dt) {
    if (!g_enabled || !s) return;

    g_accum += dt;
    if (g_accum < DIAG_INTERVAL) return;
    g_accum = 0.0f;
    ++g_frame;

    FILE* f = diag_open();
    if (!f) return;

    const HierPos2* cam = &s->camera_state.camera_hierpos;
    i32 viol = 0;

    // The camera only tracks the fleet during normal gameplay. When the free camera is
    // detached or we're viewing the galaxy map, entities are expected to fall far outside
    // the render window, so the INV3 render-offset ceiling is not enforced (it would be a
    // false positive -- the offset is still computed exactly via integer cell subtraction).
    b8 enforce_render = (!s->camera_state.free_camera_active && s->view.mode != MODE_SYSTEM) ? TRUE : FALSE;

    f64 cx, cy;
    hierpos_to_f64(cam, BS_HIERPOS_CELL_SIZE, &cx, &cy);
    fprintf(f, "==== frame %d  t=%.2f  render_enforced=%d ====\n", g_frame, s->elapsed_time, enforce_render ? 1 : 0);
    fprintf(f, "  camera     cell=(%lld, %lld) local=(%.2f, %.2f) world=(%.1f, %.1f)\n",
            (long long)cam->cell.x, (long long)cam->cell.y,
            cam->local.x, cam->local.y, cx, cy);

    // Camera itself must be canonical (INV1 only; rel-to-self is trivially zero).
    if (fabsf(cam->local.x) > INV_LOCAL_BOUND || fabsf(cam->local.y) > INV_LOCAL_BOUND) {
        fprintf(f, "  [INV1 FAIL] camera local out of range\n");
        ++viol;
    }

    // Player fleet ships.
    for (i32 i = 0; i < s->fleet_state.fleet.count(); ++i) {
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "fleet[%d]", i);
        viol += diag_check(f, lbl, &s->fleet_state.fleet.at(i).ship.origin, cam, enforce_render);
    }

    // Enemy ship.
    viol += diag_check(f, "enemy", &s->fleet_state.enemy_ship.origin, cam, enforce_render);

    // Combat entities (non-ship targets carry their own HierPos2 position).
    for (i32 i = 0; i < s->combat_entity_count; ++i) {
        const CombatEntity* ce = &s->combat_entities[i];
        if (!ce->active) continue;
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "ce[%d]", i);
        viol += diag_check(f, lbl, &ce->position, cam, enforce_render);
    }

    // Active projectiles (sample the first few to keep the log bounded).
    i32 proj_logged = 0;
    for (i32 i = 0; i < MAX_PROJECTILES && proj_logged < 8; ++i) {
        const Projectile& p = s->projectiles.pool[i];
        if (!p.active) continue;
        char lbl[16];
        snprintf(lbl, sizeof(lbl), "proj[%d]", i);
        viol += diag_check(f, lbl, &p.position, cam, enforce_render);
        ++proj_logged;
    }

    if (viol > 0)
        fprintf(f, "  >>> %d invariant violation(s) this frame <<<\n", viol);

    fclose(f);
    g_last_viol = viol;
}

#else  // !BS_DEBUG

void coord_diag_update(const game_state*, f32) {}
void coord_diag_set_enabled(b8) {}
b8   coord_diag_is_enabled(void) { return FALSE; }
i32  coord_diag_last_violations(void) { return 0; }

#endif // BS_DEBUG
