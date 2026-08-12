#pragma once



#include <defines.h>



#include <game_types.h>



#include <renderer/renderer_types.h>



#include "sim/ship.h"

#include "sim/module.h"

#include "sim/weapon_def.h"



#include "sim/projectile.h"



#include "render/star_fx.h"



#include "render/global_background.h"



#include "sim/voronoi_galaxy.h"



#include "sim/galaxy_spatial.h"



#include "sim/galaxy_params.h"



#include "sim/travel.h"



#include "sim/fleet.h"



#include "sim/rts_controls.h"



#include "render/out_sensor_detection_fx.h"



#include "core/profiler.h"



#include <containers/vector.h>



// =====================================================================================



// Black Stride prototype: art-texture ships with two view modes.



//



//   mode::global — zoomed out. Ship sprites and roof silhouettes visible.



//                  WASD flies the whole ship (Starsector-style inertial).



//   mode::system — galaxy map. The player navigates between star systems.



//



// Controls:



//   Global mode (ship flight, Starsector-style inertial):



//     W / S      — thrust forward / reverse along heading (accel / decel to max speed).



//     C          — brake: decelerate current velocity to zero.



//     Q / E      — strafe left / right.



//     A / D      — turn left / right (turn-accel ramps to max turn rate; auto-stabilizes).



//   M            — toggle between global and system mode.



//   Esc          — quit (handled by the engine).



// =====================================================================================



enum GameMode {



    MODE_GLOBAL = 0,



    MODE_SYSTEM



};



// App lifecycle phase: the game boots into the New Game SETUP screen; on "Generate" it runs the

// staged GENERATING pipeline (one heavy step per frame with a progress bar); then PLAYING. Zero-

// initialised game_state => APP_SETUP by default.

enum AppPhase {



    APP_SETUP = 0,   // New Game setup screen (choose galaxy + history parameters)



    APP_GENERATING,  // staged deterministic generation with a progress bar



    APP_PLAYING      // normal gameplay



};



// Player-chosen initial conditions for the galaxy + its simulated history. Filled by the setup

// screen, then consumed by the generators. All fields deterministic inputs (see galaxy_history).

struct GalaxySetupParams {



    u64 seed;                 // master seed (Randomize / manual)



    i32 galaxy_size;          // number of star systems (clamped to GALAXY_TARGET_SYSTEMS)



    i32 history_depth_years;  // years of simulated past before "present" (headline compute knob)



    u8  chronicle_detail;     // 0 broad .. 2 deep (event verbosity)



    u8  abundance;            // 0 rare .. 3 teeming (how common life-bearing cradle worlds are)



    u8  civ_density;          // 0 few .. 2 many (target concurrent civilizations)



    u8  conflict;             // 0 peaceful .. 3 cataclysmic (war/collapse propensity)



    u8  ambition;             // 0 insular .. 2 expansive (empire reach + size)



    u8  cataclysm;            // 0 none .. 2 common (galaxy-scale extinctions)



    u8  starting_era;         // 0 golden .. 2 dark-age flavour (reserved)



    u8  galaxy_shape;         // GalaxyShape: 0 spiral .. 5 flocculent (morphology of the disc)



};



struct CelestialBody {



    bs_math::Vec2 position;



    f32           radius;



    bs_color      color;



    // Legacy fields (kept for backward compat, orbit_radius = semi_major_axis)



    f32           orbit_radius; // distance from parent body



    f32           orbit_speed;  // radians per second



    f32           orbit_angle;  // current angle (radians)



    // Orbital elements (constant after generation)



    f32           semi_major_axis;  // a



    f32           eccentricity;     // e (0 = circular)



    f32           arg_periapsis;    // orientation of ellipse major axis (radians)



    f32           mean_anomaly_0;   // initial phase (radians)



    f32           orbital_period;   // seconds per full orbit



};



// =====================================================================================

// Procedural world-generation property model (physical star & planet attributes).

// Deterministic, seed-derived: a star's spectral class fixes mass/temperature/luminosity/radius;

// luminosity fixes the habitable zone + frost line; those thresholds plus a planet's orbital

// distance determine the planet's TYPE and physical properties. Generation lives in

// ss_generation.cpp (worldgen_star / worldgen_planet); results are stored on StarSystem below.

// =====================================================================================



// Main-sequence spectral classes, hottest/most-massive first (Morgan-Keenan, minus L/T dwarfs).

enum SpectralClass : u8 { SPEC_O, SPEC_B, SPEC_A, SPEC_F, SPEC_G, SPEC_K, SPEC_M, SPEC_COUNT };



struct StarProperties {



    SpectralClass spectral_class;



    f32 mass_solar;         // stellar masses



    f32 temperature_k;      // surface temperature (Kelvin)



    f32 luminosity_solar;   // solar luminosities (drives HZ / frost line / planet temps)



    f32 radius_solar;       // solar radii



    f32 age_gyr;            // age in billions of years (bounded by main-sequence lifetime)



    f32 metallicity;        // relative to solar (~0.2..2.5); biases planet count + giant frequency



    f32 hz_inner_au;        // habitable zone inner edge (AU)



    f32 hz_outer_au;        // habitable zone outer edge (AU)



    f32 frost_line_au;      // snow line: volatiles condense beyond this (AU)



};



// Planet archetypes, ordered roughly inner (hot) -> outer (cold).

enum PlanetType : u8 {



    PLANET_LAVA,        // molten surface, inside the hot edge



    PLANET_ROCKY,       // bare rock / airless



    PLANET_DESERT,      // dry terrestrial



    PLANET_OCEAN,       // water world in the habitable zone



    PLANET_TERRAN,      // Earth-like in the habitable zone



    PLANET_GAS_GIANT,   // Jovian, beyond the frost line



    PLANET_ICE_GIANT,   // Neptunian



    PLANET_FROZEN,      // small ice world, far out



    PLANET_TYPE_COUNT



};



// Derived descriptor tags for a planet's genome (surfaced in UI tooltips). A genome may carry

// several at once; decoded to short labels by planet_trait_name().

enum PlanetTrait : u16 {

    TRAIT_CRATERED   = 1u << 0,   // heavy impact cratering

    TRAIT_VOLCANIC   = 1u << 1,   // active/emissive volcanism

    TRAIT_BANDED     = 1u << 2,   // strong latitudinal bands

    TRAIT_STORMY     = 1u << 3,   // prominent storms

    TRAIT_ICY_CAPS   = 1u << 4,   // large polar ice caps

    TRAIT_OCEANIC    = 1u << 5,   // extensive liquid seas

    TRAIT_CLOUDY     = 1u << 6,   // thick cloud cover

    TRAIT_ARID       = 1u << 7,   // dry / dusty

    TRAIT_METALLIC   = 1u << 8,   // metal-rich crust

    TRAIT_VERDANT    = 1u << 9,   // living / vegetated

    TRAIT_EXOTIC     = 1u << 10,  // carries a rare anomaly mutation

};



// Per-planet appearance "genome": deterministic from the planet seed + physical properties.

// Produces a named subtype, a 4-stop palette, and tuned surface-feature genes so that every

// planet of a given type looks unique. Rare `anomaly` mutations override parts of it. The genome

// is regenerated lazily with the system (never serialized) and is the single source of a planet's

// visual character — read by the surface shader, the UI, and (later) civilization/history systems.

struct PlanetGenome {



    u8       subtype;         // index into the per-type subtype table (name + palette base)



    u8       anomaly;         // 0 = none; else rare-mutation id (exotic palette / structural)



    u16      trait_bits;      // OR of PlanetTrait flags (UI descriptors)



    bs_color pal_deep;        // 4-stop surface palette, jittered per planet: shadow / base



    bs_color pal_mid;         // dominant mid tone



    bs_color pal_light;       // highlight tone



    bs_color pal_accent;      // accent / emissive (lava glow, storm streaks, cloud tint anchor)



    f32      noise_freq;      // surface detail frequency multiplier (~0.6..1.8)



    f32      warp_amount;     // domain-warp strength (feature fluidity)



    f32      feature_density; // craters / storms / continents amount (0..1)



    f32      band_detail;     // gas/ice band count + sharpness (0..1)



    f32      cap_extent;      // polar ice-cap size (0..1)



    f32      roughness;       // terrain / tonal contrast (0..1)



    f32      cloud_cover;     // cloud coverage (0..1)



    bs_color cloud_tint;      // cloud colour



    bs_color atmo_tint;       // atmosphere / limb-haze colour



};



struct PlanetProperties {



    PlanetType type;



    f32 orbit_au;           // semi-major axis in AU (physical, not render units)



    f32 mass_earth;         // Earth masses



    f32 radius_earth;       // Earth radii



    f32 temperature_k;      // equilibrium temperature (Kelvin)



    f32 habitability;       // 0..1 heuristic (liquid-water potential)



    f32 water_frac;         // evolved surface water coverage 0..1



    f32 life;               // evolved biosphere accumulation 0..1



    b8  has_atmosphere;



    b8  has_rings;          // gas/ice giants may sport a ring system



    PlanetGenome genome;    // deterministic appearance genome (subtype/palette/features/anomaly)



};



// =====================================================================================
// Evolved body model (epoch-based planetary evolution; sim/system_evolution.cpp).
// The AUTHORITATIVE physical model of a star system: every celestial body (star, planets,
// moons, asteroid belts) with its bulk composition and evolved geophysical state, produced
// by the four-phase evolution pipeline (disk condensation -> accretion & migration ->
// geophysical/atmospheric epochs -> present-day synthesis). PlanetProperties/CelestialBody
// above are DERIVED render/UI views of this model. Deterministic from the node seed; never
// serialized (regenerated lazily with the system, same philosophy as the genome).
// =====================================================================================

#define MAX_SYSTEM_PLANETS 6   // planets per system (was 5 pre-evolution)
#define MAX_SYSTEM_MOONS   8   // moons per system (across all planets)
#define MAX_SYSTEM_BELTS   2   // asteroid belts per system
// bodies[] capacity: [0]=star, then planets, then moons, then belts (stable planet indices).
#define MAX_SYSTEM_BODIES  (1 + MAX_SYSTEM_PLANETS + MAX_SYSTEM_MOONS + MAX_SYSTEM_BELTS)
#define EVO_MAX_EVENTS     24  // evolution chronicle entries kept per system

enum BodyKind : u8 { BODY_STAR, BODY_PLANET, BODY_MOON, BODY_BELT };

