# Parallel Realities Tutorial Series — Save/Load System Design Plan

> **Scope:** Covers data persisted across tutorials: **Shooter** (highscores), **Quest** (world map, time), **Roguelike** (dungeon, entities, inventory), **Adventure** (map, camera, entities), **TBS** (stage, units, A* paths).  
> **Language:** C (SDL2), single-threaded, no external dependencies beyond cJSON for JSON parsing.

---

## 1. Save Format Choice: JSON (cJSON)

**Rationale:**
- Human-readable for debugging
- Hierarchical — maps naturally to tutorial structs
- cJSON is single-header, already used in `adventure/` tutorial (`json/cJSON.c/h`)
- Supports arrays, objects, numbers, strings — all we need
- Fast enough for ≤ 50 KB saves (max: roguelike ~1,700 tiles + 100 entities)

**File Layout:**
```
saves/
├── slot1.json   # Manual save slot 1
├── slot2.json
├── slot3.json
├── autosave.json   # Auto-save on scene transition / death
└── meta.json       # Slot timestamps, version, tutorial ID
```

**Versioning:** Top-level `version` field (int). Increment on breaking schema changes. Loader handles missing fields with defaults.

---

## 2. Per-Tutorial Data Catalog

### 2.1 Shooter (`shooter13`–`shooter15`) — Highscore Table Only
| Struct | Fields | Size |
|--------|--------|------|
| `Highscore` | `char name[16]`, `int score`, `int recent` | 24 B |
| `Highscores` | `Highscore[NUM_HIGHSCORES=8]` | 192 B |

**Save Key:** `"highscores"` (array of objects)

---

### 2.2 Quest (`quest01`+) — Persistent World
| Struct | Fields | Size (est.) |
|--------|--------|-------------|
| `Map` | `int width, height`, `int** data` (512×512) | ~1 MB raw → **RLE compress** |
| `Overworld` | `long seed`, `Map map`, `SDL_Texture* miniMapTexture` (rebuild on load) | seed + map |
| `Game` | `Overworld overworld`, `Map* map`, `double time` | |

**Save Keys:**
```json
{
  "tutorial": "quest",
  "seed": 123456789,
  "time": 1423.5,
  "currentMap": "overworld",      // or "town", "dungeonN"
  "overworldMap": { "width": 512, "height": 512, "dataRLE": "..." },
  "townMaps": { "town1": { ... }, "town2": { ... } }
}
```

**Compression:** Run-length encode `Map.data` (repeated tile values). Store as base64 string or JSON array of `[tile, count]` pairs.

---

### 2.3 Roguelike (`rogue01`–`rogue07`) — Deepest State
| Struct | Fields | Notes |
|--------|--------|-------|
| `MapTile` | `int tile`, `int visible`, `int revealed` | 33×19 = 627 tiles |
| `Entity` | `int id, type, x, y, dead, solid, facing`, `char name[32]`, `char desc[128]`, `void* data` (Monster* or NULL), `AtlasImage* texture` (rebuild), `Entity* next` | Linked list, `id` stable across saves |
| `Monster` (via `Entity->data`) | `int hp, maxHP, minAttack, maxAttack, defence, alert, visRange`, `SDL_Point patrolDest` | Only for `ET_MONSTER` |
| `Dungeon` | `int entityId`, `Entity entityHead`, `Entity* player`, `Entity* currentEntity`, `Entity* attackingEntity`, `MapTile map[33][19]`, `SDL_Point camera`, `SDL_Point attackDir`, `double animationTimer` | |
| `Game` | `HudMessage messages[5]`, `Entity inventoryHead` (linked list) | |
| `Node` (A*) | `int x, y, g, f, h, closed`, `Node *parent, *next` | Transient — **don't save**, rebuild |

