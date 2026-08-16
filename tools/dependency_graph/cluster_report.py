#!/usr/bin/env python3
"""Subsystem clustering metrics for the engine and sandbox sides.

Holds the proposed subsystem maps and measures them against the real edge
structure, so every number in docs/architecture/{engine,sandbox}-subsystems.md
is reproducible rather than asserted.

Reads docs/architecture/_raw/dependency-graph.json (produced by
scan_dependencies.py) and greps the sources for symbol-level usage.

Reports, per cluster:
  * cohesion    — include edges that stay inside the cluster
  * coupling    — include edges crossing to another cluster, listed individually
  * cycles      — cluster pairs that depend on each other
  * interface   — which of the cluster's headers are included from OUTSIDE it,
                  and by how many other clusters (the real used surface)
  * boundary    — engine side: which headers the sandbox includes
                  sandbox side: which engine headers each cluster reaches for
  * used symbols (engine, --symbols) — exported symbols actually referenced
                  from outside the cluster, split by engine vs sandbox

The sandbox graph is dominated by one hub: game.h -> state/game_state.h is
included by 28 of 33 clusters. HUB_CLUSTER is excluded from the cross-dependency
listing so the residual peer structure is visible; it is still counted in the
totals.

The symbol pass is a word-boundary grep, so it counts textual references
(including comments). Anything reported as used by exactly one file is worth
eyeballing before relying on it.

Usage:
  python tools/dependency_graph/cluster_report.py [--side engine|sandbox] [--symbols]
"""

from __future__ import annotations

import json
import re
import sys
from collections import defaultdict
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GRAPH = REPO / "docs" / "architecture" / "_raw" / "dependency-graph.json"

PREFIX = {"engine": "engine/source/", "sandbox": "sandbox/source/"}
# Cluster excluded from the cross-dependency listing because it is a hub every
# other cluster depends on (see module docstring). None for the engine side.
HUB_CLUSTER = {"engine": None, "sandbox": "GameStateModel"}

E = PREFIX["engine"]

# ---------------------------------------------------------------------------
# Proposed engine clustering. Paths are relative to engine/source/.
# ---------------------------------------------------------------------------
ENGINE_CLUSTERS: dict[str, list[str]] = {
    "Foundation": ["defines.h"],
    "Containers": ["containers/array.h", "containers/string.h", "containers/vector.h"],
    "Diagnostics": ["core/logger.cpp", "core/logger.h", "core/asserts.h"],
    "Memory": [
        "core/memory/bs_memory.cpp", "core/memory/bs_memory.h",
        "core/memory/arena.cpp", "core/memory/arena.h",
    ],
    "Platform": [
        "platform/platform.h", "platform/platform_sdl3.cpp",
        "platform/platform_commons.cpp", "platform/platform_commons.h",
    ],
    "EventBus": ["core/event.cpp", "core/event.h"],
    "Input": ["core/input.cpp", "core/input.h"],
    "AppLifecycle": [
        "core/application.cpp", "core/application.h", "entry.h", "game_types.h",
    ],
    "MathCore": ["math/math_utils.cpp", "math/math_utils.h"],
    "HierCoords": ["math/bs_hierpos.cpp", "math/bs_hierpos.h"],
    # renderer_backend.cpp (the factory) sits with the BACKEND, not the frontend:
    # it is the only file naming concrete sdlgpu_* symbols, and moving it here
    # removes the frontend<->backend cycle.
    "RenderFrontend": [
        "renderer/renderer.cpp", "renderer/renderer.h", "renderer/renderer_types.h",
        "renderer/renderer_backend.h", "renderer/camera2d.cpp", "renderer/camera2d.h",
    ],
    "RenderBackend": [
        "renderer/backend/renderer_backend_sdlgpu.cpp",
        "renderer/backend/renderer_backend_sdlgpu.h",
        "renderer/backend/stb_image_impl.cpp",
        "renderer/renderer_backend.cpp",
    ],
    "UiFacade": ["renderer/bs_imgui.h", "renderer/bs_rml.h"],
    "Widgets": ["renderer/bs_ui.cpp", "renderer/bs_ui.h"],
    "DeadStarfield": [
        "renderer/starfield_gpu_resources.cpp", "renderer/starfield_gpu_resources.h",
    ],
}