// Bulk composition mass fractions (sum ~= 1). Set by disk condensation, mixed by mergers.
struct BodyComposition { f32 metal; f32 silicate; f32 ice; f32 gas; };

struct EvolvedBody {
    BodyKind kind;
    i8  parent;          // bodies[] index of the parent (0 = star; planet index for moons); -1 for the star
    PlanetType type;     // final classification (render contract; unused for star/belt)
    f32 orbit_au;        // semi-major axis around the parent (AU); belts: annulus centre
    f32 width_au;        // belts only: annulus half-width (AU)
    f32 eccentricity;
    f32 mass_earth;      // Earth masses (star: solar masses * 333000)
    f32 radius_earth;    // Earth radii
    f32 temperature_k;   // greenhouse-adjusted surface/equilibrium temperature
    BodyComposition comp;
    // Evolved geophysical state (phase 3 outputs), all 0..1 unless noted:
    f32 water_frac;      // surface water/volatile coverage
    f32 atmo_pressure;   // atmospheres (not 0..1); 0 = airless
    f32 magnetic_field;  // dynamo strength
    f32 tectonics;       // current tectonic activity
    f32 volcanism;       // current volcanism (incl. tidal heating on close moons)
    f32 life;            // biosphere development
    // Present-day synthesis (phase 4 outputs), all 0..1:
    f32 habitability;
    f32 env_hazard;      // environmental hazard (radiation/impacts/volcanism/atmosphere)
    f32 res_metal;       // extractable ore richness (gameplay data; market untouched)
    f32 res_volatiles;   // volatiles/fuel richness
};

// One chronicle entry: something that happened during the system's evolution.
enum EvoEventKind : u8 {
    EVO_EV_NONE,
    EVO_EV_DISK_DISPERSED,  // gas disk photo-evaporated (end of gas accretion)
    EVO_EV_GIANT_FORMED,    // runaway gas accretion onto a >~8 Mearth core
    EVO_EV_MIGRATED,        // giant migrated inward (type II)
    EVO_EV_MERGER,          // two protoplanets collided and merged
    EVO_EV_EJECTED,         // the lighter body of an unstable pair was ejected
    EVO_EV_BELT_FORMED,     // accretion-frustrated core ground down into an asteroid belt
    EVO_EV_MOON_IMPACT,     // giant impact spun off a moon
    EVO_EV_MOON_CAPTURED,   // gas giant captured a moon
    EVO_EV_ATMO_STRIPPED,   // XUV/flares stripped an atmosphere
    EVO_EV_WATER_DELIVERED, // bombardment delivered water to an inner body
    EVO_EV_LIFE_EMERGED,    // biosphere crossed the emergence threshold
};

struct EvolutionEvent { u8 epoch; u8 kind; i8 body; i8 other; }; // body/other = bodies[] index (-1 = n/a, -2 = a protoplanet that did not survive)

struct EvolvedSystem {
    EvolvedBody    bodies[MAX_SYSTEM_BODIES]; // [0]=star, [1..planet_count]=planets sorted by a, then moons, then belts
    i32            body_count;
    i32            planet_count;              // number of BODY_PLANET entries (bodies[1..planet_count])
    i32            moon_count;
    i32            belt_count;
    EvolutionEvent events[EVO_MAX_EVENTS];
    i32            event_count;
};

// A civilian orbital installation belonging to a system's controlling civilization. Placed

// statically around the star at generation (materialize_slot); discovered when the player closes

// within the flagship's Layer 1 sensor radius. Lives with the StarSystem.

struct SystemStation {



    bs_math::HierPos2 pos;        // absolute galaxy position



    f32               radius;     // world units (drawn as a filled disc once discovered)



    f32               pulse_phase; // per-station phase offset for the undiscovered-marker pulse



    i16               owner_civ;  // controlling civilization index (galaxy.civs[])



    b8                discovered; // TRUE once scanned within Layer 1



    i32               station_id; // galaxy-unique id: (node_index << 8) | station_index



    b8                mission_hub; // TRUE for the few stations per system that issue trade contracts



};



#define SYSTEM_STATION_MAX 24



// How many of a system's stations act as trade-contract issuers ("mission hangers"). The first

// N stations of the deterministic layout are the hubs, so macro seeding (which never materialises

// the StarSystem) and the materialised stations always agree on which ones issue contracts.

#define MISSION_HUBS_PER_SYSTEM 5



// ---- Resource catalog: tradeable resources + room-based station architecture ----------------

// Stations are stateless: their room list and baseline market derive deterministically from the

// station id (see sim/station_market.*). The ONLY mutable market state is the bounded delta pool

// on GalaxyState below, which records how trader docks perturbed stock away from baseline.

// Every market stocks the full catalog; per-station supply bias derives from the node's

// abundance signals (habitability, biosphere, ore/volatile richness, civ industry) plus a

// deterministic per-station category specialization (see station_specialization).

enum TradeCategory : u8 {
    CAT_AGRICULTURE = 0,  // farmed/grown goods (habitability + biosphere driven)
    CAT_MINERALS,         // raw extraction (evolved-body ore richness driven)
    CAT_VOLATILES,        // ices/fuels (evolved-body volatile richness driven)
    CAT_INDUSTRIAL,       // manufactured goods (needs a living civilization's industry)
    CAT_COUNT
};

enum TradeGood : u8 {
    GOOD_GRAIN = 0,       // AGRICULTURE: staple crops
    GOOD_ORGANICS,        // AGRICULTURE: biomass/livestock products
    GOOD_IRON_ORE,        // MINERALS: bulk structural ore
    GOOD_RARE_METALS,     // MINERALS: rare/precious extraction
    GOOD_WATER_ICE,       // VOLATILES: water/ice
    GOOD_HYDROGEN_FUEL,   // VOLATILES: refined drive fuel
    GOOD_ALLOYS,          // INDUSTRIAL: refined structural alloys
    GOOD_ELECTRONICS,     // INDUSTRIAL: high-tech components
    GOOD_MEDICINE,        // INDUSTRIAL: pharma/medical supplies
    GOOD_LUXURIES,        // INDUSTRIAL: luxury consumer goods
    GOOD_COUNT
};



enum StationRoomKind : u8 {

    ROOM_DOCK = 0,   // every station: where ships physically dock

    ROOM_MARKET,     // every station: the baseline goods market (MarketRoom)

    ROOM_CONTRACTS   // mission-hub stations only: issues trade contracts (mission hangers)

};



// One perturbed station market: stock offsets from the deterministic baseline, decaying back to

// zero over in-game hours (station_market_decay). station_id < 0 = free pool slot.

struct StationMarketDelta {

    i32 station_id;

    f32 stock_delta[GOOD_COUNT];

};

#define STATION_MARKET_DELTA_MAX 256



// Cumulative trade revenue earned by a station from contract activity. No decay —

// revenue is permanent. station_id < 0 = free pool slot.

struct StationRevenue {

    i32 station_id;

    f32 total_credits;

};

#define STATION_REVENUE_MAX 256



// Phase 6: per-node danger rating built from observed raider activity (pirate sorties launched at
// a node, ambushes resolved there). Bounded pool: node < 0 = free slot, weakest entry is evicted
// when the table is full, and every entry decays toward 0 so old scares are forgotten.

struct NodeRisk {

    i32 node;

    f32 risk;

};

#define NODE_RISK_MAX 128



#define ASTEROID_MAX_VERTS  12

#define SYSTEM_ASTEROID_MAX 4096



// A natural asteroid belonging to a system. Placed deterministically in the system's orbital

// zones at generation (materialize_slot) for EVERY system (asteroids are natural, not civ-gated).

// Lives with the StarSystem. Indestructible: miners work them indefinitely.

struct SystemAsteroid {



    bs_math::HierPos2 pos;        // absolute galaxy position



    f32               radius;     // world units



    f32               rotation;   // base tumble angle (radians); animated by spin at draw time



    f32               spin;       // radians/second



    bs_color          color;



    i32               verts;      // silhouette vertex count (7..ASTEROID_MAX_VERTS)



    f32               vert_jitter[ASTEROID_MAX_VERTS]; // deterministic radial jitter 0.6..1.0



};



#define SYSTEM_RESOURCE_MAX 256



// A resource node belonging to a system. Concentrated in the belt/mid orbital zones at generation

// (materialize_slot) for EVERY system. Drawn as a small ring (see draw_system_resources).

// Lives with the StarSystem.

struct SystemResource {



    bs_math::HierPos2 pos;        // absolute galaxy position



    f32               radius;     // world units



    bs_color          color;



};



#define SYSTEM_DECORATION_MAX 512



// A faint ambient dust mote / debris speck scattered across all orbital zones of a system at

// generation (materialize_slot). Purely cosmetic (a tiny quad tinted by the local star; see

// draw_system_decorations). Lives with the StarSystem.

struct SystemDecoration {



    bs_math::HierPos2 pos;        // absolute galaxy position



    f32               radius;     // world units



    bs_color          color;



};



struct StarSystem {



    bs_math::HierPos2 galaxy_center; // absolute galaxy position of this system's star



    CelestialBody     star;



    CelestialBody     planets[MAX_SYSTEM_PLANETS];  // derived render view of planet bodies



    i32               planet_count; // 2–MAX_SYSTEM_PLANETS



    StarProperties    star_props;         // physical star attributes (drives planet generation)



    PlanetProperties  planet_props[MAX_SYSTEM_PLANETS]; // physical planet attributes (parallel to planets[])



    CelestialBody     moons[MAX_SYSTEM_MOONS]; // derived render view of moon bodies (parallel to evo moon order)



    PlanetProperties  moon_props[MAX_SYSTEM_MOONS]; // physical moon attributes (parallel to moons[])



    i32               moon_count;         // number of valid entries in moons[]



    EvolvedSystem     evo;                // authoritative evolved-body model (source of the views above)



    f32               system_scale; // per-system distance compression



    const char*       name;          // display label above the star on the map



    f32               star_pulse_phase;   // random phase offset for core animation



    f32               corona_pulse_phase; // random phase offset for corona animation



    f32               halo_pulse_phase;   // random phase offset for halo animation



    bs_glow_params    glow[3];            // per-star glow: [0]=core, [1]=corona, [2]=halo



    SystemStation     stations[SYSTEM_STATION_MAX]; // civilian installations (owned systems only)



    i32               station_count;      // number of valid entries in stations[]



    SystemAsteroid    asteroids[SYSTEM_ASTEROID_MAX]; // natural asteroids (every system)



