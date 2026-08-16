#define _CRT_SECURE_NO_WARNINGS
#include "sim/galaxy_history.h"
#include "game.h"                 // full game_state (GalaxyState, GalaxyNode, Civilization)
#include "sim/galaxy_rng.h"       // galaxy_rng_seed / next / f32
#include <core/logger.h>          // BS_LOG_INFO
#include <core/memory/bs_memory.h>// territory arrays (bs_memory_allocator / free)
#include <renderer/bs_ui.h>       // Legends browser window (bs_ui)
#include "sim/galaxy_map.h"        // galaxy_nearest_node (Phase C2 inspector)
#include "sim/ai_ship.h"           // ShipArchetype (P3 inspector population breakdown)
#include "sim/action_log.h"        // action_log_push (government interaction windows)
#include <stdio.h>                // snprintf
using namespace bs_math;

using GalaxyState = game_state::GalaxyState;

// ---- Tunables (Phase 1; the New-Game setup screen will parameterise these in Phase A) -------
static const i32 CIV_MEAN_LIFETIME = 250000;    // ~mean civilization lifespan (years) before collapse
static const i32 CIV_NOTABLE       = 4;         // systems held before a civ earns a chronicle entry
static const i32 CIV_GOLDEN        = 20;        // systems held to trigger a golden-age entry
static const i32 CIV_FRAGMENT_MIN  = 10;        // systems a realm must hold for a collapse to splinter it
static const f32 CIV_DEF_BONUS     = 1.3f;      // home-defender advantage when resisting a border assault
static const i32 ACTIVE_WAR_MAX    = 512;       // concurrently tracked wars (alive civs -> pairs << this)
static const f32 WAR_ABSORB_FRAC   = 0.6f;      // share of a conquered realm the victor annexes (rest devastated)
static const i32 ALLY_SCORE        = 50;        // relation score at/above which two powers become allied
static const i32 ALLY_HYST         = 30;        // an alli+ance lapses only once relations fall below this
static const i32 RIVAL_SCORE       = -30;       // relation score at/below which two powers are bitter rivals
static const f32 COALITION_JOIN    = 0.6f;      // chance a defender's ally is dragged into the war
static const i32 LIVE_STEP_YEARS        = 1;     // fine cadence for the living present
static const i32 LIVE_MAX_YEARS_PER_TICK = 8;    // clamp the per-frame advance (defensive vs hitches)

// ---- New Game setup -> generation parameter mappings ------------------------------------------
static i32 setup_house_count(const GalaxySetupParams& p) { return p.civ_density == 0 ? 2 : (p.civ_density == 1 ? 3 : 4); }
static u8  setup_hab_min   (const GalaxySetupParams& p) { const u8  v[4] = {180,128,95,65};       return v[p.abundance < 4 ? p.abundance : 1]; }
static f32 setup_collapse_k(const GalaxySetupParams& p) { const f32 v[4] = {0.5f,1.0f,1.4f,1.8f};  return v[p.conflict < 4 ? p.conflict : 1]; }
static i32 setup_cap_base  (const GalaxySetupParams& p) { return p.ambition == 0 ? 4 : (p.ambition == 1 ? 6 : 10); }
static f32 setup_cap_scale (const GalaxySetupParams& p) { return p.ambition == 0 ? 34.0f : (p.ambition == 1 ? 58.0f : 90.0f); }
static f32 setup_growth    (const GalaxySetupParams& p) { return p.ambition == 0 ? 0.035f : (p.ambition == 1 ? 0.065f : 0.11f); }
static i32 setup_cataclysms(const GalaxySetupParams& p) { return p.cataclysm == 0 ? 0 : (p.cataclysm == 1 ? 3 : 8); }
static f32 setup_war_prob     (const GalaxySetupParams& p) { const f32 v[4] = {0.0f,0.06f,0.14f,0.25f};  return v[p.conflict < 4 ? p.conflict : 1]; }
static f32 setup_fragment_prob(const GalaxySetupParams& p) { const f32 v[4] = {0.20f,0.35f,0.45f,0.55f};  return v[p.conflict < 4 ? p.conflict : 1]; }
static f32 setup_peace_base   (const GalaxySetupParams& p) { const f32 v[4] = {0.10f,0.06f,0.04f,0.02f};  return v[p.conflict < 4 ? p.conflict : 1]; }
static f32 setup_alliance_base(const GalaxySetupParams& p) { const f32 v[4] = {0.06f,0.04f,0.02f,0.01f};  return v[p.conflict < 4 ? p.conflict : 1]; }

// Cultural compatibility -> relation drift per border contact (clamped ~[-4,+4]).
static i32 ethos_affinity(u8 ea, u8 eb) {
    i32 s = 0;
    if (ea == eb) s += 2;
    if (ea == ETHOS_HARMONIOUS || eb == ETHOS_HARMONIOUS) s += 2;
    if (ea == ETHOS_MERCANTILE || eb == ETHOS_MERCANTILE) s += 1;
    if (ea == ETHOS_XENOPHOBE  || eb == ETHOS_XENOPHOBE)  s -= 3;
    if (ea == ETHOS_MILITANT   && eb == ETHOS_MILITANT)   s -= 2;
    if (s >  4) s =  4;
    if (s < -4) s = -4;
    return s;
}

// Per-attacker war eagerness from culture (government + ethos); multiplies the base assault chance.
static f32 civ_aggression(u8 government, u8 ethos) {
    f32 a = 1.0f;
    switch (ethos) {
        case ETHOS_MILITANT:   a *= 1.6f; break;
        case ETHOS_XENOPHOBE:  a *= 1.4f; break;
        case ETHOS_SCIENTIFIC: a *= 0.9f; break;
        case ETHOS_SPIRITUAL:  a *= 0.8f; break;
        case ETHOS_MERCANTILE: a *= 0.6f; break;
        case ETHOS_HARMONIOUS: a *= 0.4f; break;
        default: break;
    }
    switch (government) {
        case GOV_ABSOLUTE_MONARCHY:       a *= 1.4f; break;   // expansionist crowns
        case GOV_ECCLESIARCHY:            a *= 1.2f; break;   // crusading zeal
        case GOV_REPRESENTATIVE_REPUBLIC: a *= 0.8f; break;   // war is politically costly
        case GOV_MINARCHIST_COMPACT:      a *= 0.6f; break;   // no central war-making apparatus
        default: break;
    }
    return a;
}

// Distinct banner colours cycled across civilizations (also the territory tint in later phases).
static const bs_color CIV_PALETTE[] = {
    { 0.90f, 0.25f, 0.25f, 1.0f }, { 0.25f, 0.55f, 0.95f, 1.0f }, { 0.30f, 0.80f, 0.40f, 1.0f },
    { 0.95f, 0.75f, 0.20f, 1.0f }, { 0.70f, 0.40f, 0.90f, 1.0f }, { 0.95f, 0.55f, 0.20f, 1.0f },
    { 0.30f, 0.85f, 0.85f, 1.0f }, { 0.95f, 0.45f, 0.70f, 1.0f }, { 0.60f, 0.80f, 0.25f, 1.0f },
    { 0.55f, 0.60f, 0.95f, 1.0f }, { 0.85f, 0.85f, 0.55f, 1.0f }, { 0.40f, 0.90f, 0.65f, 1.0f },
    { 0.90f, 0.60f, 0.55f, 1.0f }, { 0.65f, 0.55f, 0.85f, 1.0f }, { 0.80f, 0.35f, 0.55f, 1.0f },
    { 0.50f, 0.75f, 0.90f, 1.0f },
};
static const i32 CIV_PALETTE_N = (i32)(sizeof(CIV_PALETTE) / sizeof(CIV_PALETTE[0]));

// House name stem: "<Root><suffix>", e.g. "Velom". One stem is minted per House (lineage) and shared
// by every polity descended from it, so successor kingdoms read as one family.
static void civ_make_stem(GalaxyRng* r, char* out, i32 n) {
    static const char* ROOT[] = {
        "Aur","Vel","Kor","Zan","Thal","Nyx","Oss","Bael","Cryn","Dro","Eld","Fen","Gul","Hesh",
        "Ith","Jor","Kel","Lum","Mor","Vor","Xen","Yls","Zeph","Quor","Sil","Tyr","Ura","Wren"
    };
    static const char* SUF[]  = { "an","ar","ok","ix","us","ra","eth","om","ul","yr","ax","en","or","is","ai","un" };
    const char* a = ROOT[galaxy_rng_next(r) % (sizeof(ROOT) / sizeof(ROOT[0]))];
    const char* b = SUF [galaxy_rng_next(r) % (sizeof(SUF)  / sizeof(SUF[0]))];
    snprintf(out, (size_t)n, "%s%s", a, b);
}

// Compose a polity name from a House stem + an optional sequence number. The first polity in a
// lineage gets the plain stem (e.g. "Velom"); later ones are "Velom 2", "Velom 3", etc. This keeps
// successor polities clearly grouped by lineage without any cosmetic government-style suffix words.
static void civ_compose_name(const char* stem, i32 ordinal, char* out, i32 n) {
    if (ordinal <= 1)
        snprintf(out, (size_t)n, "%s", stem);
    else
        snprintf(out, (size_t)n, "%s %d", stem, ordinal);
}

// Shade a House's base hue into a distinct per-polity variant, so an entire lineage reads as a
// colour FAMILY on the territory overlay (bright core kingdoms, dimmer offshoots) rather than as
// unrelated colours.
static bs_color house_shade(bs_color base, GalaxyRng* r) {
    f32 f = 0.72f + 0.5f * galaxy_rng_f32(r);   // lightness/saturation multiplier ~[0.72, 1.22]
    bs_color c;
    c.r = base.r * f; c.g = base.g * f; c.b = base.b * f; c.a = 1.0f;
    if (c.r > 1.0f) c.r = 1.0f;
    if (c.g > 1.0f) c.g = 1.0f;
    if (c.b > 1.0f) c.b = 1.0f;
    return c;
}

// ---- Phase B/C simulation working-set --------------------------------------------------------
// One object holding the entire simulation state. During generation it is stepped coarsely; it now
// stays RESIDENT after generation (freed only on regenerate / process exit) so the living present
// (Phase C1) can keep stepping it at the player's pace. Kept as a single object so it can be
// serialized in one block for save/load (Phase C3).
static const i32 REL_BYTES = (GALAXY_CIV_MAX * GALAXY_CIV_MAX + 7) / 8;

// Active wars (currently fighting pairs); the `warred` bitset means "at war NOW" (set on start,
// cleared on peace), so the same pair can fight again in a later era.
struct ActiveWar { i16 a, b; i32 start_year; };

struct GalaxyHistorySim {
    GalaxyRng rng;
    i32  start_year, present_year, step_years, cur_year;
    i32  target;
    f32  growth;
    f32  cataclysm_prob;
    u8   detail;                 // chronicle_detail captured for the importance gate
    f32  peace_base;             // per-step base chance an ongoing war tires into peace
    f32  alliance_base;          // per-qualifying-contact chance a warm pair formalises an alliance

