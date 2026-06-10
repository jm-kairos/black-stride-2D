// =====================================================================================
// Headless verification: ship-ship collision + HULL_DOOR docking geometry.
//
// Links the REAL ship.cpp against engine.lib (vec2_* + logger are DLL-exported), so this
// exercises the actual production code — ship_load parsing the real .tmap files, and the
// real ships_overlap / ships_docked under the full rigid-body pose (origin AND angle). No
// window, no input, deterministic. Pattern per blackstride-build-verify skill: extract the
// pure geometry question, replay it against the real functions, assert the discriminating
// quantity.
//
// Build & run (from repo root c:\dev\blackstride so "assets/..." resolves; exe lives in bin\
// so engine.dll is found beside it):
//   clang++ -std=c++17 -g -DBS_DEBUG -DBSIMPORT -Isandbox/source -I engine/source \
//       bin/verify_docking_headless.cpp sandbox/source/ship.cpp \
//       -L bin -lengine.lib -o bin/verify_docking_headless.exe
//   ./bin/verify_docking_headless.exe       (exit 0 = PASS)
// =====================================================================================
#include "ship.h"
#include "nav.h"
#include <math/math_utils.h>
#include <cstdio>
#include <cmath>

using namespace bs_math;

static int g_fail = 0;
static void check(const char* name, bool cond) {
    std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", name);
    if (!cond) g_fail++;
}

// Count HULL_DOOR tiles in a ship (sanity that the .tmap authored the airlocks we expect).
static int count_doors(const Ship* s) {
    int n = 0;
    for (i32 r = 0; r < s->rows; ++r)
        for (i32 c = 0; c < s->cols; ++c)
            if (ship_tile_at(s, c, r) == TILE_HULL_DOOR) ++n;
    return n;
}

