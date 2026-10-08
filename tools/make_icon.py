"""Chunk 5.2: draws the app icon (res/wg3d.ico): a puck on centre ice. Original artwork, nothing from the ROM.

Each size is drawn at 4x and downsampled, so small sizes stay sharp. Run after changing the design;
the .ico is committed.

Usage: python tools/make_icon.py
"""
from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "res" / "wg3d.ico"
SIZES = [16, 24, 32, 48, 64, 128, 256]


def draw(size: int) -> Image.Image:
    s = size * 4
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    # Ice: rounded square, vertical gradient.
    grad = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(grad)
    top, bottom = (238, 246, 255), (176, 212, 242)
    for y in range(s):
        t = y / (s - 1)
        gd.line([(0, y), (s, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,))
    mask = Image.new("L", (s, s), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, s - 1, s - 1], radius=s // 6, fill=255)
    img.paste(grad, (0, 0), mask)
    d = ImageDraw.Draw(img)
    # Centre red line and the blue centre faceoff circle (skipped at 16px, where they'd be mush).
    if size >= 24:
        d.rectangle([0, s * 0.46, s, s * 0.54], fill=(206, 32, 41, 255))
        r = s * 0.36
        d.ellipse([s / 2 - r, s / 2 - r, s / 2 + r, s / 2 + r], outline=(28, 78, 170, 255), width=max(4, s // 28))
        img.putalpha(Image.composite(img.getchannel("A"), Image.new("L", (s, s), 0), mask))
        d = ImageDraw.Draw(img)
    # Puck: a flat cylinder seen from slightly above.
    w, h, depth = s * 0.62, s * 0.30, s * 0.13
    cx, cy = s / 2, s / 2 - depth / 2
    d.rectangle([cx - w / 2, cy, cx + w / 2, cy + depth], fill=(16, 16, 18, 255))
    d.ellipse([cx - w / 2, cy + depth - h / 2, cx + w / 2, cy + depth + h / 2], fill=(16, 16, 18, 255))
    d.ellipse([cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2], fill=(58, 58, 64, 255))
    # Highlight on the top face.
    hw, hh = w * 0.62, h * 0.42
    d.ellipse([cx - hw / 2, cy - h * 0.30 - hh / 2, cx + hw / 2, cy - h * 0.30 + hh / 2], fill=(92, 92, 100, 255))
    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    OUT.parent.mkdir(exist_ok=True)
    images = [draw(n) for n in SIZES]
    images[-1].save(OUT, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    images[-1].save(ROOT / "build" / "icon_preview.png")
    print(f"wrote {OUT.relative_to(ROOT)} ({', '.join(map(str, SIZES))})")


if __name__ == "__main__":
    main()