    i32* claimed;        i32 claimed_count;     // frontier: nodes ever brought into ownership
    i32* habitable;      i32 habitable_count;   // candidate cradle / birth worlds
    i32* bfs;                                   // BFS queue scratch (flood-fill fragmentation)
    i32* cap;                                   // per-civ mature capacity (registry-indexed)
    u8*  notable;                               // per-civ: emitted its founding entry yet
    u8*  golden;                                // per-civ: emitted a golden-age entry yet
    u8*  contacted;                             // pairwise bitset: borders that have met
    u8*  warred;                                // pairwise bitset: pairs at war NOW
    u8*  allied;                                // pairwise bitset: formally allied pairs
    i8*  relation;                              // pairwise relation score [-100,+100]
    ActiveWar active[ACTIVE_WAR_MAX];  i32 active_count;

    i32  war_count, peace_count, alliance_count, conquest_count, fragment_count;

    // Phase C1 living clock
    i32  gen_step_years;     // the generation step size (reference cadence for per-year rate scaling)
    i32  live_base_year;     // present_year captured when live play begins (shared-calendar epoch)
    b8   live_mode;          // TRUE once the living present is running (routes events to the feed)
    b8   garrison_seeded;    // Step B: TRUE once node_garrison[] has been initialised from ownership
};
static GalaxyHistorySim g_sim{};

// Transitional aliases: the simulation code reads/writes these names; the STATE lives in g_sim so it
// survives the generation->play boundary (and can be serialized in one block for save/load).
static i32*&      s_claimed         = g_sim.claimed;
static i32&       s_claimed_count   = g_sim.claimed_count;
static i32*&      s_habitable       = g_sim.habitable;
static i32&       s_habitable_count = g_sim.habitable_count;
static i32*&      s_cap             = g_sim.cap;
static u8*&       s_notable         = g_sim.notable;
static u8*&       s_golden          = g_sim.golden;
static GalaxyRng& s_rng             = g_sim.rng;
static i32&       s_start_year      = g_sim.start_year;
static i32&       s_present_year    = g_sim.present_year;
static i32&       s_step_years      = g_sim.step_years;
static i32&       s_cur_year        = g_sim.cur_year;
static i32&       s_target          = g_sim.target;
static f32&       s_growth          = g_sim.growth;
static f32&       s_cataclysm_prob  = g_sim.cataclysm_prob;
static u8&        s_detail          = g_sim.detail;
static u8*&       s_contacted       = g_sim.contacted;
static u8*&       s_warred          = g_sim.warred;
static i32&       s_war_count       = g_sim.war_count;
static i32&       s_conquest_count  = g_sim.conquest_count;
static i32&       s_fragment_count  = g_sim.fragment_count;
static i32&       s_peace_count     = g_sim.peace_count;
static f32&       s_peace_base      = g_sim.peace_base;
static i32*&      s_bfs             = g_sim.bfs;
static i8*&       s_relation        = g_sim.relation;
static u8*&       s_allied          = g_sim.allied;
static f32&       s_alliance_base   = g_sim.alliance_base;
static i32&       s_alliance_count  = g_sim.alliance_count;
static ActiveWar (&s_active)[ACTIVE_WAR_MAX] = g_sim.active;
static i32&       s_active_count    = g_sim.active_count;

// Canonical (a<=b) pair -> bit in the relationship bitsets.
static inline i32  rel_bit  (i32 a, i32 b) { if (a > b) { i32 t = a; a = b; b = t; } return a * GALAXY_CIV_MAX + b; }
static inline b8   rel_test (const u8* set, i32 a, i32 b) { i32 x = rel_bit(a, b); return (set[x >> 3] & (1u << (x & 7))) != 0; }
static inline void rel_set  (u8* set, i32 a, i32 b) { i32 x = rel_bit(a, b); set[x >> 3] |= (u8)(1u << (x & 7)); }
static inline void rel_clear(u8* set, i32 a, i32 b) { i32 x = rel_bit(a, b); set[x >> 3] &= (u8)~(1u << (x & 7)); }

// Signed relation score per pair (diplomacy); indexed by the same canonical linear pair id.
static inline i32  rel_score    (i32 a, i32 b) { return (i32)s_relation[rel_bit(a, b)]; }
static inline void rel_score_add(i32 a, i32 b, i32 d) {
    i32 x = rel_bit(a, b); i32 v = (i32)s_relation[x] + d;
    if (v >  100) v =  100;
    if (v < -100) v = -100;
    s_relation[x] = (i8)v;
}

// Found a new civilization on an unclaimed or derelict (fallen-owned) habitable world. Returns the
// new registry index, or -1 if the registry is full / no site is found.
// --- Dynastic Houses helpers ------------------------------------------------------------------
// How many polities (living or fallen) have ever belonged to a lineage; used for succession ordinals.
static i32 house_polity_count(const GalaxyState& g, i16 culture) {
    i32 n = 0;
    for (i32 c = 0; c < g.civ_count; ++c) if (g.civs[c].culture_id == culture) ++n;
    return n;
}

// Map a culture id (a House's root civ index) to its houses[] slot, or -1 if not a registered House.
static i32 house_slot_of(const GalaxyState& g, i16 culture) {
    for (i32 h = 0; h < g.house_count; ++h) if (g.houses[h].root_civ == culture) return h;
    return -1;
}

// Pick a habitable world not currently held by a living civ (a free cradle). -1 if none found.
static i32 pick_free_cradle(GalaxyState& g) {
    for (i32 attempt = 0; attempt < 24; ++attempt) {
        i32 idx = (i32)(galaxy_rng_f32(&s_rng) * (f32)s_habitable_count);
        if (idx < 0 || idx >= s_habitable_count) continue;
        i32 n = s_habitable[idx];
        i16 ow = g.node_owner[n];
        if (ow >= 0 && ow < (i16)g.civ_count && g.civs[ow].status == 0) continue;  // held by a living civ
        return n;
    }
    return -1;
}

// Create a single-world polity at `node` with an explicit cultural identity and government. culture < 0
// => the polity founds its own lineage (culturing itself). Shared by root founding + succession.
static i32 spawn_polity(GalaxyState& g, const GalaxySetupParams& sp, i32 node, i32 year,
                        i16 culture, u8 ethos, bs_color hue, const char* stem, i16 parent, u8 government) {
    if (g.civ_count >= GALAXY_CIV_MAX) return -1;
    i32 c = g.civ_count++;
    Civilization& civ = g.civs[c];
    civ = Civilization{};
    civ.origin_node     = node;
    civ.founding_year   = year;
    civ.government      = government;
    civ.ethos           = ethos;
    civ.color           = house_shade(hue, &s_rng);
    civ.parent_civ      = parent;
    civ.culture_id      = (culture < 0) ? (i16)c : culture;   // a root cultures itself
    civ.power           = 1.0f;
    civ.territory_count = 1;
    civ.peak_territory  = 1;
    civ_compose_name(stem, house_polity_count(g, civ.culture_id), civ.name, (i32)sizeof(civ.name));
    f32 amb = galaxy_rng_f32(&s_rng);
    s_cap[c] = setup_cap_base(sp) + (i32)(amb * amb * setup_cap_scale(sp));
    if (s_cap[c] < 2) s_cap[c] = 2;
    s_notable[c] = 0;
    s_golden[c]  = 0;
    i16 prev = g.node_owner[node];
    g.node_owner[node] = (i16)c;
    g.node_colonized_year[node] = year;
    if (prev < 0 && s_claimed_count < g.node_count) s_claimed[s_claimed_count++] = node;
    return c;
}

// Register a new House (cultural lineage) rooted on civ `root_civ` (mints its banner colour). Returns
// the house slot, or -1 if storage is exhausted. `stem` is copied as the lineage's shared name root.
// A House is government-agnostic: every founding polity starts one, whatever its government.
static i32 house_register(GalaxyState& g, i32 root_civ, const char* stem, bs_color hue) {
    if (!g.houses || g.house_count >= g.house_capacity) return -1;
    GalaxyHouse& house = g.houses[g.house_count];
    snprintf(house.stem, sizeof(house.stem), "%s", stem);
    house.hue      = hue;
    house.root_civ = (i16)root_civ;
    return g.house_count++;
}

// Found an independent root polity with a freshly minted identity and the given government. Every root
// founds a new House (cultural lineage) regardless of government, so all polities belong to a House.
static i32 sim_spawn_root(GalaxyState& g, const GalaxySetupParams& sp, i32 year, u8 government) {
    if (s_habitable_count <= 0) return -1;
    i32 node = pick_free_cradle(g);
    if (node < 0) return -1;
    bs_color hue = CIV_PALETTE[g.civ_count % CIV_PALETTE_N];
    char stem[12]; civ_make_stem(&s_rng, stem, (i32)sizeof(stem));
    u8 ethos = (u8)(galaxy_rng_next(&s_rng) % ETHOS_COUNT);
    i32 c = spawn_polity(g, sp, node, year, -1 /*founds its own lineage*/, ethos, hue, stem, -1, government);
    if (c < 0) return -1;
    house_register(g, c, stem, hue);   // every root founds a House
    return c;
}

// Found a new root House with a random government (used to reseed a lineage after extinction).
static i32 sim_spawn_house(GalaxyState& g, const GalaxySetupParams& sp, i32 year) {
    return sim_spawn_root(g, sp, year, (u8)(galaxy_rng_next(&s_rng) % GOV_COUNT));
}

// A new polity rises on a fresh cradle within an existing House, continuing that lineage: it inherits
// the House's stem, colour family and culture id, and rolls its own government (any type is valid).
static i32 sim_spawn_successor(GalaxyState& g, const GalaxySetupParams& sp, i32 house_root, i32 year) {
    i32 hs = house_slot_of(g, (i16)house_root);
    if (hs < 0 || s_habitable_count <= 0) return -1;
    i32 node = pick_free_cradle(g);
    if (node < 0) return -1;
    u8 government = (u8)(galaxy_rng_next(&s_rng) % GOV_COUNT);
    u8 ethos = g.civs[g.houses[hs].root_civ].ethos;
    if (galaxy_rng_f32(&s_rng) < 0.25f) ethos = (u8)(galaxy_rng_next(&s_rng) % ETHOS_COUNT); // schism
    return spawn_polity(g, sp, node, year, (i16)house_root, ethos, g.houses[hs].hue,
                        g.houses[hs].stem, (i16)g.houses[hs].root_civ, government);
}

// A House is "living" if any of its polities still stand. Count Houses with at least one living kingdom.
static i32 count_living_houses(const GalaxyState& g) {
    i32 n = 0;
    for (i32 h = 0; h < g.house_count; ++h) {
        i16 culture = g.houses[h].root_civ;
        for (i32 c = 0; c < g.civ_count; ++c)
            if (g.civs[c].status == 0 && g.civs[c].culture_id == culture) { ++n; break; }
    }
    return n;
}

// Pick the root (culture id) of a random House that still has a living kingdom, or -1 if none remain.
static i32 pick_living_house_root(GalaxyState& g) {
    i16 cand[GALAXY_CIV_MAX]; i32 nc = 0;
    for (i32 h = 0; h < g.house_count; ++h) {
        i16 culture = g.houses[h].root_civ;
        for (i32 c = 0; c < g.civ_count; ++c)
            if (g.civs[c].status == 0 && g.civs[c].culture_id == culture) { cand[nc++] = culture; break; }
    }
    if (nc == 0) return -1;
    return cand[galaxy_rng_next(&s_rng) % nc];
}

