#!/usr/bin/env python3
# =====================================================================================
# ui_skin_gen.py — procedural Space Western UI skin atlas for the RmlUi in-game HUD.
#
# Generates assets/ui/skin/plates.png: a 512x256 RGBA sprite sheet of 9-slice
# "fabricated armor plate" elements (chamfered corners, hex corner bolts, machined edge
# bevels, brushed grain, paint chips, restrained wear). Consumed by the @spritesheet
# block at the top of assets/ui/theme.rcss — the sprite rectangles there MUST match the
# ATLAS layout below.
#
# Fidelity pipeline: the sheet is authored at 2x resolution (theme.rcss declares
# `resolution: 2x`, so RmlUi displays every texel pair as one screen pixel through the
# linear sampler = 2x supersampling), and each sprite is DRAWN at a further 4x
# supersample then LANCZOS-downscaled, giving anti-aliased chamfer diagonals, bevels
# and bolt rounds.
#
# Palette mirrors the Space Western tokens documented in assets/ui/theme.rcss.
#
# 9-slice discipline: edge cells stretch along one axis and the centre stretches along
# both, so texel detail there smears into bands. Therefore:
#   * corner cells (never stretched)  -> full detail: grain, scratches, chips, bolts
#   * top/bottom edge cells           -> horizontal bands only (constant along x)
#   * left/right edge cells           -> vertical bands only (constant along y)
#   * centre                          -> flat fill
#
# Usage:  python tools\ui_skin_gen.py      (from the repo root; deterministic output)
# Then re-stage assets (build-all.bat) so bin\assets picks up the sheet.
# =====================================================================================

import math
import random
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent.parent / "assets" / "ui" / "skin" / "plates.png"
SEED = 0xB5