    i32               asteroid_count;     // number of valid entries in asteroids[]



    SystemResource    resources[SYSTEM_RESOURCE_MAX]; // resource nodes (belt/mid zones; every system)



    i32               resource_count;     // number of valid entries in resources[]



    SystemDecoration  decorations[SYSTEM_DECORATION_MAX]; // ambient dust (all zones; every system)



    i32               decoration_count;   // number of valid entries in decorations[]



};



// A lightweight galaxy-scale record for ONE star system. The full galaxy holds ~10,000 of

// these (a small heap array); each is a deterministic summary derived from `seed`. Full

// StarSystem contents (planets, orbit phases, glow) are materialised lazily near the camera

// via generate_star_system(seed). Everything needed to draw a map dot, label a system, light

// the current system, or classify orbital zones lives here so those paths never materialise.

struct GalaxyNode {



    bs_math::HierPos2 galaxy_center; // absolute galaxy position of the system's star



    u64          seed;               // per-system seed -> deterministic full materialisation



    bs_color     star_color;         // summary star colour (map dot + lighting)



    f32          star_radius;        // summary star radius (dot size / lighting hint)



    f32          orbit_radii[MAX_SYSTEM_PLANETS]; // sorted ascending semi-major axes (orbital zone classification)



    i32          orbit_count;        // number of valid orbit radii



    u8           best_habitability;  // max planet habitability (0..255) — civ-cradle substrate



    u8           habitable_count;    // planets with habitability > 0.4



    u8           res_metal;          // max extractable ore richness across evolved bodies (0..255)

    u8           res_volatiles;      // max volatiles/fuel richness across evolved bodies (0..255)

    u8           biosphere;          // max biosphere development across evolved bodies (0..255)



    char         name[8];            // unique catalogue designation, e.g. "N327B" ("Sol" = home)



};



// Connectivity graph over the galaxy nodes: a spanning tree (guarantees every system is

// reachable) plus a fraction of extra short edges for loops/alternate routes. Stored as a flat

// edge list plus a CSR adjacency for O(1) neighbour iteration. Heap-allocated at generation.

struct GalaxyLaneGraph {



    i32* lane_a;         // lane_count endpoints (node index)



    i32* lane_b;         // lane_count endpoints (node index)



    i32  lane_count;



    i32* adj_start;      // (node_count + 1) CSR offsets into adj_neighbor



    i32* adj_neighbor;   // 2 * lane_count neighbour node indices



    i32  node_count;



};



// =====================================================================================

// Cross-system Ship AI travel: macro "ship mission" layer (Step 1: data model only).

//

// A ShipMission is the PERSISTENT, player-independent representation of a ship travelling the

// galaxy graph toward a simple objective. It exists whether or not the player is nearby; when the

// player is co-located with a mission it is materialised into a live NpcShip (later steps), and

// dematerialised back to this record on departure. Positions live on the lane graph: either parked

// AT a node, or in transit ALONG a lane (from_node -> to_node) with a 0..1 progress fraction.

// Step 1 defines the type + a route finder only -- nothing spawns, moves, or renders yet.

// =====================================================================================

#define MISSION_ROUTE_MAX 32   // max node hops cached in a mission's route (galaxy diameter headroom)

#define MISSION_MAX       8192 // galaxy-wide contract cap (one per mission-hub station; heap pool)



// A mission's simple objective. The field is defined now; per-objective behaviour lands in later

// steps (Step 3+). Each objective ultimately reduces to "reach dest_node, then act".

enum MissionObjective : u8 {



    OBJ_IDLE = 0,   // no goal (parked)

    OBJ_TRADE,      // route between wealthy owned systems

    OBJ_REINFORCE,  // carry fleet strength to an owned frontier node

    OBJ_PATROL,     // cycle a small ring of owned nodes

    OBJ_RAID        // pirate strike toward a rich/border node

};



// One-time trade contract lifecycle. A contract is issued by a mission-hub station; a trader

// spawns in-system and flies to the station (ai_speed_in_system), loads, flies to the system's

// jump-point (a circle of radius 2x the furthest planet orbit), jumps between systems at

// ai_speed_jump, crosses intermediate systems in-system between their entry/exit jump-points,

// docks at the destination station, then retires; the origin re-issues after a cooldown.

enum MissionStage : u8 {



    MISSION_STAGE_ORIGIN_DOCK = 0,  // loading at the issuing station (waits for the live trader when the player is present)

    MISSION_STAGE_ACQUIRE,          // spawn point -> origin station (ai_speed_in_system)

    MISSION_STAGE_TO_JUMP,          // origin station -> exit jump-point of the home system (ai_speed_in_system)

    MISSION_STAGE_JUMP,             // exit jump-point -> entry jump-point of the next node (ai_speed_jump)

    MISSION_STAGE_CROSS,            // intermediate system: entry jump-point -> exit jump-point (ai_speed_in_system)

    MISSION_STAGE_FINAL_APPROACH,   // final system: entry jump-point -> destination station (ai_speed_in_system)

    MISSION_STAGE_MARKET_DOCK,      // unloading at the destination station

    MISSION_STAGE_COOLDOWN          // contract done/failed; origin station re-issues after respawn_hours

};



struct ShipMission {



    i16  owner;       // owning civilization index (>=0) or a static faction sentinel



    u8   archetype;   // ShipArchetype governing it when materialised



    u8   objective;   // MissionObjective



    i32  dest_node;   // final destination galaxy node (route target)



    // ---- Graph position: parked at a node, or in transit along a lane ----



    i32  at_node;     // current node when parked; -1 while in transit



    i32  from_node;   // lane origin  (valid while in transit)



    i32  to_node;     // lane target  (valid while in transit)



    f32  progress;    // 0..1 fraction along the current from->to lane (legacy; kept for tooling)



    // ---- Continuous spatial movement (natural AI): authoritative position + current leg ----



    bs_math::HierPos2 pos;        // the trader's world position right now (moved every tick)



    bs_math::HierPos2 leg_target; // point the current stage is flying toward (set on stage entry)



    // ---- Cached route toward dest_node (node hops, excluding the start node) ----



    i32  route[MISSION_ROUTE_MAX]; // sequence of node indices to visit in order



    i32  route_len;   // number of valid hops in route[]



    i32  route_pos;   // index of the next hop to take in route[]



    // ---- Objective state (Step 3: OBJ_TRADE) ----



    i32  home_node;   // trader's base system; trade runs shuttle home <-> a wealth-ranked market



    f32  dwell_hours; // macro dock timer: stands in for live docking when the player is elsewhere



    u64  seed;        // deterministic per-mission rng seed



    // ---- Station-issued contract state (one contract per mission-hub station) ----



    i32  station_id;      // issuing (origin) station id: (node << 8) | station_index



    i32  dest_station_id; // delivery station id in the market system (-1 = system centre fallback)



    bs_math::HierPos2 station_pos;      // cached origin-station anchor (node centre fallback)



    bs_math::HierPos2 dest_station_pos; // cached destination-station anchor (node centre fallback)



    f32  station_radius;      // cached origin-station radius (world units; dock-range scaling)



    f32  dest_station_radius; // cached destination-station radius



    // ---- Cargo manifest (minimal economy) ----



    u8   cargo_good;      // TradeGood hauled this contract (origin's biggest surplus)



    f32  cargo_units;     // units loaded at the origin market



    f32  reward_credits;  // settled at delivery: units * (dest price - origin price)



    // ---- Return leg (bidirectional contract) ----

    u8   return_cargo_good;    // good hauled back from destination (GOOD_COUNT = none)

    f32  return_cargo_units;   // units loaded at destination (0 = no return leg)

    f32  return_reward;        // settled at issue time: units * (origin_price - dest_price)

    b8   return_leg;           // TRUE while flying the return leg

    b8   escorted;             // Phase 5: high-value contract carries a warship escort (abstract defense)

    b8   awaiting_hull;        // Phase 6: contract was destroyed -- it cannot re-issue until the owning
                               // civ's shipyard delivers a replacement hull (see Civilization::hull_pool)

    b8   raid_engaged;         // Phase 5/7: raid reached its target WHILE THE PLAYER IS THERE -- abstract
                               // resolution is suspended so the live hulls fight it out on screen

    f32  stall_hours;          // Watchdog: game-hours this mission has waited on a live agent WITHOUT
                               // MAKING PROGRESS. The macro tier hands its current leg or dock stage
                               // to a materialised hull and waits on local_ready; if that hull is
                               // pinned in combat, culled mid-handshake, or otherwise unable to
                               // finish, the mission would wait forever. Measuring progress rather
                               // than raw elapsed time matters because the two tiers move at very
                               // different speeds -- a dock approach runs at the hull's own
                               // max_speed, orders of magnitude below the macro leg speed, so a
                               // pure timeout would fire on perfectly healthy approaches.

    f32  stall_ref;            // Last measured distance to the current stage target; progress is
                               // "this got meaningfully smaller".



    i32  ship_slot;       // bound live NpcShip pool index while docked in the player's system (-1 = none)



    f32  respawn_hours;   // MISSION_STAGE_COOLDOWN countdown before the station issues a new contract



    u8   stage;           // MissionStage



    b8   local_ready;     // set by the bound trader's AI when it finishes docking at the current stage



    b8   active;      // FALSE = free pool slot



};



// ---- Galaxy history: macro-scale civilizations (Phase 1: origins only) --------------------

// A civilization is ONE aggregate agent, seeded deterministically on a habitable world; later

// phases add expansion/territory/chronicle. Types live here (like GalaxyNode/GalaxyLaneGraph) so

// GalaxyState can embed the civs[] array; the algorithms live in sim/galaxy_history.cpp.

// Minimal government taxonomy (one archetype per compendium category). Government is *who rules*;

// the separate CivEthos axis is *how they behave*. u8-backed, so up to 255 future types can be added.

enum CivGovernment : u8 {



    GOV_REPRESENTATIVE_REPUBLIC,  // Democratic / Representative

    GOV_ABSOLUTE_MONARCHY,        // Autocratic / Centralized (the only DYNASTIC government)

    GOV_ECCLESIARCHY,             // Oligarchic / Elite-Led (theocracy)

    GOV_MINARCHIST_COMPACT,       // Decentralized / Alternative

    GOV_COUNT



};



// Per-government player interaction window (launched from the Live Civ Inspector). Each government

// exposes a themed window reflecting how its polity is governed.

enum GovInteractionWindow : u8 {