**Save Keys:**
```json
{
  "tutorial": "rogue",
  "version": 7,
  "entityIdCounter": 42,
  "camera": { "x": 5, "y": 3 },
  "attackDir": { "x": 1, "y": 0 },
  "animationTimer": 0.0,
  "map": [
    { "x": 0, "y": 0, "tile": 1, "visible": 1, "revealed": 1 },
    ...
  ],
  "entities": [
    { "id": 1, "type": 2, "name": "Player", "x": 10, "y": 8, "dead": 0, "solid": 1, "facing": 1, "description": "", "monster": { "hp": 15, "maxHP": 20, "minAttack": 2, "maxAttack": 5, "defence": 2, "alert": 0, "visRange": 12, "patrolDest": { "x": 0, "y": 0 } } },
    { "id": 2, "type": 3, "name": "Micro Mouse", "x": 12, "y": 9, "dead": 0, "solid": 1, "facing": 0, "description": "", "monster": { "hp": 3, "maxHP": 3, "minAttack": 1, "maxAttack": 3, "defence": 1, "alert": 1, "visRange": 12, "patrolDest": { "x": 10, "y": 8 } } }
  ],
  "inventory": [ { "id": 5, "type": 4, "name": "Key", "description": "A regular key..." } ],
  "messages": [ { "type": 0, "text": "Picked up a Key." }, ... ]
}
```

**Entity Type Enum (stable):**
```
0 = ET_UNKNOWN
1 = ET_PLAYER
2 = ET_MONSTER
3 = ET_ITEM
```

---

### 2.4 Adventure (`adventure01`–`adventure03`) — Map + Entities
| Struct | Fields |
|--------|--------|
| `Map` | `int data[57][30]` |
| `Dungeon` | `SDL_Point renderOffset`, `SDL_Point camera`, `Entity entityHead`, `Map map` |
| `Entity` | `int x, y, facing`, `AtlasImage* texture`, `Entity* next` |

**Save Keys:** Similar to roguelike but simpler (no Monster data, no FOV).

---

### 2.5 TBS (`tbs01`–`tbs03`) — Tactical State
| Struct | Fields |
|--------|--------|
| `MapTile` | `int tile`, `int inMoveRange` |
| `Unit` (via `Entity->data`) | `int moveRange` |
| `Entity` | `unsigned int id`, `int type`, `char name[32]`, `int x, y`, `int side`, `int solid`, `int facing`, `AtlasImage* texture`, `void (*draw)(Entity*)`, `Entity* next` |
| `Stage` | `unsigned int entityId`, `MapTile map[33][18]`, `Entity entityHead`, `Entity* currentEntity`, `Node routeHead` (transient), `int animating`, `int showRange`, `SDL_Point selectedTile` |

**Save Keys:**
```json
{
  "tutorial": "tbs",
  "entityIdCounter": 10,
  "currentEntityId": 3,
  "animating": 0,
  "showRange": 1,
  "selectedTile": { "x": 15, "y": 9 },
  "map": [ ... ],
  "entities": [
    { "id": 1, "type": 2, "name": "Andy", "x": 5, "y": 5, "side": 1, "solid": 1, "facing": 1, "unit": { "moveRange": 10 } },
    { "id": 2, "type": 2, "name": "Danny", "x": 8, "y": 6, "side": 1, "solid": 1, "facing": 0, "unit": { "moveRange": 9 } }
  ]
}
```

---

## 3. Common Save/Load API (`save.c` / `save.h`)

