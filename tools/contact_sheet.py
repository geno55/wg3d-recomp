"""Tiles a numbered capture series into one image with labels (debug helper).
Usage: python tools/contact_sheet.py build/ares_*.png -o build/ares_sheet.png [--cols 6] [--label-fmt '{i}s']"""
import argparse
import glob

from PIL import Image, ImageDraw

ap = argparse.ArgumentParser()
ap.add_argument("files", nargs="+")
ap.add_argument("-o", required=True)
ap.add_argument("--cols", type=int, default=6)
ap.add_argument("--width", type=int, default=256)
ap.add_argument("--label-fmt", default="{i}")
a = ap.parse_args()
files = sorted(f for pat in a.files for f in glob.glob(pat))
ims = [Image.open(f).convert("RGB") for f in files]
w = a.width
h = int(w * ims[0].height / ims[0].width)
rows = (len(ims) + a.cols - 1) // a.cols
sheet = Image.new("RGB", (a.cols * w, rows * h), (40, 40, 40))
d = ImageDraw.Draw(sheet)
for i, im in enumerate(ims):
    x, y = (i % a.cols) * w, (i // a.cols) * h
    sheet.paste(im.resize((w, h)), (x, y))
    d.rectangle([x, y, x + 34, y + 14], fill=(0, 0, 0))
    d.text((x + 2, y + 1), a.label_fmt.format(i=i), fill=(255, 255, 0))
sheet.save(a.o)
print(f"{len(ims)} images -> {a.o}")