    GOV_WIN_NONE, GOV_WIN_PARLIAMENT, GOV_WIN_ROYAL_COURT, GOV_WIN_SYNOD, GOV_WIN_CHARTER_COUNCIL, GOV_WIN_COUNT



};



enum CivEthos : u8 {



    ETHOS_MILITANT, ETHOS_MERCANTILE, ETHOS_SPIRITUAL, ETHOS_SCIENTIFIC, ETHOS_XENOPHOBE, ETHOS_HARMONIOUS, ETHOS_COUNT



};



struct Civilization {



    char     name[24];       // procedural civilization name, e.g. "Velar" or "Velar 2"



    i32      origin_node;    // galaxy node index of the homeworld system



    i32      homeworld_pi;   // planet index within that system (0 = resolve lazily for now)



    i32      founding_year;  // <= 0; years relative to present (0), negative = in the past



    u8       government;     // CivGovernment



    u8       ethos;          // CivEthos



    bs_color color;          // banner / emblem colour (territory tint in later phases)



    i32      territory_count; // present-day systems controlled (Phase 2)



    u8       status;         // 0 = alive, 1 = fallen (collapsed/extinct) (Phase 3)



    i32      fall_year;      // year the civ collapsed (0 if still alive)



    i32      peak_territory; // largest extent reached (for legends flavour)



    f32      power;          // logistic "strength" (drives expansion + war; Phase B)



    i16      parent_civ;     // successor lineage: civ this one fragmented from (-1 = none; Phase B)



    i16      culture_id;     // Dynastic Houses: index of this lineage's founding House (root civ). A

                             // root has culture_id == its own civ index; successors inherit the parent's.



    // ---- Phase 6: the economic loop (trade -> wealth -> power -> fleets) --------------------

    // Trade delivered inside this civ's territory credits `treasury`. The economy tick spends it:

    // part becomes `power` drift (prosperity really grows a polity), part is earmarked as

    // `mil_budget` which pays for reinforcement/patrol/raid missions and rebuilds lost hulls.

    // Strangle a civ's lanes and all three sink together.

    f32      treasury;       // unsettled trade credits earned since the last economy tick

    f32      mil_budget;     // credits earmarked for military missions + hull replacement

    f32      hull_progress;  // partial credits banked toward the next replacement hull

    i16      hull_pool;      // replacement hulls ready to re-crew a destroyed contract

};



static const i32 GALAXY_CIV_MAX = 1024;



// Dynastic House: a galaxy-wide cultural lineage, independent of government type. Every founding polity

// starts a House (whatever its government) and every successor/splinter inherits its stem, hue and

// culture id, so kingdoms, republics, theocracies and compacts all rise and fall within their lineage.

// Houses are seeded at the dawn of history and reseeded on extinction, so storage is dynamic

// (see GalaxyState.houses / house_capacity).

struct GalaxyHouse {



    char     stem[12];       // shared name root, e.g. "Velom" -> "Velom Dominion", "Velom Remnant"



    bs_color hue;            // base banner colour; each polity gets a shaded variant (a colour family)



    i16      root_civ;       // civ index of this House's founding polity (the lineage root)



};



// ---- Galaxy history: chronicle of macro events (Phase 3) ----------------------------------

// A bounded, importance-ordered log of era-scale events produced by the deterministic history

// generation; read by the Legends browser. Stored inline in GalaxyState.

enum HistoryEventType : u8 {



    EVT_FOUNDING, EVT_FIRST_CONTACT, EVT_WAR, EVT_CONQUEST, EVT_GOLDEN_AGE, EVT_COLLAPSE, EVT_CATACLYSM, EVT_PEACE, EVT_ALLIANCE, EVT_PLAYER_RAID, EVT_PLAYER_AID, EVT_TYPE_COUNT



};



struct HistoryEvent {



    i32 year;    // when (<= 0, relative to present)



    i16 civ_a;   // primary civ index (-1 = none)



    i16 civ_b;   // secondary civ index (-1 = none)



    i32 node;    // location galaxy node (-1 = none)



    u8  type;    // HistoryEventType



};



static const i32 GALAXY_EVENT_MAX = 4096;



// Live (Phase C1) "Galactic News" ring: events that occur while the player is in the galaxy, kept

// separate from the deep-time chronicle above so the backstory stays frozen.

static const i32 GALAXY_LIVE_FEED_MAX = 96;



// ---- In-game calendar (shared clock for local gameplay AND galaxy history) ----------------

// Base pace: 1 real second = 1 in-game hour at 1x. All time accrues via sim_dt (dt * time_scale),

// so pausing/accelerating scales the whole simulation — local gameplay and galaxy news alike.

static const i32 HOURS_PER_DAY  = 24;

static const i32 DAYS_PER_YEAR  = 365;

static const i32 HOURS_PER_YEAR = HOURS_PER_DAY * DAYS_PER_YEAR; // 8760



// A generic entity that appears on the galaxy map. Any world object with a Vec2 position



// can be synced here each frame, making the map system extensible without hardcoding ships.



#define MAX_MAP_ENTITIES 16



struct MapEntity {



    bs_math::HierPos2 galaxy_pos;



    bs_color          color;



    f32               radius;



    b8                has_outline; // if TRUE, draws an animated quad (reserved for player)



    const char*       name;        // display label for hover tooltip



};



// A lightweight combat entity that can be hit by projectiles.



// Ships register themselves here so the projectile system can sweep a flat array.



#define MAX_COMBAT_ENTITIES 416



#define HEAT_HISTORY_LEN 8



struct CombatEntity {



    b8            active;



    bs_math::HierPos2 position;



    bs_math::Vec2 render_pos; // TRANSIENT: render-space position, recomputed each frame



    bs_math::Vec2 velocity; // world velocity, for aim prediction / hit detection



    f32           radius;



    VesselFaction faction;



    i16           faction_id; // Feature B: unified faction (civ index / FACTION_PLAYER / FACTION_PIRATE)



    b8            is_npc;   // TRUE when this entity mirrors an NpcShip (AI agent); enables hit damage



    i32           npc_index; // index into game_state.npc_ships (valid when is_npc); else -1



    f32           hp;



    Ship*         ship;   // NULL for non-ship targets; provides visual + physics



    bs_color      tint;   // fallback colour when ship == NULL



    bs_glow_params glow;  // per-entity bloom/glow override



    f32           radiation_emission; // 0..1 heat-source strength; 0 means invisible to detector



    b8            is_drone; // TRUE for SHIP_TYPE_DRONE fleet ships; FALSE for raider / non-ship targets



    bs_math::HierPos2 heat_history[HEAT_HISTORY_LEN]; // recent world positions for the heat trail



    i32           heat_history_count;             // valid entries (0..HEAT_HISTORY_LEN)



};



// =====================================================================================

// NPC AI ship (General Ship AI). A live, moving, Ship-backed agent driven by a behavior FSM +

// steering, parameterised by a per-archetype AiProfile (see sim/ai_ship.h). Transient pool: NOT

// persisted (agents move), so kills clear and re-populate when the player returns. Reuses

// Ship + ShipFlight so rendering / weapons / collision / combat-entity hit-testing come for free.

// =====================================================================================

#define NPC_SHIP_MAX 384



struct NpcShip {



    Ship          ship;         // full hull (shares the template's visual texture handles; own weapon)



    ShipFlight    flight;       // inertial flight state (same model as fleet ships)



    i16           faction;      // owning civilization index (>=0) or a static faction sentinel



    u8            archetype;    // ShipArchetype (drives which AiProfile governs it)



    u8            state;        // AiState (behavior FSM)



    // Phase 7: contact memory so a lost target is INVESTIGATEd rather than instantly forgotten.

    bs_math::HierPos2 last_contact;   // last known position of the most recent hostile

    f32           contact_timer;  // seconds of memory left (0 = trail cold)

    i16           sense_countdown; // Phase 8: frames until this agent re-scans for targets (staggered)

    // Phase 10: combat wings. A wing is one leader plus two wingmen flying a triangle on its
    // flanks. `wing_leader` is the leader's npc_ships[] index (-1 = leader or independent);
    // `wing_slot` selects the formation offset (0 = leader/apex, 1 = rear-left, 2 = rear-right).
    // Formation is held while cruising and BROKEN while fighting, then reformed.
    i16           wing_leader;

    u8            wing_slot;

    // Identity token of the leader at the moment the wing was formed. Pool slots are RECYCLED by
    // spawn_npc, so a bare index is not a safe reference: if a leader is culled and an unrelated
    // ship is later spawned into its slot, its wingmen would silently fly formation on a stranger
    // (potentially a hostile). Validating this against the leader's spawn_seed makes the reference
    // exact and self-healing.
    u64           wing_leader_seed;



    f32           hp;           // current health



    f32           max_hp;       // health at spawn



    bs_math::HierPos2 home;     // the system centre (star) this agent belongs to / defends



    i32           home_node;    // galaxy node index of the home system (garrison identity)



    f32           orbit_radius; // this agent's personal patrol orbit radius around home (spread)



    bs_math::HierPos2 patrol_wp;// current patrol waypoint (orbit / planet loop)



    i32           target_ce;    // combat-entity index being pursued (-1 = none)



    bs_math::HierPos2 last_seen;// last-known target position (for INVESTIGATE)



    f32           timer;        // free-running timer (patrol phase / wander)



    f32           state_timer;  // time in the current FSM state



    f32           fire_cd;      // weapon cooldown remaining



    u64           rng;          // per-agent deterministic-ish rng (splitmix64 state)



    // ---- MINER work state (asteroid mining; sim/ai_ship.cpp) ----------------------------



    bs_math::HierPos2 work_pos; // target asteroid position (mining)



    f32           work_timer;   // seconds spent mining the current asteroid



    i64           work_node;      // home node whose materialized system holds the target asteroid



    i32           work_asteroid;     // index into StarSystem::asteroids[]; -1 = no target



    // ---- Civilian cargo (Phase 3: miners produce, ambient traders micro-haul) -----------



    i32           work_station;  // StarSystem::stations[] index currently targeted (-1 = none)



    f32           cargo_units;   // units in the hold (miner ore run / ambient micro-haul)



    u8            cargo_good;    // TradeGood being hauled (valid while cargo_units > 0)



    // ---- Discovery (single-ship discovery system) ---------------------------------------



    u64           spawn_seed;   // deterministic per-agent seed; keys the persistent discovery list



    b8            discovered;   // TRUE once the player closed within discovery range (renders normally)