void galaxy_history_sim_begin(game_state* s) {
    galaxy_history_sim_free(s);   // release any prior resident sim (safe no-op on first run)
    GalaxyState& g = s->galaxy;
    const GalaxySetupParams& sp = s->setup;
    g.civ_count = 0;
    g.house_count = 0;
    g.event_count = 0;
    g.show_gov_window = false; g.gov_window_type = GOV_WIN_NONE; g.gov_window_civ = -1;
    if (g.node_owner)          bs_memory_free(g.node_owner, sizeof(i16) * g.node_count, MEMORY_TAG_GAME);
    if (g.node_owner_gen)      bs_memory_free(g.node_owner_gen, sizeof(i16) * g.node_count, MEMORY_TAG_GAME);
    if (g.node_colonized_year) bs_memory_free(g.node_colonized_year, sizeof(i32) * g.node_count, MEMORY_TAG_GAME);
    if (g.node_garrison)       bs_memory_free(g.node_garrison, sizeof(f32) * g.node_count, MEMORY_TAG_GAME);
    if (g.missions)            bs_memory_free(g.missions, sizeof(ShipMission) * g.mission_capacity, MEMORY_TAG_GAME);
    if (g.houses)              bs_memory_free(g.houses, sizeof(GalaxyHouse) * g.house_capacity, MEMORY_TAG_GAME);
    g.node_owner = nullptr; g.node_owner_gen = nullptr; g.node_colonized_year = nullptr; g.node_garrison = nullptr;
    g.missions = nullptr; g.mission_count = 0; g.mission_capacity = 0;
    g.houses = nullptr; g.house_capacity = 0;
    s_claimed = nullptr; s_claimed_count = 0; s_habitable = nullptr; s_habitable_count = 0;
    s_cap = nullptr; s_notable = nullptr; s_golden = nullptr;
    s_contacted = nullptr; s_warred = nullptr; s_bfs = nullptr;
    s_relation = nullptr; s_allied = nullptr;
    s_war_count = 0; s_conquest_count = 0; s_fragment_count = 0; s_peace_count = 0;
    s_alliance_count = 0;
    s_active_count = 0;
    g_sim.live_base_year = 0; g_sim.live_mode = FALSE;
    if (g.node_count <= 0) return;
    g.node_owner          = (i16*)bs_memory_allocator(sizeof(i16) * g.node_count, MEMORY_TAG_GAME);
    g.node_owner_gen      = (i16*)bs_memory_allocator(sizeof(i16) * g.node_count, MEMORY_TAG_GAME);
    g.node_colonized_year = (i32*)bs_memory_allocator(sizeof(i32) * g.node_count, MEMORY_TAG_GAME);
    for (i32 i = 0; i < g.node_count; ++i) { g.node_owner[i] = -1; g.node_owner_gen[i] = -1; g.node_colonized_year[i] = 0; }
    g.node_garrison       = (f32*)bs_memory_allocator(sizeof(f32) * g.node_count, MEMORY_TAG_GAME);
    for (i32 i = 0; i < g.node_count; ++i) g.node_garrison[i] = 0.0f;
    // Cross-system Ship AI travel: allocate the macro traveler pool (empty; populated in later steps).
    g.mission_capacity = MISSION_MAX;
    g.missions = (ShipMission*)bs_memory_allocator(sizeof(ShipMission) * g.mission_capacity, MEMORY_TAG_GAME);
    for (i32 i = 0; i < g.mission_capacity; ++i) g.missions[i].active = FALSE;
    g.mission_count = 0;
    // Dynastic Houses (monarchy-only): a House per monarchy at most, so one slot per possible civ.
    g.house_capacity = GALAXY_CIV_MAX;
    g.houses = (GalaxyHouse*)bs_memory_allocator(sizeof(GalaxyHouse) * g.house_capacity, MEMORY_TAG_GAME);
    g_sim.garrison_seeded = FALSE;
    g.live_head = 0; g.live_count = 0;
    g.current_owner_civ = -1; g.current_hostile = TRUE; g.debug_force_civ = -1;
    for (i32 i = 0; i < GALAXY_CIV_MAX; ++i) g.player_rep[i] = 0;

    s_rng          = galaxy_rng_seed(g.galaxy_seed ^ 0x5CA1AB1E0F1157A9ull);
    s_start_year   = g.clock.start_year;
    s_present_year = g.clock.present_year;
    s_cur_year     = s_start_year;
    s_target       = setup_house_count(sp) * 3;   // concurrent living-kingdom target across all Houses
    s_growth       = setup_growth(sp);
    s_peace_base   = setup_peace_base(sp);
    s_alliance_base = setup_alliance_base(sp);
    s_detail       = sp.chronicle_detail;
    u8 hab_min     = setup_hab_min(sp);

    i32 span = s_present_year - s_start_year; if (span < 1) span = 1;
    i32 target_steps = sp.chronicle_detail == 0 ? 800 : (sp.chronicle_detail == 1 ? 2000 : 5000);
    s_step_years = span / target_steps; if (s_step_years < 1) s_step_years = 1;
    g_sim.gen_step_years = s_step_years;   // reference cadence for per-year rate scaling (Phase C1)
    i32 steps = span / s_step_years; if (steps < 1) steps = 1;
    s_cataclysm_prob = (f32)setup_cataclysms(sp) / (f32)steps;

    s_claimed   = (i32*)bs_memory_allocator(sizeof(i32) * g.node_count, MEMORY_TAG_GAME);
    s_habitable = (i32*)bs_memory_allocator(sizeof(i32) * g.node_count, MEMORY_TAG_GAME);
    s_cap       = (i32*)bs_memory_allocator(sizeof(i32) * GALAXY_CIV_MAX, MEMORY_TAG_GAME);
    s_notable   = (u8*) bs_memory_allocator(sizeof(u8)  * GALAXY_CIV_MAX, MEMORY_TAG_GAME);
    s_golden    = (u8*) bs_memory_allocator(sizeof(u8)  * GALAXY_CIV_MAX, MEMORY_TAG_GAME);
    s_contacted = (u8*) bs_memory_allocator((size_t)REL_BYTES, MEMORY_TAG_GAME);
    s_warred    = (u8*) bs_memory_allocator((size_t)REL_BYTES, MEMORY_TAG_GAME);
    s_bfs       = (i32*)bs_memory_allocator(sizeof(i32) * g.node_count, MEMORY_TAG_GAME);
    s_relation  = (i8*) bs_memory_allocator((size_t)GALAXY_CIV_MAX * GALAXY_CIV_MAX, MEMORY_TAG_GAME);
    s_allied    = (u8*) bs_memory_allocator((size_t)REL_BYTES, MEMORY_TAG_GAME);
    for (i32 i = 0; i < REL_BYTES; ++i) { s_contacted[i] = 0; s_warred[i] = 0; s_allied[i] = 0; }
    for (i32 i = 0; i < GALAXY_CIV_MAX * GALAXY_CIV_MAX; ++i) s_relation[i] = 0;
    for (i32 i = 0; i < g.node_count; ++i)
        if (g.nodes[i].best_habitability >= hab_min) s_habitable[s_habitable_count++] = i;

    // Dawn of recorded history: seed the founding Houses. Each genesis root rolls its own government
    // (any of the four types) and founds a House (cultural lineage); every later polity descends from one.
    i32 roots = setup_house_count(sp);
    for (i32 i = 0; i < roots; ++i)
        sim_spawn_root(g, sp, s_start_year, (u8)(galaxy_rng_next(&s_rng) % GOV_COUNT));
}

// How weighty an event is for chronicle retention (computed on demand from type + the civ's reach,
// so no per-event storage is needed). Great powers and decisive moments outrank minor skirmishes.
static i32 evt_importance(const GalaxyState& g, u8 type, i32 civ_a) {
    i32 base;
    switch (type) {
        case EVT_CATACLYSM:     base = 90; break;
        case EVT_CONQUEST:      base = 80; break;
        case EVT_COLLAPSE:      base = 55; break;
        case EVT_FOUNDING:      base = 50; break;
        case EVT_GOLDEN_AGE:    base = 45; break;
        case EVT_WAR:           base = 40; break;
        case EVT_ALLIANCE:      base = 50; break;
        case EVT_PEACE:         base = 35; break;
        case EVT_FIRST_CONTACT: base = 20; break;
        default:                base = 10; break;
    }
    if (civ_a >= 0 && civ_a < g.civ_count) {
        i32 pk = g.civs[civ_a].peak_territory;
        base += pk > 40 ? 20 : (pk > 15 ? 10 : (pk > 6 ? 4 : 0));
    }
    return base;
}

// Push an event onto the live "Galactic News" ring (newest wins; drops the oldest when full).
static void live_feed_push(GalaxyState& g, i32 year, u8 type, i32 civ_a, i32 civ_b, i32 node) {
    i32 idx = (g.live_head + g.live_count) % GALAXY_LIVE_FEED_MAX;
    if (g.live_count < GALAXY_LIVE_FEED_MAX) ++g.live_count;
    else g.live_head = (g.live_head + 1) % GALAXY_LIVE_FEED_MAX;
    HistoryEvent& le = g.live_feed[idx];
    le.year = year; le.type = type; le.civ_a = (i16)civ_a; le.civ_b = (i16)civ_b; le.node = node;
}

// Append a chronicle event. Gated by chronicle detail (broad chronicles keep only weighty events);
// once the bounded log is full, the least-important existing entry is evicted if this one outranks it.
static void hist_add(GalaxyState& g, i32 year, u8 type, i32 civ_a, i32 civ_b, i32 node) {
    i32 imp  = evt_importance(g, type, civ_a);
    i32 gate = s_detail == 0 ? 45 : (s_detail == 1 ? 25 : 0);   // broad / standard / deep
    if (imp < gate) return;
    if (g_sim.live_mode) {   // living era: route to the separate news ring; the backstory stays frozen
        live_feed_push(g, year, type, civ_a, civ_b, node);
        return;
    }
    if (g.event_count < GALAXY_EVENT_MAX) {
        HistoryEvent& e = g.events[g.event_count++];
        e.year = year; e.type = type;
        e.civ_a = (i16)civ_a; e.civ_b = (i16)civ_b; e.node = node;
        return;
    }
    i32 min_i = 0, min_imp = 0x7fffffff;
    for (i32 i = 0; i < g.event_count; ++i) {
        i32 ei = evt_importance(g, g.events[i].type, g.events[i].civ_a);
        if (ei < min_imp) { min_imp = ei; min_i = i; }
    }
    if (imp > min_imp) {
        HistoryEvent& e = g.events[min_i];
        e.year = year; e.type = type;
        e.civ_a = (i16)civ_a; e.civ_b = (i16)civ_b; e.node = node;
    }
}

