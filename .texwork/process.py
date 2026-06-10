import numpy as np
from PIL import Image, ImageFilter

src = Image.open(".texwork/source.jpg").convert("RGB")
OUT = 256  # final texture size (power of two)

# (name, x0,y0,x1,y1, target_mean_luma, saturation_scale, contrast)
JOBS = [
    ("wall_texture",   144, 312, 208, 376, 188, 0.50, 1.32),
    ("hull_texture",     8, 392,  72, 456, 202, 0.55, 1.22),
    ("window_texture", 120, 100, 184, 164, 170, 1.12, 1.10),
    ("door_texture",   168, 520, 232, 584, 200, 0.70, 1.18),
]

def make_seamless(im):
    """Offset by half in both axes, then feather-blend the cross seams that
    land in the image center. Guarantees the outer edges already wrap (offset
    moved them inward) and kills the central seam with a soft mask."""
    a = np.asarray(im).astype(np.float32)
    h, w = a.shape[:2]
    # toroidal half-shift: now the ORIGINAL seam is at center, edges are seamless
    off = np.roll(np.roll(a, h // 2, axis=0), w // 2, axis=1)
    # feathered cross mask over the central seam lines — keep the band NARROW
    # so we only hide the 1-2px discontinuity and preserve panel detail.
    yy = np.abs(np.arange(h) - h / 2.0)
    xx = np.abs(np.arange(w) - w / 2.0)
    band = max(3, h // 32)
    my = np.clip(1.0 - yy[:, None] / band, 0, 1)
    mx = np.clip(1.0 - xx[None, :] / band, 0, 1)
    seam = np.maximum(my, mx)[..., None]  # 1 on the seam, 0 away
    # blur a copy and composite it over the seam to hide the discontinuity
    blur = np.asarray(Image.fromarray(off.astype(np.uint8)).filter(
        ImageFilter.GaussianBlur(band / 1.5))).astype(np.float32)
    out = off * (1 - seam) + blur * seam
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))

def grade(im, target_luma, sat_scale, contrast):
    a = np.asarray(im).astype(np.float32)
    lum = 0.299*a[...,0] + 0.587*a[...,1] + 0.114*a[...,2]
    # desaturate toward luma (tint provides the hue in-engine)
    a = lum[..., None] + (a - lum[..., None]) * sat_scale
    # normalize mean luminance to target (bright detail map for clean tinting)
    m = max(1.0, lum.mean())
    a *= (target_luma / m)
    # per-job S-curve contrast around mid to keep panel detail crisp
    a = np.clip(a, 0, 255) / 255.0
    a = np.clip((a - 0.5) * contrast + 0.5, 0, 1) * 255.0
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))

results = []
for name, x0, y0, x1, y1, tl, ss, ct in JOBS:
    crop = src.crop((x0, y0, x1, y1)).resize((OUT, OUT), Image.LANCZOS)
    crop = make_seamless(crop)
    crop = grade(crop, tl, ss, ct)
    crop = crop.convert("RGBA")  # engine wants RGBA8
    path = f".texwork/out_{name}.png"
    crop.save(path)
    a = np.asarray(crop)
    results.append((name, path, a[...,:3].mean()))
    print(f"WROTE {path}  size={crop.size} mean_luma={a[...,:3].mean():.0f}")

# Build a preview: each texture tiled 2x2 + shown under its in-engine tint.
tints = {
    "wall_texture":   (0.38, 0.40, 0.45),
    "hull_texture":   (0.56, 0.61, 0.70),
    "window_texture": (0.50, 0.75, 0.85),
    "door_texture":   (0.85, 0.72, 0.20),
}
CELL = 128
pad = 16
preview = Image.new("RGB", (4*(CELL+pad)+pad, 2*CELL+3*pad+20), (24,24,28))
from PIL import ImageDraw
d = ImageDraw.Draw(preview)
for i,(name,path,_) in enumerate(results):
    tex = Image.open(path).convert("RGB").resize((CELL//2, CELL//2), Image.LANCZOS)
    # 2x2 tile to expose any seam
    tiled = Image.new("RGB", (CELL, CELL))
    for ty in range(2):
        for tx in range(2):
            tiled.paste(tex, (tx*CELL//2, ty*CELL//2))
    x = pad + i*(CELL+pad)
    preview.paste(tiled, (x, pad+10))
    # tinted version
    t = tints[name]
    ta = np.asarray(tiled).astype(np.float32) * np.array(t)
    tint_img = Image.fromarray(np.clip(ta,0,255).astype(np.uint8))
    preview.paste(tint_img, (x, pad+10+CELL+pad))
    d.text((x, 0), name.replace("_texture",""), fill=(255,255,0))
    d.text((x, pad+10+CELL+2), "x tint:", fill=(150,220,255))
preview.save(".texwork/preview.png")
print("WROTE .texwork/preview.png (top: 2x2 tiled raw, bottom: x in-engine tint)")
