"""Build the Karafin brand art, then the app icons.

Run: python tools\\make_brand.py   (needs Pillow, NumPy and SciPy)

Sources:
- tools/jelly_source.png: the jellyfish silhouette, upright, as a grayscale mask.
- tools/fonts/comfortaa-*.ttf (OFL, see Comfortaa-OFL.txt): the wordmark font. The wordmark is
  rendered exactly like Karastream's tools/make_wordmark.py (same font size, stroke and gap), with
  "fin" as the name and the jellyfish holding the stem of the i in place of its dot.

Outputs:
- tools/icon_source.png: the app icon (open frame mark above the wordmark), laid out like
  Karastream's so the wordmark has the same font size. tools/gen_icon.py then makes the
  Homebrew and README icons from it.
- romfs/frame.png, romfs/jelly.png, romfs/wordmark.png and src/screens/brand_art.hpp: the splash
  mark (open frame + the jellyfish, animated by the app) and wordmark.
- romfs/wordmark-ink.png: the wordmark in Karastream's dark ink colours, for white highlights.
"""
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont
from scipy.ndimage import map_coordinates

import gen_icon

ROOT = Path(__file__).resolve().parent.parent
FONTS = ROOT / "tools" / "fonts"
PURPLE = (169, 147, 255)
WHITE = (245, 245, 250)
# Karastream's romfs/wordmark-ink.png colours: the wordmark on a white list highlight.
INK = (11, 11, 18)
INK_ACCENT = (91, 75, 224)
# Karastream's tools/make_icon.py background: a violet glow behind the mark.
BG_BASE = np.array([14, 13, 22], float)
BG_GLOW = np.array([78, 58, 178], float)
GLOW_CENTER = (511, 400)
GLOW_RADIUS = 330
GLOW_FALLOFF = 2.4

SLANT = -30  # degrees, clockwise: the jellyfish leans right as in the original logo
MARK = 2048  # working size of the mark square
JELLY_HEIGHT = 1010  # slanted jellyfish height in the mark square

# Karastream's icon: 1024 px; its mark is ~580 px tall around y = 407; its "k" spans rows 773-858
# (measured on the final image, so the ascender's anti-aliased tip is included).
ICON = 1024
ICON_MARK_CONTENT = 580
ICON_MARK_CENTER_Y = 407
ICON_KARA_HEIGHT = 88
ICON_KARA_TOP = 772

# Splash: the mark square is cropped to its artwork plus this margin, so a 176 px mark matches
# Karastream's logo size. Rendered at twice the docked size (176 x 1.5) so the app always
# decodes it smaller, like Karastream's logo.
SPLASH_MARGIN = 0.08
SPLASH_PX = 528
JELLY_PAD = 0.03  # transparent margin around the upright jellyfish sprite, as a fraction of its height

# swim(): where the bell ends (fraction of the source height) and the tentacle wave (amplitude as a
# fraction of the width, and how many half-waves run down the tentacles). The app animates the
# same deformation, so these go into brand_art.hpp.
SWIM_RIM = 0.56
SWIM_WAVE = 0.05
SWIM_WAVE_TRAVEL = 2.2

# Karastream's tools/make_wordmark.py defaults: 4x supersampling, 160 px output, prefix stroke.
SS = 4
WORDMARK_HEIGHT = 160
PREFIX_WEIGHT = 0.011
# The jellyfish i, designed at font size 600 and scaled with the font: extra room before the i
# for the tentacles, the dot area removed, and the jellyfish's height and top above the baseline.
I_DESIGN_SIZE = 600
I_EXTRA_GAP = 30
I_STEM_PROBE = 330
I_DOT = (550, 350)
I_JELLY_HEIGHT = 178
I_JELLY_TOP = 485


def load_mask(name):
    return Image.open(ROOT / "tools" / name).convert("L")


def contraction(phase):
    """Bell contraction 0..1 over one stroke: a quick squeeze, then a slower relaxation."""
    squeeze = 0.35
    if phase < squeeze:
        return 0.5 - 0.5 * math.cos(math.pi * phase / squeeze)
    return 0.5 + 0.5 * math.cos(math.pi * (phase - squeeze) / (1 - squeeze))


