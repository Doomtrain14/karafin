"""Generate the app icons.

Run: python tools\\gen_icon.py

- tools/icon_source.png (square, opaque) -> assets/icon.jpg, the Homebrew Menu icon.
- tools/logo_source.png (square, transparent) -> romfs/logo.png for the splash screen and
  docs/images/icon.png for the README. The logo is cropped to its artwork plus a margin.
"""
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent

# The Homebrew Menu shrinks the 256 px icon to its small tiles with a plain 4-tap bilinear filter,
# which breaks up thin lines. Area-averaging plus a slight blur keeps those tiles smooth.
NRO_SIZE = 256
NRO_SOFTEN = 0.6
SPLASH_SIZE = 512
README_SIZE = 320  # shown at 160 px; 2x for high-DPI screens
LOGO_MARGIN = 0.06


def square(image):
    if image.width != image.height:
        raise ValueError("Icon sources must be square")
    return image


def crop_logo(image):
    left, top, right, bottom = image.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()
    side = max(right - left, bottom - top)
    side = int(side * (1 + 2 * LOGO_MARGIN))
    cx, cy = (left + right) // 2, (top + bottom) // 2
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(image.crop((cx - side // 2, cy - side // 2, cx - side // 2 + side, cy - side // 2 + side)))
    return canvas


def resize_rgba(image, size):
    # Resize premultiplied so transparent pixels don't bleed dark fringes into the edges.
    premul = image.convert("RGBa").resize((size, size), Image.Resampling.LANCZOS)
    return premul.convert("RGBA")


def main():
    with Image.open(ROOT / "tools" / "icon_source.png") as source:
        image = square(source.convert("RGB"))
        nro = image.resize((NRO_SIZE, NRO_SIZE), Image.Resampling.BOX)
        nro.filter(ImageFilter.GaussianBlur(NRO_SOFTEN)).save(
            ROOT / "assets" / "icon.jpg", quality=95, subsampling=0
        )

    with Image.open(ROOT / "tools" / "logo_source.png") as source:
        logo = crop_logo(square(source.convert("RGBA")))
        resize_rgba(logo, SPLASH_SIZE).save(ROOT / "romfs" / "logo.png", optimize=True)
        resize_rgba(logo, README_SIZE).save(ROOT / "docs" / "images" / "icon.png", optimize=True)


if __name__ == "__main__":
    main()