// Splinter a collapsing realm: a successor civilization inherits a scattered ~half of the parent's
// worlds (the rest go derelict) and carries its lineage forward via parent_civ. Returns its index.
static i32 sim_fragment_civ(GalaxyState& g, const GalaxySetupParams& sp, i32 parent, i32 year) {
    (void)sp;
    if (g.civ_count >= GALAXY_CIV_MAX) return -1;
    i32 capital = -1;
    i32 start = (i32)(galaxy_rng_f32(&s_rng) * (f32)g.node_count);
    for (i32 i = 0; i < g.node_count; ++i) {
        i32 nn = (start + i) % g.node_count;
        if (g.node_owner[nn] == (i16)parent) { capital = nn; break; }
    }
    if (capital < 0) return -1;
    i32 c = g.civ_count++;
    Civilization& civ = g.civs[c];
    civ = Civilization{};
    civ.origin_node   = capital;
    civ.founding_year = year;
    u8  government     = (u8)(galaxy_rng_next(&s_rng) % GOV_COUNT);
    i16 parent_culture = g.civs[parent].culture_id;
    i32 parent_hs      = house_slot_of(g, parent_culture);   // >= 0 => parent belongs to a House
    civ.government    = government;
    civ.ethos         = g.civs[parent].ethos;
    if (galaxy_rng_f32(&s_rng) < 0.25f) civ.ethos = (u8)(galaxy_rng_next(&s_rng) % ETHOS_COUNT); // schism
    civ.parent_civ    = (i16)parent;
    civ.power         = g.civs[parent].power * 0.5f; if (civ.power < 1.0f) civ.power = 1.0f;
    if (parent_hs >= 0) {
        // Continue the parent's lineage regardless of the splinter's government: inherit the House's
        // stem, colour family and culture id (all polities descend from a founding House).
        civ.culture_id = parent_culture;
        civ.color      = house_shade(g.houses[parent_hs].hue, &s_rng);
        civ_compose_name(g.houses[parent_hs].stem,
                         house_polity_count(g, parent_culture), civ.name, (i32)sizeof(civ.name));
    } else {
        // Defensive fallback (a parent with no House): mint a fresh identity and found a new House.
        bs_color hue = CIV_PALETTE[c % CIV_PALETTE_N];
        char stem[12]; civ_make_stem(&s_rng, stem, (i32)sizeof(stem));
        civ.culture_id = (i16)c;
        civ.color      = house_shade(hue, &s_rng);
        civ_compose_name(stem, house_polity_count(g, (i16)c), civ.name, (i32)sizeof(civ.name));
        house_register(g, c, stem, hue);
    }
    s_cap[c]     = s_cap[parent] > 2 ? s_cap[parent] : 4;
    s_notable[c] = 0;
    s_golden[c]  = 0;
    // Contiguous breakaway: flood-fill from the capital over the parent's lane-connected worlds,
    // annexing ~40-60% of them; the rest stay with the (now fallen) parent and go derelict.
    i32 parent_nodes = 0;
    for (i32 nn = 0; nn < g.node_count; ++nn) if (g.node_owner[nn] == (i16)parent) ++parent_nodes;
    i32 target = (i32)((0.4f + 0.2f * galaxy_rng_f32(&s_rng)) * (f32)parent_nodes);
    if (target < 1) target = 1;
    i32 inherited = 0, head = 0, tail = 0;
    g.node_owner[capital] = (i16)c; g.node_colonized_year[capital] = year; ++inherited;
    s_bfs[tail++] = capital;
    while (head < tail && inherited < target) {
        i32 nn = s_bfs[head++];
        i32 a0 = g.lanes.adj_start[nn], a1 = g.lanes.adj_start[nn + 1];
        for (i32 k = a0; k < a1 && inherited < target; ++k) {
            i32 nb = g.lanes.adj_neighbor[k];
            if (nb < 0 || nb >= g.node_count) continue;
            if (g.node_owner[nb] != (i16)parent) continue;   // only annex the parent's own worlds
            g.node_owner[nb] = (i16)c; g.node_colonized_year[nb] = year; ++inherited;
            if (tail < g.node_count) s_bfs[tail++] = nb;
        }
    }
    civ.territory_count = inherited;
    civ.peak_territory  = inherited;
    if (inherited >= CIV_NOTABLE) { s_notable[c] = 1; hist_add(g, year, EVT_FOUNDING, c, -1, capital); }
    ++s_fragment_count;
    return c;
}

b8 galaxy_history_sim_step(game_state* s, i32 max_steps) {
    GalaxyState& g = s->galaxy;
    const GalaxySetupParams& sp = s->setup;
    if (!s_claimed) return TRUE;
    f32 war_prob  = setup_war_prob(sp);
    f32 frag_prob = setup_fragment_prob(sp);
    // Per-year rate scale: 1.0 during generation (step == gen step) so output is byte-identical; < 1
    // during the fine living cadence so per-step-calibrated rates stay consistent per real year.
    f32 rate_scale = g_sim.gen_step_years > 0 ? (f32)s_step_years / (f32)g_sim.gen_step_years : 1.0f;
    for (i32 it = 0; it < max_steps; ++it) {
        if (s_cur_year >= s_present_year) return TRUE;
        s_cur_year += s_step_years;
        if (s_cur_year > s_present_year) s_cur_year = s_present_year;

        // --- Growth: logistic power toward each living civ's capacity ---
        for (i32 c = 0; c < g.civ_count; ++c) {
            if (g.civs[c].status != 0) continue;
            f32 cap = (f32)s_cap[c];
            g.civs[c].power += rate_scale * s_growth * g.civs[c].power * (1.0f - g.civs[c].power / cap);
            if (g.civs[c].power < 1.0f) g.civs[c].power = 1.0f;
            if (g.civs[c].power > cap)  g.civs[c].power = cap;
        }

        // --- Expansion + War: each owned frontier node either colonises one derelict/unclaimed
        //     lane-neighbour (up to the size its power supports) or, against an adjacent living
        //     rival, wages a Lanchester border assault. At most one action per node per step. ---
        i32 snap = s_claimed_count;
        for (i32 li = 0; li < snap; ++li) {
            i32 n = s_claimed[li];
            i32 c = g.node_owner[n];
            if (c < 0 || c >= g.civ_count || g.civs[c].status != 0) continue;
            i32 a0 = g.lanes.adj_start[n], a1 = g.lanes.adj_start[n + 1];
            i32 span2 = a1 - a0; if (span2 <= 0) continue;
            b8  can_expand = (f32)g.civs[c].territory_count < g.civs[c].power;
            i32 off = (i32)(galaxy_rng_f32(&s_rng) * (f32)span2);
            for (i32 t = 0; t < span2; ++t) {
                i32 k = a0 + ((off + t) % span2);
                i32 nb = g.lanes.adj_neighbor[k];
                if (nb < 0 || nb >= g.node_count) continue;
                i16 ow = g.node_owner[nb];
                b8 claimable = (ow < 0) || (ow < (i16)g.civ_count && g.civs[ow].status != 0);
                if (claimable) {
                    if (!can_expand) continue;              // keep scanning for a colonisable world
                    b8 was_unclaimed = (ow < 0);
                    g.node_owner[nb] = (i16)c;
                    g.node_colonized_year[nb] = s_cur_year;
                    g.civs[c].territory_count++;
                    if (g.civs[c].territory_count > g.civs[c].peak_territory)
                        g.civs[c].peak_territory = g.civs[c].territory_count;
                    if (was_unclaimed && s_claimed_count < g.node_count) s_claimed[s_claimed_count++] = nb;
                    if (!s_notable[c] && g.civs[c].territory_count >= CIV_NOTABLE) {
                        s_notable[c] = 1;
                        hist_add(g, g.civs[c].founding_year, EVT_FOUNDING, c, -1, g.civs[c].origin_node);
                    }
                    if (!s_golden[c] && g.civs[c].territory_count >= CIV_GOLDEN) {
                        s_golden[c] = 1;
                        hist_add(g, s_cur_year, EVT_GOLDEN_AGE, c, -1, g.civs[c].origin_node);
                    }
                    break;
                }
                // Neighbour held by a different LIVING civilization -> diplomacy + potential war.
                i32 d = ow;
                if (d < 0 || d >= g.civ_count || d == c) continue;
                if (!rel_test(s_contacted, c, d)) {
                    rel_set(s_contacted, c, d);
                    rel_score_add(c, d, ethos_affinity(g.civs[c].ethos, g.civs[d].ethos) * 4);
                    if (g.civs[c].territory_count >= CIV_NOTABLE && g.civs[d].territory_count >= CIV_NOTABLE)
                        hist_add(g, s_cur_year, EVT_FIRST_CONTACT, c, d, nb);
                }
                // Relations drift toward cultural compatibility wherever borders touch.
                rel_score_add(c, d, ethos_affinity(g.civs[c].ethos, g.civs[d].ethos));
                // Warm, peaceful neighbours may formalise an alliance (non-aggression + mutual defense).
                if (!rel_test(s_allied, c, d)) {
                    if (!rel_test(s_warred, c, d) && rel_score(c, d) >= ALLY_SCORE &&
                        galaxy_rng_f32(&s_rng) < s_alliance_base * rate_scale) {
                        rel_set(s_allied, c, d);
                        ++s_alliance_count;
                        if (s_notable[c] || s_notable[d]) hist_add(g, s_cur_year, EVT_ALLIANCE, c, d, -1);
                    }
                } else if (rel_score(c, d) < ALLY_HYST) {
                    rel_clear(s_allied, c, d);                      // drifted apart; the alliance lapses
                }
                if (rel_test(s_allied, c, d)) continue;            // allies keep the peace; no assault
                f32 eff_war = war_prob * civ_aggression(g.civs[c].government, g.civs[c].ethos) * rate_scale;
                if (rel_score(c, d) <= RIVAL_SCORE) eff_war *= 1.5f;
                if (eff_war > 1.0f) eff_war = 1.0f;
                if (galaxy_rng_f32(&s_rng) >= eff_war) continue;   // no assault this step; keep scanning
                if (!rel_test(s_warred, c, d) && s_active_count < ACTIVE_WAR_MAX) {
                    rel_set(s_warred, c, d);                        // now "at war" (cleared on peace)
                    s_active[s_active_count].a = (i16)c;
                    s_active[s_active_count].b = (i16)d;
                    s_active[s_active_count].start_year = s_cur_year;
                    ++s_active_count;
                    ++s_war_count;
                    hist_add(g, s_cur_year, EVT_WAR, c, d, nb);
                    rel_score_add(c, d, -25);
                    // Coalition: the defender's allies are dragged in against the aggressor.
                    for (i32 y = 0; y < g.civ_count; ++y) {
                        if (y == c || y == d || g.civs[y].status != 0) continue;
                        if (!rel_test(s_allied, d, y)) continue;
                        if (rel_test(s_allied, c, y) || rel_test(s_warred, c, y)) continue;
                        if (s_active_count >= ACTIVE_WAR_MAX) break;
                        if (galaxy_rng_f32(&s_rng) >= COALITION_JOIN) continue;
                        rel_set(s_warred, c, y);
                        s_active[s_active_count].a = (i16)c;
                        s_active[s_active_count].b = (i16)y;
                        s_active[s_active_count].start_year = s_cur_year;
                        ++s_active_count;
                        ++s_war_count;
                        rel_score_add(c, y, -25);
                        if (s_notable[c] || s_notable[y]) hist_add(g, s_cur_year, EVT_WAR, c, y, nb);
                    }
                }
                rel_score_add(c, d, -1);
                f32 aw = g.civs[c].power, dw = g.civs[d].power * CIV_DEF_BONUS;
                if (galaxy_rng_f32(&s_rng) * (aw + dw + 0.001f) < aw) {
                    b8 was_capital = (nb == g.civs[d].origin_node);
                    g.node_owner[nb] = (i16)c;
                    g.node_colonized_year[nb] = s_cur_year;
                    ++g.civs[c].territory_count;
                    if (g.civs[d].territory_count > 0) --g.civs[d].territory_count;
                    if (g.civs[c].territory_count > g.civs[c].peak_territory)
                        g.civs[c].peak_territory = g.civs[c].territory_count;
                    g.civs[c].power *= 0.99f; if (g.civs[c].power < 1.0f) g.civs[c].power = 1.0f;
                    g.civs[d].power *= 0.90f; if (g.civs[d].power < 1.0f) g.civs[d].power = 1.0f;
                    if (was_capital) {
                        g.civs[d].status = 1; g.civs[d].fall_year = s_cur_year;
                        ++s_conquest_count;
                        hist_add(g, s_cur_year, EVT_CONQUEST, c, d, nb);
                        // The victor annexes a share of the fallen realm's remaining worlds.
                        for (i32 an = 0; an < g.node_count; ++an) {
                            if (g.node_owner[an] != (i16)d) continue;
                            if (galaxy_rng_f32(&s_rng) < WAR_ABSORB_FRAC) {
                                g.node_owner[an] = (i16)c;
                                g.node_colonized_year[an] = s_cur_year;
                                ++g.civs[c].territory_count;
                            }
                        }
                        if (g.civs[c].territory_count > g.civs[c].peak_territory)
                            g.civs[c].peak_territory = g.civs[c].territory_count;
                    }
                } else {
                    g.civs[c].power *= 0.98f; if (g.civs[c].power < 1.0f) g.civs[c].power = 1.0f;
                }
                break;                                       // one assault per node this step
            }
        }

        // --- Collapse: hazard rises with age + overextension; fallen worlds go derelict, and a
        //     large enough realm may splinter into a successor state (parent_civ lineage). ---
        f32 kk = setup_collapse_k(sp);
        f32 base = (f32)s_step_years / (f32)CIV_MEAN_LIFETIME;
        i32 pre_collapse = g.civ_count;
        for (i32 c = 0; c < pre_collapse; ++c) {
            if (g.civs[c].status != 0) continue;
            f32 age_norm = (f32)(s_cur_year - g.civs[c].founding_year) / (f32)CIV_MEAN_LIFETIME;
            f32 overext  = (f32)g.civs[c].territory_count / ((f32)s_cap[c] + 1.0f);
            f32 hazard   = base * kk * (0.6f + 0.8f * age_norm + 0.4f * overext);
            if (hazard > 0.9f) hazard = 0.9f;
            if (galaxy_rng_f32(&s_rng) < hazard) {
                g.civs[c].status = 1; g.civs[c].fall_year = s_cur_year;
                if (s_notable[c]) hist_add(g, s_cur_year, EVT_COLLAPSE, c, -1, g.civs[c].origin_node);
                if (g.civs[c].territory_count >= CIV_FRAGMENT_MIN && galaxy_rng_f32(&s_rng) < frag_prob)
                    sim_fragment_civ(g, sp, c, s_cur_year);
            }
        }

        // --- Cataclysm: rare galaxy-scale shock; may fell the affected civ ---
        if (s_cataclysm_prob > 0.0f && galaxy_rng_f32(&s_rng) < s_cataclysm_prob * rate_scale) {
            i32 node = (i32)(galaxy_rng_f32(&s_rng) * (f32)g.node_count);
            if (node >= 0 && node < g.node_count) {
                i32 owner = g.node_owner[node];
                i32 oc = (owner >= 0 && owner < g.civ_count) ? owner : -1;
                hist_add(g, s_cur_year, EVT_CATACLYSM, oc, -1, node);
                if (oc >= 0 && g.civs[oc].status == 0 && galaxy_rng_f32(&s_rng) < 0.5f) {
                    g.civs[oc].status = 1; g.civs[oc].fall_year = s_cur_year;
                    if (s_notable[oc]) hist_add(g, s_cur_year, EVT_COLLAPSE, oc, -1, g.civs[oc].origin_node);
                }
            }
        }

        // --- War resolution: ongoing wars tire into peace (more readily when evenly matched or long-
        //     running), or dissolve silently when a belligerent has already fallen. ---
        for (i32 wi = 0; wi < s_active_count; ) {
            i32 wa = s_active[wi].a, wb = s_active[wi].b;
            b8 a_dead = (wa < 0 || wa >= g.civ_count || g.civs[wa].status != 0);
            b8 b_dead = (wb < 0 || wb >= g.civ_count || g.civs[wb].status != 0);
            if (a_dead || b_dead) {
                rel_clear(s_warred, wa, wb);
                s_active[wi] = s_active[--s_active_count];
                continue;
            }
            f32 pa = g.civs[wa].power, pb = g.civs[wb].power;
            f32 hi = pa > pb ? pa : pb, lo = pa < pb ? pa : pb;
            f32 ratio = lo / (hi + 0.001f);
            f32 dur   = (f32)(s_cur_year - s_active[wi].start_year) / (f32)CIV_MEAN_LIFETIME;
            f32 p_end = s_peace_base * rate_scale * (0.5f + 0.5f * ratio) + 0.4f * dur;
            if (p_end > 0.95f) p_end = 0.95f;
            if (galaxy_rng_f32(&s_rng) < p_end) {
                rel_clear(s_warred, wa, wb);
                rel_score_add(wa, wb, 5);                    // reconciliation nudges relations back up
                if (s_notable[wa] || s_notable[wb]) hist_add(g, s_cur_year, EVT_PEACE, wa, wb, -1);
                ++s_peace_count;
                s_active[wi] = s_active[--s_active_count];
                continue;
            }
            ++wi;
        }

        // --- Birth: keep a modest spread of living kingdoms. New polities descend from an existing
        //     House (successor kingdoms rising within a lineage); a brand-new House only arises to
        //     backfill toward the House target (rare renaissance), so the galaxy never empties. ---
        i32 alive = 0;
        for (i32 c = 0; c < g.civ_count; ++c) if (g.civs[c].status == 0) ++alive;
        i32 living_houses = count_living_houses(g);
        i32 house_target  = setup_house_count(sp);
        if (living_houses < house_target && galaxy_rng_f32(&s_rng) < 0.05f * rate_scale)
            sim_spawn_house(g, sp, s_cur_year);
        f32 exp_births = 0.15f * (f32)(s_target - alive);
        if (exp_births < 0.0f) exp_births = 0.0f;
        exp_births += 0.04f;
        exp_births *= rate_scale;
        i32 births = (i32)exp_births;
        if (galaxy_rng_f32(&s_rng) < (exp_births - (f32)births)) ++births;
        for (i32 b = 0; b < births; ++b) {
            i32 root = pick_living_house_root(g);
            if (root >= 0) sim_spawn_successor(g, sp, root, s_cur_year);
            else           sim_spawn_house(g, sp, s_cur_year);   // no living House left -> reseed one
        }
    }
    return (s_cur_year >= s_present_year);
}