SS = 4                       # draw-time supersample factor (per 2x-atlas texel)
ATLAS_W, ATLAS_H = 512, 256  # final 2x atlas size

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
    sprite is drawn at SS times that and downscaled."""

    def __init__(self, w, h, chamfer, border, fill, alpha=255, raised=True, outline_px=3,
                 edge_light=STEEL_LITE, edge_dark=(16, 17, 20), outline=IRON_VOID,
                 bolts=False, wear=1.0, ramp=1.0):
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


def row_extent(j, w, h, c):
    """Horizontal span [x0, x1] of row j inside a w*h plate with chamfer c."""
    if j < c:
        inset = c - j
    elif j >= h - c:
        inset = c - (h - 1 - j)
    else:
        inset = 0
    return inset, w - 1 - inset


def draw_hex_bolt(d, cx, cy, r, fill, alpha, rng):
    """Anti-aliased (via SS) hex-head bolt: recess ring, hex head, cross slot, highlight."""
    # Recess ring (dark countersink).
    d.ellipse((cx - r * 1.45, cy - r * 1.45, cx + r * 1.45, cy + r * 1.45),
              fill=rgba(shade(fill, -18), alpha))
    d.ellipse((cx - r * 1.18, cy - r * 1.18, cx + r * 1.18, cy + r * 1.18),
              fill=rgba(IRON_VOID, alpha))
    # Hex head, slightly rotated per bolt for a hand-assembled feel.
    rot = rng.uniform(0, math.pi / 3)
    pts = [(cx + r * math.cos(rot + k * math.pi / 3), cy + r * math.sin(rot + k * math.pi / 3))
           for k in range(6)]
    d.polygon(pts, fill=rgba(shade(fill, 14), alpha))
    # Top-lit facet: brighter upper half of the head.
    top = [(x, y) for (x, y) in pts if y <= cy + 0.1] + [(cx, cy)]
    if len(top) >= 3:
        d.polygon(top, fill=rgba(shade(fill, 30), alpha))
    # Cross slot.
    sl = r * 0.62
    lw = max(1, int(r * 0.22))
    d.line((cx - sl, cy, cx + sl, cy), fill=rgba(shade(fill, -34), alpha), width=lw)
    d.line((cx, cy - sl, cx, cy + sl), fill=rgba(shade(fill, -34), alpha), width=lw)
    # Specular glint.
    d.ellipse((cx - r * 0.30 - r * 0.38, cy - r * 0.30 - r * 0.38,
               cx + r * 0.30 - r * 0.38, cy + r * 0.30 - r * 0.38),
              fill=rgba(NICKEL, alpha))


def draw_plate(spec, rng):
    """Render one plate sprite at SS supersample and return the downscaled 2x image."""
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

    # ---- brushed grain -----------------------------------------------------------------
    # Horizontal strokes in the top/bottom bands (constant per row -> stretch-safe there),
    # vertical strokes in the side bands, free 2D strokes in the corner cells.
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
        # Faint discoloration blotch (aged coating).
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
        # Fine scratches (1-texel light/dark diagonal strokes).
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
                    # chip on the top or bottom outline run within this corner cell
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
            draw_hex_bolt(d, bx, by, r, spec.fill, spec.alpha, rng)

    # ---- silhouette clip: kill any detail that escaped the chamfer polygon -------------
    px = img.load()
    for j in range(H):
        x0, x1 = row_extent(j, W, H, c)
        for i in range(0, x0):
            px[i, j] = (0, 0, 0, 0)
        for i in range(x1 + 1, W):
            px[i, j] = (0, 0, 0, 0)

    return img.resize((spec.w, spec.h), Image.LANCZOS)


def main():
    rng = random.Random(SEED)
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))

    # ATLAS layout (2x texels; @spritesheet in theme.rcss must match; 4-texel gutters).
    # Row 0 (y=0):   plate 128x128 @ (0,0)   plate-amber @ (132,0)   rail 128x64 @ (264,0)
    # Row 1 (y=132): btn / btn-hi / btn-amber / well / socket / copper-btn / plate-sm, 64x64 every 68.
    sprites = {
        # gunmetal armor plate: chamfer 20, ring 32, bolts (fully opaque — solid alloy, no ghosting)
        (0, 0):    PlateSpec(128, 128, 20, 32, GUNMETAL, alpha=255, outline_px=3, bolts=True,
                             edge_light=(60, 66, 74), edge_dark=(15, 16, 19)),
        # amber-painted alert frame
        (132, 0):  PlateSpec(128, 128, 20, 32, GUNMETAL, alpha=255, outline_px=3, bolts=True,
                             edge_light=AMBER_HI, edge_dark=shade(AMBER, -60),
                             outline=shade(AMBER, -95)),
        # stamped header rail (darker band, no bolts, shallow ring)
        (264, 0):  PlateSpec(128, 64, 12, 16, (28, 31, 36), outline_px=2, ramp=1.3,
                             edge_light=(62, 68, 76), edge_dark=IRON_VOID, wear=0.7),
        # raised machined caps
        (0, 132):   PlateSpec(64, 64, 10, 16, (43, 47, 54), outline_px=2, wear=0.8),
        (68, 132):  PlateSpec(64, 64, 10, 16, (54, 60, 68), outline_px=2, wear=0.8,
                              edge_light=shade(STEEL_LITE, 20), edge_dark=(20, 22, 26)),
        (136, 132): PlateSpec(64, 64, 10, 16, (45, 49, 56), outline_px=2, wear=0.8,
                              edge_light=AMBER_HI, edge_dark=shade(AMBER, -70),
                              outline=shade(AMBER, -95)),
        # recessed trays
        (204, 132): PlateSpec(64, 64, 10, 16, (16, 17, 21), raised=False, outline_px=2,
                              edge_light=(58, 63, 70), edge_dark=(6, 7, 8), wear=0.5),
        (272, 132): PlateSpec(64, 64, 10, 16, (14, 15, 19), raised=False, outline_px=2,
                              edge_light=(51, 56, 63), edge_dark=(5, 6, 7), wear=0.5),
        # oxidized copper destructive control (bright top facet so the cap reads raised)
        (340, 132): PlateSpec(64, 64, 10, 16, COPPER, outline_px=2, wear=1.4, ramp=1.6,
                              edge_light=shade(COPPER_HI, 28), edge_dark=shade(COPPER, -45)),
        # light plate for small HUD readouts / tooltips (no bolts)
        (408, 132): PlateSpec(64, 64, 10, 16, GUNMETAL, alpha=255, outline_px=2, wear=0.9,
                              edge_light=(60, 66, 74), edge_dark=(15, 16, 19)),
    }

    for (x, y), spec in sprites.items():
        atlas.alpha_composite(draw_plate(spec, rng), (x, y))

    OUT.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(OUT)
    print(f"wrote {OUT} ({ATLAS_W}x{ATLAS_H}, 2x resolution)")


if __name__ == "__main__":
    main()
