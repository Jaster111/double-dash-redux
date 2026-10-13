"""Contact sheet of screenshots:  python tools/sheet.py OUT.png shots/a.png shots/b.png ...  (or a glob)"""
import glob
import sys
from PIL import Image, ImageDraw

out, pats = sys.argv[1], sys.argv[2:]
files = [f for p in pats for f in sorted(glob.glob(p))]
W, H, COLS = 400, 300, 4
rows = (len(files) + COLS - 1) // COLS
sheet = Image.new("RGB", (W * COLS, H * rows))
d = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert("RGB")
    im.thumbnail((W, H))
    x, y = (i % COLS) * W, (i // COLS) * H
    sheet.paste(im, (x, y))
    d.rectangle([x, y, x + 150, y + 14], fill=(0, 0, 0))
    d.text((x + 2, y + 1), f.replace("\\", "/").split("/")[-1], fill=(255, 255, 0))
sheet.save(out)
print(out, len(files), "images")
