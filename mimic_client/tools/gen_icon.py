#!/usr/bin/env python3
"""gen_icon.py — produce monitor_app/app.ico for the exe / taskbar icon.

One-time dev tool; the resulting app.ico is committed. Two paths:
  1. Rasterize monitor_web/public/favicon.svg via svglib+reportlab (pure Python,
     no native deps) → PNG → multi-size .ico.
  2. Fallback if svglib is unavailable: draw a brand-colored rounded tile with
     PIL so a build is always possible offline.

Run:  python tools/gen_icon.py    (cwd = monitor_app)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))          # monitor_app/tools
SVG = os.path.join(HERE, "..", "..", "monitor_web", "public", "favicon.svg")
OUT = os.path.join(HERE, "..", "app.ico")
SIZES = [(16, 16), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)]
BRAND = (134, 59, 255)  # #863bff — favicon primary


def from_svg():
    """Render favicon.svg → PIL RGBA Image at 256px. Raises if tooling missing."""
    from io import BytesIO
    from svglib.svglib import svg2rlg
    from reportlab.graphics import renderPM
    from PIL import Image

    drawing = svg2rlg(SVG)
    # scale drawing up to ~256px on its larger dimension
    scale = 256.0 / max(drawing.width, drawing.height)
    drawing.width *= scale
    drawing.height *= scale
    drawing.scale(scale, scale)
    png = renderPM.drawToString(drawing, fmt="PNG", bg=0xFFFFFF)
    img = Image.open(BytesIO(png)).convert("RGBA")
    # square-pad to 256x256, centered
    side = max(img.size)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(img, ((side - img.width) // 2, (side - img.height) // 2), img)
    return canvas.resize((256, 256), Image.LANCZOS)


def from_pil():
    """Fallback: brand rounded tile with a white 'G' — no external assets."""
    from PIL import Image, ImageDraw, ImageFont

    s = 256
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([8, 8, s - 8, s - 8], radius=48, fill=BRAND)
    try:
        font = ImageFont.truetype("segoeui.ttf", 150)
    except Exception:
        font = ImageFont.load_default()
    d.text((s / 2, s / 2 - 10), "G", font=font, fill=(255, 255, 255, 255), anchor="mm")
    return img


def main():
    try:
        base = from_svg()
        src = "svg"
    except Exception as e:
        print(f"gen_icon: svg path unavailable ({e.__class__.__name__}: {e}); using PIL fallback")
        base = from_pil()
        src = "pil-fallback"
    base.save(OUT, format="ICO", sizes=SIZES)
    print(f"gen_icon: wrote {os.path.relpath(OUT)} ({src}, sizes={[w for w, _ in SIZES]})")


if __name__ == "__main__":
    main()
