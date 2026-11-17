"""Generate the app icons.

Run: python tools\\gen_icon.py   (tools/make_brand.py runs it too)

tools/icon_source.png (square, opaque) -> assets/icon.jpg for the Homebrew Menu and
docs/images/icon.png for the README.
"""
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent

# The Homebrew Menu shrinks the 256 px icon to its small tiles with a plain 4-tap bilinear filter,
# which breaks up thin lines. Area-averaging plus a slight blur keeps those tiles smooth.
NRO_SIZE = 256
NRO_SOFTEN = 0.6
README_SIZE = 320  # shown at 160 px; 2x for high-DPI screens


def square(image):
    if image.width != image.height:
        raise ValueError("Icon sources must be square")
    return image


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
        resize_rgba(source.convert("RGBA"), README_SIZE).save(
            ROOT / "docs" / "images" / "icon.png", optimize=True
        )


if __name__ == "__main__":
    main()