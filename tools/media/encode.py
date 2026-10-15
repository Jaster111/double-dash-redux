"""Encode the README media into docs/media from the shots of tools/scripts/media_race.txt and
media_vs4p.txt (run from the project root, frame numbers picked by hand from those captures).

  python tools/media/encode.py [stills] [clips] [small]     small = only the two side-by-side clips, 640x360
The launcher picture comes from DD_LAUNCHER_SHOT (see runtime/host/launcher.cpp).
"""
import os
import sys
from PIL import Image

OUT = "docs/media"
os.makedirs(OUT, exist_ok=True)
W, H = (640, 360) if "small" in sys.argv else (800, 450)


def frame(prefix, i, size=(W, H)):
    return Image.open("shots/%s_%03d.png" % (prefix, i)).convert("RGB").resize(size, Image.LANCZOS)


def clip(name, prefix, first, last):
    fr = [frame(prefix, i) for i in range(first, last + 1)]
    p = os.path.join(OUT, name)
    fr[0].save(p, save_all=True, append_images=fr[1:], duration=40, loop=0, quality=58, method=5)
    print(p, len(fr), "frames", "%.1f MB" % (os.path.getsize(p) / 1e6))


def still(name, prefix, i):
    p = os.path.join(OUT, name)
    frame(prefix, i, (1280, 720)).save(p, quality=88, optimize=True)
    print(p, "%.0f KB" % (os.path.getsize(p) / 1e3))


what = sys.argv[1:] or ["stills", "clips"]
if "stills" in what:
    still("shot_wario.jpg", "media_race", 340)
    still("shot_chomp.jpg", "media_race", 545)
    still("shot_items.jpg", "media_race", 700)
    still("shot_4p.jpg", "media_vs4p", 440)
if "clips" in what:
    if "small" not in what:
        clip("race.webp", "media_race", 115, 265)
    clip("chomp.webp", "media_race", 465, 575)
    clip("splitscreen.webp", "media_vs4p", 0, 110)