def swim(mask, phase):
    """Deform the upright jellyfish for one animation phase in [0, 1).

    The bell narrows towards its rim and lengthens as it contracts; the tentacles follow the rim,
    stretch a little and carry a wave that travels down to their tips. The body stays in place.
    """
    src = np.asarray(mask, dtype=np.float32)
    h, w = src.shape
    pad = round(h * 0.08)
    oh, ow = h + 2 * pad, w + 2 * pad
    c = contraction(phase)
    rim = SWIM_RIM * h  # bell / tentacle boundary in the source
    cx = w / 2

    # Rows: forward map from source y to output y, inverted by interpolation.
    sy = np.arange(h, dtype=np.float32)
    bell_stretch = 1 + 0.07 * c
    tail_stretch = 1 + 0.08 * c
    fy = np.where(sy <= rim, sy * bell_stretch, rim * bell_stretch + (sy - rim) * tail_stretch)
    fy = fy - 0.012 * h * c + pad
    oy = np.arange(oh, dtype=np.float32)
    src_y = np.interp(oy, fy, sy, left=-10, right=h + 10)

    # Columns, per source row: squeeze about the axis, plus the tentacle wave.
    v = np.clip(src_y / rim, 0, 1)
    squeeze = 1 - 0.17 * c * v ** 1.5
    s = np.clip((src_y - rim) / (h - rim), 0, 1)
    wave = SWIM_WAVE * w * s ** 1.5 * np.sin(2 * math.pi * phase - SWIM_WAVE_TRAVEL * math.pi * s)
    ox = np.arange(ow, dtype=np.float32) - pad
    src_x = cx + (ox[None, :] - cx - wave[:, None]) / squeeze[:, None]
    coords = np.stack([np.broadcast_to(src_y[:, None], src_x.shape), src_x])
    out = map_coordinates(src, coords, order=1, cval=0)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))


def slanted(mask):
    return mask.rotate(SLANT, resample=Image.Resampling.BICUBIC, expand=True)


def jelly_layer(mask, rest_box):
    """The leaning jellyfish in the mark square, aligned so its rest pose fills `rest_box`."""
    lean = slanted(mask)
    left, top, right, bottom = rest_box["src"]
    k = JELLY_HEIGHT / (bottom - top)
    size = (round(lean.width * k), round(lean.height * k))
    lean = lean.resize(size, Image.Resampling.LANCZOS)
    layer = Image.new("L", (MARK, MARK))
    layer.paste(lean, (round(rest_box["x"] - left * k), round(rest_box["y"] - top * k)))
    return layer


def rest_box(mask):
    """Where the rest pose goes: its leaning bounds, centred in the mark square."""
    lean = slanted(swim(mask, 0.0))
    box = lean.point(lambda a: 255 if a > 8 else 0).getbbox()
    k = JELLY_HEIGHT / (box[3] - box[1])
    return {"src": box, "x": (MARK - (box[2] - box[0]) * k) / 2, "y": (MARK - JELLY_HEIGHT) / 2}


def frame_layers():
    """Open screen brackets (violet) and two square connection terminals (white with violet core)."""
    violet = Image.new("L", (MARK, MARK))
    white = Image.new("L", (MARK, MARK))
    dv, dw = ImageDraw.Draw(violet), ImageDraw.Draw(white)
    a, b = 320, 1728
    length = 365
    line_width = 56
    dv.rounded_rectangle((a, a, b, b), radius=160, outline=255, width=line_width)
    for gap in ((a + length, a - 1, b - length, a + line_width + 1),
                (a + length, b - line_width - 1, b - length, b + 1),
                (a - 1, a + length, a + line_width + 1, b - length),
                (b - line_width - 1, a + length, b + 1, b - length)):
        dv.rectangle(gap, fill=0)
    center_a, center_b = a + line_width / 2, b - line_width / 2
    for x, y in ((a + length, center_a), (b - length, center_a),
                 (a + length, center_b), (b - length, center_b),
                 (center_a, a + length), (center_a, b - length),
                 (center_b, a + length), (center_b, b - length)):
        r = line_width / 2
        dv.ellipse((round(x - r), round(y - r), round(x + r), round(y + r)), fill=255)
    for x, y in ((center_a, a + length), (center_b, b - length)):
        r, r2 = 66, 26
        dw.rounded_rectangle((x - r, y - r, x + r, y + r), radius=25, fill=255)
        dw.rounded_rectangle((x - r2, y - r2, x + r2, y + r2), radius=10, fill=0)
        dv.rounded_rectangle((x - r2, y - r2, x + r2, y + r2), radius=10, fill=255)
    return violet, white