// Refresh the present-day VIEW from the resident sim: derelict fallen civs' worlds, recount the
// survivors' holdings, re-sort the chronicle, and log a summary. Idempotent and repeatable -> this is
// the checkpoint the living present (Phase C1) calls after advancing. Does NOT free the working-set.
void galaxy_history_finalize_view(game_state* s) {
    GalaxyState& g = s->galaxy;
    if (!s_claimed) return;
    // Free every fallen civ's territory (derelict worlds return to the wild) and recount the
    // present-day holdings of the survivors.
    for (i32 c = 0; c < g.civ_count; ++c) g.civs[c].territory_count = 0;
    for (i32 n = 0; n < g.node_count; ++n) {
        i32 ow = g.node_owner[n];
        if (ow < 0) continue;
        if (ow >= g.civ_count || g.civs[ow].status != 0) { g.node_owner[n] = -1; continue; }
        g.civs[ow].territory_count++;
    }
    // Chronicle oldest -> newest (insertion sort; event_count is bounded).
    for (i32 i = 1; i < g.event_count; ++i) {
        HistoryEvent tmp = g.events[i]; i32 j = i - 1;
        while (j >= 0 && g.events[j].year > tmp.year) { g.events[j + 1] = g.events[j]; --j; }
        g.events[j + 1] = tmp;
    }
}

// Emit the one-shot generation summary (called once when generation completes, not per live checkpoint).
void galaxy_history_log_summary(const game_state* s) {
    const GalaxyState& g = s->galaxy;
    i32 claimed = 0, alive = 0;
    for (i32 n = 0; n < g.node_count; ++n) if (g.node_owner[n] >= 0) ++claimed;
    for (i32 c = 0; c < g.civ_count; ++c) if (g.civs[c].status == 0) ++alive;
    BS_LOG_INFO("PhaseB history: %d civs ever, %d alive, %d events, %d wars, %d peace, %d alliances, %d conquests, %d fragments, %d/%d systems held",
                g.civ_count, alive, g.event_count, s_war_count, s_peace_count, s_alliance_count, s_conquest_count, s_fragment_count, claimed, g.node_count);
}

// Release the resident simulation working-set. Idempotent (no-op if nothing is allocated); called at
// the start of a (re)generation. The durable outputs (civs[], node_owner[], events[]) are NOT touched.
void galaxy_history_sim_free(game_state* s) {
    GalaxyState& g = s->galaxy;
    if (!s_claimed) return;
    bs_memory_free(s_claimed,   sizeof(i32) * g.node_count, MEMORY_TAG_GAME);
    bs_memory_free(s_habitable, sizeof(i32) * g.node_count, MEMORY_TAG_GAME);
    bs_memory_free(s_cap,     sizeof(i32) * GALAXY_CIV_MAX, MEMORY_TAG_GAME);
    bs_memory_free(s_notable, sizeof(u8)  * GALAXY_CIV_MAX, MEMORY_TAG_GAME);
    bs_memory_free(s_golden,  sizeof(u8)  * GALAXY_CIV_MAX, MEMORY_TAG_GAME);
    bs_memory_free(s_contacted, (size_t)REL_BYTES, MEMORY_TAG_GAME);
    bs_memory_free(s_warred,    (size_t)REL_BYTES, MEMORY_TAG_GAME);
    bs_memory_free(s_bfs,       sizeof(i32) * g.node_count, MEMORY_TAG_GAME);
    bs_memory_free(s_relation, (size_t)GALAXY_CIV_MAX * GALAXY_CIV_MAX, MEMORY_TAG_GAME);
    bs_memory_free(s_allied,   (size_t)REL_BYTES, MEMORY_TAG_GAME);
    s_claimed = nullptr; s_habitable = nullptr; s_cap = nullptr; s_notable = nullptr; s_golden = nullptr;
    s_contacted = nullptr; s_warred = nullptr; s_bfs = nullptr; s_relation = nullptr; s_allied = nullptr;
    s_claimed_count = 0; s_habitable_count = 0;
    if (g.houses) bs_memory_free(g.houses, sizeof(GalaxyHouse) * g.house_capacity, MEMORY_TAG_GAME);
    g.houses = nullptr; g.house_capacity = 0; g.house_count = 0;
}

f32 galaxy_history_sim_progress(const game_state* s) {
    (void)s;
    i32 span = s_present_year - s_start_year;
    if (span <= 0) return 1.0f;
    f32 p = (f32)(s_cur_year - s_start_year) / (f32)span;
    return p < 0.0f ? 0.0f : (p > 1.0f ? 1.0f : p);
}

void galaxy_history_generate(game_state* s) {
    galaxy_history_sim_begin(s);
    while (!galaxy_history_sim_step(s, 100000)) { }
    galaxy_history_finalize_view(s);
    galaxy_history_log_summary(s);
    galaxy_history_seed_garrison(s);
}