    // ---- Cross-system Ship AI handoff (Step 4) ------------------------------------------



    i32           mission_id;   // >=0 = this agent is the live form of galaxy.missions[mission_id]; -1 = ambient/garrison



    b8            active;       // FALSE = free pool slot



};



// ShipFlight (global-mode inertial flight dynamics) is defined in fleet.h.



// =====================================================================================

// Discovery system (single-ship discovery).

//

// On first entry to a system, NPC garrison ships and per-system stations render as

// generic "unidentified" markers. When the player closes within an object's discovery radius it

// becomes discovered: rendered normally, logged, and remembered so re-visits show it identified.

// NPC discovery persists in npc_discovered[] keyed by (home_node, spawn_seed); station discovery

// persists in SystemStation::discovered (with the StarSystem cache).

// =====================================================================================

#define DISCOVERY_NPC_MAX 512

#define DISCOVERY_LOG_MAX 128



struct DiscoveredNpc { i32 home_node; u64 seed; };



enum DiscoveryKind : u8 {

    DISCOVERY_KIND_PATROL = 0,

    DISCOVERY_KIND_MINER,

    DISCOVERY_KIND_TRADER,

    DISCOVERY_KIND_WARSHIP,

    DISCOVERY_KIND_STATION,

    DISCOVERY_KIND_OTHER,

};



struct DiscoveryLogEntry {

    char name[64];      // object name, e.g. "Velom Patrol"

    char system[64];    // system / owner context

    f32  time;          // elapsed_time when discovered

    u8   kind;          // DiscoveryKind

};



// =====================================================================================



// Edit mode: click-to-select, drag-to-reposition ships and lights.



// Toggled from the EDITOR PANEL. Dragging mutates the entity's live world-space position



// (light.position or ship.origin) directly, so changes persist once edit mode is exited --



// the entity state IS the game state, no separate save step is needed.



// =====================================================================================



// What kind of entity is currently selected in edit mode.



enum EditEntityKind {



    EDIT_NONE = 0,



    EDIT_LIGHT,     // selection.index = light index into game_state::lights



    EDIT_SHIP,      // selection.index: 0 = player ship, 1 = enemy ship



};



// The editor's current selection. Always check `kind` before interpreting `index`.



struct EditSelection {



    EditEntityKind kind;



    i32            index;  // valid only when kind != EDIT_NONE



};



// How the user is currently dragging a selected entity.



enum EditDragMode : int {



    EDIT_DRAG_NONE = 0,



    EDIT_DRAG_FREE,     // unconstrained translation (existing behaviour)



    EDIT_DRAG_AXIS_X,   // translating along the X axis



    EDIT_DRAG_AXIS_Y,   // translating along the Y axis



    EDIT_DRAG_ROTATE,   // rotating via the ring



};



// Drag-in-progress state. `active` is TRUE only between mouse-down and mouse-up on a



// selected entity. Anchors capture the world-space cursor and entity positions at the



// instant the drag began, so the entity tracks the cursor without snapping.



struct EditorDrag {



    b8            active;         // TRUE while the left button is held on a selected entity



    EditDragMode  mode;           // which drag mode is active this frame



    bs_math::HierPos2 drag_anchor;    // world-space cursor position at mouse-down



    bs_math::HierPos2 entity_anchor;  // entity position at mouse-down (light.position or ship.origin)



    f32           entity_angle;   // entity angle at mouse-down (ships only, radians)



};



// Forward declaration: defined in global_background.h (included by game.cpp).



struct GlobalBackground;



// Point-defense beam recorded by the point-defense subsystem each frame for the overlay to

// draw. One beam per firing ship (single locked target). `intensity` (0..1) rises as the

// target's HP is depleted, for a brighter beam / impact flash near destruction.



#define MAX_DEFENSE_BEAMS FLEET_MAX_SHIPS



struct DefenseBeam {



    bs_math::HierPos2 origin;    // ship hardpoint (world)



    bs_math::HierPos2 target;    // current target projectile position (world)



    f32               intensity; // 0..1



};



struct game_state {



    u16 fb_width;



    u16 fb_height;



    // ===== CameraState — named sub-struct (consolidated: smooth zoom + free camera +

    // floating-origin system panning), accessed as s->camera_state.camera etc.

    struct CameraState {



    Camera2D  camera;          // persistent; zoom mutated by the wheel



    f32       target_zoom;      // wheel sets this; camera.zoom eases toward it (smooth zoom)



    f32       zoom_smooth_rate; // 1/s exponential ease rate for zoom (higher = snappier); editor slider



    // Free camera mode in MODE_GLOBAL: when TRUE, the camera is detached from the ship

    // and can be panned with WASD/middle-mouse. The ship continues to coast under physics.

    b8         free_camera_active;

    bs_math::HierPos2 free_camera_pos;

    // (Removed: global_free_camera_saved. It remembered piloting-vs-free-camera intent across a
    // galaxy-map round trip, which only existed because crossing ZOOM_MIN force-detached the
    // camera. Zoom no longer decides the control mode -- TAB and the HUD button are the only
    // things that do -- so there is no round trip to survive and nothing to remember:
    // free_camera_active alone is the state. See sim/camera_controller.cpp.)



    // Camera->ship recenter glide: smoothstep-eases free_camera_pos onto the piloted ship, then

    // hands control back to ship-follow. Driven by TAB re-pilot, the HUD pilot button, and the

    // galaxy on-screen re-entry. Advanced in game_update; ends with free_camera_active = FALSE.

    b8                recentering;

    f32               recenter_t;

    bs_math::HierPos2 recenter_from_pos;



    // Floating-origin camera for galaxy map (MODE_SYSTEM).

    // Tracks which galaxy cell we're viewing from; camera_local is the pan offset.

    bs_math::HierPos2 camera_hierpos;

    // System-view middle-mouse drag anchors (screen-space panning).

    bs_math::Vec2 system_drag_cam;    // camera position when drag started

    bs_math::Vec2 system_drag_world;  // screen pixel position when drag started

    // Tunable system-view zoom (editor-adjustable; artists pick the best scale).

    f32           system_zoom;



    } camera_state; // end CameraState



    // ===== PlanetApproachState — zoom-based feed-forward follow. Zoom toward an orbiting planet

    // and the camera is captured and follows it: it moves WITH the planet each frame (velocity

    // feed-forward -> no catch-up lag however fast the planet crosses the screen) and softly

    // corrects any residual gap to keep it centred. WASD becomes a small bounded pan. Engage/

    // release ease via `weight`. PURE CAMERA — the simulation is untouched. Driven in game_update.

    struct PlanetApproachState {

        b8       engaged;                  // TRUE while a planet is captured (weight easing in/out)

        i32      planet_index;             // captured planet within that system

        bs_math::HierPos2 system_center;   // captured system's galaxy_center (identity across frames)

        bs_math::HierPos2 planet_abs_prev; // planet's parallax-corrected abs pos last frame (feed-forward)

        f32      weight;                   // 0..1 eased capture weight

        f32      zoom_damp;                // 0..1: how much to slow zoom-IN as the planet nears framed size

        b8       candidate;                // TRUE while zooming toward a centred planet (pre-lock): gates cursor-pin

        b8       leaving;                  // TRUE once the player zooms OUT to leave: drops sticky hold + screen clamp

        f32      settle_t;                 // s: countdown for the smooth ease-to-centre after acquire (no hard snap)

    } planet_approach;



    // ===== FleetState — named sub-struct, accessed as s->fleet_state.fleet etc.

    struct FleetState {



    Fleet     fleet;           // player ships; member 0 is the flagship (loaded from assets/ship_deck.ship)



    Ship      enemy_ship;      // hostile hull (assets/enemy_ship.ship); combat prototype



    f32       enemy_orbit_phase; // hardcoded demo orbit angle around the player flagship



    char      patrol_name[48];  // Feature B: current owner's patrol label ("<Civ> Patrol" / "Pirate Raider")



    } fleet_state; // end FleetState



    // Convenience accessors for the flagship (the historical single "player ship").



    Ship&       player_ship()        { return fleet_state.fleet.flagship().ship; }

    // The ship the full-screen inspector is looking at (clamped to the live fleet; defaults to
    // the flagship). The loadout POOL stays the flagship's stash arrays regardless -- one
    // fleet-wide inventory serving whichever hull is being refitted.
    Ship&       inspected_ship() {
        i32 i = inspected_ship_idx;
        if (i < 0 || i >= fleet_state.fleet.count()) i = 0;
        return fleet_state.fleet.at(i).ship;
    }



    const Ship& player_ship()  const { return fleet_state.fleet.at(0).ship; }



    ShipFlight& player_flight()       { return fleet_state.fleet.flagship().flight; }



    // ================= RenderState (lighting, bloom, nebula, starfield, star FX, backgrounds) =================

    // Named sub-struct: members accessed as s->render.lights, s->render.glow_params, etc.

    struct RenderState {



    // ---- Editor-managed 2D point lights ------------------------------------------------------



    // The player spawns/removes/edits these from the EDITOR PANEL and drags them around the scene.



    // Submitted to the renderer each frame (game_render -> renderer_set_lights) and accumulated



    // per-pixel by the sprite shader over `light_ambient`. lights are stored in WORLD space.



    Vector(bs_light2d) lights;



    bs_color      light_ambient;     // scene-global ambient floor for lighting



    i32           light_selected;    // index of the selected/edited light (-1 = none)



    // ---- Tunable shader glow parameters (editor panel) -------------------------------------



    bs_glow_params glow_params;      // copied to GPU each frame via renderer_set_glow_params



    // ---- HDR Bloom post-process tuning -----------------------------------------------------



    b8  bloom_enabled;



    f32 bloom_threshold;



    f32 bloom_intensity;



    b8  dynamic_bloom;          // when TRUE, bloom/glow intensity scales with ship speed



    b8  star_light_enabled;       // toggle volumetric star light in system view



    b8  bg_layer0_enabled;        // toggle far starfield layer (parallax 0.05)



    b8  bg_layer1_enabled;        // toggle mid starfield layer (parallax 0.40)



    b8  bg_layer2_enabled;        // toggle mapped system layer (parallax 1.0)



    b8  bg_nebula_enabled;        // toggle nebula/dust cloud layer (parallax 0.02)



    // Editor toggles for the placeholder celestial content (galaxy_map_render + arena layer).

    b8  celestial_draw_planets;     // planets + orbit rings (default ON)



    b8  celestial_draw_testsprites; // volumetric-light demo dots orbiting the current star (default OFF)