def colorize(layers, size):
    image = Image.new("RGBA", size, (0, 0, 0, 0))
    for mask, color in layers:
        image.paste(color + (255,), (0, 0), mask)
    return image


def resize_rgba(image, size):
    # Premultiplied, so transparent pixels don't bleed dark fringes into the edges.
    return image.convert("RGBa").resize(size, Image.Resampling.LANCZOS).convert("RGBA")


def alpha_box(image):
    return image.getchannel("A").point(lambda a: 255 if a > 8 else 0).getbbox()


def text_mask(size, text, font, x, base, stroke=0):
    mask = Image.new("L", size)
    ImageDraw.Draw(mask).text((x, base), text, font=font, fill=255, anchor="ls",
                              stroke_width=stroke, stroke_fill=255)
    return mask


def make_wordmark(mask):
    """Karastream's make_wordmark.py --height 160 with "fin" as the name, the jellyfish on the i.

    Returns the supersampled image (for the icon), the image at Karastream's 160 px wordmark
    scale (for the app), and where that image's top goes relative to Karastream's.
    """
    size = WORDMARK_HEIGHT * SS
    reg = ImageFont.truetype(str(FONTS / "comfortaa-400.ttf"), size)
    bold = ImageFont.truetype(str(FONTS / "comfortaa-700.ttf"), size)
    stroke = max(0, round(size * PREFIX_WEIGHT))
    gap = int(size * 0.02) + 2 * stroke
    pad = size // 4
    base = size + pad
    k = size / I_DESIGN_SIZE
    i_gap = round(I_EXTRA_GAP * k)
    canvas = (int(reg.getlength("kara") + gap + bold.getlength("fin") + i_gap) + 2 * pad, size * 2)

    white = text_mask(canvas, "kara", reg, pad + stroke, base, stroke)
    name_x = pad + reg.getlength("kara") + gap
    accent = text_mask(canvas, "f", bold, name_x, base)
    ix = round(name_x + bold.getlength("f") + i_gap)
    accent.paste(255, (0, 0), text_mask(canvas, "in", bold, ix, base))
    iw = round(bold.getlength("i"))
    stem = accent.crop((ix, base - round(I_STEM_PROBE * k), ix + iw, base)).getbbox()
    stem_center = ix + (stem[0] + stem[2]) / 2
    ImageDraw.Draw(accent).rectangle((ix, base - round(I_DOT[0] * k), ix + iw, base - round(I_DOT[1] * k)), fill=0)
    jelly = mask.crop(mask.getbbox())
    jelly_h = round(I_JELLY_HEIGHT * k)
    jelly = jelly.resize((round(jelly.width * jelly_h / jelly.height), jelly_h), Image.Resampling.LANCZOS)
    accent.paste(255, (round(stem_center - jelly.width / 2), base - round(I_JELLY_TOP * k)), jelly)

    rgba = colorize([(white, WHITE), (accent, PURPLE)], canvas)
    ink = colorize([(white, INK), (accent, INK_ACCENT)], canvas)
    # Cropped like Karastream's: to the letters without the prefix stroke, plus a margin.
    plain = text_mask(canvas, "kara", reg, pad + stroke, base)
    plain.paste(255, (0, 0), accent)
    m = max(SS * 2, stroke + SS)
    l, t, r, b = plain.getbbox()
    big = rgba.crop((l - m, t - m, r + m, b + m))
    big_ink = ink.crop((l - m, t - m, r + m, b + m))
    # Karastream's scale: its "karastream" crop, made 160 px tall. Applying the same scale here keeps
    # the font size (and stroke) identical even if the jellyfish rises above the letters.
    ref = text_mask(canvas, "kara", reg, pad + stroke, base)
    ref.paste(255, (0, 0), text_mask(canvas, "stream", bold, name_x, base))
    rt, rb = ref.getbbox()[1], ref.getbbox()[3]
    ref_h = round((rb - rt + 2 * m) / SS)
    scale = WORDMARK_HEIGHT / ref_h
    def to_app_scale(image):
        image = image.resize((round(image.width / SS), round(image.height / SS)), Image.Resampling.LANCZOS)
        return image.resize((round(image.width * scale), round(image.height * scale)), Image.Resampling.LANCZOS)

    # Where this image's top goes relative to Karastream's wordmark top so the letters line up
    # (negative when the jellyfish rises above the ascenders).
    offset_y = (t - rt) / SS * scale
    return big, to_app_scale(big), to_app_scale(big_ink), offset_y