// Step B: civilization fleet garrison (macro fleet strength stationed at each node). Player-independent:
// seeded from ownership at generation, then evolved every living tick (reinforce toward capacity +
// attrition at active-war borders = the ongoing civ-vs-civ space battles). Materialisation reads it for
// how many NPC ships to spawn; player kills write back so losses persist and feed the macro balance.
static const f32 GARRISON_MAX = 12.0f;

static f32 garrison_capacity(const GalaxyState& g, i32 civ, i32 node) {
    if (civ < 0 || civ >= g.civ_count) return 0.0f;
    f32 cap = 2.0f + g.civs[civ].power * 0.6f;
    if (g.civs[civ].origin_node == node) cap += 4.0f;   // homeworld bastion
    if (cap > GARRISON_MAX) cap = GARRISON_MAX;
    return cap;
}

static void garrison_seed(GalaxyState& g) {
    if (!g.node_garrison) return;
    for (i32 n = 0; n < g.node_count; ++n) {
        i32 ow = g.node_owner[n];
        g.node_garrison[n] = (ow >= 0) ? garrison_capacity(g, ow, n) : 0.0f;
    }
}

// One background step over `years` sim-years: reinforce toward capacity, then grind down where an
// owned node borders an at-war rival (Lanchester-ish: losses scale with the enemy garrison).
static void garrison_step(GalaxyState& g, i32 years) {
    if (!g.node_garrison || years <= 0) return;
    const f32 REINFORCE = 0.15f; // fraction of the gap closed per year
    const f32 ATTR      = 0.02f; // per-enemy-strength loss per year at an active-war border
    f32 yf = (f32)years;
    f32 r = REINFORCE * yf; if (r > 1.0f) r = 1.0f;
    for (i32 n = 0; n < g.node_count; ++n) {
        i32 ow = g.node_owner[n];
        if (ow < 0) { g.node_garrison[n] = 0.0f; continue; }
        f32 gcur = g.node_garrison[n];
        gcur += (garrison_capacity(g, ow, n) - gcur) * r;   // reinforce toward capacity
        i32 a0 = g.lanes.adj_start[n], a1 = g.lanes.adj_start[n + 1];
        for (i32 k = a0; k < a1; ++k) {
            i32 nb = g.lanes.adj_neighbor[k];
            if (nb < 0 || nb >= g.node_count) continue;
            i32 od = g.node_owner[nb];
            if (od < 0 || od == ow) continue;
            if (!rel_test(s_warred, ow, od)) continue;      // only active wars grind the frontier
            gcur -= ATTR * g.node_garrison[nb] * yf;
        }
        if (gcur < 0.0f) gcur = 0.0f;
        if (gcur > GARRISON_MAX) gcur = GARRISON_MAX;
        g.node_garrison[n] = gcur;
    }
}

// Seed the garrison layer from present-day ownership (call once at generation completion).
void galaxy_history_seed_garrison(game_state* s) {
    garrison_seed(s->galaxy);
    g_sim.garrison_seeded = TRUE;
}

// Fleet strength stationed at a node right now (rounded), or 0.
i32 galaxy_history_garrison_at(const game_state* s, i32 node) {
    const GalaxyState& g = s->galaxy;
    if (!g.node_garrison || node < 0 || node >= g.node_count) return 0;
    f32 v = g.node_garrison[node];
    return (i32)(v + 0.5f);
}

// Adjust a node's garrison (e.g. player kills -> negative). Clamped to [0, GARRISON_MAX].
void galaxy_history_garrison_add(game_state* s, i32 node, i32 delta) {
    GalaxyState& g = s->galaxy;
    if (!g.node_garrison || node < 0 || node >= g.node_count) return;
    f32 v = g.node_garrison[node] + (f32)delta;
    if (v < 0.0f) v = 0.0f;
    if (v > GARRISON_MAX) v = GARRISON_MAX;
    g.node_garrison[node] = v;
}

// DEBUG: locate an inter-civ border and ensure it is an active war so the frontier visibly grinds.
i32 galaxy_history_debug_war_frontier(game_state* s, i32 start_node, i32* out_civ_a, i32* out_civ_b) {
    GalaxyState& g = s->galaxy;
    if (out_civ_a) *out_civ_a = -1;
    if (out_civ_b) *out_civ_b = -1;
    if (!g.node_owner || g.node_count <= 0) return -1;
    i32 begin = (start_node < 0 || start_node >= g.node_count) ? 0 : start_node + 1;
    i32 war_node = -1, war_a = -1, war_b = -1;   // already-at-war border (preferred)
    i32 any_node = -1, any_a = -1, any_b = -1;   // any inter-civ border (fallback -> force war)
    for (i32 i = 0; i < g.node_count && war_node < 0; ++i) {
        i32 n = (begin + i) % g.node_count;
        i32 ow = g.node_owner[n];
        if (ow < 0) continue;
        i32 a0 = g.lanes.adj_start[n], a1 = g.lanes.adj_start[n + 1];
        for (i32 k = a0; k < a1; ++k) {
            i32 nb = g.lanes.adj_neighbor[k];
            if (nb < 0 || nb >= g.node_count) continue;
            i32 od = g.node_owner[nb];
            if (od < 0 || od == ow) continue;
            if (rel_test(s_warred, ow, od)) { war_node = n; war_a = ow; war_b = od; break; }
            if (any_node < 0) { any_node = n; any_a = ow; any_b = od; }
        }
    }
    i32 node = (war_node >= 0) ? war_node : any_node;
    if (node < 0) return -1;
    i32 ca = (war_node >= 0) ? war_a : any_a;
    i32 cb = (war_node >= 0) ? war_b : any_b;
    if (war_node < 0) {
        rel_set(s_warred, ca, cb);   // force the war so garrison_step grinds this frontier
        BS_LOG_INFO("Debug war frontier: forced war %s vs %s at node %d", g.civs[ca].name, g.civs[cb].name, node);
    } else {
        BS_LOG_INFO("Debug war frontier: existing war %s vs %s at node %d", g.civs[ca].name, g.civs[cb].name, node);
    }
    if (out_civ_a) *out_civ_a = ca;
    if (out_civ_b) *out_civ_b = cb;
    return node;
}

// Phase C1: advance the living present on the shared in-game calendar. Derives the target present-year
// from s->sim_hours (1 real sec = 1 in-game hour at 1x, scaled by time_scale); once a whole year has
// accrued it steps the resident sim at ~1 yr/step and refreshes the view. dt is only a pause guard.
void galaxy_history_live_tick(game_state* s, f32 dt) {
    if (!s_claimed || dt <= 0.0f) return;
    if (!g_sim.live_mode) {
        g_sim.live_mode      = TRUE;
        g_sim.live_base_year = s_present_year; // calendar epoch: present-year when live play begins
    }
    // Target present-year = epoch + whole in-game years elapsed on the shared clock.
    i64 elapsed_years = (i64)(s->sim_hours / (f64)HOURS_PER_YEAR);
    i32 years = (i32)(((i64)g_sim.live_base_year + elapsed_years) - (i64)s_present_year);
    if (years <= 0) return;
    if (years > LIVE_MAX_YEARS_PER_TICK) years = LIVE_MAX_YEARS_PER_TICK;
    s_step_years    = LIVE_STEP_YEARS;
    s_present_year += years;
    s->galaxy.clock.present_year = s_present_year;
    galaxy_history_sim_step(s, years / LIVE_STEP_YEARS);
    // Step B: evolve civilization fleet garrisons in the background (player-independent). Seed on the
    // first live tick if generation didn't already; then reinforce + grind active-war frontiers.
    if (!g_sim.garrison_seeded) { garrison_seed(s->galaxy); g_sim.garrison_seeded = TRUE; }
    else                        garrison_step(s->galaxy, years);
    galaxy_history_finalize_view(s);
}

// Phase C2: player write-backs into the living macro sim. Immediate mutations + a news headline; the
// resident sim (C0) + living clock (C1) then propagate the change over the following live-minutes.
static void player_rep_add(GalaxyState& g, i32 civ, i32 d) {
    i32 v = (i32)g.player_rep[civ] + d;
    if (v >  100) v =  100;
    if (v < -100) v = -100;
    g.player_rep[civ] = (i8)v;
}

void galaxy_history_player_raid(game_state* s, i32 civ, f32 strength) {
    GalaxyState& g = s->galaxy;
    if (civ < 0 || civ >= g.civ_count || g.civs[civ].status != 0) return;
    g.civs[civ].power -= strength;
    if (g.civs[civ].power < 1.0f) g.civs[civ].power = 1.0f;
    player_rep_add(g, civ, -12);
    live_feed_push(g, g.clock.present_year, EVT_PLAYER_RAID, civ, -1, g.civs[civ].origin_node);
    BS_LOG_INFO("PhaseC player raid: %s -> power %.1f, rep %d", g.civs[civ].name, g.civs[civ].power, (i32)g.player_rep[civ]);
}

void galaxy_history_player_aid(game_state* s, i32 civ, f32 strength) {
    GalaxyState& g = s->galaxy;
    if (civ < 0 || civ >= g.civ_count || g.civs[civ].status != 0) return;
    g.civs[civ].power += strength * 0.25f;
    player_rep_add(g, civ, +12);
    live_feed_push(g, g.clock.present_year, EVT_PLAYER_AID, civ, -1, g.civs[civ].origin_node);
    BS_LOG_INFO("PhaseC player aid: %s -> power %.1f, rep %d", g.civs[civ].name, g.civs[civ].power, (i32)g.player_rep[civ]);
}

i32 galaxy_history_player_rep(const game_state* s, i32 civ) {
    const GalaxyState& g = s->galaxy;
    if (civ < 0 || civ >= g.civ_count) return 0;
    return (i32)g.player_rep[civ];
}

// Faction Step 1: does the civ's patrols treat the player as hostile? Reputation vs an ethos-adjusted
// threshold; a civ index < 0 (lawless wild space) always reads hostile (pirates).
b8 galaxy_history_is_hostile(const game_state* s, i32 civ) {
    const GalaxyState& g = s->galaxy;
    if (civ < 0 || civ >= g.civ_count) return TRUE;
    i32 thr = -40;
    switch (g.civs[civ].ethos) {
        case ETHOS_XENOPHOBE:  thr = -20; break;   // touchy: turns hostile sooner
        case ETHOS_MILITANT:   thr = -30; break;
        case ETHOS_MERCANTILE: thr = -55; break;
        case ETHOS_HARMONIOUS: thr = -60; break;   // forgiving
        default: break;
    }
    return (i32)g.player_rep[civ] <= thr ? TRUE : FALSE;
}

i32 galaxy_history_owner_at_node(const game_state* s, i32 node_index) {
    const GalaxyState& g = s->galaxy;
    if (!g.node_owner || node_index < 0 || node_index >= g.node_count) return -1;
    return g.node_owner[node_index];
}

// Feature B (B-3): public reads over the resident relation matrix (currently-at-war / allied bitsets).
b8 galaxy_history_civ_at_war(const game_state* s, i32 a, i32 b) {
    const GalaxyState& g = s->galaxy;
    if (a < 0 || b < 0 || a == b || a >= g.civ_count || b >= g.civ_count) return FALSE;
    return rel_test(s_warred, a, b);
}

