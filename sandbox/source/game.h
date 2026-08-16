#pragma once

// game.h is now a thin shim. The game_state god-struct, its supporting types, the shared
// extern constants/helpers, and the game_* entry-point declarations now live in
// state/game_state.h. Every existing module still includes "game.h" unchanged; new code may
// include "state/game_state.h" directly for the state definition.
#include "state/game_state.h"