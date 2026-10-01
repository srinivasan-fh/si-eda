#!/usr/bin/env python3
"""Renders the SiEDA app icon (blue chip on a navy squircle) into the asset catalog.

Usage: python3 tools/make_icon.py   (requires Pillow)
"""
import json
import os

from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "SiEDA", "Assets.xcassets", "AppIcon.appiconset")
S = 1024


def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(len(a)))


def render():
    img = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    # Background squircle with vertical blue gradient.
    grad = Image.new("RGBA", (S, S))
    gd = ImageDraw.Draw(grad)
    top, bottom = (22, 52, 120, 255), (6, 12, 34, 255)
    for y in range(S):
        gd.line([(0, y), (S, y)], fill=lerp(top, bottom, y / S))
    mask = Image.new("L", (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle([100, 100, S - 100, S - 100], radius=185, fill=255)
    img.paste(grad, (0, 0), mask)

    d = ImageDraw.Draw(img)
    sky = (125, 212, 252, 255)
    blue = (51, 120, 245, 255)
    ice = (205, 232, 255, 255)

    # Circuit traces with glow.
    glow = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    g = ImageDraw.Draw(glow)
    traces = [
        [(200, 360), (330, 360), (380, 410)],
        [(200, 664), (330, 664), (380, 614)],
        [(824, 512), (700, 512)],
        [(512, 200), (512, 330)],
        [(512, 824), (512, 694)],
    ]
    for t in traces:
        g.line(t, fill=sky, width=26, joint="curve")
    glow = glow.filter(ImageFilter.GaussianBlur(14))
    img.alpha_composite(glow)
    for t in traces:
        d.line(t, fill=sky, width=16, joint="curve")
        x, y = t[0]
        d.ellipse([x - 22, y - 22, x + 22, y + 22], fill=ice)

    # Chip body with pins.
    d.rounded_rectangle([350, 350, 674, 674], radius=46, fill=(12, 30, 72, 255), outline=blue, width=18)
    for i in range(4):
        o = 400 + i * 75
        d.rounded_rectangle([o - 14, 312, o + 14, 350], radius=6, fill=sky)
        d.rounded_rectangle([o - 14, 674, o + 14, 712], radius=6, fill=sky)
        d.rounded_rectangle([312, o - 14, 350, o + 14], radius=6, fill=sky)
        d.rounded_rectangle([674, o - 14, 712, o + 14], radius=6, fill=sky)
    # AI spark in the die.
    cx, cy, r = 512, 512, 92
    d.polygon([(cx, cy - r), (cx + 22, cy - 22), (cx + r, cy), (cx + 22, cy + 22),
               (cx, cy + r), (cx - 22, cy + 22), (cx - r, cy), (cx - 22, cy - 22)], fill=ice)
    return img


def main():
    os.makedirs(OUT, exist_ok=True)
    base = render()
    images = []
    for size in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = size * scale
            name = f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"
            base.resize((px, px), Image.LANCZOS).save(os.path.join(OUT, name))
            images.append({"idiom": "mac", "size": f"{size}x{size}", "scale": f"{scale}x", "filename": name})
    with open(os.path.join(OUT, "Contents.json"), "w") as f:
        json.dump({"images": images, "info": {"author": "xcode", "version": 1}}, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