def glow_background():
    yy, xx = np.mgrid[0:ICON, 0:ICON]
    g = np.exp(-(np.hypot(xx - GLOW_CENTER[0], yy - GLOW_CENTER[1]) / GLOW_RADIUS) ** GLOW_FALLOFF)
    rgb = BG_BASE + g[..., None] * BG_GLOW
    return Image.fromarray(np.clip(rgb + 0.5, 0, 255).astype(np.uint8), "RGB").convert("RGBA")


def make_icon(mark, wordmark):
    icon = glow_background()
    box = alpha_box(mark)
    content = max(box[2] - box[0], box[3] - box[1])
    side = round(MARK * ICON_MARK_CONTENT / content)
    small = resize_rgba(mark, (side, side))
    k = side / MARK
    cx, cy = (box[0] + box[2]) / 2 * k, (box[1] + box[3]) / 2 * k
    icon.alpha_composite(small, (round(ICON / 2 - cx), round(ICON_MARK_CENTER_Y - cy)))
    px = np.asarray(wordmark)
    white = (px[..., :3].min(axis=2) > 200) & (px[..., 3] > 128)
    rows = np.nonzero(white.any(axis=1))[0]
    kara_top, kara_height = rows[0], rows[-1] - rows[0]
    k = ICON_KARA_HEIGHT / kara_height
    word = resize_rgba(wordmark, (round(wordmark.width * k), round(wordmark.height * k)))
    icon.alpha_composite(word, (round((ICON - word.width) / 2), round(ICON_KARA_TOP - kara_top * k)))
    icon.convert("RGB").save(ROOT / "tools" / "icon_source.png", optimize=True)


def make_splash(mask, rest, frame):
    # Crop the mark square to its artwork plus a margin, then scale to SPLASH_PX.
    jelly = jelly_layer(swim(mask, 0.0), rest)
    union = frame.getchannel("A").point(lambda a: 255 if a > 8 else 0)
    union.paste(255, (0, 0), jelly.point(lambda a: 255 if a > 8 else 0))
    l, t, r, b = union.getbbox()
    side = max(r - l, b - t) * (1 + 2 * SPLASH_MARGIN)
    cx, cy = (l + r) / 2, (t + b) / 2
    square = (round(cx - side / 2), round(cy - side / 2), round(cx + side / 2), round(cy + side / 2))
    side = square[2] - square[0]

    resize_rgba(frame.crop(square), (SPLASH_PX, SPLASH_PX)).save(ROOT / "romfs" / "frame.png", optimize=True)

    # The jellyfish upright and undeformed: the app runs swim() on it (in bands) and leans it.
    # jelly_layer() puts the centre of the leaning swim() image here, scaled by k.
    w, h = mask.size
    k = JELLY_HEIGHT / (rest["src"][3] - rest["src"][1])
    lean = slanted(swim(mask, 0.0))
    center = (rest["x"] - rest["src"][0] * k + lean.width * k / 2, rest["y"] - rest["src"][1] * k + lean.height * k / 2)
    scale = k / side  # mark-square fraction per source px
    pad = round(JELLY_PAD * h)
    sprite = Image.new("L", (w + 2 * pad, h + 2 * pad))
    sprite.paste(mask, (pad, pad))
    px = scale * SPLASH_PX  # sprite px per source px
    sprite = colorize([(sprite, PURPLE)], sprite.size)
    resize_rgba(sprite, (round(sprite.width * px), round(sprite.height * px))).save(ROOT / "romfs" / "jelly.png",
                                                                                  optimize=True)
    return {"cx": (center[0] - square[0]) / side, "cy": (center[1] - square[1]) / side, "scale": scale, "w": w,
            "h": h, "pad": pad}


