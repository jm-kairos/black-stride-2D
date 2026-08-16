#!/usr/bin/env python3
# =====================================================================================
# ui_icon_gen.py — procedural Space Western weapon/module emblem stencils for the HUD.
#
# Generates assets/ui/skin/icons.png: a 256x64 RGBA sheet of WHITE-on-transparent
# military-insignia emblems, tinted at runtime via RCSS `image-color` (ivory at rest,
# amber selected, dim for empty sockets). Consumed by the @spritesheet ui-icons block
# in assets/ui/theme.rcss — rectangles there MUST match the ATLAS layout below.
#
# Shape grammar (strict, per the art direction):
#   * rectilinear forms and 45-degree cuts ONLY — no arcs, no organic curves;
#   * 5-8 major shapes per emblem, symmetric about the vertical axis;
#   * minimum feature size 1 display px; negative space does half the work;
#   * every hardware emblem stands on the same notched base plate (family insignia cue).
#
# Crispness pipeline: authored on a 26-unit display grid with ALL coordinates snapped
# to 0.5-unit steps. At the 2x atlas (52px cells) a 0.5 unit is exactly 1 texel, so
# horizontal/vertical edges land on texel boundaries and stay razor sharp; the sheet is
# drawn at a further 4x supersample and BOX-downsampled, which anti-aliases only the
# 45-degree diagonals (16-sample SSAA) without smearing straight edges.
#
# Usage:  python tools\ui_icon_gen.py     (deterministic; then re-stage assets)
# =====================================================================================

from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent.parent / "assets" / "ui" / "skin" / "icons.png"

CELL = 52          # 2x-atlas cell size (26px display)
SS = 4             # supersample factor (16-sample box SSAA on diagonals)
S = CELL * SS      # draw-time canvas size
U = S / 26.0       # one display unit at draw scale (integer: 8)

ATLAS_W, ATLAS_H = 320, 64
SLOTS = {"ic-cannon": 0, "ic-pd": 56, "ic-sensor": 112, "ic-empty": 168, "ic-missile": 224}

CX = 13.0          # symmetry axis


def u(*vals):
    return [v * U for v in vals]


def new_mask():
    m = Image.new("L", (S, S), 0)
    return m, ImageDraw.Draw(m)


def base_plate(d):
    """Shared family cue: chamfered base plate with a centre index notch."""
    d.polygon(u(5.0, 21.5, 21.0, 21.5, 23.5, 24.0, 23.5, 25.0, 2.5, 25.0, 2.5, 24.0), fill=255)
    d.polygon(u(CX - 1.5, 25.0, CX + 1.5, 25.0, CX, 23.5), fill=0)


def icon_cannon():
    """Fixed cannon emplacement (Option-3 blueprint): stepped muzzle, barrel, chamfered
    breech, twin support legs, two cooling slots, notched base. 8 shapes."""
    m, d = new_mask()
    # stepped muzzle brake
    d.rectangle(u(10.0, 1.0, 16.0, 3.0), fill=255)
    # barrel
    d.rectangle(u(12.0, 3.0, 14.0, 9.5), fill=255)
    # breech block, 45-degree top corners
    d.polygon(u(10.0, 9.5, 16.0, 9.5, 17.5, 11.0, 17.5, 14.0, 8.5, 14.0, 8.5, 11.0), fill=255)
    # centre column carrying the breech to the base
    d.rectangle(u(9.5, 14.0, 16.5, 21.5), fill=255)
    # cooling slots cut into the column
    d.rectangle(u(10.5, 15.5, 15.5, 17.0), fill=0)
    d.rectangle(u(10.5, 18.5, 15.5, 20.0), fill=0)
    # support legs, outer-top 45-degree shoulders (clear negative gap to the column)
    d.polygon(u(3.5, 10.5, 5.5, 8.5, 7.5, 8.5, 7.5, 21.5, 3.5, 21.5), fill=255)
    d.polygon(u(22.5, 10.5, 20.5, 8.5, 18.5, 8.5, 18.5, 21.5, 22.5, 21.5), fill=255)
    base_plate(d)
    return m


def icon_pd():
    """Point defense: one squat half-octagon turret, single short barrel stub, one
    functional vent slot. 5 shapes — the simplest emblem in the set."""
    m, d = new_mask()
    # barrel stub with muzzle step
    d.rectangle(u(11.5, 2.0, 14.5, 7.0), fill=255)
    d.rectangle(u(10.5, 2.0, 15.5, 3.5), fill=255)
    # half-octagon dome (45-degree shoulders, no curves)
    d.polygon(u(4.5, 18.0, 4.5, 12.5, 9.5, 7.5, 16.5, 7.5, 21.5, 12.5, 21.5, 18.0), fill=255)
    # single horizontal vent slot
    d.rectangle(u(9.0, 12.5, 17.0, 14.0), fill=0)
    # seam between dome and base
    d.rectangle(u(4.0, 18.0, 22.0, 19.5), fill=0)
    # mount skirt (0.5 gap above the base plate keeps the masses separate)
    d.polygon(u(6.0, 19.5, 20.0, 19.5, 21.5, 21.0, 4.5, 21.0), fill=255)
    base_plate(d)
    return m


