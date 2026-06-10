from PIL import Image, ImageDraw

src = Image.open(".texwork/source.jpg").convert("RGB")

# Targeted candidate regions per material: (label, x0,y0,x1,y1)
regions = [
    # WALL — interior structural grey
    ("WALL-a deck",   144, 312, 208, 376),
    ("WALL-b struct", 128, 576, 192, 640),
    ("WALL-c struct", 152, 568, 216, 632),
    # HULL — exterior armor plating / side wings
    ("HULL-a Lwing",    8, 264,  72, 328),
    ("HULL-b Lwing",    8, 392,  72, 456),
    ("HULL-c Rwing",  322, 264, 386, 328),
    ("HULL-d deck",    96, 360, 160, 424),
    # WINDOW — blue console / glass
    ("WIN-a console", 112,  88, 176, 152),
    ("WIN-b console", 120, 100, 184, 164),
    ("WIN-c bridge",   72, 240, 136, 304),
    # DOOR — airlock hatch / louvered vent
    ("DOOR-a hatchM", 184, 244, 248, 308),
    ("DOOR-b hatchL", 168, 520, 232, 584),
    ("DOOR-c ventL",   88, 508, 152, 540),
    ("DOOR-d ventR",  240, 508, 304, 540),
]

cols = 4
DISP = 112
cell_w, cell_h = DISP + 8, DISP + 30
rows = (len(regions) + cols - 1) // cols
sheet = Image.new("RGB", (cols*cell_w, rows*cell_h), (18, 18, 22))
d = ImageDraw.Draw(sheet)
for i, (lab, x0, y0, x1, y1) in enumerate(regions):
    crop = src.crop((x0, y0, x1, y1)).resize((DISP, DISP), Image.NEAREST)
    px = (i % cols) * cell_w + 4
    py = (i // cols) * cell_h + 4
    sheet.paste(crop, (px, py))
    d.text((px, py+DISP+1),  lab, fill=(255, 255, 0))
    d.text((px, py+DISP+13), f"{x0},{y0},{x1},{y1}", fill=(150, 220, 255))
sheet.save(".texwork/candidates.png")
print("WROTE .texwork/candidates.png", sheet.size, "regions:", len(regions))