# ---------------------------------------------------------------------------
# Proposed sandbox clustering. Paths are relative to sandbox/source/.
#
# Four grouping calls were settled by the author rather than inferred:
#   * ShipCombatModel merges the ship/module model with weapons and projectiles
#     (they share one object graph; the split produced a 4/1 cycle).
#   * GalaxyGeneration keeps node placement and per-system generation together
#     (splitting them produced a cycle via SSGenEnv, declared in galaxy_gen.h).
#   * LocalAgentAi stays whole and is flagged as a god object (2524 lines,
#     three cycles) rather than being speculatively split.
#   * DevPanels groups the three bs_ui panel builders despite zero shared edges.
# ---------------------------------------------------------------------------
SANDBOX_CLUSTERS: dict[str, list[str]] = {
    # tier 0 — shared primitives
    "CoordinateFrames": [
        "core/view_transform.cpp", "core/view_transform.h",
        "core/cursor_world.cpp", "core/cursor_world.h",
        "core/galaxy_coords.cpp", "core/galaxy_coords.h",
    ],
    "DeterministicRng": ["sim/galaxy_rng.h"],
    "Geometry2D": ["core/geom2d.cpp", "core/geom2d.h"],
    "RenderLayerTable": ["core/render_layers.h"],
    # Not a subsystem and deliberately has no page (see InWorldOverlays) -- but it is a real
    # tier-0 module with zero sandbox dependencies, written by ShipCombatModel and read by
    # InWorldOverlays. It gets its own bucket so those two edges show up as the T1 -> T0 and
    # T4 -> T0 they actually are; folding it into either owner would hide one of them.
    "ProjectileFxRing": ["core/projectile_fx.cpp", "core/projectile_fx.h"],
    "Profiling": ["core/profiler.cpp", "core/profiler.h"],
    "BitmapText": ["render/text.cpp", "render/text.h", "font8x8.h"],
    # tier 1 — ship and combat model
    "ShipCombatModel": [
        "sim/ship.cpp", "sim/ship.h", "sim/module.cpp", "sim/module.h",
        "sim/weapon.cpp", "sim/weapon.h", "sim/weapon_def.cpp", "sim/weapon_def.h",
        "sim/projectile.cpp", "sim/projectile.h",
        "render/ship_visual.cpp", "render/ship_visual.h",
    ],
    "FleetControl": [
        "sim/fleet.cpp", "sim/fleet.h", "sim/ship_control.cpp", "sim/ship_control.h",
        "sim/steering.cpp", "sim/steering.h",
    ],
    "CombatArena": [
        "sim/combat_arena.cpp", "sim/combat_arena.h",
        "sim/point_defense.cpp", "sim/point_defense.h",
        "sim/sensor_system.cpp", "sim/sensor_system.h",
    ],
    "LocalAgentAi": ["sim/ai_ship.cpp", "sim/ai_ship.h"],
    # tier 2 — galaxy simulation
    "GalaxyGeneration": [
        "sim/galaxy_gen.cpp", "sim/galaxy_gen.h", "sim/galaxy_params.h",
        "sim/galaxy_spatial.cpp", "sim/galaxy_spatial.h",
        "sim/ss_generation.cpp", "sim/ss_generation.h",
        "sim/system_evolution.cpp", "sim/system_evolution.h",
    ],
    "GalaxyRuntime": ["sim/galaxy_map.cpp", "sim/galaxy_map.h"],
    "GalaxyHistory": ["sim/galaxy_history.cpp", "sim/galaxy_history.h"],
    "MacroMissions": ["sim/ship_mission.cpp", "sim/ship_mission.h"],
    "Economy": ["sim/station_market.cpp", "sim/station_market.h"],
    "Territory": [
        "sim/voronoi_galaxy.cpp", "sim/voronoi_galaxy.h", "jc_voronoi.h",
        "render/voronoi_cell_hover_effect.cpp", "render/voronoi_cell_hover_effect.h",
    ],
    # tier 3 — player interaction
    "RtsControl": ["sim/rts_controls.cpp", "sim/rts_controls.h"],
    "CameraControl": ["sim/camera_controller.cpp", "sim/camera_controller.h"],
    "WorldEditor": ["sim/editor_tools.cpp", "sim/editor_tools.h"],
    "Discovery": ["sim/discovery.cpp", "sim/discovery.h"],
    "ActionLog": ["sim/action_log.cpp", "sim/action_log.h"],
    "TravelDebug": ["sim/travel.cpp", "sim/travel.h"],
    # tier 4 — rendering
    "SceneOrchestration": [
        "render/scene_renderer.cpp", "render/scene_renderer.h",
        "render/frame_lighting.cpp", "render/frame_lighting.h",
    ],
    "ShipRendering": [
        "render/ship_scene.cpp", "render/ship_scene.h",
        "render/ship_render.cpp", "render/ship_render.h",
    ],
    "Backdrop": [
        "render/global_background.cpp", "render/global_background.h",
        "render/starfield_layer.cpp", "render/starfield_layer.h",
        "render/nebula_layer.cpp", "render/nebula_layer.h",
        "render/mapped_system_layer.cpp", "render/mapped_system_layer.h",
        "render/parallax_background.cpp", "render/parallax_background.h",
    ],
    "CelestialParallax": ["sim/celestial_parallax.cpp", "sim/celestial_parallax.h"],
    "CelestialFx": ["render/star_fx.cpp", "render/star_fx.h"],
    "GalaxyMapRendering": ["render/galaxy_map_render.cpp", "render/galaxy_map_render.h"],
    "SystemContentRendering": [
        "render/system_content_render.cpp", "render/system_content_render.h",
    ],
    "InWorldOverlays": [
        "render/gameplay_overlays.cpp", "render/gameplay_overlays.h",
        "render/sensor_overlay.cpp", "render/sensor_overlay.h",
        "render/defense_laser_overlay.cpp", "render/defense_laser_overlay.h",
        "render/out_sensor_detection_fx.cpp", "render/out_sensor_detection_fx.h",
        # projectile_marker and weapon_hub were missing here, which made this report refuse to
        # run on the sandbox side long before projectile_fx existed. The subsystem page has
        # listed all three under Source paths throughout.
        "render/projectile_marker.cpp", "render/projectile_marker.h",
        "render/weapon_hub.cpp", "render/weapon_hub.h",
        "render/projectile_fx.cpp", "render/projectile_fx.h",
        "sim/heat_map.cpp", "sim/heat_map.h",
    ],
    "CoordinateDiagnostics": [
        "core/coord_diag.cpp", "core/coord_diag.h",
        "render/debug_overlay.cpp", "render/debug_overlay.h",
    ],
    # tier 5 — UI and shell (the last four are not subsystems; see the doc)
    "DevPanels": [
        "ui/editor_ui.cpp", "ui/editor_ui.h",
        "ui/new_game_setup.cpp", "ui/new_game_setup.h",
        "ui/system_inspector.cpp", "ui/system_inspector.h",
    ],
    "GameStateModel": ["state/game_state.h", "game.h"],
    "FrameOrchestrator": ["game.cpp", "game_modules.h"],
    "Bootstrap": ["entry.cpp"],
    "DeadStarfieldGen": ["render/starfield_generator.cpp", "render/starfield_generator.h"],
}