b8 galaxy_history_civ_allied(const game_state* s, i32 a, i32 b) {
    const GalaxyState& g = s->galaxy;
    if (a < 0 || b < 0 || a == b || a >= g.civ_count || b >= g.civ_count) return FALSE;
    return rel_test(s_allied, a, b);
}

// Feature B: unified faction stance folding transitive diplomacy. A static FACTION_PLAYER is never
// hostile; any other negative id (pirates / wild space) is always hostile. For a civ, direct
// reputation decides first (galaxy_history_is_hostile); otherwise, if the player is closely allied
// with some living civ A, that civ's active war enemies treat the player as hostile too.
b8 galaxy_history_faction_is_hostile(const game_state* s, i16 faction_id) {
    if (faction_id == FACTION_PLAYER) return FALSE;
    if (faction_id < 0)               return TRUE;   // FACTION_PIRATE / wild raiders
    i32 civ = (i32)faction_id;
    if (galaxy_history_is_hostile(s, civ)) return TRUE;
    const GalaxyState& g = s->galaxy;
    if (civ >= g.civ_count) return TRUE;
    const i32 FRIEND_T = 50;  // player standing that reads as "allied" for transitive purposes
    for (i32 a = 0; a < g.civ_count; ++a) {
        if (a == civ || g.civs[a].status != 0) continue;
        if ((i32)g.player_rep[a] >= FRIEND_T && rel_test(s_warred, a, civ)) return TRUE;
    }
    return FALSE;
}

// Phase 1 (autonomous universe): pairwise stance between any two unified faction ids.
b8 galaxy_history_factions_hostile(const game_state* s, i16 a, i16 b) {
    if (a == b) return FALSE;                                        // never hostile to own faction
    if (a == FACTION_PLAYER) return galaxy_history_faction_is_hostile(s, b);
    if (b == FACTION_PLAYER) return galaxy_history_faction_is_hostile(s, a);
    if (a < 0 || b < 0) return TRUE;                                 // pirates / wild: hostile to all
    return galaxy_history_civ_at_war(s, (i32)a, (i32)b);             // civs: only while at war
}

// Feature B: a human-readable banner label for any faction id.
void galaxy_history_faction_label(const game_state* s, i16 faction_id, char* out, i32 out_size) {
    if (!out || out_size <= 0) return;
    if (faction_id == FACTION_PLAYER) { snprintf(out, (size_t)out_size, "Your Fleet"); return; }
    if (faction_id < 0)               { snprintf(out, (size_t)out_size, "Pirate Raider"); return; }
    const GalaxyState& g = s->galaxy;
    i32 civ = (i32)faction_id;
    if (civ >= g.civ_count) { snprintf(out, (size_t)out_size, "Pirate Raider"); return; }
    snprintf(out, (size_t)out_size, "%s Patrol", g.civs[civ].name);
}

// Format one event into a sentence (shared by the deep-time Legends and the live Galactic News).
static void format_event(const GalaxyState& g, const HistoryEvent& e, char* out, i32 out_size) {
    i32 bp = g.clock.present_year - e.year;   // years before present
    const char* na  = (e.civ_a >= 0 && e.civ_a < g.civ_count) ? g.civs[e.civ_a].name : "an unknown people";
    const char* nb  = (e.civ_b >= 0 && e.civ_b < g.civ_count) ? g.civs[e.civ_b].name : "another";
    const char* sys = (e.node  >= 0 && e.node  < g.node_count) ? g.nodes[e.node].name : "the deep";
    switch (e.type) {
        case EVT_FOUNDING:      snprintf(out, (size_t)out_size, "%d BP  The %s arose on %s.", bp, na, sys); break;
        case EVT_FIRST_CONTACT: snprintf(out, (size_t)out_size, "%d BP  The %s made first contact with the %s.", bp, na, nb); break;
        case EVT_WAR:           snprintf(out, (size_t)out_size, "%d BP  War erupted: the %s against the %s.", bp, na, nb); break;
        case EVT_CONQUEST:      snprintf(out, (size_t)out_size, "%d BP  The %s subjugated the %s.", bp, na, nb); break;
        case EVT_GOLDEN_AGE:    snprintf(out, (size_t)out_size, "%d BP  A golden age dawned for the %s.", bp, na); break;
        case EVT_COLLAPSE:      snprintf(out, (size_t)out_size, "%d BP  The %s collapsed into ruin.", bp, na); break;
        case EVT_CATACLYSM:     snprintf(out, (size_t)out_size, "%d BP  A cataclysm devastated %s.", bp, sys); break;
        case EVT_PEACE:         snprintf(out, (size_t)out_size, "%d BP  The %s and the %s made peace.", bp, na, nb); break;
        case EVT_ALLIANCE:      snprintf(out, (size_t)out_size, "%d BP  The %s and the %s forged an alliance.", bp, na, nb); break;
        case EVT_PLAYER_RAID:   snprintf(out, (size_t)out_size, "%d BP  A rogue captain raided the %s.", bp, na); break;
        case EVT_PLAYER_AID:    snprintf(out, (size_t)out_size, "%d BP  A captain lent aid to the %s.", bp, na); break;
        default:                snprintf(out, (size_t)out_size, "%d BP  A forgotten event.", bp); break;
    }
}

void galaxy_history_event_text(const game_state* s, i32 event_index, char* out, i32 out_size) {
    const GalaxyState& g = s->galaxy;
    if (out_size <= 0) return;
    if (event_index < 0 || event_index >= g.event_count) { out[0] = 0; return; }
    format_event(g, g.events[event_index], out, out_size);
}

void galaxy_history_build_legends(game_state* s) {
    GalaxyState& g = s->galaxy;
    b8 open = g.show_legends ? TRUE : FALSE;
    if (bs_ui_begin_window("GALAXY LEGENDS", &open)) {
        i32 alive = 0;
        for (i32 c = 0; c < g.civ_count; ++c) if (g.civs[c].status == 0) ++alive;
        bs_ui_text_colored(0.88f, 0.82f, 0.55f, 1.0f, "GALAXY LEGENDS");
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "%d civilizations (%d alive)  -  %d recorded events",
                 g.civ_count, alive, g.event_count);
        bs_ui_text(hdr);
        bs_ui_separator();
        char line[192];
        for (i32 i = 0; i < g.event_count; ++i) {
            galaxy_history_event_text(s, i, line, (i32)sizeof(line));
            i16 ca = g.events[i].civ_a;
            if (ca >= 0 && ca < g.civ_count) {
                bs_color col = g.civs[ca].color;
                bs_ui_text_colored(col.r, col.g, col.b, 1.0f, line);
            } else {
                bs_ui_text_colored(0.75f, 0.75f, 0.80f, 1.0f, line);
            }
        }
    }
    bs_ui_end_window();
    g.show_legends = open ? true : false;
}

// Recursive helper: render one polity row of a lineage's genealogy, then its successor polities
// (children in the lineage). Each row shows the polity's government; living polities show their reach,
// fallen ones show their span and are dimmed. Depth indents the row so the succession reads as a tree.
static void house_tree_row(const GalaxyState& g, i32 civ, i16 culture, i32 depth) {
    const Civilization& cv = g.civs[civ];
    bs_ui_set_cursor_pos_x(12.0f + (f32)depth * 16.0f);
    char row[224];
    const char* gov = civ_government_name(cv.government);
    i32 born = g.clock.present_year - cv.founding_year;
    if (cv.status == 0) {
        snprintf(row, sizeof(row), "%s%s [%s]  -  founded %d BP, ruling (%d systems)",
                 depth > 0 ? "|- " : "", cv.name, gov, born, cv.territory_count);
    } else {
        i32 fell = g.clock.present_year - cv.fall_year;
        snprintf(row, sizeof(row), "%s%s [%s]  -  %d BP -> %d BP (peak %d)",
                 depth > 0 ? "|- " : "", cv.name, gov, born, fell, cv.peak_territory);
    }
    bs_color col = cv.color;
    if (cv.status != 0) { col.r *= 0.55f; col.g *= 0.55f; col.b *= 0.55f; }   // dim the fallen
    bs_ui_text_colored(col.r, col.g, col.b, 1.0f, row);
    for (i32 ch = civ + 1; ch < g.civ_count; ++ch)   // children were created after their parent
        if (g.civs[ch].parent_civ == (i16)civ && g.civs[ch].culture_id == culture)
            house_tree_row(g, ch, culture, depth + 1);
}

// Civilization Lineages browser (H): a per-lineage heredity tree of the successor polities (of any
// government) that rose and fell within each founding line. Read-only. Mirrors build_legends' pattern.
void galaxy_history_build_houses(game_state* s) {
    GalaxyState& g = s->galaxy;
    b8 open = g.show_houses ? TRUE : FALSE;
    if (bs_ui_begin_window("CIVILIZATION LINEAGES", &open)) {
        bs_ui_text_colored(0.88f, 0.82f, 0.55f, 1.0f, "CIVILIZATION LINEAGES");
        char hdr[96];
        snprintf(hdr, sizeof(hdr), "%d lineages  -  %d still living", g.house_count, count_living_houses(g));
        bs_ui_text(hdr);
        bs_ui_separator();
        for (i32 h = 0; h < g.house_count; ++h) {
            i16 culture = g.houses[h].root_civ;
            i32 total = 0, living = 0, territory = 0;
            for (i32 c = 0; c < g.civ_count; ++c) {
                if (g.civs[c].culture_id != culture) continue;
                ++total;
                if (g.civs[c].status == 0) { ++living; territory += g.civs[c].territory_count; }
            }
            const char* root_gov = (culture >= 0 && culture < (i16)g.civ_count)
                                 ? civ_government_name(g.civs[culture].government) : "?";
            bs_color hue = g.houses[h].hue;
            char head[160];
            snprintf(head, sizeof(head), "LINEAGE %s [founded as %s]  -  %d polities, %d living (%d systems)",
                     g.houses[h].stem, root_gov, total, living, territory);
            bs_ui_text_colored(hue.r, hue.g, hue.b, 1.0f, head);
            if (culture >= 0 && culture < (i16)g.civ_count)
                house_tree_row(g, culture, culture, 0);
            bs_ui_separator();
        }
    }
    bs_ui_end_window();
    g.show_houses = open ? true : false;
}

// Phase C1: the live "Galactic News" window - recent events from the ongoing present (newest first).
void galaxy_history_build_news(game_state* s) {
    GalaxyState& g = s->galaxy;
    b8 open = g.show_news ? TRUE : FALSE;
    if (bs_ui_begin_window("GALACTIC NEWS", &open)) {
        bs_ui_text_colored(0.55f, 0.85f, 0.95f, 1.0f, "GALACTIC NEWS");
        char hdr[96];
        i64 total_hours = (i64)s->sim_hours;
        i32 day = (i32)((total_hours % HOURS_PER_YEAR) / HOURS_PER_DAY) + 1;
        snprintf(hdr, sizeof(hdr), "Year %d, Day %d  -  %d recent events", g.clock.present_year, day, g.live_count);
        bs_ui_text(hdr);
        bs_ui_separator();
        char line[192];
        for (i32 k = g.live_count - 1; k >= 0; --k) {
            i32 idx = (g.live_head + k) % GALAXY_LIVE_FEED_MAX;
            const HistoryEvent& e = g.live_feed[idx];
            format_event(g, e, line, (i32)sizeof(line));
            i16 ca = e.civ_a;
            if (ca >= 0 && ca < g.civ_count) {
                bs_color col = g.civs[ca].color;
                bs_ui_text_colored(col.r, col.g, col.b, 1.0f, line);
            } else {
                bs_ui_text_colored(0.75f, 0.75f, 0.80f, 1.0f, line);
            }
        }
    }
    bs_ui_end_window();
    g.show_news = open ? true : false;
}

