#!/usr/bin/env python3
# =====================================================================================
# ui_skin_gen.py — procedural Space Western UI skin atlas for the RmlUi in-game HUD.
#
# Generates assets/ui/skin/plates.png: a 512x288 RGBA sprite sheet of 9-slice
# "fabricated armor plate" elements (chamfered corners, hex corner bolts, machined edge
# bevels, brushed grain, restrained wear). Consumed by the @spritesheet block in
# assets/ui/theme.rcss — the sprite rectangles there MUST match the ATLAS layout below.
#
# Polish pass (all 9-slice safe — every treatment is constant along an edge cell's
# stretch axis, with full 2D detail confined to never-stretched corner cells):
#   * BAKED DROP SHADOW: the three panel plates carry a soft alpha shadow in a 12-texel
#     transparent margin (6 display px), offset down-right — panels float over the scene
#     with zero runtime cost. Their RCSS padding compensates for the inset visible edge.
#   * AMBIENT OCCLUSION: interiors darken toward edges (quadratic falloff) — seats the
#     surface. Depends only on distance-to-edge, so edge cells stretch cleanly.
#   * WARM/COOL BEVEL SPLIT: lit edges tint slightly warm, shadow edges slightly cool.
#   * BOLT SEATS + WRENCH MARKS: dark countersink ring + radial brush speckle.
#   * GAUGE CHANNEL: a small recessed-channel sprite with baked inner shadow for the
#     fleet-panel capacitor gauge (the fill stays an RCSS gradient).
#
# Fidelity: authored at 2x resolution (theme.rcss declares `resolution: 2x`), each
# sprite drawn at a further 4x supersample then LANCZOS-downscaled.
#
# 9-slice discipline: edge cells stretch along one axis and the centre stretches along
# both, so texel detail there smears into bands. Therefore:
#   * corner cells (never stretched)  -> full detail: grain, scratches, chips, bolts
#   * top/bottom edge cells           -> horizontal bands only (constant along x)
#   * left/right edge cells           -> vertical bands only (constant along y)
#   * centre                          -> flat fill (+ axis-symmetric AO is constant here)
#
# Usage:  python tools\ui_skin_gen.py      (from the repo root; deterministic output)
# Then re-stage assets (build-all.bat) so bin\assets picks up the sheet.
# =====================================================================================

import math
import random
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

OUT = Path(__file__).resolve().parent.parent / "assets" / "ui" / "skin" / "plates.png"
SEED = 0xB5

SS = 4                       # draw-time supersample factor (per 2x-atlas texel)
ATLAS_W, ATLAS_H = 512, 288  # final 2x atlas size

# ---- Space Western tokens (keep in sync with theme.rcss header) ----------------------
IRON_VOID  = (11, 12, 14)
CHARCOAL   = (23, 25, 29)
GUNMETAL   = (34, 38, 44)
GRAPHITE   = (46, 51, 58)
STEEL_HI   = (61, 67, 75)
STEEL_LITE = (86, 92, 99)
NICKEL     = (120, 128, 137)
AMBER      = (201, 150, 63)
AMBER_HI   = (232, 180, 90)
COPPER     = (138, 70, 48)
COPPER_HI  = (179, 104, 74)

# Warm/cool bevel split: lit edges lean warm (key light), shadow edges lean cool.
PLATE_EDGE_LIGHT = (70, 71, 68)
PLATE_EDGE_DARK  = (13, 15, 22)


def clamp(v):
    return max(0, min(255, int(v)))


def shade(rgb, dv):
    return (clamp(rgb[0] + dv), clamp(rgb[1] + dv), clamp(rgb[2] + dv))


def mix(a, b, t):
    return (clamp(a[0] + (b[0] - a[0]) * t), clamp(a[1] + (b[1] - a[1]) * t), clamp(a[2] + (b[2] - a[2]) * t))


def rgba(rgb, a=255):
    return (rgb[0], rgb[1], rgb[2], a)


class PlateSpec:
    """All knobs for one chamfered plate sprite. Sizes are in 2x-atlas texels; the
    sprite is drawn at SS times that and downscaled. `shadow` adds a transparent
    margin (2x texels) carrying a baked drop shadow; `ao` scales the edge-darkening
    ambient-occlusion pass (0 disables)."""

    def __init__(self, w, h, chamfer, border, fill, alpha=255, raised=True, outline_px=3,
                 edge_light=STEEL_LITE, edge_dark=(16, 17, 20), outline=IRON_VOID,
                 bolts=False, wear=1.0, ramp=1.0, shadow=0, ao=0.0):
        self.w, self.h = w, h
        self.chamfer = chamfer
        self.border = border
        self.fill = fill
        self.alpha = alpha
        self.raised = raised
        self.outline_px = outline_px
        self.edge_light = edge_light
        self.edge_dark = edge_dark
        self.outline = outline
        self.bolts = bolts
        self.wear = wear
        self.ramp = ramp
        self.shadow = shadow
        self.ao = ao