    // Depth-based parallax for the celestial backdrop (stars/planets/orbit lines/test sprites).

    // Depth 0 = foreground (moves 1:1 with camera, like gameplay entities); depth 1 = fully locked

    // to the shared anchor on screen. Every system is parallaxed against ONE shared anchor (the

    // system under the camera) so their relative layout stays rigid at any depth, and the effect

    // fades to zero below bg_parallax_fade_zoom so ships stay glued to their systems on the map.

    // The same factors are used by the Map and Arena renderers, so ratios match with no cross-fade

    // seam. Co-located pairs share depth to avoid separation: planets sit on their orbit rings

    // (depth_planet == depth_orbit), test sprites orbit the star (depth_testsprite == depth_star).

    b8  bg_parallax_enabled;      // master toggle for celestial depth parallax



    f32 depth_star;               // system star (and test sprites)



    f32 depth_planet;             // planets



    f32 depth_orbit;              // orbit lines



    f32 depth_testsprite;         // test sprites (volumetric-light demo dots)



    // Zoom at/below which celestial parallax has fully faded to zero (stars sit at true positions

    // so ships stay glued to their systems on the map). Parallax ramps back to full over a fixed

    // smoothstep band above this value. Editable in the editor panel.

    f32 bg_parallax_fade_zoom;



    // Procedural nebula/dust tunables (editor panel).



    f32 nebula_intensity;          // global nebula opacity multiplier (default 0.5)



    f32 nebula_dust_intensity;     // dust cloud opacity multiplier (default 0.4)



    bs_color nebula_gas_color_a;   // deep base gas color



    bs_color nebula_gas_color_b;   // mid gas color



    bs_color nebula_gas_color_c;   // bright highlight core color



    bs_color nebula_dust_color;    // dark dust silhouette color



    f32 nebula_gas_brightness_mul; // extra luminance multiplier for gas (default 1.0)



    f32 nebula_highlight_power;    // how much the bright core is emphasized (default 1.0)



    f32 nebula_palette_shift;      // rotates the cosine palette index (default 0.0)



    f32 nebula_swirl_strength;     // domain rotation strength (default 1.0)



    f32 nebula_falloff_radius;     // controls radial/band fade (default 2.0)



    f32 nebula_band_strength;      // how much Milky Way band falloff is applied (default 0.5)



    f32 nebula_lod_target;         // world units per noise unit at zoom==1 (LOD feature scale, default 3500)



    f32 nebula_parallax;           // scroll parallax 0..1 (1 == world-locked, <1 == distant/slower, default 0.08)



    // Galaxy-wide "biome" variation: a low-frequency macro field modulates nebula properties across

    // the galaxy so different regions look distinct. biome_strength==0 reproduces the uniform look.



    f32 nebula_biome_strength;     // 0..1 how strongly biomes recolor/reshape the nebula (default 0.6)



    f32 nebula_biome_scale;        // world units per biome noise unit (region size, default 250000)



    f32 nebula_biome_hue_spread;   // 0..1 how far the color family rotates between biomes (default 0.5)



    f32 nebula_zoom_detail;        // 0..1 extra fine filament detail emerging as you zoom in (default 0.6)



    f32 nebula_zoom_saturation;    // 0..1 extra saturation/contrast as you zoom in (default 0.5)



    // Procedural starfield tunables (editor panel).



    f32 starfield_lod_density;     // cell fill rate 0..1 (default 0.06)



    f32 starfield_lod_size;        // star size multiplier (default 1.0)



    f32 starfield_lod_brightness;  // overall brightness multiplier (default 1.0)



    // Virtual-quadtree LOD controls (feed the starfield shader's level selection).



    f32 starfield_lod_target_px;   // desired on-screen cell size in px for LOD peak (default 40)



    f32 starfield_lod_levels;      // number of LOD slots accumulated per pixel (default 4)



    f32 starfield_lod_factor;      // cell-size ratio between LOD levels (quadtree = 4, default 4)



    f32 starfield_parallax_near;   // finest-level scroll parallax 0..1 (1 == world-locked, default 0.5)



    f32 starfield_parallax_falloff;// per-level slowdown ratio; coarser levels move slower (default 0.75)



    // Star dazzle effect: fade starfield near the bright central star.

    // NOTE: star_pos lives in the per-frame RenderContext block below (transient render scratch).



    f32 star_dazzle_inner_radius;  // world units: fully suppressed inside this



    f32 star_dazzle_outer_radius;  // world units: no suppression outside this



    f32 star_dazzle_intensity;     // 0..1 suppression strength



    f32 star_light_intensity_mul; // multiplier on star light intensity (default 1.0)



    f32 star_light_radius_mul;    // multiplier on star light radius (default 1.0)



    bs_texture exhaust_texture;   // soft radial gradient for engine exhaust (runtime-generated)



    bs_texture emblem_drone;      // combat-mode ship type emblem (loaded from PNG)



    bs_texture emblem_extractor;  // combat-mode ship type emblem (loaded from PNG)



    bs_glow_params exhaust_glow;  // per-entity glow for engine exhaust



    // Per-VISUAL-FAMILY projectile glow, indexed by VfxFamily (sim/weapon_def.h):
    // [VFX_SHELL] inert kinetic tracer, [VFX_SLUG] rail-driven, [VFX_ORDNANCE] rocket exhaust.
    //
    // These carry the shader's colour-TEMPERATURE ramp (temp_cool -> temp_warm -> temp_hot along
    // the sprite's length) plus the additive glow tint and the heat-distortion amplitude, so a
    // family's identity is as much in here as it is in the geometry projectile_fx.cpp draws.
    // Splitting them only became worth doing once the scene went HDR: on the old 8-bit targets a
    // saturated tint clipped to white in the composite, so three differently-coloured families
    // would have converged on the same white anyway.
    //
    // Three separate structs, not one shared: the backend breaks draw runs on glow_override
    // POINTER IDENTITY, so this is at most three runs for the projectile pass rather than one.
    // That is the deliberate cost of the feature and it is small.
    bs_glow_params projectile_glow[3];



    StarFxSystem  star_fx;            // owns textures + draw logic for star visuals



    GlobalBackground global_background; // parallax background layers for MODE_GLOBAL



    } render; // ================= end RenderState =================



    // ===== EditorState — named sub-struct, accessed as s->editor.edit_mode_active etc.

    struct EditorState {



    // ---- Edit mode: click to select, drag to reposition ships and lights -------------------



    b8            edit_mode_active;  // toggled from the EDITOR PANEL



    b8            multi_ship_enabled; // EDITOR PANEL: FALSE = command only the flagship (default);

                                      // TRUE = reveal + command the escort wing (multi-ship RTS)



    b8            draw_discovery_sensor_range; // EDITOR PANEL: TRUE = show the discovery scan radius ring



    EditSelection edit_selection;    // what is currently selected



    EditorDrag    edit_drag;         // drag-in-progress state



    } editor; // end EditorState



    // ===== ViewState — named sub-struct, accessed as s->view.mode etc.

    struct ViewState {



    GameMode  mode;            // current view/control mode



    b8         alt_movement_active; // TRUE while SHIFT-toggled mouse-follow flight mode is active



    } view; // end ViewState



    // ===== RenderContext: PER-FRAME transient render scratch — recomputed every frame in

    // game_render / the render passes. NOT persistent game state; must never be saved/loaded or

    // persisted across frames. Anonymous so members stay accessible as s->view_arena_w, s->star_pos.

    struct {



    // STEP 2 (view fusion): continuous arena<->galaxy-map blend weight for the current frame,

    // derived from zoom (1.0 = arena/gameplay look, 0.0 = galaxy-map look). Recomputed every frame

    // in game_render; every render site cross-fades on this instead of branching on `mode`.

    f32        view_arena_w;



    // Shared parallax anchor for the celestial backdrop this frame: the galaxy_center of the

    // system under the camera. ONE anchor is used for every system so their relative layout stays

    // rigid under depth parallax (a per-system anchor collapses all systems to screen center as

    // depth->1). Transient; recomputed every frame in game_render. Never persist.

    bs_math::HierPos2 celestial_anchor;



    // Current star world position (render-space offset from camera center): set by the parallax

    // background pass and the ship pass, read by ship_scene + starfield. Per-frame only.

    bs_math::Vec2 star_pos;



    }; // end RenderContext



    // NOTE: free-camera fields (free_camera_active/free_camera_pos) are now part of the

    // consolidated CameraState sub-struct near the top of game_state.



    // ---- RTS controls (selection, orders, control groups) ----------------------------------



    RtsControls rts_controls;



    // ---- Editor-gated continuous travel (fork #1 prototype) ----------------------------------



    b8            travel_enabled; // EDITOR PANEL: master toggle for travel system



    b8            travel_paused;  // EDITOR PANEL: pause travel mid-flight



    TravelState   travel;         // hierarchical-precision travel state



    // ---- Galaxy cluster (multiple star systems) --------------------------------------------



#define GALAXY_MAX_SYSTEMS 64



    // ===== GalaxyState — named sub-struct (consolidated: systems + map entities +

    // map marker/anim/ranges + recenter animation), accessed as s->galaxy.systems etc.

    struct GalaxyState {



    // ---- Full galaxy (~10,000 lightweight nodes) ----------------------------------------

    // The complete deterministic layout. Positions/attributes only; full StarSystem contents

    // are materialised on demand into `systems[]` (the hot cache) for systems near the camera.



    GalaxyNode*   nodes;              // heap array, node_count entries (MEMORY_TAG_GAME)



    i32           node_count;         // total systems in the galaxy



    u64           galaxy_seed;        // master seed the whole galaxy derives from



    GalaxyGenParams gen_params;       // morphology params the galaxy was generated with (drives

                                      // placement + per-star population/colour; see galaxy_params.h)



    GalaxySpatialGrid grid;           // bucket grid for nearest / radius node queries



    GalaxyLaneGraph   lanes;          // MST + add-back travel-lane connectivity



    // ---- Galaxy history: seeded civilizations (Phase 1: origins only) -------------------



    Civilization  civs[GALAXY_CIV_MAX];



    i32           civ_count;



    // ---- Dynastic Houses: the cultural lineages, one per founding polity (dynamic; all governments) --



    GalaxyHouse*  houses;               // heap array of registered Houses (nullptr until sim begins)



    i32           house_count;          // number of Houses currently registered



    i32           house_capacity;       // allocated slots in houses[]