CLUSTER_MAPS = {"engine": ENGINE_CLUSTERS, "sandbox": SANDBOX_CLUSTERS}

# Exported symbols per engine cluster, for the "actually used outside" pass.
SYMBOLS: dict[str, list[str]] = {
    "Platform": [
        "platform_initialize", "platform_terminate", "platform_pump_messages",
        "platform_get_window_handle", "platform_allocate_virtual_memory_reserve",
        "platform_allocate_virtual_memory_commit", "platform_virtual_free",
        "platform_allocate", "platform_free", "platform_zero_memory",
        "platform_copy_memory", "platform_set_memory", "platform_console_write",
        "platform_console_write_error", "platform_get_absolute_time",
        "platform_sleep", "PAGE_SIZE",
    ],
    "Memory": [
        "bs_memory_initialize", "bs_memory_terminate", "bs_memory_allocator",
        "bs_memory_free", "bs_memory_zero", "bs_memory_copy", "bs_memory_set",
        "bs_memory_get_memory_usage_string",
        "bs_memory_allocator_virtual_memory_reserve",
        "bs_memory_allocator_virtual_memory_commit", "bs_memory_virtual_free",
        "arena_initialize", "arena_allocate", "arena_reset", "arena_terminate",
        "MEMORY_TAG_",
    ],
    "EventBus": [
        "event_initialize", "event_terminate", "event_register",
        "event_unregister", "event_fire", "EVENT_CODE_",
    ],
    "Input": [
        "input_initialize", "input_terminate", "input_update", "input_process_key",
        "input_process_button", "input_process_mouse_move",
        "input_process_mouse_wheel", "input_is_key_down", "input_was_key_down",
        "input_is_key_up", "input_was_key_up", "input_is_button_down",
        "input_was_button_down", "input_get_mouse_position",
        "input_get_previous_mouse_position", "input_get_mouse_wheel",
    ],
    "Diagnostics": [
        "logger_initialize", "logger_terminate", "logger_output",
        "report_assertion_failure", "BS_LOG_FATAL", "BS_LOG_ERROR", "BS_LOG_WARN",
        "BS_LOG_INFO", "BS_LOG_DEBUG", "BS_LOG_TRACE", "BS_ASSERT",
    ],
    "MathCore": [
        "vec2_add", "vec2_sub", "vec2_scale", "vec2_dot", "vec2_length",
        "vec2_normalized", "vec2_rotate", "vec3_add", "vec3_sub", "vec3_scale",
        "mat4_identity", "mat4_mul", "mat4_ortho", "mat4_translation",
        "mat4_scale", "mat4_rotation_z", "clampf", "BS_PI", "BS_DEG2RAD",
        "BS_RAD2DEG",
    ],
    "HierCoords": [
        "hierpos_from_vec2", "hierpos_to_vec2", "hierpos_to_f64",
        "hierpos_normalize", "hierpos_lerp", "hierpos_add_f64", "hierpos_add_vec2",
        "hierpos_diff", "bs_hierpos_selftest", "BS_HIERPOS_CELL_SIZE",
        "BS_HIERPOS_HALF_CELL",
    ],
    "RenderFrontend": [
        "renderer_initialize", "renderer_shutdown", "renderer_on_resize",
        "renderer_begin_frame", "renderer_end_frame", "renderer_set_clear_color",
        "renderer_load_texture", "renderer_create_texture", "renderer_update_texture",
        "renderer_destroy_texture", "renderer_set_camera", "renderer_draw_sprite",
        "renderer_set_draw_alpha", "renderer_get_draw_alpha",
        "renderer_draw_mapped_sprite", "renderer_draw_starfield",
        "renderer_draw_sunburst", "renderer_draw_starsurface",
        "renderer_draw_planetsurface", "renderer_draw_heat_map",
        "renderer_draw_nebula", "renderer_set_lights", "renderer_set_glow_params",
        "renderer_set_bloom_enabled", "renderer_set_bloom_params",
        "renderer_set_streak_enabled", "renderer_set_streak_params",
        "renderer_set_streak_intensity", "renderer_set_streak_source",
        "renderer_set_streak_flare_intensity", "renderer_set_aux_bloom_mode",
        "renderer_draw_line", "renderer_draw_quad", "renderer_draw_rect_outline",
        "renderer_draw_circle", "renderer_draw_grid", "renderer_get_frame_stats",
        "renderer_report_frame_timing", "renderer_get_frame_timing",
        "renderer_set_present_mode", "renderer_is_present_immediate",
        "renderer_get_present_breakdown", "camera2d_default", "camera2d_view_proj",
        "camera2d_screen_to_world", "camera2d_world_to_screen",
        "renderer_backend_create", "renderer_backend_destroy",
    ],
    "UiFacade": [
        "bs_imgui_initialize", "bs_imgui_shutdown", "bs_imgui_process_event",
        "bs_imgui_wants_mouse", "bs_imgui_wants_keyboard", "bs_imgui_get_hud_font",
        "bs_rml_initialize", "bs_rml_shutdown", "bs_rml_load_fonts",
        "bs_rml_load_document", "bs_rml_show", "bs_rml_unload_document",
        "bs_rml_update", "bs_rml_process_event", "bs_rml_wants_mouse",
        "bs_rml_wants_keyboard", "bs_rml_on_resize", "bs_rml_debugger_toggle",
        "bs_rml_set_sharpen", "bs_rml_hud_init", "bs_rml_hud_shutdown",
        "bs_rml_hud_update", "bs_rml_hud_poll_action",
    ],
    "Widgets": [
        "bs_ui_begin_panel", "bs_ui_end_panel", "bs_ui_begin_window",
        "bs_ui_end_window", "bs_ui_text", "bs_ui_text_colored", "bs_ui_progress",
        "bs_ui_button", "bs_ui_button_sized", "bs_ui_same_line",
        "bs_ui_set_cursor_pos_x", "bs_ui_separator", "bs_ui_checkbox",
        "bs_ui_slider_float", "bs_ui_color_edit3", "bs_ui_combo", "bs_ui_label_at",
        "bs_ui_selectable", "bs_ui_color_button", "bs_ui_is_window_hovered",
        "bs_ui_push_alpha", "bs_ui_pop_alpha", "bs_ui_begin_hud_panel",
        "bs_ui_end_hud_panel",
    ],
    "AppLifecycle": [
        "application_init", "application_run", "game_create", "ApplicationConfig",
    ],
}


