"""Generate the NRO, splash and README icons from tools/icon_source.png.

Run: python tools\\gen_icon.py
The full supplied square artwork is retained without cropping.
"""
from pathlib import Path

from PIL import Image, ImageFilter

ROOT = Path(__file__).resolve().parent.parent

# The Homebrew Menu shrinks the 256 px icon to its small tiles with a plain 4-tap bilinear filter,
# which breaks up thin lines. Area-averaging plus a slight blur keeps those tiles smooth.
NRO_SIZE = 256
NRO_SOFTEN = 0.6
README_SIZE = 320  # shown at 160 px; 2x for high-DPI screens


def main():
    with Image.open(ROOT / "tools" / "icon_source.png") as source:
        image = source.convert("RGB")
        if image.width != image.height:
            raise ValueError("The icon source must be square")
        nro = image.resize((NRO_SIZE, NRO_SIZE), Image.Resampling.BOX)
        nro.filter(ImageFilter.GaussianBlur(NRO_SOFTEN)).save(
            ROOT / "assets" / "icon.jpg", quality=95, subsampling=0
        )
        image.save(ROOT / "romfs" / "icon.jpg", quality=95, subsampling=0)
        image.resize((README_SIZE, README_SIZE), Image.Resampling.BOX).save(
            ROOT / "docs" / "images" / "icon.png", optimize=True
        )


if __name__ == "__main__":
    main()