    bool          show_houses;          // H: toggle the Dynastic Houses heredity-tree window



    // ---- Galaxy history: present-day territory (Phase 2; node-parallel, heap) -----------



    i16*          node_owner;           // civ index controlling each node (-1 = unclaimed)



    i16*          node_owner_gen;       // FROZEN snapshot of node_owner at galaxy generation (present-year 0);
                                        // uninhabited station policy reads this so its station sets stay
                                        // stable while live borders shift. NULL until history has generated.



    i32*          node_colonized_year;  // year the current owner claimed each node



    f32*          node_garrison;        // Step B: fleet strength stationed at each node (macro, background-simulated)



    u8*           node_has_stations;    // 1 if the node's deterministic station layout yields >0 stations
                                        // (precomputed once after the node_owner_gen freeze; F11 overlay
                                        // uses it to flag uninhabited station-markets without re-running
                                        // the layout core per node per frame). NULL until galaxy init.



    // ---- Cross-system Ship AI travel: macro traveler pool (heap; see ShipMission above) --



    ShipMission*  missions;             // heap pool of persistent galaxy-graph travelers (nullptr until sim begins)



    // Bounded market-delta pool: the only mutable station-market state in the whole economy

    // (baseline markets are pure functions; see sim/station_market.*). Initialised to all-free

    // (station_id = -1) in galaxy_map_finalize because station id 0 is a valid id.

    StationMarketDelta market_deltas[STATION_MARKET_DELTA_MAX];



    // Bounded station revenue pool: cumulative credits earned per station from trade contracts.

    StationRevenue  station_revenues[STATION_REVENUE_MAX];



    // Phase 6: bounded "hot" pirate-risk table. Only nodes that have actually seen raider activity
    // occupy a slot, so the galaxy pays no per-node storage. Risk decays each economy tick and is
    // read by trade_pick_market to steer contracts away from bleeding corridors.

    NodeRisk        node_risks[NODE_RISK_MAX];



    i32           mission_count;        // high-water mark of used slots in missions[]



    i32           mission_capacity;     // allocated slots in missions[] (== MISSION_MAX)



    f32           economy_tick_hours;   // Phase 6: hours since the last civ economy settlement



    f32           military_tick_hours;  // Phase 4: hours since the last military logistics survey



    // ---- Galaxy history: chronicle + legends browser (Phase 3) --------------------------



    HistoryEvent  events[GALAXY_EVENT_MAX];



    i32           event_count;



    bool          show_legends;         // L: toggle the Legends browser window



    // ---- Galaxy history: live "Galactic News" feed (Phase C1) ---------------------------



    HistoryEvent  live_feed[GALAXY_LIVE_FEED_MAX]; // ring of events from the ongoing living present



    i32           live_head;            // ring start index (oldest entry)



    i32           live_count;           // number of entries currently in the ring



    bool          show_news;            // N: toggle the Galactic News window



    // ---- Galaxy history: player coupling (Phase C2) -------------------------------------



    i8            player_rep[GALAXY_CIV_MAX]; // each civ's standing toward the player [-100,+100]



    bool          show_inspector;       // I: toggle the Live Civ Inspector window



    bool          show_system_inspector; // toggle the System Inspector window (evolved bodies)



    // ---- Government interaction window (launched from the Live Civ Inspector) ------------



    bool          show_gov_window;       // whether a per-government interaction window is open



    u8            gov_window_type;       // GovInteractionWindow currently shown



    i16           gov_window_civ;        // civ index the window targets (-1 = none)



    // ---- Galaxy history: current-system faction stance (Faction Step 1) -----------------



    i16           current_owner_civ;    // civ owning the player's current system (-1 = wild space)



    b8            current_hostile;      // whether that faction's patrols will engage the player now



    i16           debug_force_civ;      // DEBUG (F3): pin the current faction to this civ (-1 = off)



    // ---- Materialised hot cache ---------------------------------------------------------

    // `systems[]` no longer stores the whole galaxy; it is a small working set of fully

    // materialised systems (star + planets + orbit phases) for the nodes nearest the camera.

    // `system_count` is the number of ACTIVE cache slots [0, system_count). `cache_node[i]`

    // is the galaxy node index materialised in slot i (-1 when the slot is unused).



    StarSystem    systems[GALAXY_MAX_SYSTEMS];



    i32           system_count;       // number of active cache slots



    i32           cache_node[GALAXY_MAX_SYSTEMS]; // node index per slot (-1 = empty)



    GalaxyVoronoi galaxy_voronoi;      // (legacy) retained; no longer populated at 10k scale



    i32           current_system;     // cache slot of the nearest system (-1 = deep space)





    // Generic galaxy-map entities (ships, stations, asteroids, etc.).



    // Rebuilt each frame from world-space positions via world_to_galaxy_pos().



    MapEntity     map_entities[MAX_MAP_ENTITIES];



    i32           map_entity_count;



    // ---- Galaxy map player marker animation --------------------------------------------

    f32  galaxy_map_time;

    bool map_anim_scale;

    bool map_anim_rotate;

    bool map_anim_alpha;

    bool map_anim_thickness;

    // Hyperjump range circle (editor-tunable, shown on galaxy map)

    bool map_draw_jump_range;

    f32  map_jump_range;

    // Sensor detection range (editor-tunable, shown on galaxy map)

    bool map_draw_sensor_range;

    f32  map_sensor_range;

    // ---- Natural AI movement speeds (world units per second of simulation; the game clock maps

    // 1 real second == 1 in-game hour at 1x, so these scale with time acceleration) -----------

    f32  ai_speed_in_system;  // trader speed inside a system (to/from stations and jump-points)

    f32  ai_speed_jump;       // trader speed between systems (jump-point to jump-point)

    // Delaunay lane connections between star systems (galaxy map)

    bool map_draw_lanes;

    // War room (debug): differentiates mission glyphs by objective, reddens lanes across borders
    // whose owners are at war, and prints garrison / lane-risk readouts at contested nodes. Off
    // during normal play -- toggled with G or from the editor panel.
    bool map_war_room;



    bool map_draw_habitability;   // F10 overlay: tint galaxy-map dots by habitability substrate



    bool map_draw_civs;           // F11 overlay: draw civilization homeworld markers



    // Galaxy history clock (deterministic macro-history axis; distinct from real-time elapsed_time).

    struct GalaxyClock { i32 start_year; i32 present_year; } clock;



    } galaxy; // end GalaxyState



    // NOTE: floating-origin + system-panning fields (camera_hierpos, system_drag_cam,

    // system_drag_world, system_zoom) are now part of the consolidated CameraState sub-struct

    // near the top of game_state.



    // NOTE: galaxy-map marker/anim/range fields (galaxy_map_time, map_anim_*, map_draw_*,

    // map_jump_range, map_sensor_range) are now part of the consolidated GalaxyState sub-struct.



    // ---- Dedicated ship sensor (visual contact range in global/combat mode) ---------------



    f32 ship_sensor_range;        // radius within which the enemy ship renders as itself



    OutSensorDetectionFX out_sensor_fx; // out-of-sensor-range interference effect



    // ---- Three-layer sensor overlay (toggled with V) ------------------------------------



    b8  show_sensor_layers;       // draw the flagship's Layer 0/1/2 sensor rings



    // ---- Radiation detector / Metaball movement UI (EDITOR PANEL controlled) ------------



    b8  show_metaball_ui;



    f32 base_detection_radius;    // radius of the detector field around each player ship

    f32 heat_signature_radius;    // visual radius of the heat signature blob around emitters; independent of detection range



    // ---- Heat map gradient controls ------------------------------------------------------

    i32      heat_palette;             // bs_heat_palette index

    bs_color heat_color_low;           // custom edge color

    bs_color heat_color_high;          // custom center color

    f32      heat_color_falloff_power; // power applied to normalized value before gradient lookup



    f32 metaball_radius_factor;   // multiplier for encounter detection range



    f32 metaball_threshold;       // threshold for heat map alpha and encounter detection



    f32 heat_map_intensity;       // global opacity multiplier for the heat map overlay



    f32 heat_tail_length;         // number of active trail points (0 = no tail)



    f32 heat_tail_fade;           // falloff exponent for trail emission



    f32 heat_warp_strength;       // world units of domain warp displacement



    f32 heat_map_venn_sharpness;  // 0.0 = organic soft Venn boundary, 1.0 = hard-edged sensor circle



    // ---- Performance timing (EDITOR PANEL readouts) -----------------------------------------



    f64 perf_frame_start_ns;      // std::chrono nanoseconds at the start of the frame



    f64 perf_heat_map_start_ns;   // std::chrono nanoseconds at start of heat map work



    f32 heat_map_cpu_ms;          // rolling average heat map CPU time in milliseconds



    f32 frame_ms;                 // rolling average frame time in milliseconds



    Profiler profiler;            // per-subsystem CPU profiler (PROFILER panel readout)



    // ---- New Game / galaxy generation (Phase A) ------------------------------------------



    AppPhase         app_phase;   // SETUP -> GENERATING -> PLAYING



    GalaxySetupParams setup;      // player-chosen initial conditions (setup screen)



    i32              gen_stage;   // current generation stage index (while APP_GENERATING)



    f32              gen_progress;// 0..1 progress bar



    const char*      gen_label;   // label for the current generation stage



    // ---- Time control (RTS-style pause/resume, extensible to speed-up) --------------------



    f32 time_scale;         // 0.0 = paused; 1/2/3/5 = speed tiers (Pause + 1x/2x/3x/5x)

    // ---- Command overlay (RTS command layer) ------------------------------------------------
    // While up, the world dilates and the LEFT BUTTON stops being the ballistic trigger and
    // becomes selection + order issuing. That trade is the whole reason the overlay exists: the
    // trigger owns the left button in both control modes, so selection could only come back by
    // taking the button for a bounded moment rather than by sharing it with a modifier.
    //
    // DILATION, not pause: the galaxy runs a live clock (sim_hours, orbits, history), and
    // stopping it to think is a different game. 0.25x keeps everything moving while giving the
    // player time to read a fight. It multiplies the SAME global time_scale the HUD speed tiers
    // set, so the two must not both own it -- hence the saved value below.
    b8  command_overlay_active;
    f32 command_overlay_saved_time_scale; // restored on close; the tier the player had chosen



    f32 elapsed_time;       // monotonic REAL seconds since game_init (unscaled; for procedural FX)



    f64 sim_hours;          // in-game hours elapsed, scaled by time_scale (shared calendar clock)