def row_extent(j, w, h, c):
    """Horizontal span [x0, x1] of row j inside a w*h plate with chamfer c."""
    if j < c:
        inset = c - j
    elif j >= h - c:
        inset = c - (h - 1 - j)
    else:
        inset = 0
    return inset, w - 1 - inset


def draw_hex_bolt(d, px, cx, cy, r, fill, alpha, rng, W, H):
    """Anti-aliased (via SS) hex-head bolt: countersink seat, wrench-brush speckle
    ring, hex head with top-lit facet, cross slot, specular glint."""
    # Wrench-brush speckle ring around the seat (installation traces).
    for rr in range(int(r * 1.7), int(r * 2.2)):
        for a in range(0, 360, 4):
            if rng.random() < 0.35:
                sx = int(cx + rr * math.cos(math.radians(a)))
                sy = int(cy + rr * math.sin(math.radians(a)))
                if 0 <= sx < W and 0 <= sy < H and px[sx, sy][3] > 0:
                    base = px[sx, sy]
                    dv = rng.choice((-5, 4))
                    px[sx, sy] = rgba(shade(base[:3], dv), base[3])
    # Countersink seat: dark ring, then the recess.
    d.ellipse((cx - r * 1.55, cy - r * 1.55, cx + r * 1.55, cy + r * 1.55),
              fill=rgba(shade(fill, -22), alpha))
    d.ellipse((cx - r * 1.22, cy - r * 1.22, cx + r * 1.22, cy + r * 1.22),
              fill=rgba(IRON_VOID, alpha))
    # Hex head, slightly rotated per bolt for a hand-assembled feel.
    rot = rng.uniform(0, math.pi / 3)
    pts = [(cx + r * math.cos(rot + k * math.pi / 3), cy + r * math.sin(rot + k * math.pi / 3))
           for k in range(6)]
    d.polygon(pts, fill=rgba(shade(fill, 14), alpha))
    top = [(x, y) for (x, y) in pts if y <= cy + 0.1] + [(cx, cy)]
    if len(top) >= 3:
        d.polygon(top, fill=rgba(shade(fill, 30), alpha))
    sl = r * 0.62
    lw = max(1, int(r * 0.22))
    d.line((cx - sl, cy, cx + sl, cy), fill=rgba(shade(fill, -34), alpha), width=lw)
    d.line((cx, cy - sl, cx, cy + sl), fill=rgba(shade(fill, -34), alpha), width=lw)
    d.ellipse((cx - r * 0.30 - r * 0.38, cy - r * 0.30 - r * 0.38,
               cx + r * 0.30 - r * 0.38, cy + r * 0.30 - r * 0.38),
              fill=rgba(NICKEL, alpha))


