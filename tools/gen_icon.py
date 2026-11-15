"""Generate the NRO and splash icons from tools/icon_source.png.

Run: python tools\\gen_icon.py
The full supplied square artwork is retained without cropping.
"""
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent.parent


def main():
    with Image.open(ROOT / "tools" / "icon_source.png") as source:
        image = source.convert("RGB")
        if image.width != image.height:
            raise ValueError("The icon source must be square")
        image.resize((256, 256), Image.Resampling.LANCZOS).save(
            ROOT / "assets" / "icon.jpg", quality=95, subsampling=0
        )
        image.save(ROOT / "romfs" / "icon.jpg", quality=95, subsampling=0)


if __name__ == "__main__":
    main()
