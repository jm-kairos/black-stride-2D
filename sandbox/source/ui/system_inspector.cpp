#include "ui/system_inspector.h"
#include "game.h"
#include "sim/ss_generation.h"          // spectral_class_name / planet_type_name / subtype names
#include "sim/system_evolution.h"   // evo_event_name
#include <renderer/bs_ui.h>         // bs_ui_* panel/widget API
#include <stdio.h>                  // snprintf

// =====================================================================================
// System Inspector — browse the evolved-body model of the current star system.
// =====================================================================================

static const f32 HDR[4] = { 0.55f, 0.85f, 0.95f, 1.0f };   // section header cyan
static const f32 DIM[4] = { 0.62f, 0.66f, 0.72f, 1.0f };   // secondary grey

// One labelled 0..1 gauge line.
static void gauge(const char* label, f32 v, const char* overlay_fmt = "%.0f%%") {
    char buf[48];
    snprintf(buf, sizeof(buf), overlay_fmt, v * 100.0f);
    bs_ui_text_colored(DIM[0], DIM[1], DIM[2], DIM[3], label);
    bs_ui_same_line();
    bs_ui_progress(v, buf);
}

// Composition summary line, e.g. "metal 22%  rock 61%  ice 17%".
static void comp_line(const BodyComposition& c) {
    char buf[128];
    i32 off = 0;
    if (c.metal    > 0.01f) off += snprintf(buf + off, sizeof(buf) - (size_t)off, "metal %.0f%%  ", c.metal * 100.0f);
    if (c.silicate > 0.01f) off += snprintf(buf + off, sizeof(buf) - (size_t)off, "rock %.0f%%  ",  c.silicate * 100.0f);
    if (c.ice      > 0.01f) off += snprintf(buf + off, sizeof(buf) - (size_t)off, "ice %.0f%%  ",   c.ice * 100.0f);
    if (c.gas      > 0.01f) off += snprintf(buf + off, sizeof(buf) - (size_t)off, "gas %.0f%%",     c.gas * 100.0f);
    if (off == 0) snprintf(buf, sizeof(buf), "(no composition data)");
    bs_ui_text(buf);
}

// Hazard badge: green/amber/red by severity.
static void hazard_badge(f32 hz) {
    char buf[48];
    const char* word = hz < 0.25f ? "BENIGN" : hz < 0.55f ? "HARSH" : "EXTREME";
    snprintf(buf, sizeof(buf), "Hazard: %s (%.0f%%)", word, hz * 100.0f);
    if (hz < 0.25f)      bs_ui_text_colored(0.45f, 0.90f, 0.50f, 1.0f, buf);
    else if (hz < 0.55f) bs_ui_text_colored(0.95f, 0.75f, 0.30f, 1.0f, buf);
    else                 bs_ui_text_colored(0.95f, 0.35f, 0.30f, 1.0f, buf);
}

// Full detail block for one evolved body (planet or moon).
static void body_detail(const EvolvedBody& b) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%.2f AU   %.2f Me   %.2f Re   %.0f K",
             b.orbit_au, b.mass_earth, b.radius_earth, b.temperature_k);
    bs_ui_text(buf);
    comp_line(b.comp);
    if (b.comp.gas <= 0.35f) {
        gauge("Water    ", b.water_frac);
        snprintf(buf, sizeof(buf), "%.2f atm", b.atmo_pressure);
        bs_ui_text_colored(DIM[0], DIM[1], DIM[2], DIM[3], "Atmosph. ");
        bs_ui_same_line();
        bs_ui_progress(b.atmo_pressure > 3.0f ? 1.0f : b.atmo_pressure / 3.0f, buf);
        gauge("Magnetic ", b.magnetic_field);
        gauge("Tectonics", b.tectonics);
        gauge("Volcanism", b.volcanism);
        if (b.life > 0.01f) gauge("Biosphere", b.life);
    }
    if (b.habitability > 0.01f) gauge("Habitable", b.habitability);
    hazard_badge(b.env_hazard);
    snprintf(buf, sizeof(buf), "Resources: ore %.0f%%   volatiles %.0f%%",
             b.res_metal * 100.0f, b.res_volatiles * 100.0f);
    bs_ui_text_colored(DIM[0], DIM[1], DIM[2], DIM[3], buf);
}