def draw_plate(spec, rng):
    """Render one plate sprite at SS supersample and return the downscaled 2x image
    (size (w+2*shadow, h+2*shadow) when a shadow margin is requested)."""
    W, H = spec.w * SS, spec.h * SS
    c, b, o = spec.chamfer * SS, spec.border * SS, spec.outline_px * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    px = img.load()
    d = ImageDraw.Draw(img)

    # ---- base fill: silhouette rows with top-lit ramp confined to the border bands ----
    for j in range(H):
        x0, x1 = row_extent(j, W, H, c)
        if spec.raised:
            if j < b:
                t = 1.0 - j / b
                col = shade(spec.fill, int(14 * t * t * spec.ramp + 6 * t * spec.ramp))
            elif j >= H - b:
                t = (j - (H - b)) / b
                col = shade(spec.fill, int(-10 * t * spec.ramp))
            else:
                col = spec.fill
        else:
            if j < b:
                t = 1.0 - j / b                       # recess: shadow under the rim
                col = shade(spec.fill, int(-16 * t * t))
            elif j >= H - b:
                t = (j - (H - b)) / b                 # faint floor light near the lip
                col = shade(spec.fill, int(5 * t))
            else:
                col = spec.fill
        d.line((x0, j, x1, j), fill=rgba(col, spec.alpha))

    # ---- ambient occlusion: interiors darken toward edges (9-slice safe) ---------------
    # In edge cells the falloff depends on a single axis (constant along the stretch);
    # corner cells combine both axes for a stronger seated-corner read.
    if spec.ao > 0.0:
        ao_range = 12 * SS
        for j in range(H):
            x0, x1 = row_extent(j, W, H, c)
            dy = min(j, H - 1 - j)
            ky = (1.0 - dy / ao_range) if dy < ao_range else 0.0
            for i in range(x0, x1 + 1):
                dx = min(i - x0, x1 - i)
                kx = (1.0 - dx / ao_range) if dx < ao_range else 0.0
                k = (kx * kx + ky * ky) * 9.0 * spec.ao
                if k > 0.5:
                    r, g, bb, a = px[i, j]
                    px[i, j] = (clamp(r - k), clamp(g - k), clamp(bb - k), a)

    # ---- brushed grain -----------------------------------------------------------------
    for j in range(H):                                # top/bottom bands: per-row tint
        if j < b or j >= H - b:
            if rng.random() < 0.55:
                x0, x1 = row_extent(j, W, H, c)
                dv = rng.choice((-3, -2, 2, 3))
                base = px[(x0 + x1) // 2, j]
                d.line((x0 + o, j, x1 - o, j), fill=rgba(shade(base[:3], dv), base[3]))
    for i in range(W):                                # side bands: per-column tint
        if i < b or i >= W - b:
            if rng.random() < 0.55:
                dv = rng.choice((-3, -2, 2, 3))
                for j in range(b, H - b):
                    x0, x1 = row_extent(j, W, H, c)
                    if x0 + o < i < x1 - o:
                        base = px[i, j]
                        px[i, j] = rgba(shade(base[:3], dv), base[3])

    # ---- corner-cell detail: scratches, chips, discoloration ---------------------------
    corners = [(0, 0), (W - b, 0), (0, H - b), (W - b, H - b)]
    for zx, zy in corners:
        if spec.wear > 0 and rng.random() < 0.8:
            bx = zx + rng.randint(0, b - 1)
            by = zy + rng.randint(0, b - 1)
            br = rng.randint(b // 3, b)
            blotch = Image.new("RGBA", (br * 2, br * 2), (0, 0, 0, 0))
            bd = ImageDraw.Draw(blotch)
            dv = rng.choice((-6, 5))
            bd.ellipse((0, 0, br * 2 - 1, br * 2 - 1), fill=(128 + dv * 8, 128 + dv * 6, 128, 14))
            img.alpha_composite(blotch, (bx - br, by - br))
            px = img.load()
        for _ in range(int(3 * spec.wear)):
            sx = zx + rng.randint(o, b - 1)
            sy = zy + rng.randint(o, b - 1)
            ln = rng.randint(SS * 2, SS * 5)
            ang = rng.uniform(-0.6, 0.6) + (0 if rng.random() < 0.5 else math.pi / 2)
            ex = sx + int(ln * math.cos(ang))
            ey = sy + int(ln * math.sin(ang))
            base = px[min(max(sx, 0), W - 1), min(max(sy, 0), H - 1)]
            if base[3] == 0:
                continue
            dv = rng.choice((-9, 8))
            d.line((sx, sy, ex, ey), fill=rgba(shade(base[:3], dv), base[3]), width=max(1, SS // 3))

    # ---- machined bevels (stretch-safe: constant along their own edge) -----------------
    top_col, bot_col = (spec.edge_light, spec.edge_dark) if spec.raised else (spec.edge_dark, spec.edge_light)
    bev = max(1, SS)                                   # 1 logical texel
    for j in range(H):
        x0, x1 = row_extent(j, W, H, c)
        if o <= j < o + bev:                           # top inner bevel
            d.line((x0 + o, j, x1 - o, j), fill=rgba(top_col, spec.alpha))
        if o <= j < o + bev * 2 and spec.raised:       # soft secondary catch under it
            if j >= o + bev:
                d.line((x0 + o, j, x1 - o, j), fill=rgba(mix(top_col, spec.fill, 0.55), spec.alpha))
        if H - o - bev <= j < H - o:                   # bottom inner bevel
            d.line((x0 + o, j, x1 - o, j), fill=rgba(bot_col, spec.alpha))
    for j in range(c + o, H - c - o):                  # side inner bevels
        x0, x1 = row_extent(j, W, H, c)
        lcol = mix(top_col, spec.fill, 0.45) if spec.raised else spec.edge_dark
        rcol = mix(bot_col, spec.fill, 0.35) if spec.raised else spec.edge_light
        d.line((x0 + o, j, x0 + o + bev - 1, j), fill=rgba(lcol, spec.alpha))
        d.line((x1 - o - bev + 1, j, x1 - o, j), fill=rgba(rcol, spec.alpha))
    # Chamfer diagonal facets: lit on the upper cuts, shadowed on the lower cuts.
    for j in range(c):
        x0, x1 = row_extent(j, W, H, c)
        d.line((x0 + o, j, x0 + o + bev + SS // 2, j), fill=rgba(mix(top_col, spec.fill, 0.3), spec.alpha))
        d.line((x1 - o - bev - SS // 2, j, x1 - o, j), fill=rgba(mix(top_col, spec.fill, 0.6), spec.alpha))
    for j in range(H - c, H):
        x0, x1 = row_extent(j, W, H, c)
        d.line((x0 + o, j, x0 + o + bev + SS // 2, j), fill=rgba(mix(bot_col, spec.fill, 0.5), spec.alpha))
        d.line((x1 - o - bev - SS // 2, j, x1 - o, j), fill=rgba(bot_col, spec.alpha))

    # ---- iron outline: darken the outermost ring of the silhouette ---------------------
    for j in range(H):
        x0, x1 = row_extent(j, W, H, c)
        if j < o or j >= H - o:
            d.line((x0, j, x1, j), fill=rgba(spec.outline, spec.alpha))
        else:
            d.line((x0, j, x0 + o - 1, j), fill=rgba(spec.outline, spec.alpha))
            d.line((x1 - o + 1, j, x1, j), fill=rgba(spec.outline, spec.alpha))
    px = img.load()

    # ---- edge chips: tiny nicks biting into the outline (corner cells only) ------------
    if spec.wear > 0:
        for zx, zy in corners:
            for _ in range(int(2 * spec.wear)):
                if rng.random() < 0.6:
                    cx0 = zx + rng.randint(0, b - SS)
                    cy0 = 0 if zy == 0 else H - o
                    ch = rng.randint(SS // 2, o - 1) if o > 1 else 1
                    base = GUNMETAL if spec.raised else spec.fill
                    d.rectangle((cx0, cy0 + (o - ch if zy == 0 else 0),
                                 cx0 + rng.randint(SS, SS * 2), cy0 + (o - 1 if zy == 0 else ch)),
                                fill=rgba(shade(base, 22), spec.alpha))

    # ---- corner bolts -------------------------------------------------------------------
    if spec.bolts:
        r = 3.4 * SS
        inset = c + 4 * SS
        for bx, by in ((inset, inset), (W - inset, inset), (inset, H - inset), (W - inset, H - inset)):
            draw_hex_bolt(d, px, bx, by, r, spec.fill, spec.alpha, rng, W, H)

    # ---- silhouette clip: kill any detail that escaped the chamfer polygon -------------
    px = img.load()
    for j in range(H):
        x0, x1 = row_extent(j, W, H, c)
        for i in range(0, x0):
            px[i, j] = (0, 0, 0, 0)
        for i in range(x1 + 1, W):
            px[i, j] = (0, 0, 0, 0)

    # ---- baked drop shadow: compose the plate over a blurred offset silhouette ---------
    if spec.shadow > 0:
        M = spec.shadow * SS
        canvas = Image.new("RGBA", (W + 2 * M, H + 2 * M), (0, 0, 0, 0))
        sil = Image.new("RGBA", canvas.size, (0, 0, 0, 0))
        sd = ImageDraw.Draw(sil)
        ox, oy = 2 * SS, 4 * SS               # 1px right, 2px down at display scale
        poly = [(M + ox + p[0], M + oy + p[1]) for p in
                [(c, 0), (W - c, 0), (W, c), (W, H - c), (W - c, H), (c, H), (0, H - c), (0, c)]]
        sd.polygon(poly, fill=(0, 0, 0, 150))
        sil = sil.filter(ImageFilter.GaussianBlur(int(M * 0.55)))
        canvas.alpha_composite(sil)
        canvas.alpha_composite(img, (M, M))
        return canvas.resize((spec.w + 2 * spec.shadow, spec.h + 2 * spec.shadow), Image.LANCZOS)

    return img.resize((spec.w, spec.h), Image.LANCZOS)


def draw_channel(w, h):
    """Recessed gauge channel with baked inner shadow (capacitor bar). 9-slice with a
    thin ring; all rows constant along x, so any stretch is safe."""
    W, H = w * SS, h * SS
    img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, W - 1, H - 1), fill=rgba((9, 10, 13)))
    d.rectangle((0, 0, W - 1, SS - 1), fill=rgba((4, 5, 6)))              # outline top
    d.rectangle((0, H - SS, W - 1, H - 1), fill=rgba((80, 86, 94)))       # light bottom lip
    d.rectangle((0, 0, SS - 1, H - 1), fill=rgba((4, 5, 6)))
    d.rectangle((W - SS, 0, W - 1, H - 1), fill=rgba((4, 5, 6)))
    d.rectangle((SS, SS, W - SS - 1, 2 * SS - 1), fill=rgba((3, 4, 5)))   # inner shadow row
    d.rectangle((SS, 2 * SS, W - SS - 1, 3 * SS - 1), fill=rgba((6, 7, 9)))
    return img.resize((w, h), Image.LANCZOS)


def main():
    rng = random.Random(SEED)
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))

    # ATLAS layout (2x texels; @spritesheet in theme.rcss must match; 4-texel gutters).
    # Panel plates carry a 12-texel shadow margin -> their cells are sprite+24 square.
    # Row 0 (y=0):   plate 152x152 @ (0,0)   plate-amber 152x152 @ (156,0)   rail 128x64 @ (312,0)
    # Row 1 (y=156): btn/btn-hi/btn-amber/well/socket/copper 64x64 @ x=0,68,136,204,272,340
    #                plate-sm 88x88 @ (408,156)
    # Row 2 (y=248): chan 32x16 @ (0,248)
    SH = 12   # shadow margin (2x texels = 6 display px)
    sprites = {
        (0, 0):    PlateSpec(128, 128, 20, 32, GUNMETAL, alpha=255, outline_px=3, bolts=True,
                             edge_light=PLATE_EDGE_LIGHT, edge_dark=PLATE_EDGE_DARK,
                             shadow=SH, ao=1.0),
        (156, 0):  PlateSpec(128, 128, 20, 32, GUNMETAL, alpha=255, outline_px=3, bolts=True,
                             edge_light=AMBER_HI, edge_dark=shade(AMBER, -60),
                             outline=shade(AMBER, -95), shadow=SH, ao=1.0),
        (312, 0):  PlateSpec(128, 64, 12, 16, (28, 31, 36), outline_px=2, ramp=1.3,
                             edge_light=(64, 66, 64), edge_dark=IRON_VOID, wear=0.7),
        (0, 156):   PlateSpec(64, 64, 10, 16, (43, 47, 54), outline_px=2, wear=0.8, ao=0.5),
        (68, 156):  PlateSpec(64, 64, 10, 16, (54, 60, 68), outline_px=2, wear=0.8, ao=0.5,
                              edge_light=shade(STEEL_LITE, 20), edge_dark=(20, 22, 26)),
        (136, 156): PlateSpec(64, 64, 10, 16, (45, 49, 56), outline_px=2, wear=0.8, ao=0.5,
                              edge_light=AMBER_HI, edge_dark=shade(AMBER, -70),
                              outline=shade(AMBER, -95)),
        (204, 156): PlateSpec(64, 64, 10, 16, (16, 17, 21), raised=False, outline_px=2,
                              edge_light=(58, 63, 70), edge_dark=(6, 7, 8), wear=0.5, ao=0.6),
        (272, 156): PlateSpec(64, 64, 10, 16, (14, 15, 19), raised=False, outline_px=2,
                              edge_light=(51, 56, 63), edge_dark=(5, 6, 7), wear=0.5, ao=0.6),
        (340, 156): PlateSpec(64, 64, 10, 16, COPPER, outline_px=2, wear=1.4, ramp=1.6,
                              edge_light=shade(COPPER_HI, 28), edge_dark=shade(COPPER, -45)),
        (408, 156): PlateSpec(64, 64, 10, 16, GUNMETAL, alpha=255, outline_px=2, wear=0.9,
                              edge_light=PLATE_EDGE_LIGHT, edge_dark=PLATE_EDGE_DARK,
                              shadow=SH, ao=0.8),
    }

    for (x, y), spec in sprites.items():
        atlas.alpha_composite(draw_plate(spec, rng), (x, y))

    atlas.alpha_composite(draw_channel(32, 16), (0, 248))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(OUT)
    print(f"wrote {OUT} ({ATLAS_W}x{ATLAS_H}, 2x resolution)")


if __name__ == "__main__":
    main()
