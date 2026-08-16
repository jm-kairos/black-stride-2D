#include "ui/new_game_setup.h"
#include "game.h"                 // game_state, GalaxySetupParams, AppPhase
#include "sim/galaxy_rng.h"       // galaxy_splitmix64 (seed reroll)
#include <renderer/bs_ui.h>
#include <stdio.h>

// Discrete option value tables + their combo label strings (ImGui \0-separated, \0\0 terminated).
static const i32 SIZE_VALUES[]  = { 2000, 6000, 10000 };
static const char* SIZE_ITEMS   = "Small (2000)\0Standard (6000)\0Large (10000)\0";
static const i32 DEPTH_VALUES[] = { 100000, 500000, 1000000, 2000000, 5000000 };
static const char* DEPTH_ITEMS  = "100k years\0" "500k years\0" "1M years\0" "2M years\0" "5M years\0";
static const char* ABUND_ITEMS  = "Rare\0Sparse\0Common\0Teeming\0";
static const char* DENS_ITEMS   = "Two\0Three\0Four\0";
static const char* CONF_ITEMS   = "Peaceful\0Balanced\0Turbulent\0Cataclysmic\0";
static const char* AMBI_ITEMS   = "Insular\0Steady\0Expansive\0";
static const char* CATA_ITEMS   = "None\0Rare\0Common\0";
static const char* DETAIL_ITEMS = "Broad\0Balanced\0Deep\0";
static const char* SHAPE_ITEMS  = "Spiral\0Barred spiral\0Elliptical\0Ring\0Irregular\0Flocculent\0";

// Nearest index into `vals` for `v` (for restoring a combo's selection from the stored value).
static i32 value_to_index(const i32* vals, i32 count, i32 v) {
    i32 best = 0, bestd = 0x7fffffff;
    for (i32 i = 0; i < count; ++i) { i32 d = vals[i] > v ? vals[i] - v : v - vals[i]; if (d < bestd) { bestd = d; best = i; } }
    return best;
}

void build_new_game_setup(game_state* s) {
    GalaxySetupParams& p = s->setup;
    if (bs_ui_begin_panel("NEW GALAXY", BS_UI_ANCHOR_CENTER, 0.0f, BS_UI_TYPE_GAME)) {
        bs_ui_text_colored(0.85f, 0.80f, 0.55f, 1.0f, "NEW GALAXY");
        bs_ui_text("Choose the initial conditions of your galaxy and its history.");
        bs_ui_separator();

        // --- Seed ---
        char seedbuf[48];
        snprintf(seedbuf, sizeof(seedbuf), "Seed:  %llu", (unsigned long long)p.seed);
        bs_ui_text(seedbuf);
        if (bs_ui_button("Randomize seed", TRUE))
            p.seed = galaxy_splitmix64(p.seed + 0x9E3779B97F4A7C15ull);
        bs_ui_separator();

        // --- World ---
        i32 size_idx = value_to_index(SIZE_VALUES, (i32)(sizeof(SIZE_VALUES) / sizeof(SIZE_VALUES[0])), p.galaxy_size);
        bs_ui_combo("Galaxy size", &size_idx, SIZE_ITEMS);
        p.galaxy_size = SIZE_VALUES[size_idx];

        i32 shp = p.galaxy_shape; bs_ui_combo("Galaxy shape", &shp, SHAPE_ITEMS); p.galaxy_shape = (u8)shp;

        i32 depth_idx = value_to_index(DEPTH_VALUES, (i32)(sizeof(DEPTH_VALUES) / sizeof(DEPTH_VALUES[0])), p.history_depth_years);
        bs_ui_combo("History depth", &depth_idx, DEPTH_ITEMS);
        p.history_depth_years = DEPTH_VALUES[depth_idx];

        i32 detail = p.chronicle_detail; bs_ui_combo("Chronicle detail", &detail, DETAIL_ITEMS); p.chronicle_detail = (u8)detail;
        bs_ui_separator();

        // --- Life & civilizations ---
        i32 ab = p.abundance;   bs_ui_combo("Abundance of life",   &ab, ABUND_ITEMS); p.abundance   = (u8)ab;
        i32 cd = p.civ_density; bs_ui_combo("Dynastic Houses",   &cd, DENS_ITEMS);  p.civ_density = (u8)cd;
        i32 cf = p.conflict;    bs_ui_combo("Conflict level",      &cf, CONF_ITEMS);  p.conflict    = (u8)cf;
        i32 am = p.ambition;    bs_ui_combo("Expansion ambition",  &am, AMBI_ITEMS);  p.ambition    = (u8)am;
        i32 ca = p.cataclysm;   bs_ui_combo("Cataclysms",          &ca, CATA_ITEMS);  p.cataclysm   = (u8)ca;
        bs_ui_separator();

        // --- Rough generation-cost estimate (qualitative) ---
        f32 civ_scale = p.civ_density == 0 ? 0.5f : (p.civ_density == 1 ? 1.0f : 1.8f);
        f32 cost = ((f32)p.galaxy_size / 10000.0f) * ((f32)p.history_depth_years / 1000000.0f) * civ_scale
                 * (1.0f + 0.5f * (f32)p.chronicle_detail);
        const char* speed = cost < 0.7f ? "Fast" : (cost < 2.0f ? "Moderate" : (cost < 5.0f ? "Slow" : "Very slow"));
        char est[64]; snprintf(est, sizeof(est), "Estimated generation: %s", speed);
        bs_ui_text_colored(0.70f, 0.75f, 0.82f, 1.0f, est);
        bs_ui_separator();

        if (bs_ui_button("Generate Galaxy", TRUE)) {
            s->app_phase    = APP_GENERATING;
            s->gen_stage    = 0;
            s->gen_progress = 0.0f;
            s->gen_label    = "Preparing";
        }
    }
    bs_ui_end_panel();
}

void build_generation_progress(game_state* s) {
    if (bs_ui_begin_panel("GENERATING", BS_UI_ANCHOR_CENTER, 0.0f, BS_UI_TYPE_GAME)) {
        bs_ui_text_colored(0.85f, 0.80f, 0.55f, 1.0f, "GENERATING GALAXY");
        char lbl[96];
        snprintf(lbl, sizeof(lbl), "%s ...", s->gen_label ? s->gen_label : "");
        bs_ui_progress(s->gen_progress, lbl);
    }
    bs_ui_end_panel();
}
