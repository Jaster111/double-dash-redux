"""README banner (docs/media/banner.jpg) over a 16:9 race frame from tools/scripts/media_race.txt.

  python tools/media/banner.py shots/media_race_060.png docs/media/banner.jpg 0.55
yfrac picks the vertical crop of the frame (0 = top, 1 = bottom).  Uses Windows' Segoe UI fonts.
"""
import sys
from PIL import Image, ImageDraw, ImageFilter, ImageFont

src, out = sys.argv[1], sys.argv[2]
yfrac = float(sys.argv[3]) if len(sys.argv) > 3 else 0.5
W, H = 1600, 480
F = "C:/Windows/Fonts/"

FW = 1150   # the frame fills the right part; the title sits on a dark panel at the left
im = Image.open(src).convert("RGB").resize((FW, round(FW * 9 / 16)), Image.LANCZOS)
top = int((im.height - H) * yfrac)
bg = Image.new("RGB", (W, H), (8, 14, 48))
bg.paste(im.crop((0, top, FW, top + H)), (W - FW, 0))
shade = Image.new("L", (W, H))
sd = ImageDraw.Draw(shade)
for x in range(W):
    t = min(1.0, max(0.0, 1 - (x - (W - FW)) / 520))
    sd.line([(x, 0), (x, H)], fill=int(60 + 195 * t ** 1.5))
dark = Image.new("RGB", (W, H), (8, 14, 48))
bg = Image.composite(dark, bg, shade)
d = ImageDraw.Draw(bg, "RGBA")
# checkered stripe along the bottom
sq = 16
for i in range(W // sq + 1):
    for j in range(2):
        if (i + j) % 2 == 0:
            d.rectangle([i * sq, H - 2 * sq + j * sq, i * sq + sq - 1, H - sq + j * sq - 1], fill=(255, 255, 255, 230))
        else:
            d.rectangle([i * sq, H - 2 * sq + j * sq, i * sq + sq - 1, H - sq + j * sq - 1], fill=(10, 10, 20, 230))


def shadow_text(xy, text, font, fill, off=5):
    layer = Image.new("RGBA", (W, H))
    ImageDraw.Draw(layer).text((xy[0] + off, xy[1] + off), text, font=font, fill=(0, 0, 20, 200))
    bg.paste(layer.filter(ImageFilter.GaussianBlur(4)), (0, 0), layer.filter(ImageFilter.GaussianBlur(4)))
    ImageDraw.Draw(bg).text(xy, text, font=font, fill=fill)


big = ImageFont.truetype(F + "seguibli.ttf", 116)
mid = ImageFont.truetype(F + "seguibli.ttf", 58)
small = ImageFont.truetype(F + "segoeuib.ttf", 30)

x0, y0 = 70, 70
shadow_text((x0, y0), "DOUBLE DASH", big, (255, 255, 255))
# REDUX in a red slanted pill
tw = ImageDraw.Draw(bg).textbbox((0, 0), "REDUX", font=mid)
pw, ph = tw[2] - tw[0] + 66, 84
pill = Image.new("RGBA", (pw + 40, ph + 20))
pd = ImageDraw.Draw(pill)
pd.rounded_rectangle([20, 10, 20 + pw, 10 + ph], radius=18, fill=(230, 30, 40), outline=(255, 210, 0), width=5)
pd.text((55 - tw[0], 10 + (ph - (tw[3] - tw[1])) // 2 - tw[1]), "REDUX", font=mid, fill=(255, 255, 255))
pill = pill.transform(pill.size, Image.AFFINE, (1, 0.18, -10, 0, 1, 0), Image.BICUBIC)
bg.paste(pill, (x0 + 4, y0 + 150), pill)
shadow_text((x0 + 8, y0 + 262), "A native PC port of Mario Kart: Double Dash!!", small, (255, 214, 0), off=3)
bg.save(out, quality=90, optimize=True)
print(out, bg.size)
