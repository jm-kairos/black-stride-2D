import sys
from PIL import Image, ImageDraw

src = Image.open(".texwork/source.jpg").convert("RGB")
W, H = src.size
print("SOURCE_SIZE", W, H)

# Draw a coordinate grid every 64 px with labels so we can pick crop boxes.
g = src.copy()
d = ImageDraw.Draw(g)
step = 64
for x in range(0, W, step):
    d.line([(x, 0), (x, H)], fill=(255, 0, 0), width=1)
    d.text((x + 2, 2), str(x), fill=(255, 255, 0))
for y in range(0, H, step):
    d.line([(0, y), (W, y)], fill=(255, 0, 0), width=1)
    d.text((2, y + 2), str(y), fill=(255, 255, 0))
g.save(".texwork/grid.png")
print("WROTE .texwork/grid.png")
