#pragma once

struct game_state;

// System Inspector — free-floating bs_ui window showing the evolved-body model of the
// CURRENT star system (s->galaxy.current_system): star card, body tree (planets with
// nested moons, belts), per-body evolved state (composition, atmosphere/water/geology
// gauges, habitability, hazard, resources) and the system's evolution chronicle.
// Gated by s->galaxy.show_system_inspector (toggled from the editor panel).
void build_system_inspector(game_state* s);
