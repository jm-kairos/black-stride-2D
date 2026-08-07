# MathCore

**Responsibility:** Owns the engine's linear-algebra vocabulary — `Vec2`, `Vec3`, `Vec4`, `Mat4`,
the angle constants, and the operations on them — under a fixed column-major, Vulkan-clip
convention. It explicitly does not own coordinate *frames* (screen/world/render conversions are
Camera2D's, in RenderFrontend), nor galaxy-scale precision (that is HierCoords, which builds on
`Vec2`). It holds no state, allocates nothing, and calls nothing but `<math.h>` — the one engine
subsystem with no coupling beyond its own header.

**Public interface:** `engine/source/math/math_utils.h` — `namespace bs_math`;
`Vec2`/`Vec3`/`Vec4`/`Mat4`; `BS_PI`, `BS_DEG2RAD`, `BS_RAD2DEG`; `clamp`, `clampf`;
`vec2_add`/`_sub`/`_scale`/`_dot`/`_length`/`_normalized`/`_rotate`;
`vec3_add`/`_sub`/`_scale`; `mat4_identity`/`_mul`/`_ortho`/`_translation`/`_scale`/
`_rotation_z`. All 22 functions are `bs__api__`, and the four struct definitions additionally
carry `bs__api__` on the type.

The two audiences are disjoint: **`vec2_*`, `clampf` and `BS_PI` are sandbox-dominated**
(`vec2_length` in 25 files, `vec2_add`/`_scale` in 22, `clampf` in 21), while **every `mat4_*`
is engine-only** — used by `renderer/camera2d.cpp` and the backend, never by the sandbox.

**Depends on:** Foundation.
**Depended on by:** HierCoords, RenderFrontend, RenderBackend, DeadStarfield, plus 23 sandbox
files.

**Key invariants:**
- **`Mat4` is column-major, `data[col * 4 + row]`** — stated at `math_utils.h:35-37` explicitly
  so matrices can be uploaded to SDL GPU without transposition. Relied on by
  `mat4_mul` (`math_utils.cpp:61`), `camera2d_view_proj`, and the shader uniform layout.
  **Enforced nowhere**; a mismatch would silently produce wrong transforms.
- **`mat4_ortho` maps z into `[0,1]`** (Vulkan/SDL-GPU), not the OpenGL `[-1,1]`
  (`math_utils.cpp:69-85`, documented `math_utils.h:63-64`). It also negates z
  (`math_utils.cpp:78`), flipping handedness relative to the other builders.
- **`mat4_mul(a, b)` applies `b` first** — documented at `math_utils.h:60-61` and relied on by
  `camera2d.cpp:39`'s `mat4_mul(s, mat4_mul(r, t))`. A convention only; reversing it compiles.
- `vec2_normalized` is the only function guarding a degenerate input, returning the zero vector
  for zero length (`math_utils.cpp:27`).

**Extension points:** Adding an operation means a `bs__api__` declaration in `math_utils.h` and
a definition in `math_utils.cpp` — there is no dispatch, registry, or template machinery to hook
into. `Vec4` already exists as a type with **no operations at all**, so a `vec4_*` family would
be the natural first extension. Note that adding anything here widens the DLL export surface,
which currently has no versioning (see `docs/architecture/engine-api-boundary.md` §9).

**Known limitations / tech debt:**
- **`Vec4` is declared and has zero operations** — no `vec4_*` function exists in the header or
  the implementation. It is a data type only.
- **All three `vec3_*` functions and all six `mat4_*` functions are unused by the sandbox**, and
  `vec3_add`/`_sub`/`_scale` are unused outside `math_utils.cpp` entirely.
- **`mat4_ortho` has no division guard** (`math_utils.cpp:72-74`): degenerate
  `left == right`, `bottom == top`, or `near == far` produce infinities silently. This is
  reachable — a minimised window legitimately reports zero dimensions, and
  `camera2d_view_proj` derives its bounds from the framebuffer size.
- `mat4_mul` takes both operands **by value** (128 bytes copied per call) and runs a scalar
  triple loop (`math_utils.cpp:54-67`) — no SIMD, no in-place variant.
- `clamp`/`clampf` do not check `min <= max`; inverted bounds return `max` without complaint.
- The `Vec2f32` / `Vec3f32` / `Vec4f32` typedef names (`math_utils.h:18,25,33`) are declared but
  the codebase consistently uses `Vec2`/`Vec3`/`Vec4`; the aliases are effectively unused.
- `bs__api__` on the struct definitions (`math_utils.h:14,20,27,38`) applies `dllexport` to
  plain PODs, which exports nothing useful — value semantics work regardless of the annotation.
- All of `Vec2`, `Vec3` and `Mat4` cross the DLL boundary **by value** (see
  `engine-api-boundary.md` §5), so their layouts are part of the ABI with no size assertions
  guarding them.
- No operator overloads — arithmetic is free functions, which keeps the ABI C-like but makes
  expressions verbose.
- *Inferred:* `mat4_rotation_z`'s comment "the only rotation a 2D game needs"
  (`math_utils.h:68`) reads as the reason there is no `mat4_rotation_x/y`, no quaternion type,
  and no matrix inverse or transpose. That is my reading of intent, not a stated rule.

**Source paths:** `engine/source/math/math_utils.{cpp,h}`

**Last verified:** 2026-08-07, commit `812680c`
