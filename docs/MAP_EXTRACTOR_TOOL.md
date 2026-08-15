# Map Extractor — 4-map ship pipeline (`tools/map_extractor`)

Turns a single top-down ship PNG (plus, optionally, a side-view PNG of the same hull) into
the four maps the engine's mapped-sprite pass consumes: **diffuse**, **normal**, **depth**,
**position**. Built by `tools/map_extractor/build.bat` to `bin/map_extractor.exe`. The
Vanguard flagship (`assets/ships/ship/ship.ship`) is the worked example; its card records
the exact regeneration command.

## Pipeline: one height field drives everything

Everything derives from a single height field in pixel units, so the normal map (lighting)
and the depth map (parallax) can never disagree:

1. **Silhouette bevel** — alpha mask → true signed Euclidean distance transform
   (Felzenszwalb–Huttenlocher; a chamfer variant is selectable) → a quarter-ellipse bevel
   profile over `bevel_px` from the rim (auto-sized from the art if 0), flat top with an
   optional ease-out **dome** toward the spine. Hulls read as plated slabs, not cones.
2. **Side-view dorsal elevation** (optional, `--side`) — the side view's top silhouette is
   read per station along the length: elevation above the lowest deck line, gaps between
   pods bridged, smoothed, converted to top-down pixels via the silhouette length ratio.
   Nose direction is auto-detected by correlating side thickness against top-down width
   (overridable). The 1D curve is shaped laterally as a spine-centered **ridge** with a
   **vaulted (quarter-ellipse) crest** — walls face port/starboard so the elevation lights
   from every star bearing, and a flat-topped tower can't shade identically to the deck.
   Wide outboard structures (wing pods) stay at deck level.
3. **Painted detail** — mask-weighted band-pass of luminance (features smaller than
   `detail_radius_px` survive; broad painted lighting is rejected so it can't double-light)
   becomes real relief, faded across the bevel to keep silhouettes clean.
4. **Outputs** —
   - *Normal*: analytic gradient of the height field. Conventions (verified against the
     engine's quad): texture row 0 = ship-local +Y (nose), so **G = `+dH/drow`**; R =
     `-dH/dx`; `normal_strength` multiplies slope (1 = geometric).
   - *Depth*: the height field normalized, then **compressed to 0.25..0.75** — the shader
     offsets UVs by `(depth − 0.5) × 0.02` in normalized UV space, and the full range smears
     tall art ~30 px along the light axis.
   - *Diffuse*: original color and antialiased alpha; cavity AO bakes recessed detail
     darker; an **altitude gain** (`0.85 + 0.30 × height`) makes raised structures (command
     hub) read brighter than low decks at any light angle; rim colors are dilated outward so
     bilinear sampling never pulls in black fringes.
   - *Position*: RG = normalized texel UV, B = depth.

All tunables live in one `extract_params_t` (`extractor.h`), shared by the GUI, the worker
thread, and the headless modes.

## CLI

```
map_extractor.exe                                  GUI (open PNGs, tune sliders, export)
map_extractor.exe --export <in.png> <out_dir> [name] [--side <side.png>]
map_extractor.exe --preview <in.png> <out.png> [star_deg] [--side <side.png>]
```

- `--export` writes `<out_dir>/<name>/<name>_{diffuse,normal,depth,position}_map.png` plus a
  bare generated `.ship` (visual card only — discard it when the hull has a hand-authored
  card). **`<out_dir>` must already exist**; only the `<name>` subfolder is created.
- `--preview` renders a lit screenshot through the very shader blobs the engine uses
  (`assets/shaders/{dxil,spirv}/mapped_sprite.*`), so what it shows is what the game draws.
  `preview_light_t` in `preview.cpp` must stay layout-identical to `LightUBO` in
  `mapped_sprite.frag.hlsl` and `mapped_light` in `renderer_backend_sdlgpu.cpp`.

## Regenerating the Vanguard's maps

```
bin\map_extractor.exe --export assets\ships\ships_cruisers_yellow\cruisers_r0_c0.png <dir> ship
    --side assets\ships\ships_cruisers_yellow\cruisers_side_r0_c1.png
```

Copy the four PNGs over `assets/ships/ship/ship_*_map.png` (and mirror to `bin/assets/…`,
or just run `build-all.bat`, whose XCOPY staging does it). The side view is the atlas cell
row 1 col 2 of `2DCruisersCollectionIISideViewAtlasYellow2.png`, stored pre-cropped in the
repo; its dorsal profile peaks ~157 px (top-down units) at 66 % from the nose — the command
hub.

## History / rationale

The original extractor had three correctness bugs (signed-EDT distances re-negated at the
rim, an inverted normal-map G channel, and `normal_strength` acting as the Z component so
higher = flatter) plus a cone-shaped depth map and silhouette-only normals. The rewrite
fixed the conventions against the engine's actual quad mapping and rebuilt the pipeline
around the unified height field above. The side-view profile was added after: first as a
full-beam ribbon (invisible under abeam light — its slopes only ran along the length), then
as the lateral ridge; the vaulted crest and diffuse altitude gain landed when a flat-topped
command hub still shaded identically to the deck below it.