```c
// save.h
#ifndef SAVE_H
#define SAVE_H

#include "structs.h"   // Forward-declare tutorial-specific structs

typedef enum {
    SAVE_OK = 0,
    SAVE_ERR_IO,
    SAVE_ERR_JSON,
    SAVE_ERR_VERSION,
    SAVE_ERR_CORRUPT
} SaveResult;

typedef struct {
    int version;
    char tutorial[32];          // "shooter", "quest", "rogue", "adventure", "tbs"
    long timestamp;             // time()
    char buildHash[16];         // git short hash or build timestamp
} SaveMeta;

// --- High-level ---
SaveResult saveGame(int slot, void *gameState);       // Dispatches per-tutorial
SaveResult loadGame(int slot, void **outGameState);   // Allocates & returns tutorial state
SaveResult deleteSave(int slot);
int        listSaves(SaveMeta *out, int max);         // Fills array, returns count

// --- Per-tutorial (called by above) ---
SaveResult saveShooter(const char *path, Highscores *hs);
SaveResult loadShooter(const char *path, Highscores *hs);

SaveResult saveQuest(const char *path, Game *game);
SaveResult loadQuest(const char *path, Game **outGame);

SaveResult saveRoguelike(const char *path, Dungeon *dungeon, Game *game);
SaveResult loadRoguelike(const char *path, Dungeon **outDungeon, Game **outGame);

SaveResult saveAdventure(const char *path, Dungeon *dungeon);
SaveResult loadAdventure(const char *path, Dungeon **outDungeon);

SaveResult saveTBS(const char *path, Stage *stage);
SaveResult loadTBS(const char *path, Stage **outStage);

// --- Utilities ---
cJSON *createSaveRoot(const char *tutorial);          // Adds version, timestamp, tutorial
void    writeSaveToFile(cJSON *root, const char *path);
cJSON  *readSaveFromFile(const char *path);
int     validateSaveVersion(cJSON *root, int expected);

#endif
```

---

## 4. Serialization Strategies

| Data Type | Approach |
|-----------|----------|
| **Primitive (int, double, SDL_Point)** | Direct JSON number / object `{x, y}` |
| **Fixed arrays** (`MapTile[33][19]`) | JSON array of objects — index implicit by order (row-major) |
| **Linked lists** (`Entity*`) | JSON array — assign each node a stable `id` (already in struct) |
| **Pointers** (`Entity *player`, `*currentEntity`) | Save as `int entityId` reference; resolve on load |
| **Function pointers** (`draw`, `touch`) | **Don't save** — rebind by `type`/`name` on load |
| **Textures** (`AtlasImage*`) | **Don't save** — reload via atlas by filename stored in entity |
| **Dynamic arrays** (`int** data` in Quest) | RLE → base64 string or `[[tile, count], ...]` array |