void build_system_inspector(game_state* s) {
    b8 open = s->galaxy.show_system_inspector ? TRUE : FALSE;
    if (bs_ui_begin_window("SYSTEM INSPECTOR", &open)) {
        i32 slot = s->galaxy.current_system;
        if (slot < 0 || slot >= s->galaxy.system_count) {
            bs_ui_text("No current system (deep space).");
        } else {
            const StarSystem& ss = s->galaxy.systems[slot];
            const EvolvedSystem& evo = ss.evo;
            i32 node = s->galaxy.cache_node[slot];
            const char* name = (node >= 0 && s->galaxy.nodes) ? s->galaxy.nodes[node].name : "?";
            char buf[192];

            // ---- Star card ----
            snprintf(buf, sizeof(buf), "%s  --  %s-class star", name,
                     spectral_class_name(ss.star_props.spectral_class));
            bs_ui_text_colored(HDR[0], HDR[1], HDR[2], HDR[3], buf);
            snprintf(buf, sizeof(buf), "%.2f Msun   %.2f Lsun   %.0f K   %.1f Gyr   Z %.2f",
                     ss.star_props.mass_solar, ss.star_props.luminosity_solar,
                     ss.star_props.temperature_k, ss.star_props.age_gyr, ss.star_props.metallicity);
            bs_ui_text(buf);
            snprintf(buf, sizeof(buf), "HZ %.2f-%.2f AU   frost %.2f AU",
                     ss.star_props.hz_inner_au, ss.star_props.hz_outer_au,
                     ss.star_props.frost_line_au);
            bs_ui_text_colored(DIM[0], DIM[1], DIM[2], DIM[3], buf);
            bs_ui_separator();

            // ---- Body tree: planets with nested moons ----
            static i32 sel_body = -1; // bodies[] index of the expanded body
            i32 moon_base = 1 + evo.planet_count;
            for (i32 p = 0; p < evo.planet_count; ++p) {
                const EvolvedBody& b = evo.bodies[1 + p];
                const PlanetGenome& g = ss.planet_props[p].genome;
                snprintf(buf, sizeof(buf), "%s %d  --  %s %s##body%d", name, p + 1,
                         planet_subtype_name(b.type, g.subtype), planet_type_name(b.type), 1 + p);
                if (bs_ui_selectable(buf, sel_body == 1 + p))
                    sel_body = (sel_body == 1 + p) ? -1 : 1 + p;
                if (sel_body == 1 + p) body_detail(b);
                // Nested moons of this planet.
                for (i32 m = 0; m < evo.moon_count; ++m) {
                    const EvolvedBody& mb = evo.bodies[moon_base + m];
                    if ((i32)mb.parent != 1 + p) continue;
                    snprintf(buf, sizeof(buf), "   moon  --  %s##body%d",
                             planet_type_name(mb.type == PLANET_FROZEN ? PLANET_FROZEN : PLANET_ROCKY),
                             moon_base + m);
                    if (bs_ui_selectable(buf, sel_body == moon_base + m))
                        sel_body = (sel_body == moon_base + m) ? -1 : moon_base + m;
                    if (sel_body == moon_base + m) body_detail(mb);
                }
            }
            // Belts.
            i32 belt_base = moon_base + evo.moon_count;
            for (i32 bl = 0; bl < evo.belt_count; ++bl) {
                const EvolvedBody& b = evo.bodies[belt_base + bl];
                snprintf(buf, sizeof(buf), "Asteroid belt  --  %.2f AU (+/- %.2f)##body%d",
                         b.orbit_au, b.width_au, belt_base + bl);
                if (bs_ui_selectable(buf, sel_body == belt_base + bl))
                    sel_body = (sel_body == belt_base + bl) ? -1 : belt_base + bl;
                if (sel_body == belt_base + bl) {
                    snprintf(buf, sizeof(buf), "%.2f Me remnant mass", b.mass_earth);
                    bs_ui_text(buf);
                    comp_line(b.comp);
                    hazard_badge(b.env_hazard);
                    snprintf(buf, sizeof(buf), "Resources: ore %.0f%%   volatiles %.0f%%",
                             b.res_metal * 100.0f, b.res_volatiles * 100.0f);
                    bs_ui_text_colored(DIM[0], DIM[1], DIM[2], DIM[3], buf);
                }
            }
            bs_ui_separator();

            // ---- Chronicle: the system's evolution event log, epoch by epoch ----
            bs_ui_text_colored(HDR[0], HDR[1], HDR[2], HDR[3], "SYSTEM CHRONICLE");
            if (evo.event_count == 0)
                bs_ui_text_colored(DIM[0], DIM[1], DIM[2], DIM[3], "A quiet formation: no notable events.");
            for (i32 e = 0; e < evo.event_count; ++e) {
                const EvolutionEvent& ev = evo.events[e];
                if (ev.body >= 1 && ev.body <= evo.planet_count)
                    snprintf(buf, sizeof(buf), "epoch %2d   %s (%s %d)",
                             ev.epoch, evo_event_name(ev.kind), name, ev.body);
                else
                    snprintf(buf, sizeof(buf), "epoch %2d   %s",
                             ev.epoch, evo_event_name(ev.kind));
                bs_ui_text(buf);
            }
        }
    }
    bs_ui_end_window();
    s->galaxy.show_system_inspector = open ? true : false;
}