    // ---- Encounter system (blob merge → pause → choose action) ---------------------------



    b8  encounter_active;       // TRUE while encounter panel is showing



    b8  encounter_was_active;   // previous-frame value for edge detection



    b8  encounter_can_retrigger;// TRUE when ships have separated after last encounter



    // ---- Action Log (player-activity HUD) ----------------------------------------------



    // Rolling buffer of up to 30 significant player actions. Fades after 3s inactivity,



    // expands to full history on hover.



    struct {



        char entries[30][128]; // rolling message buffer (oldest at 0)



        i32  count;             // valid entries (0..30)



        f32  inactivity_timer;  // seconds since last push



    } action_log;



    // NOTE: the camera->ship recenter-glide fields (recentering, recenter_t, recenter_from_pos)

    // live in CameraState (a camera concern, arena-wide). The old galaxy map_recenter_from_zoom /

    // map_recenter_target_pos / map_input_cooldown / map_drag_needs_fresh_press were unused/dead.



    // ---- Projectile system -------------------------------------------------------------



    ProjectileSystem projectiles;



    // ---- Projectile VFX event ring (cosmetic; core/projectile_fx.h) ---------------------
    // Muzzle flashes, impacts, flak airbursts and point-defense intercepts. Written by the
    // sim (ProjectileSystem::spawn / ::retire), read only by render/projectile_fx.cpp.
    // projectiles.fx points here; nulling that pointer disables every effect and must leave
    // the simulation bit-identical.

    ProjectileFx projectile_fx;



    // ---- Missile flight model (Phase A guided missiles; editor-tunable) -----------------
    // Global v1 flight model shared by every PROJ_MISSILE in flight (single missile type).
    // Consumed by combat_arena_steer_missiles each tick. Split per-projectile when multiple
    // missile classes exist.

    struct MissileTuning {

        f32 turn_rate;        // max steering rate (rad/s); the maneuver-counterplay knob

        f32 accel;            // forward thrust (world units/s^2)

        f32 max_speed;        // velocity clamp (below cannon shell speed: outrunnable head-on)

        f32 seeker_cone_deg;  // seeker half-angle; targets outside the cone are not tracked

        f32 seeker_range;     // seeker acquisition range (world units)

    } missile_tuning;

    // ---- Flak burst model (Phase D; editor-tunable) --------------------------------------
    // PROJ_FLAK shells proximity-detonate against hostile ordnance in
    // combat_arena_update_projectiles; damage falls off linearly to the burst edge.

    struct FlakTuning {

        f32 fuse_radius;      // detonate when hostile ordnance is this close (world units)

        f32 burst_radius;     // damage radius of the burst

        f32 burst_damage;     // ordnance HP damage at burst center (missile hp = 3.5)

    } flak_tuning;



    // ---- Point-defense laser beams (rebuilt each frame by sim/point_defense.cpp) --------



    DefenseBeam  defense_beams[MAX_DEFENSE_BEAMS];



    i32          defense_beam_count;



    // ---- Combat entities ---------------------------------------------------------------



    CombatEntity combat_entities[MAX_COMBAT_ENTITIES];



    i32          combat_entity_count;



    // ---- NPC AI ships (General Ship AI; sim/ai_ship.cpp) --------------------------------

    // Live moving agents. Registered into combat_entities[] each frame starting at npc_combat_base,

    // so they are hit-testable / RTS-targetable exactly like the player fleet + enemy hull.



    NpcShip      npc_ships[NPC_SHIP_MAX];



    i32          npc_ship_count;           // high-water count of pool slots ever used



    Ship         npc_template;             // shared hull template (loaded once; struct-copied on spawn)



    // Phase 7: per-archetype hull templates (data-driven; see archetype_hull_path). Slots whose

    // asset is missing fall back to npc_template, so art is optional.

    Ship         npc_hulls[7];             // indexed by ShipArchetype (ARCHETYPE_COUNT)

    b8           npc_hull_ready[7];



    b8           npc_template_ready;        // TRUE once the template + its textures are loaded



    i32          npc_combat_base;           // combat_entities[] index where the NPC window begins



    i32          npc_spawned_node;          // galaxy node whose garrison is currently populated (-1 none)



    f32          npc_pop_timer;             // throttle for the population manager



    // ---- Discovery system (single-ship discovery; sim/discovery.cpp) --------------------

    // Persistent record of discovered NPC agents (keyed by home_node + spawn_seed) so re-entering

    // a system keeps them identified. discovery_log[] is the browser feed (newest kept; capped).



    DiscoveredNpc    npc_discovered[DISCOVERY_NPC_MAX];



    i32              npc_discovered_count;



    DiscoveryLogEntry discovery_log[DISCOVERY_LOG_MAX];



    i32              discovery_log_count;



    bool             show_discoveries;         // Discoveries browser window toggle (editor button / O key)



    bool             show_flagship_inspector;  // Ship inspector window toggle (bottom-center Inspector button / roster "i")

    i32              inspected_ship_idx;       // Fleet member the full-screen inspector is looking at (clamped; 0 = flagship)

    i32              insp_tab;                 // Inspector middle-section tab: 0 = Loadout, 1 = Doctrine



    // ---- Space-station interaction (hover -> right-click menu -> Inspect window) -----------------

    i32              hovered_station_id;       // station under the cursor this frame (-1 = none)

    bool             station_menu_visible;     // right-click context menu open (positioned at menu x/y)

    i32              station_menu_station_id;  // the station the open context menu refers to (-1 = none)

    i32              station_menu_x;           // context-menu anchor, screen px

    i32              station_menu_y;

    bool             show_station_inspector;   // fullscreen station Inspect window toggle (RML)

    i32              inspect_station_id;        // station currently shown in the Inspect window (-1 = none)

    i32              station_insp_tab;          // active tab: 0=Dock, 1=Market, 2=Contracts



    // ---- Planet inspector (left-click a planet on the galaxy map -> RML window) -------------------

    bool              show_planet_inspector;    // planet inspector window open (RML)

    bs_math::HierPos2 planet_insp_center;       // galaxy_center of the selected planet's system (cache-slot-independent identity)

    i32               planet_insp_planet;       // planet index within that system (-1 = none)



    i32              fleet_pd_stock;           // Point-defense devices in the FLEET-WIDE pool (the bay tile shows while > 0; mounting decrements, unmounting/evicting increments). The per-hull DefenseLaser is only tuning storage.



    i32              pending_weapon_drag;      // Arsenal drag-drop: source index (hardpoint slot or stash index); -1 = none



    i32              pending_weapon_drag_kind; // Arsenal drag source kind: 0=mounted weapon (mounts[]), 1=stash weapon (weapon_stash[]), 2=point-defense from inventory, 3=mounted point-defense, 4=rack module (module_stash[]), 5=mounted module (module_mounts[])

    b8               world_module_drag;        // TRUE while a mounted item picked straight off a flagship hardpoint box rides the cursor (game-side drag; reuses pending_weapon_drag/_kind)



    // ---- Weapon micro-selection hub (hold middle mouse) ------------------------------------
    // A radial selector blooming around the cursor. The seven directional slots are the weapons
    // of the piloted ship's ACTIVE FIRE GROUP in hardpoint order (never the whole hull, so the
    // hub and the number row always agree); South is always "All", the default fire-the-
    // active-group behaviour. Held open while BUTTON_MIDDLE is down; releasing commits whatever
    // slot the cursor is over. Purely presentation + input state -- the committed result lives
    // on the Ship as `weapon_override`.

    b8               weapon_hub_open;          // TRUE while the middle mouse button holds the hub open
    i32              weapon_hub_hover;         // highlighted slot: 0=N, 1=E, 2=W, 3=S("All"),
                                               // 4=NE, 5=NW, 6=SE, 7=SW, -1 = none
    f32              weapon_hub_open_time;     // elapsed_time at which the hub opened (reveal animation)
    bs_math::Vec2    weapon_hub_press_px;      // cursor screen position at the opening press. Nothing is
                                               // highlighted until the cursor has moved away from it, so a
                                               // stray middle-click cannot commit whichever slot the cursor
                                               // happened to already be pointing at.
    bs_math::HierPos2 weapon_hub_target;       // what the per-weapon range/arc readout is judged against:
                                               // the piloted ship's attack target if it has one, else the
                                               // cursor world position frozen at the moment the hub opened
                                               // (the cursor sits over the hub itself while it is held)



    // ---- Ship module registry (immutable defs loaded once from assets/modules) -------------------

    ModuleRegistry   module_registry;         // shared ModuleDef pool; ships mount entries by pointer

    // ---- Weapon def registry (immutable stat blocks loaded once from assets/weapons) -------------

    WeaponRegistry   weapon_registry;         // shared WeaponDef pool; instances built via weapon_instantiate



    i32              ui_font_kit;              // Active in-game UI font kit: 0=Neon, 1=Clean, 2=Minimal, 3=Frontier (editor panel)

    f32              ui_sharpen;               // CAS-lite UI atlas sharpening amount (0..1; editor panel -> bs_rml_set_sharpen)



};



// Shared constants / helpers used by parallax layer code (mapped_system_layer.cpp).



extern const f32 STAR_MIN_SCREEN_RADIUS;



extern const f32 STAR_DIST_SCALE_FACTOR;



extern const f32 STAR_MAX_DIST_SCALE;



extern const f32 STAR_HERO_MAP_MIN_RADIUS;



extern f32 sensor_visibility_from_dist(f32 dist, f32 range);



// Sensor visibility (0..1) for an entity at render-local position (galaxy-map look + lighting).

extern f32 get_sensor_visibility(const game_state* s, bs_math::Vec2 pos);



// Ship physics constants (shared with RTS autopilot in rts_controls.cpp).



extern const f32 SHIP_ACCEL;



extern const f32 SHIP_DECEL;



extern const f32 SHIP_MAX_SPEED;



extern const f32 SHIP_TURN_ACCEL;



extern const f32 SHIP_MAX_TURN;



// NOTE: the module-include cascade that used to live here now lives in game_modules.h, which is

// included ONLY by game.cpp. Modules must include the specific peer headers they call (they get

// the game_state definition from game.h). This keeps a single module-header change from

// recompiling every game.h includer and makes real peer dependencies explicit.



b8 game_init(Game* game_inst);



b8 game_update(Game* game_inst, f32 dt);



b8 game_render(Game* game_inst, f32 dt);



void game_on_resize(Game* game_inst, u32 width, u32 height);