### 4.1 Entity Reference Resolution (Two-Pass Load)
1. **Pass 1:** Parse all entities into array, build `id → Entity*` hash map (or linear search, n ≤ 100)
2. **Pass 2:** Walk entities again; for each pointer field (`player`, `currentEntity`, `attackingEntity`, `next`, `inventory`'s `next`), look up target by `id` and assign pointer

---

## 5. Versioning & Migration

| Version | Tutorial | Change | Migration |
|---------|----------|--------|-----------|
| 1 | all | Initial | — |
| 2 | rogue | Added `Monster.alert`, `visRange`, `patrolDest` | Default to 0 / 12 / (0,0) |
| 3 | rogue | Added `Entity.description`, `touch` | Default `""`, rebind `touch` by name |
| 4 | quest | Added town maps | Optional block |
| 5 | tbs | Added `Unit.moveRange`, `MapTile.inMoveRange` | Default moveRange=10 |

**Loader pattern:**
```c
cJSON *monster = cJSON_GetObjectItem(entityJson, "monster");
if (monster) {
    m->alert = cJSON_GetObjectItem(monster, "alert") ? cJSON_GetObjectItem(monster, "alert")->valueint : 0;
    m->visRange = cJSON_GetObjectItem(monster, "visRange") ? cJSON_GetObjectItem(monster, "visRange")->valueint : 12;
    // ...
}
```

---

## 6. Integration Points Per Tutorial

### Shooter
- Call `saveShooter("saves/autosave.json", &highscores)` in `addHighscore()`
- Call `loadShooter()` in `initHighscoreTable()` (fallback to defaults if missing)

### Quest
- Save on: `enterOverworld()`, `enterTown()`, player death, manual save (Esc menu)
- Load in `main()` before `initGame()` — if save exists, skip `generateOverworld()` and restore

### Roguelike
- **Auto-save:** `saveRoguelike("saves/autosave.json", &dungeon, &game)` on:
  - Level transition (stairs)
  - Player death (before highscore)
  - Manual save (new key, e.g., `F5`)
- **Load:** New "Continue" option on title screen → `loadRoguelike()` → `enterDungeonView()`

### Adventure
- Same pattern as roguelike (simpler state)

### TBS
- Save on: turn end, unit death, manual
- Load → restore `stage.currentEntity` by `id`, rebuild A* route if `routeHead` was saved (optional)

---

## 7. File I/O & Error Handling

- **Atomic write:** Write to `slotN.json.tmp`, `rename()` to `slotN.json` (POSIX atomic on same filesystem)
- **Corruption recovery:** On JSON parse error, log, offer "Delete corrupted save", fall back to defaults
- **Disk full:** Check `fwrite` return, return `SAVE_ERR_IO`
- **Path:** Use `SDL_GetPrefPath("ParallelRealities", "Tutorials")` for cross-platform save dir

---

## 8. Testing Checklist

| Scenario | Expected |
|----------|----------|
| Fresh install → play → save → quit → continue | State identical |
| Save mid-combat (rogue) → load → animationTimer respected | Animation plays or resumes |
| Save with dead entities in list → load → dead list empty | Dead entities not resurrected |
| Quest: save overworld → load → miniMapTexture regenerates | No missing texture |
| TBS: save with path in progress → load → unit resumes movement | `routeHead` rebuilt or cleared |
| Version mismatch (old save, new code) | Graceful migration, no crash |
| Corrupted JSON (truncated) | Error reported, slot skipped in list |

---

## 9. Implementation Order (Suggested)

1. **`save.c/h` + cJSON integration** — compile test with `adventure` (uses cJSON already)
2. **Shooter highscore save/load** — simplest, validates file I/O
3. **Roguelike entity + map serialization** — most complex, builds reference-resolution logic
4. **Quest map compression (RLE)** — separate utility, reusable
5. **Adventure & TBS** — adapt roguelike patterns
6. **UI integration** — "Save Game", "Load Game", "Continue" menus (reuses widget tutorial patterns)

---

## 10. Widget Tutorial Tie-In

The **Widget tutorial** (`#widgets`) covers UI building blocks (buttons, sliders, text input, checkboxes). Use it to build:
- **Save Slot Screen:** 3 buttons (slots) + "Autosave" + "Back"
- **Load Screen:** Same, shows timestamp + tutorial name from `meta.json`
- **Confirmation Dialog:** "Overwrite slot 1?" (Yes/No buttons)
- **Text Input:** "Enter save name" (optional, for named saves)

Widget patterns from tutorial: `Widget` struct with `logic`, `draw`, `data` — perfect for save/load UI.

---

## 11. Estimated Effort

| Component | Lines of Code | Complexity |
|-----------|---------------|------------|
| `save.c/h` core + cJSON helpers | ~400 | Medium |
| Per-tutorial serializers (5×) | ~200 each | Medium-High |
| Reference resolution (id→ptr) | ~150 | Medium |
| Quest RLE compression | ~100 | Low |
| UI (widget-based) | ~300 | Medium |
| **Total** | **~1,750** | **~2-3 days** |

---

## 12. Open Questions for You

1. **Single save file per slot vs. per-tutorial?**  
   Current plan: one JSON per slot containing `tutorial` field. Simpler for UI.

2. **Compress saves with zlib?**  
   Roguelike ~15 KB JSON → ~4 KB gzipped. Worth it? Adds dependency.

3. **Steam Cloud / cloud sync?**  
   Not in scope for tutorial code, but save dir should be portable.

4. **Save screenshot thumbnail?**  
   `SDL_RenderReadPixels` → encode PNG → base64 in `meta.json` for pretty load screen.

---

*Generated from analysis of Parallel Realities SDL2 tutorials (shooter, quest, rogue, adventure, tbs) — June 2025*