def main() -> int:
    side = "engine"
    if "--side" in sys.argv:
        side = sys.argv[sys.argv.index("--side") + 1]
    if side not in CLUSTER_MAPS:
        print(f"error: --side must be engine or sandbox, got {side!r}", file=sys.stderr)
        return 1

    clusters = CLUSTER_MAPS[side]
    prefix = PREFIX[side]
    hub = HUB_CLUSTER[side]

    graph = json.loads(GRAPH.read_text(encoding="utf-8"))
    files = graph["files"]

    owner: dict[str, str] = {}
    for cluster, members in clusters.items():
        for rel in members:
            owner[prefix + rel] = cluster

    mine = [k for k, v in files.items() if v["side"] == side]
    unassigned = [k for k in mine if k not in owner]
    unknown = [k for k in owner if k not in files]
    if unassigned or unknown:
        print(f"error: unassigned={unassigned} unknown={unknown}", file=sys.stderr)
        return 1

    intra: dict[str, int] = defaultdict(int)
    cross: dict[tuple[str, str], list[tuple[str, str]]] = defaultdict(list)
    for src in mine:
        for dst in files[src]["includes"]:
            if files[dst]["side"] != side:
                continue
            a, b = owner[src], owner[dst]
            if a == b:
                intra[a] += 1
            else:
                cross[(a, b)].append((src[len(prefix):], dst[len(prefix):]))

    n_intra = sum(intra.values())
    n_cross = sum(len(v) for v in cross.values())
    print(f"side: {side}   files: {len(mine)}   clusters: {len(clusters)}")
    print(f"intra-cluster edges: {n_intra}   cross-cluster edges: {n_cross}")
    if hub:
        print(f"hub cluster excluded from the cross-dependency listing: {hub}")
    print()

    out_deg: dict[str, int] = defaultdict(int)
    in_deg: dict[str, int] = defaultdict(int)
    for (a, b), v in cross.items():
        out_deg[a] += len(v)
        in_deg[b] += len(v)

    print("=== per cluster ===")
    print(f"  {'cluster':24} {'files':>5} {'intra':>5} {'out':>4} {'in':>4}")
    for c in clusters:
        print(f"  {c:24} {len(clusters[c]):5} {intra[c]:5} {out_deg[c]:4} {in_deg[c]:4}")
    print()

    # Real interface: which of a cluster's headers are pulled in from outside it.
    used_by: dict[str, dict[str, set]] = defaultdict(lambda: defaultdict(set))
    for (a, b), v in cross.items():
        for s, t in v:
            used_by[b][t].add(a)
    print("=== interface actually used (headers included from outside the cluster) ===")
    for c in sorted(clusters, key=lambda c: -sum(len(x) for x in used_by[c].values())):
        if not used_by[c]:
            print(f"  {c}: no consumers inside this side")
            continue
        print(f"  {c}:")
        for header, consumers in sorted(used_by[c].items(), key=lambda kv: -len(kv[1])):
            print(f"      {header:44} <- {len(consumers):2} clusters: "
                  + ", ".join(sorted(consumers)))
    print()

    print("=== cross-cluster dependencies (>=2 edges) ===")
    for (a, b), v in sorted(cross.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        if hub and b == hub:
            continue
        if len(v) < 2:
            continue
        print(f"  {len(v):3}  {a} -> {b}")
        for s, t in sorted(v):
            print(f"         {s}  ->  {t}")
    print()

    print("=== cycles (mutually dependent cluster pairs) ===")
    seen: set[tuple[str, str]] = set()
    for a, b in cross:
        if (b, a) in cross and (b, a) not in seen and hub not in (a, b):
            seen.add((a, b))
            print(f"  {a} <-> {b}   ({len(cross[(a, b)])} / {len(cross[(b, a)])} edges)")
            for s, t in cross[(a, b)]:
                print(f"        fwd: {s} -> {t}")
            for s, t in cross[(b, a)]:
                print(f"        rev: {s} -> {t}")
    if not seen:
        print("  none")
    print()

    per: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))
    if side == "engine":
        print("=== boundary: engine headers the sandbox includes ===")
        for edge in graph["boundary_edges"]:
            per[owner[edge["to"]]][edge["to"][len(prefix):]] += 1
    else:
        print("=== boundary: engine headers each sandbox cluster reaches for ===")
        for edge in graph["boundary_edges"]:
            per[owner[edge["from"]]][edge["to"].replace(PREFIX["engine"], "")] += 1
    for c in sorted(per, key=lambda c: -sum(per[c].values())):
        print(f"  {sum(per[c].values()):3}  {c}")
        print("        " + ", ".join(f"{h}({n})" for h, n
                                     in sorted(per[c].items(), key=lambda kv: -kv[1])))
    print()

    if "--symbols" not in sys.argv:
        print("(re-run with --symbols for the symbol-level external-usage pass; engine only)")
        return 0
    if side != "engine":
        print("(the symbol table is engine-only)")
        return 0

    text = {}
    for path in files:
        try:
            text[path] = (REPO / path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            text[path] = ""

    print("=== symbols actually referenced OUTSIDE their cluster ===")
    for cluster, syms in SYMBOLS.items():
        inside = {prefix + rel for rel in clusters[cluster]}
        outside = [f for f in files if f not in inside]
        used, unused = [], []
        for sym in syms:
            pat = re.compile(r"" + re.escape(sym))
            eng = sum(1 for f in outside
                      if files[f]["side"] == "engine" and pat.search(text[f]))
            sbx = sum(1 for f in outside
                      if files[f]["side"] == "sandbox" and pat.search(text[f]))
            (used if eng + sbx else unused).append((sym, eng, sbx))
        print(f"  --- {cluster} ---")
        for sym, eng, sbx in sorted(used, key=lambda x: -(x[1] + x[2])):
            print(f"      {sym:44} engine:{eng:3}  sandbox:{sbx:3}")
        if unused:
            print(f"      unused outside: {', '.join(u[0] for u in unused)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