def icon_sensor():
    """Sensor array: solid shallow V-dish, feed pin with diamond emitter pip, tapered
    mast, notched base. 5 shapes."""
    m, d = new_mask()
    # diamond emitter pip
    d.polygon(u(CX, 0.5, CX + 2.0, 2.5, CX, 4.5, CX - 2.0, 2.5), fill=255)
    # feed pin dropping into the bowl
    d.rectangle(u(12.5, 4.0, 13.5, 8.5), fill=255)
    # solid shallow dish bowl (45-degree walls, no hollow -- reads radar, not glassware)
    d.polygon(u(3.5, 7.5, 22.5, 7.5, 17.0, 13.0, 9.0, 13.0), fill=255)
    # rim shadow line separating bowl from mast
    d.rectangle(u(9.0, 13.0, 17.0, 14.0), fill=0)
    # tapered mast
    d.polygon(u(12.0, 14.0, 14.0, 14.0, 15.0, 21.5, 11.0, 21.5), fill=255)
    base_plate(d)
    return m


def icon_empty():
    """Empty bay: open-cornered chamfered socket brackets + centre cross-drill."""
    m, d = new_mask()

    def bracket_frame(x0, y0, x1, y1, c, fill):
        d.polygon(u(x0 + c, y0, x1 - c, y0, x1, y0 + c, x1, y1 - c, x1 - c, y1,
                    x0 + c, y1, x0, y1 - c, x0, y0 + c), fill=fill)

    bracket_frame(4.5, 4.5, 21.5, 21.5, 2.0, 255)
    bracket_frame(6.5, 6.5, 19.5, 19.5, 1.5, 0)
    # open the edge midpoints -> four corner brackets
    d.rectangle(u(10.5, 3.5, 15.5, 7.5), fill=0)
    d.rectangle(u(10.5, 18.5, 15.5, 22.5), fill=0)
    d.rectangle(u(3.5, 10.5, 7.5, 15.5), fill=0)
    d.rectangle(u(18.5, 10.5, 22.5, 15.5), fill=0)
    # centre cross-drill
    d.rectangle(u(CX - 1.0, 9.5, CX + 1.0, 16.5), fill=255)
    d.rectangle(u(9.5, 12.0, 16.5, 14.0), fill=255)
    return m


def icon_missile():
    """Guided missile: finned dart, exhaust dashes, family base plate. 6 shapes."""
    m, d = new_mask()
    # nose cone (45-degree tip)
    d.polygon(u(CX, 1.0, CX + 2.5, 4.5, CX - 2.5, 4.5), fill=255)
    # body
    d.rectangle(u(CX - 2.5, 4.5, CX + 2.5, 14.0), fill=255)
    # body panel line
    d.rectangle(u(CX - 2.5, 8.0, CX + 2.5, 9.0), fill=0)
    # swept tail fins
    d.polygon(u(CX - 2.5, 10.5, CX - 6.5, 15.5, CX - 6.5, 17.5, CX - 2.5, 15.0), fill=255)
    d.polygon(u(CX + 2.5, 10.5, CX + 6.5, 15.5, CX + 6.5, 17.5, CX + 2.5, 15.0), fill=255)
    # exhaust dashes
    d.rectangle(u(CX - 1.5, 15.5, CX + 1.5, 17.0), fill=255)
    d.rectangle(u(CX - 1.0, 18.0, CX + 1.0, 19.5), fill=255)
    base_plate(d)
    return m


def main():
    atlas = Image.new("RGBA", (ATLAS_W, ATLAS_H), (0, 0, 0, 0))
    icons = {"ic-cannon": icon_cannon(), "ic-pd": icon_pd(),
             "ic-sensor": icon_sensor(), "ic-empty": icon_empty(),
             "ic-missile": icon_missile()}
    for name, mask in icons.items():
        small = mask.resize((CELL, CELL), Image.BOX)   # box SSAA: crisp edges, AA diagonals
        white = Image.new("RGBA", (CELL, CELL), (255, 255, 255, 0))
        white.putalpha(small)
        atlas.alpha_composite(white, (SLOTS[name], 4))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    atlas.save(OUT)
    print(f"wrote {OUT} ({ATLAS_W}x{ATLAS_H}, 2x resolution)")


if __name__ == "__main__":
    main()