int main() {
    Ship player{}, enemy{};
    if (!ship_load(&player, "assets/ship.tmap")) { std::printf("FATAL: load player\n"); return 2; }
    if (!ship_load(&enemy,  "assets/enemy_ship.tmap")) { std::printf("FATAL: load enemy\n"); return 2; }

    const f32 ts  = player.tile_size;
    const f32 tol = ts * 1.5f;                 // same tolerance game_update uses

    std::printf("Player %dx%d  doors=%d  r=%.1f\n", player.cols, player.rows, count_doors(&player), ship_bounding_radius(&player));
    std::printf("Enemy  %dx%d  doors=%d  r=%.1f\n", enemy.cols,  enemy.rows,  count_doors(&enemy),  ship_bounding_radius(&enemy));

    std::printf("\n-- airlock authoring --\n");
    check("player has >=2 HULL_DOOR airlocks", count_doors(&player) >= 2);
    check("enemy  has >=2 HULL_DOOR airlocks", count_doors(&enemy)  >= 2);

    // Player stays at the world origin, upright (matches game_init).
    player.origin = Vec2{ 0.0f, 0.0f };
    player.angle  = 0.0f;

    // ---- 1. Default placement (game_init): far apart => undocked, broad phase rejects ----
    std::printf("\n-- default placement {520,360} @135deg --\n");
    enemy.origin = Vec2{ 520.0f, 360.0f };
    enemy.angle  = 2.36f;
    check("default: ships_overlap == FALSE (bounding circles clear)", !ships_overlap(&player, &enemy));
    check("default: ships_docked == FALSE (enemy interior hidden)",
          !ships_docked(&player, &enemy, tol, nullptr, nullptr, nullptr, nullptr));

    // Mating target: one tile to the right of the player's right airlock (col11,row7), so the two
    // airlock tiles end up edge-adjacent. For ANY enemy heading theta we solve the origin that puts
    // the enemy's left airlock (col1,row6) exactly on that target: origin = target - R(theta)*local.
    Vec2 player_door = ship_tile_center_world(&player, 11, 7);
    Vec2 target      = vec2_add(player_door, Vec2{ ts, 0.0f });
    Vec2 enemy_local = ship_tile_center_local(&enemy, 1, 6);

    auto dock_origin_for = [&](f32 theta) {
        return vec2_sub(target, vec2_rotate(enemy_local, theta));
    };

    // ---- 2. Mated, upright (the screenshot pose): docked TRUE, overlap TRUE ----
    std::printf("\n-- mated, enemy upright (angle 0) --\n");
    enemy.angle  = 0.0f;
    enemy.origin = dock_origin_for(0.0f);
    std::printf("   computed docked origin = {%.1f, %.1f}\n", enemy.origin.x, enemy.origin.y);
    i32 ca, ra, cb, rb;
    b8  docked = ships_docked(&player, &enemy, tol, &ca, &ra, &cb, &rb);
    check("mated: ships_overlap == TRUE", ships_overlap(&player, &enemy));
    check("mated: ships_docked == TRUE", docked);
    if (docked) {
        std::printf("   matched airlocks: player(col=%d,row=%d) <-> enemy(col=%d,row=%d)\n", ca, ra, cb, rb);
        check("matched player airlock is a HULL_DOOR", ship_tile_at(&player, ca, ra) == TILE_HULL_DOOR);
        check("matched enemy  airlock is a HULL_DOOR", ship_tile_at(&enemy,  cb, rb) == TILE_HULL_DOOR);
        f32 dd = vec2_length(vec2_sub(ship_tile_center_world(&player, ca, ra),
                                      ship_tile_center_world(&enemy,  cb, rb)));
        check("matched airlock centers within tolerance", dd <= tol);
    }

    // ---- 3. Rotation correctness: mate with the enemy ROTATED 90deg ----
    // Proves the docking handshake honors BOTH origin AND angle (uses ship_tile_center_world).
    std::printf("\n-- mated, enemy rotated 90deg --\n");
    enemy.angle  = BS_PI * 0.5f;
    enemy.origin = dock_origin_for(enemy.angle);
    std::printf("   computed docked origin = {%.1f, %.1f}\n", enemy.origin.x, enemy.origin.y);
    check("rotated mate: ships_docked == TRUE (pose-correct under rotation)",
          ships_docked(&player, &enemy, tol, nullptr, nullptr, nullptr, nullptr));

    // ---- 4. Narrow-phase has teeth: overlapping hulls but airlocks too far => NOT docked ----
    // Slide the upright enemy outward so the airlocks are >tolerance apart while the bounding
    // circles still intersect. Docking must reject this (contact != docked).
    std::printf("\n-- overlapping but airlocks too far (angle 0, +60px) --\n");
    enemy.angle  = 0.0f;
    enemy.origin = vec2_add(dock_origin_for(0.0f), Vec2{ 60.0f, 0.0f }); // 32 -> 92px door gap (> 48 tol)
    check("too-far: ships_overlap == TRUE (broad phase still in contact)", ships_overlap(&player, &enemy));
    check("too-far: ships_docked == FALSE (narrow phase rejects)",
          !ships_docked(&player, &enemy, tol, nullptr, nullptr, nullptr, nullptr));

    // ---- 5. OBB COLLISION (ships_collide) — the "no phasing" core --------------------------
    // The enemy is an immovable obstacle; the player must never penetrate its hull. We verify the
    // SAT/MTV contract: deep overlap is detected, the MTV separates on a SINGLE application, the
    // separated state then reports no collision (idempotent — proves no residual penetration), and
    // far-apart / lightly-touching poses behave correctly.
    std::printf("\n-- OBB collision: deep overlap (enemy origin == player origin) --\n");
    {
        Ship p2 = player; // copies (flat POD) so we can mutate origin without disturbing the docked tests
        Ship e2 = enemy;
        p2.origin = Vec2{ 0.0f, 0.0f }; p2.angle = 0.0f;
        e2.origin = Vec2{ 0.0f, 0.0f }; e2.angle = 0.0f; // fully coincident -> maximal penetration

        Vec2 mtv{ 0.0f, 0.0f };
        b8 hit = ships_collide(&p2, &e2, &mtv);
        check("coincident hulls: ships_collide == TRUE", hit);
        check("coincident hulls: MTV is non-zero", vec2_length(mtv) > 1.0f);
        if (hit) {
            // Push the PLAYER out by the MTV (enemy immovable) — exactly what game_update will do.
            p2.origin = vec2_add(p2.origin, mtv);
            // After one push the hulls must no longer overlap (allow a hair of float slop).
            Vec2 mtv2{ 0.0f, 0.0f };
            b8 still = ships_collide(&p2, &e2, &mtv2);
            check("after applying MTV once: hulls separated (no phasing)", !still || vec2_length(mtv2) < 0.5f);
        }
    }

    std::printf("\n-- OBB collision: glancing overlap from the side --\n");
    {
        Ship p2 = player; Ship e2 = enemy;
        p2.origin = Vec2{ 0.0f, 0.0f }; p2.angle = 0.0f;
        // Player occupied half-width = 11*32/2 = 176; enemy half-width = 7*32/2 = 112. Centers
        // 250 apart on X => 288-250 = 38px penetration: a real but shallow side overlap.
        e2.origin = Vec2{ 250.0f, 0.0f }; e2.angle = 0.0f;
        Vec2 mtv{ 0.0f, 0.0f };
        b8 hit = ships_collide(&p2, &e2, &mtv);
        check("side overlap: ships_collide == TRUE", hit);
        if (hit) {
            // Minimum-penetration axis is X here; MTV should push the player in -X (away from enemy).
            check("side overlap: MTV pushes player away on -X", mtv.x < -1.0f && fabsf(mtv.y) < 1.0f);
            p2.origin = vec2_add(p2.origin, mtv);
            check("side overlap: separated after one MTV push",
                  !ships_collide(&p2, &e2, nullptr));
        }
    }

    std::printf("\n-- OBB collision: clearly apart => no collision --\n");
    {
        Ship p2 = player; Ship e2 = enemy;
        p2.origin = Vec2{ 0.0f, 0.0f }; p2.angle = 0.0f;
        e2.origin = Vec2{ 520.0f, 360.0f }; e2.angle = 2.36f; // the shipped default placement
        check("apart: ships_collide == FALSE", !ships_collide(&p2, &e2, nullptr));
    }

    std::printf("\n-- OBB collision vs docking: mated pose is in contact but barely penetrating --\n");
    {
        // At the mated pose the airlocks are edge-adjacent (32px center-to-center == one tile), so the
        // tight occupied OBBs are JUST touching. Collision may read a hair of overlap; what matters is
        // it's tiny (<~ a third of a tile) — docking will zero the velocity and lock the joint, so a
        // sub-tile residual never accumulates into a visible interpenetration.
        Ship p2 = player; Ship e2 = enemy;
        p2.origin = Vec2{ 0.0f, 0.0f }; p2.angle = 0.0f;
        e2.angle  = 0.0f;
        e2.origin = dock_origin_for(0.0f);
        Vec2 mtv{ 0.0f, 0.0f };
        b8 hit = ships_collide(&p2, &e2, &mtv);
        f32 pen = hit ? vec2_length(mtv) : 0.0f;
        std::printf("   mated-pose penetration = %.2f px (tile=%.0f)\n", pen, ts);
        check("mated pose: penetration is sub-tile (docking & collision coexist)", pen < ts * 0.5f);
    }

    // ---- 6. DOCK SNAP (dock_snap_delta) — the Phase 2 mating geometry --------------------------
    // When the player presses T while dock-eligible, the game translates the player by
    // dock_snap_delta so its airlock sits exactly one tile off the enemy's, the clean mated pose.
    // Verify against the REAL function: from a slightly-off but eligible approach, the snap must land
    // a pose that (a) reports docked, (b) barely penetrates (sub-tile, so collision+dock coexist),
    // and (c) is idempotent (re-snapping from the snapped pose moves ~nothing). Player is the mover;
    // enemy is the immovable anchor — matches game_update (the player snaps to the derelict).
    std::printf("\n-- dock snap: upright approach, slightly off --\n");
    {
        Ship p2 = player; Ship e2 = enemy;
        e2.angle  = 0.0f;
        e2.origin = Vec2{ 520.0f, 360.0f }; // arbitrary enemy placement (anchor)
        p2.angle  = 0.0f;
        // Put the player roughly mated but a touch too close / off: start from the ideal then nudge.
        Vec2 pd  = ship_tile_center_world(&e2, 1, 6);              // enemy left airlock (world)
        Vec2 tgt = vec2_add(pd, Vec2{ - enemy.tile_size, 0.0f });  // one tile out on -X
        Vec2 pl  = ship_tile_center_local(&p2, 11, 7);             // player right airlock (local)
        p2.origin = vec2_sub(tgt, vec2_rotate(pl, p2.angle));      // mate, then perturb:
        p2.origin = vec2_add(p2.origin, Vec2{ 14.0f, 9.0f });     // 16.6px off -> still within 48 tol

        check("pre-snap: dock-eligible (within tolerance)",
              ships_docked(&p2, &e2, ts * 1.5f, nullptr, nullptr, nullptr, nullptr));
        Vec2 delta{ 0.0f, 0.0f };
        f32  dtheta = 0.0f;
        b8 ok = dock_snap_delta(&p2, &e2, ts * 1.5f, &delta, &dtheta);
        check("dock_snap_delta returns TRUE when eligible", ok);
        if (ok) {
            // Upright approach: both airlock normals already lie on X, so the corrective rotation is ~0.
            check("upright snap: corrective rotation ~0 (already parallel)", fabsf(dtheta) < 0.01f);
            p2.angle  = p2.angle + dtheta;             // apply rotation (doors parallel) ...
            p2.origin = vec2_add(p2.origin, delta);    // ... then translation (one-tile flush gap)
            check("post-snap: ships_docked == TRUE (mated)",
                  ships_docked(&p2, &e2, ts * 1.5f, nullptr, nullptr, nullptr, nullptr));
            // Doors must end PARALLEL & FACING: the two airlock outward normals are anti-parallel.
            i32 ca2, ra2, cb2, rb2;
            if (ships_docked(&e2, &p2, ts * 1.5f, &ca2, &ra2, &cb2, &rb2)) {
                Vec2 ne, np;
                if (ship_airlock_outward_normal(&e2, ca2, ra2, &ne) &&
                    ship_airlock_outward_normal(&p2, cb2, rb2, &np)) {
                    f32 dot = vec2_dot(ne, np);
                    std::printf("   upright snap: airlock-normal dot = %.3f (want -1)\n", dot);
                    check("upright snap: doors parallel & facing (normals anti-parallel)", dot < -0.999f);
                }
            }
            Vec2 mtv{ 0.0f, 0.0f };
            b8  hit = ships_collide(&p2, &e2, &mtv);
            f32 pen = hit ? vec2_length(mtv) : 0.0f;
            std::printf("   post-snap penetration = %.2f px\n", pen);
            check("post-snap: penetration sub-tile (collision+dock coexist)", pen < ts * 0.5f);
            // Idempotent: re-snapping from the snapped pose should move/turn essentially nothing.
            Vec2 delta2{ 0.0f, 0.0f }; f32 dtheta2 = 0.0f;
            dock_snap_delta(&p2, &e2, ts * 1.5f, &delta2, &dtheta2);
            std::printf("   re-snap delta = %.3f px, dtheta = %.4f rad\n", vec2_length(delta2), dtheta2);
            check("dock snap is idempotent (re-snap ~0)", vec2_length(delta2) < 1.0f && fabsf(dtheta2) < 0.01f);
        }
    }

    std::printf("\n-- dock snap: rotated approach (enemy at 135deg, player at 30deg) --\n");
    {
        // Proves the snap honors BOTH ships' full poses AND now ROTATES the mover so the doors end
        // PARALLEL & FACING. Because the snap forces anti-parallel airlock normals, the clean mate is
        // restored at ANY approach angle: we can now assert BOTH (a) matched doors exactly one tile
        // apart AND (b) sub-tile hull penetration — the no-overlap guarantee that pure translation
        // could not provide at a rotated relative angle.
        Ship p2 = player; Ship e2 = enemy;
        e2.angle  = 2.36f;
        e2.origin = Vec2{ 300.0f, -150.0f };
        p2.angle  = 0.52f; // ~30deg

        // Start dock-eligible: drop the player's right airlock ~22px off the enemy's left airlock.
        Vec2 e_door  = ship_tile_center_world(&e2, 1, 6);        // enemy airlock (world)
        Vec2 p_local = ship_tile_center_local(&p2, 11, 7);       // player airlock (local)
        p2.origin = vec2_sub(vec2_add(e_door, Vec2{ 20.0f, 10.0f }), vec2_rotate(p_local, p2.angle));

        check("rotated: pre-snap dock-eligible",
              ships_docked(&p2, &e2, ts * 1.5f, nullptr, nullptr, nullptr, nullptr));
        Vec2 delta{ 0.0f, 0.0f };
        f32  dtheta = 0.0f;
        b8 ok = dock_snap_delta(&p2, &e2, ts * 1.5f, &delta, &dtheta);
        check("rotated: dock_snap_delta TRUE", ok);
        if (ok) {
            // The corrective turn must be real here (the approach was ~108deg off parallel) and minimal.
            std::printf("   rotated: corrective dtheta = %.4f rad (%.1f deg)\n", dtheta, dtheta * 180.0f / BS_PI);
            check("rotated: corrective rotation is non-trivial", fabsf(dtheta) > 0.05f);
            check("rotated: corrective rotation is the MINIMAL turn (|dtheta| <= PI)", fabsf(dtheta) <= BS_PI + 0.001f);
            p2.angle  = p2.angle + dtheta;          // rotate first (doors parallel) ...
            p2.origin = vec2_add(p2.origin, delta); // ... then translate (one-tile flush gap)
            i32 ca, ra, cb, rb;
            b8 dk = ships_docked(&e2, &p2, ts * 1.5f, &ca, &ra, &cb, &rb);
            check("rotated: post-snap ships_docked == TRUE (pose-correct)", dk);
            if (dk) {
                f32 dd = vec2_length(vec2_sub(ship_tile_center_world(&e2, ca, ra),
                                              ship_tile_center_world(&p2, cb, rb)));
                std::printf("   post-snap matched-door distance = %.2f px (tile=%.0f)\n", dd, ts);
                check("rotated: matched doors exactly one tile apart (snap is pose-correct)",
                      fabsf(dd - ts) < 2.0f);
                // Doors PARALLEL & FACING: airlock outward normals anti-parallel (the req-1 guarantee).
                Vec2 ne, np;
                if (ship_airlock_outward_normal(&e2, ca, ra, &ne) &&
                    ship_airlock_outward_normal(&p2, cb, rb, &np)) {
                    f32 dot = vec2_dot(ne, np);
                    std::printf("   rotated: airlock-normal dot = %.3f (want -1)\n", dot);
                    check("rotated: doors parallel & facing (normals anti-parallel)", dot < -0.999f);
                }
                // NO-OVERLAP (req 2): now that doors are parallel, the hulls only touch flush.
                Vec2 mtv{ 0.0f, 0.0f };
                b8  hit = ships_collide(&p2, &e2, &mtv);
                f32 pen = hit ? vec2_length(mtv) : 0.0f;
                std::printf("   rotated: post-snap penetration = %.2f px\n", pen);
                check("rotated: NO hull overlap after parallel snap (penetration sub-tile)", pen < ts * 0.5f);
            }
        }
    }

    std::printf("\n-- dock snap: not eligible => returns FALSE (no snap) --\n");
    {
        Ship p2 = player; Ship e2 = enemy;
        p2.origin = Vec2{ 0.0f, 0.0f };   p2.angle = 0.0f;
        e2.origin = Vec2{ 520.0f, 360.0f }; e2.angle = 2.36f; // far apart (shipped default)
        Vec2 delta{ 123.0f, 456.0f };
        f32  dtheta = 7.0f;
        b8 ok = dock_snap_delta(&p2, &e2, ts * 1.5f, &delta, &dtheta);
        check("not-eligible: dock_snap_delta == FALSE", !ok);
        check("not-eligible: out_delta left untouched", delta.x == 123.0f && delta.y == 456.0f);
        check("not-eligible: out_angle left untouched", dtheta == 7.0f);
    }

    // =====================================================================================
    // Phase 3 — cross-ship navigation geometry. With the hulls docked (airlocks mated), prove the
    // crew-transfer route primitives: each airlock's interior landfall tile, the seam handoff, and
    // the two A* legs that join player deck -> seam -> enemy deck. All pure / headless (ship.cpp +
    // nav.cpp); no game_state, no window. Stage the SAME upright mated pose the dock-snap test uses.
    // =====================================================================================
    std::printf("\n-- cross-ship nav: airlock interior landfall tiles --\n");
    {
        // HULL_DOOR is solid + non-walkable, so a crew uses the walkable DECK tile just inside it.
        // Player right airlock (11,7) -> interior neighbor (10,7); enemy left airlock (1,6) -> (2,6).
        i32 pic = -1, pir = -1;
        b8 pok = ship_airlock_interior_tile(&player, 11, 7, &pic, &pir);
        check("player airlock (11,7) has an interior tile", pok);
        check("player airlock interior tile == (10,7)", pok && pic == 10 && pir == 7);
        check("player airlock interior tile is walkable", ship_tile_is_walkable(&player, pic, pir));
        check("player HULL_DOOR itself is NOT walkable", !ship_tile_is_walkable(&player, 11, 7));

        i32 eic = -1, eir = -1;
        b8 eok = ship_airlock_interior_tile(&enemy, 1, 6, &eic, &eir);
        check("enemy airlock (1,6) has an interior tile", eok);
        check("enemy airlock interior tile == (2,6)", eok && eic == 2 && eir == 6);
        check("enemy airlock interior tile is walkable", ship_tile_is_walkable(&enemy, eic, eir));

        // Non-airlock tile -> FALSE (guards misuse on a plain floor tile).
        i32 xc = 7, xr = 7;
        check("interior-tile on a non-airlock tile == FALSE",
              !ship_airlock_interior_tile(&player, 5, 5, &xc, &xr));
        check("interior-tile FALSE leaves out params untouched", xc == 7 && xr == 7);
    }

    std::printf("\n-- cross-ship nav: seam landfall + two-leg route (docked) --\n");
    {
        // Stage the verified upright mated pose: enemy anchor at {520,360} angle 0, player snapped so
        // its right airlock sits one tile off the enemy's left airlock (reusing dock_snap_delta).
        Ship p2 = player; Ship e2 = enemy;
        e2.angle  = 0.0f; e2.origin = Vec2{ 520.0f, 360.0f };
        p2.angle  = 0.0f;
        Vec2 pd  = ship_tile_center_world(&e2, 1, 6);
        Vec2 tgt = vec2_add(pd, Vec2{ -enemy.tile_size, 0.0f });
        Vec2 pl  = ship_tile_center_local(&p2, 11, 7);
        p2.origin = vec2_sub(tgt, vec2_rotate(pl, p2.angle));
        Vec2 snap{ 0.0f, 0.0f }; f32 snap_th = 0.0f;
        if (dock_snap_delta(&p2, &e2, ts * 1.5f, &snap, &snap_th)) {
            p2.angle  = p2.angle + snap_th;
            p2.origin = vec2_add(p2.origin, snap);
        }
        check("staged pose is docked", ships_docked(&p2, &e2, ts * 1.5f, nullptr, nullptr, nullptr, nullptr));

        // Seam landfall from player -> enemy: the enemy-side interior tile the crew steps onto.
        i32 lc = -1, lr = -1; Vec2 lloc{ 0.0f, 0.0f };
        b8 lok = ship_seam_landfall(&p2, &e2, ts * 1.5f, &lc, &lr, &lloc);
        check("seam landfall player->enemy succeeds when docked", lok);
        check("seam landfall == enemy interior tile (2,6)", lok && lc == 2 && lr == 6);
        // Its local center must match the tile center (the re-root point used on handoff).
        if (lok) {
            Vec2 tc = ship_tile_center_local(&e2, lc, lr);
            check("seam landfall local center == tile center",
                  fabsf(lloc.x - tc.x) < 0.01f && fabsf(lloc.y - tc.y) < 0.01f);
        }
        // Reverse direction (enemy -> player) lands on the player interior tile (10,7).
        i32 rc = -1, rr = -1;
        b8 rok = ship_seam_landfall(&e2, &p2, ts * 1.5f, &rc, &rr, nullptr);
        check("seam landfall enemy->player == player interior tile (10,7)", rok && rc == 10 && rr == 7);

        // The seam hop is a short WORLD-space jump across the mated airlocks (the two interior tiles
        // are a few tiles apart; nothing walkable bridges them, so it's a discrete handoff not an A* span).
        if (lok) {
            Vec2 player_landfall_w = ship_tile_center_world(&p2, 10, 7); // leg-A end (player side)
            Vec2 enemy_landfall_w  = ship_tile_center_world(&e2, lc, lr); // leg-B start (enemy side)
            f32  seam = vec2_length(vec2_sub(player_landfall_w, enemy_landfall_w));
            std::printf("   seam hop (player interior -> enemy interior) = %.1f px (tile=%.0f)\n", seam, ts);
            check("seam hop is short (<= 4 tiles across mated airlocks)", seam <= ts * 4.0f);
            check("seam hop spans the gap (> 1 tile; not the same deck)", seam > ts * 1.0f);
        }

        // Leg A: A* across the PLAYER deck, from a crew start near center to the player's airlock-
        // interior tile (10,7). Must succeed entirely within the player's own grid.
        Vec2 legA[NAV_MAX_PATH]; i32 lenA = 0;
        b8 aok = nav_find_path(&p2, 6, 8, 10, 7, legA, &lenA); // (6,8) interior floor -> airlock-interior
        check("leg A: A* player-center -> player airlock-interior succeeds", aok && lenA > 0);
        if (aok) {
            // Endpoints land on the requested tiles (path is start->goal, local centers).
            i32 sc, sr, gc, gr;
            ship_local_to_tile(&p2, legA[0], &sc, &sr);
            ship_local_to_tile(&p2, legA[lenA - 1], &gc, &gr);
            check("leg A: starts on (6,8)", sc == 6 && sr == 8);
            check("leg A: ends on player airlock-interior (10,7)", gc == 10 && gr == 7);
        }

        // Leg B: A* across the ENEMY deck, from the seam landfall tile (2,6) to a goal on the enemy
        // deck (e.g. the enemy helm interior). Must succeed entirely within the enemy's own grid.
        i32 ehc = -1, ehr = -1;
        b8 found_helm = ship_find_first_tile(&e2, TILE_HELM, &ehc, &ehr);
        check("enemy hull has a helm (leg-B goal)", found_helm);
        if (lok && found_helm) {
            Vec2 legB[NAV_MAX_PATH]; i32 lenB = 0;
            b8 bok = nav_find_path(&e2, lc, lr, ehc, ehr, legB, &lenB);
            check("leg B: A* enemy airlock-interior -> enemy helm succeeds", bok && lenB > 0);
            if (bok) {
                i32 sc, sr, gc, gr;
                ship_local_to_tile(&e2, legB[0], &sc, &sr);
                ship_local_to_tile(&e2, legB[lenB - 1], &gc, &gr);
                check("leg B: starts on enemy airlock-interior (2,6)", sc == lc && sr == lr);
                check("leg B: ends on enemy helm", gc == ehc && gr == ehr);
            }
        }
    }

    std::printf("\n-- cross-ship nav: seam landfall FALSE when undocked --\n");
    {
        Ship p2 = player; Ship e2 = enemy;
        p2.origin = Vec2{ 0.0f, 0.0f };     p2.angle = 0.0f;
        e2.origin = Vec2{ 520.0f, 360.0f }; e2.angle = 2.36f; // far apart (shipped default)
        i32 lc = 77, lr = 88;
        b8 lok = ship_seam_landfall(&p2, &e2, ts * 1.5f, &lc, &lr, nullptr);
        check("seam landfall == FALSE when not docked", !lok);
        check("seam landfall FALSE leaves out params untouched", lc == 77 && lr == 88);
    }

    std::printf("\n==== %s ====\n", g_fail == 0 ? "ALL DOCKING CHECKS PASSED" : "DOCKING CHECKS FAILED");
    return g_fail == 0 ? 0 : 1;
}