def write_header(jelly, wordmark_size, offset_y):
    text = f"""#pragma once

// Generated by tools/make_brand.py; do not edit.
namespace karafin::screens::brand {{

// romfs/frame.png fills the mark square. romfs/jelly.png is the jellyfish upright and undeformed:
// kJellySrcW x kJellySrcH source px plus kJellyPad px of transparent margin on each side. Its
// source centre goes to (kJellyCX, kJellyCY) in the mark square, leaning kJellyLeanDeg clockwise,
// at kJellyScale mark-square units per source px.
inline constexpr float kJellyCX = {jelly["cx"]:.6f}f;
inline constexpr float kJellyCY = {jelly["cy"]:.6f}f;
inline constexpr float kJellyScale = {jelly["scale"]:.9f}f;
inline constexpr float kJellySrcW = {jelly["w"]}.0f;
inline constexpr float kJellySrcH = {jelly["h"]}.0f;
inline constexpr float kJellyPad = {jelly["pad"]}.0f;
inline constexpr float kJellyLeanDeg = {-SLANT:.1f}f;
// tools/make_brand.py swim(): the bell ends at kSwimRim of the height; the tentacle wave is
// kSwimWave of the width with kSwimWaveTravel half-waves down the tentacles. The icon shows phase 0.
inline constexpr float kSwimRim = {SWIM_RIM}f;
inline constexpr float kSwimWave = {SWIM_WAVE}f;
inline constexpr float kSwimWaveTravel = {SWIM_WAVE_TRAVEL}f;

// romfs/wordmark.png in px at Karastream's wordmark scale (its wordmark.png is kWordmarkRef px
// tall), and where its top goes relative to Karastream's wordmark top so the letters line up.
// Scale all three by (drawn Karastream wordmark height / kWordmarkRef).
inline constexpr float kWordmarkRef = {WORDMARK_HEIGHT}.0f;
inline constexpr float kWordmarkW = {wordmark_size[0]}.0f;
inline constexpr float kWordmarkH = {wordmark_size[1]}.0f;
inline constexpr float kWordmarkOffsetY = {offset_y:.2f}f;

}}  // namespace karafin::screens::brand
"""
    (ROOT / "src" / "screens" / "brand_art.hpp").write_text(text, newline="\n")


def main():
    mask = load_mask("jelly_source.png")
    rest = rest_box(mask)
    violet, white = frame_layers()
    frame = colorize([(violet, PURPLE), (white, WHITE)], (MARK, MARK))
    mark = colorize([(violet, PURPLE), (white, WHITE), (jelly_layer(swim(mask, 0.0), rest), PURPLE)],
                    (MARK, MARK))

    big, small, small_ink, offset_y = make_wordmark(mask)
    make_icon(mark, big)
    small.save(ROOT / "romfs" / "wordmark.png", optimize=True)
    small_ink.save(ROOT / "romfs" / "wordmark-ink.png", optimize=True)

    write_header(make_splash(mask, rest, frame), small.size, offset_y)
    gen_icon.main()


if __name__ == "__main__":
    main()