// Phase C2: the Live Civ Inspector - the civ that holds the player's current system + your standing.
void galaxy_history_build_inspector(game_state* s) {
    GalaxyState& g = s->galaxy;
    b8 open = g.show_inspector ? TRUE : FALSE;
    if (bs_ui_begin_window("LIVE CIV INSPECTOR", &open)) {
        bs_ui_text_colored(0.75f, 0.90f, 0.65f, 1.0f, "LIVE CIV INSPECTOR");
        bs_ui_separator();
        i32 civ = g.current_owner_civ;   // set each frame by the update loop (respects the F3 debug pin)
        if (civ < 0 || civ >= g.civ_count) {
            bs_ui_text("Current system: unclaimed / wild space.");
        } else {
            const Civilization& cv = g.civs[civ];
            char line[192];
            bs_ui_text_colored(cv.color.r, cv.color.g, cv.color.b, 1.0f, cv.name);
            snprintf(line, sizeof(line), "%s - %s%s", civ_government_name(cv.government),
                     civ_ethos_name(cv.ethos), cv.status != 0 ? "  [FALLEN]" : "");
            bs_ui_text(line);
            snprintf(line, sizeof(line), "Power %.1f    Territory %d systems", cv.power, cv.territory_count);
            bs_ui_text(line);
            i32 rep = (i32)g.player_rep[civ];
            b8 hostile = galaxy_history_is_hostile(s, civ);
            const char* stance = hostile ? "HOSTILE - patrols will engage" : (rep >= 40 ? "Friendly" : "Neutral");
            snprintf(line, sizeof(line), "Reputation with you: %d  (%s)", rep, stance);
            bs_ui_text(line);
            // Step B: live fleet garrison stationed at the player's current system.
            i32 pnode = galaxy_nearest_node(s, &s->fleet_state.fleet.flagship().ship.origin);
            snprintf(line, sizeof(line), "Fleet garrison here: %d ships", galaxy_history_garrison_at(s, pnode));
            bs_ui_text(line);
            // P3: live NPC population breakdown by role (the materialized ships in this system).
            i32 n_patrol = 0, n_miner = 0, n_trader = 0;
            for (i32 i = 0; i < NPC_SHIP_MAX; ++i) {
                const NpcShip& np = s->npc_ships[i];
                if (!np.active) continue;
                if      (np.archetype == ARCHETYPE_MINER)  ++n_miner;
                else if (np.archetype == ARCHETYPE_TRADER) ++n_trader;
                else                                       ++n_patrol;
            }
            snprintf(line, sizeof(line), "Ships present: %d patrol / %d miner / %d trader",
                     n_patrol, n_miner, n_trader);
            bs_ui_text(line);
            // Government interaction: open the polity's themed engagement window (Parliament, Royal
            // Court, Sacred Synod, Charter Council) for the civ that owns the player's current system.
            if (cv.status == 0) {
                bs_ui_separator();
                char btn[64];
                snprintf(btn, sizeof(btn), "Open %s", civ_gov_window_name(cv.government));
                if (bs_ui_button(btn, TRUE)) {
                    switch (cv.government) {
                        case GOV_REPRESENTATIVE_REPUBLIC: g.gov_window_type = GOV_WIN_PARLIAMENT;      break;
                        case GOV_ABSOLUTE_MONARCHY:       g.gov_window_type = GOV_WIN_ROYAL_COURT;     break;
                        case GOV_ECCLESIARCHY:            g.gov_window_type = GOV_WIN_SYNOD;           break;
                        case GOV_MINARCHIST_COMPACT:      g.gov_window_type = GOV_WIN_CHARTER_COUNCIL; break;
                        default:                          g.gov_window_type = GOV_WIN_NONE;           break;
                    }
                    g.gov_window_civ  = (i16)civ;
                    g.show_gov_window = true;
                }
            }
        }
    }
    bs_ui_end_window();
    g.show_inspector = open ? true : false;
}

// ---- Government interaction windows (Phase: player <-> polity) --------------------------------
// Each government exposes a themed window with flavour-only buttons for now; clicking pushes a
// descriptive Action Log line. All windows target the civ that owns the player's current system.

// Shared header for a government window: civ banner name + government/ethos line.
static void gov_window_header(GalaxyState& g, i32 civ) {
    const Civilization& cv = g.civs[civ];
    bs_ui_text_colored(cv.color.r, cv.color.g, cv.color.b, 1.0f, cv.name);
    char line[128];
    snprintf(line, sizeof(line), "%s - %s", civ_government_name(cv.government), civ_ethos_name(cv.ethos));
    bs_ui_text(line);
    bs_ui_separator();
}

// One themed action button: on click, log "<verb> <civ name>." to the Action Log.
static void gov_action_button(game_state* s, i32 civ, const char* label, const char* verb) {
    if (bs_ui_button(label, TRUE))
        action_log_push(s, "%s %s.", verb, s->galaxy.civs[civ].name);
}

void galaxy_history_build_parliament(game_state* s, i32 civ) {
    GalaxyState& g = s->galaxy;
    b8 open = TRUE;
    if (bs_ui_begin_window("PARLIAMENT", &open)) {
        bs_ui_text_colored(0.60f, 0.85f, 0.95f, 1.0f, "PARLIAMENT");
        gov_window_header(g, civ);
        gov_action_button(s, civ, "Acquire Seat",    "Acquired a parliamentary seat in the");
        gov_action_button(s, civ, "Propose Policy",  "Proposed a policy to the");
        gov_action_button(s, civ, "Lobby Delegates", "Lobbied the delegates of the");
        gov_action_button(s, civ, "Call Vote",       "Called a vote in the");
    }
    bs_ui_end_window();
    if (!open) g.show_gov_window = false;
}

void galaxy_history_build_royal_court(game_state* s, i32 civ) {
    GalaxyState& g = s->galaxy;
    b8 open = TRUE;
    if (bs_ui_begin_window("ROYAL COURT", &open)) {
        bs_ui_text_colored(0.95f, 0.80f, 0.45f, 1.0f, "ROYAL COURT");
        gov_window_header(g, civ);
        gov_action_button(s, civ, "Petition the Throne", "Petitioned the throne of the");
        gov_action_button(s, civ, "Present Tribute",     "Presented tribute to the");
        gov_action_button(s, civ, "Seek Audience",       "Sought an audience with the");
        gov_action_button(s, civ, "Pledge Fealty",       "Pledged fealty to the");
    }
    bs_ui_end_window();
    if (!open) g.show_gov_window = false;
}

void galaxy_history_build_synod(game_state* s, i32 civ) {
    GalaxyState& g = s->galaxy;
    b8 open = TRUE;
    if (bs_ui_begin_window("SACRED SYNOD", &open)) {
        bs_ui_text_colored(0.85f, 0.70f, 0.95f, 1.0f, "SACRED SYNOD");
        gov_window_header(g, civ);
        gov_action_button(s, civ, "Attend Conclave",  "Attended the conclave of the");
        gov_action_button(s, civ, "Propose Doctrine", "Proposed doctrine to the");
        gov_action_button(s, civ, "Fund Mission",     "Funded a mission for the");
        gov_action_button(s, civ, "Request Blessing", "Requested a blessing from the");
    }
    bs_ui_end_window();
    if (!open) g.show_gov_window = false;
}

void galaxy_history_build_charter_council(game_state* s, i32 civ) {
    GalaxyState& g = s->galaxy;
    b8 open = TRUE;
    if (bs_ui_begin_window("CHARTER COUNCIL", &open)) {
        bs_ui_text_colored(0.60f, 0.90f, 0.70f, 1.0f, "CHARTER COUNCIL");
        gov_window_header(g, civ);
        gov_action_button(s, civ, "Sign Contract",  "Signed a contract with the");
        gov_action_button(s, civ, "Propose Compact", "Proposed a compact to the");
        gov_action_button(s, civ, "Join Militia",    "Joined the militia of the");
        gov_action_button(s, civ, "File Grievance",  "Filed a grievance with the");
    }
    bs_ui_end_window();
    if (!open) g.show_gov_window = false;
}

// Dispatcher: route to the window matching the targeted civ's government. Auto-closes if the target
// is no longer the civ that owns the player's current system, or if that civ has fallen.
void galaxy_history_build_gov_interaction(game_state* s) {
    GalaxyState& g = s->galaxy;
    if (!g.show_gov_window) return;
    i32 civ = (i32)g.gov_window_civ;
    if (civ < 0 || civ >= g.civ_count || civ != g.current_owner_civ || g.civs[civ].status != 0) {
        g.show_gov_window = false; g.gov_window_civ = -1; g.gov_window_type = GOV_WIN_NONE;
        return;
    }
    switch (g.gov_window_type) {
        case GOV_WIN_PARLIAMENT:      galaxy_history_build_parliament(s, civ);      break;
        case GOV_WIN_ROYAL_COURT:     galaxy_history_build_royal_court(s, civ);     break;
        case GOV_WIN_SYNOD:           galaxy_history_build_synod(s, civ);           break;
        case GOV_WIN_CHARTER_COUNCIL: galaxy_history_build_charter_council(s, civ); break;
        default:                      g.show_gov_window = false;                    break;
    }
}

i32 galaxy_history_civ_at_node(const game_state* s, i32 node_index) {
    const GalaxyState& g = s->galaxy;
    for (i32 c = 0; c < g.civ_count; ++c)
        if (g.civs[c].origin_node == node_index) return c;
    return -1;
}

const char* civ_government_name(u8 government) {
    switch (government) {
        case GOV_REPRESENTATIVE_REPUBLIC: return "Representative Republic";
        case GOV_ABSOLUTE_MONARCHY:       return "Absolute Monarchy";
        case GOV_ECCLESIARCHY:            return "Theocracy";
        case GOV_MINARCHIST_COMPACT:      return "Minarchist Frontier Compact";
        default:                          return "?";
    }
}

// Themed player-interaction window title for a government (Live Civ Inspector -> Open <Window>).
const char* civ_gov_window_name(u8 government) {
    switch (government) {
        case GOV_REPRESENTATIVE_REPUBLIC: return "Parliament";
        case GOV_ABSOLUTE_MONARCHY:       return "Royal Court";
        case GOV_ECCLESIARCHY:            return "Sacred Synod";
        case GOV_MINARCHIST_COMPACT:      return "Charter Council";
        default:                          return "Assembly";
    }
}
const char* civ_ethos_name(u8 ethos) {
    switch (ethos) {
        case ETHOS_MILITANT:   return "Militant";   case ETHOS_MERCANTILE:  return "Mercantile";
        case ETHOS_SPIRITUAL:  return "Spiritual";  case ETHOS_SCIENTIFIC:  return "Scientific";
        case ETHOS_XENOPHOBE:  return "Xenophobe";  case ETHOS_HARMONIOUS:  return "Harmonious";
        default:               return "?";
    }
}
