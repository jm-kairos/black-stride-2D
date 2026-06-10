import numpy as np
from PIL import Image, ImageDraw

src = Image.open(".texwork/source.jpg").convert("RGB")
W, H = src.size
arr = np.asarray(src).astype(np.float32)
P = 64       # patch size
STEP = 24    # sampling stride

cands = []
for y in range(0, H - P + 1, STEP):
    for x in range(0, W - P + 1, STEP):
        patch = arr[y:y+P, x:x+P]
        lum = 0.299*patch[...,0] + 0.587*patch[...,1] + 0.114*patch[...,2]
        std = float(lum.std())
        mr, mg, mb = (float(patch[...,c].mean()) for c in range(3))
        blue = mb - (mr + mg) / 2.0          # +ve => bluish
        cands.append((x, y, std, mr, mg, mb, blue))

# Uniform candidates (good for wall/hull): lowest luminance std.
uniform = sorted(cands, key=lambda c: c[2])[:24]
# Bluish candidates (good for window/glass): highest blue, but not too dark.
bluish = sorted([c for c in cands if (c[3]+c[4]+c[5])/3 > 40],
                key=lambda c: -c[6])[:8]

def sheet(rows, name, cols=6, label=lambda c: ""):
    n = len(rows)
    r = (n + cols - 1) // cols
    cell = P + 34
    sh = Image.new("RGB", (cols*cell, r*cell), (20, 20, 24))
    d = ImageDraw.Draw(sh)
    for i, c in enumerate(rows):
        x, y = c[0], c[1]
        px = (i % cols) * cell + 4
        py = (i // cols) * cell + 4
        sh.paste(src.crop((x, y, x+P, y+P)), (px, py))
        d.text((px, py+P+1),  f"{x},{y}", fill=(255, 255, 0))
        d.text((px, py+P+12), label(c), fill=(140, 230, 255))
    sh.save(name)
    print("WROTE", name, "n=", n)

sheet(uniform, ".texwork/cand_uniform.png",
      label=lambda c: f"s{c[2]:.0f} L{(c[3]+c[4]+c[5])/3:.0f}")
sheet(bluish, ".texwork/cand_bluish.png",
      label=lambda c: f"b{c[6]:+.0f} L{(c[3]+c[4]+c[5])/3:.0f}")

print("\nTOP UNIFORM (x,y,std,R,G,B):")
for c in uniform[:12]:
    print(f"  {c[0]:3d},{c[1]:3d}  std={c[2]:5.1f}  rgb=({c[3]:.0f},{c[4]:.0f},{c[5]:.0f})")
print("\nTOP BLUISH (x,y,blue,R,G,B):")
for c in bluish:
    print(f"  {c[0]:3d},{c[1]:3d}  blue={c[6]:+5.1f}  rgb=({c[3]:.0f},{c[4]:.0f},{c[5]:.0f})")
