#!/usr/bin/env python3
"""
make_logo.py -- turns the Terpline logo into carta2/logo_strip.h: the strip
(flame mark + "Terpline" wordmark) drawn under the Carta 2 ramp chart.

The strip is a full-colour RGB565 image, streamed to the panel through the
stock LCD driver (see logo_blit in carta2/ramp_display.c). Both parts are cut
from the full-resolution logo, so the real gradients and the real wordmark come
through:
  - the badge's near-black background is clipped to true black, so the strip
    sits seamlessly on the ramp screen's black;
  - each part is scaled down in linear light (Lanczos), which keeps thin edges
    and the colour gradients right, then lightly sharpened for the tiny size;
  - the result is quantised to RGB565 with error diffusion, so the gradients
    don't band.

Usage:
    python3 tools/make_logo.py --mark mark.png --word wordmark.png [--preview strip.png]
(the shipped strip: --mark "terpline logo.png" --word "terpline logo text.png",
from terpline-web/public/icons)
    python3 tools/make_logo.py --logo logo.png [--preview strip.png]
--mark / --word: hand-cut art with a transparent background (preferred; the
alpha gives clean edges on black). --logo: the 1254 x 1254 master, cut
automatically (the badge ring and swoosh sit close to the wordmark).
"""
import argparse
from pathlib import Path

from PIL import Image, ImageFilter

REPO = Path(__file__).resolve().parent.parent

STRIP_Y = 202                       # drawn at y 202-224 on the device
STRIP_H = 23
LEFT = 6                            # the screen's common left edge
GAP = 5                             # between the mark and the wordmark
MARK_H = 23                         # flame mark height, px
WORD_H = 21                         # wordmark height (cap top to descender), px

# regions of the 1254 px master, as fractions so a resized master still works
MARK_BOX = (385 / 1254, 122 / 1254, 868 / 1254, 728 / 1254)
WORD_BOX = (120 / 1254, 728 / 1254, 1100 / 1254, 950 / 1254)
WORD_BASELINE = 900 / 1254          # below it only the p's descender belongs to the wordmark
SHARPEN = 0                         # unsharp percent; 0 = none (it rings on the emboss shadow)
BLACK_POINT = 30                    # the badge background is about rgb(11, 15, 21)
TRIM = 48                           # a pixel this bright belongs to the art (for the tight crop)


def to_linear(v):
    v /= 255.0
    return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4


def to_srgb(v):
    v = min(max(v, 0.0), 1.0)
    return 255.0 * (12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055)


LIN = [to_linear(float(i)) for i in range(256)]


def drop_below(im, cut):
    """Below row `cut`, keep only each column's art that runs on unbroken from
    above it (the p's descender), and black out the rest (the swoosh)."""
    px = im.load()
    for x in range(im.width):
        y = cut
        while y < im.height and max(px[x, y]) >= TRIM:
            y += 1
        for yy in range(y, im.height):
            px[x, yy] = (0, 0, 0)


def part(master, box, out_h, baseline=None):
    """Crop a region, clip the background to black, trim to the art, and scale
    it to out_h rows in linear light."""
    w, h = master.size
    im = master.crop(tuple(int(round(f * s)) for f, s in zip(box, (w, h, w, h))))
    im = im.point(lambda v: max(0, v - BLACK_POINT) * 255 // (255 - BLACK_POINT))
    if baseline is not None:
        drop_below(im, int(round(baseline * h)) - int(round(box[1] * h)))
    bbox = im.convert("L").point(lambda v: 255 if v >= TRIM else 0).getbbox()
    return scale(im.crop(bbox).convert("RGBA"), out_h)


def cut_out(path, out_h):
    """Hand-cut art with a transparent background, trimmed to its alpha."""
    im = Image.open(path).convert("RGBA")
    return scale(im.crop(im.getchannel("A").getbbox()), out_h)


def scale(im, out_h):
    """RGBA art onto black, scaled to out_h rows in linear light (each pixel
    weighted by its alpha, so edges blend into the black cleanly)."""
    out_w = max(1, round(im.width * out_h / im.height))
    alpha = im.getchannel("A").load()
    chans = []
    for c in im.convert("RGB").split():                     # linear light, per channel
        lin = Image.new("F", c.size)
        src, dst = c.load(), lin.load()
        for y in range(c.height):
            for x in range(c.width):
                dst[x, y] = LIN[src[x, y]] * alpha[x, y] / 255.0
        small = lin.resize((out_w, out_h), Image.LANCZOS)
        out = Image.new("L", small.size)
        sp, op = small.load(), out.load()
        for y in range(out_h):
            for x in range(out_w):
                op[x, y] = int(round(to_srgb(sp[x, y])))
        chans.append(out)
    rgb = Image.merge("RGB", chans)
    if SHARPEN:
        rgb = rgb.filter(ImageFilter.UnsharpMask(radius=0.6, percent=SHARPEN, threshold=0))
    return rgb


def baseline_row(word):
    """The row most letters end on: the commonest lowest-ink row over the
    columns (the p's descender is the exception)."""
    px = word.load()
    bottoms = {}
    for x in range(word.width):
        ys = [y for y in range(word.height) if max(px[x, y]) >= 96]
        if ys:
            bottoms[ys[-1]] = bottoms.get(ys[-1], 0) + 1
    return max(bottoms, key=bottoms.get)


def quantise(img):
    """RGB565 with Floyd-Steinberg error diffusion; pure black stays 0."""
    w, h = img.size
    px = [[list(map(float, img.getpixel((x, y)))) for x in range(w)] for y in range(h)]
    out = [[0] * w for _ in range(h)]
    bits = (5, 6, 5)
    for y in range(h):
        for x in range(w):
            old = px[y][x]
            q, err = [], []
            for v, b in zip(old, bits):
                v = min(max(v, 0.0), 255.0)
                levels = (1 << b) - 1
                n = int(round(v * levels / 255.0))
                q.append(n)
                err.append(v - n * 255.0 / levels)
            out[y][x] = (q[0] << 11) | (q[1] << 5) | q[2]
            for dx, dy, f in ((1, 0, 7 / 16), (-1, 1, 3 / 16), (0, 1, 5 / 16), (1, 1, 1 / 16)):
                xx, yy = x + dx, y + dy
                if 0 <= xx < w and yy < h:
                    for k in range(3):
                        px[yy][xx][k] += err[k] * f
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logo", help="the master logo, cut automatically")
    ap.add_argument("--mark", help="the flame mark, hand-cut, transparent background")
    ap.add_argument("--word", help="the wordmark, hand-cut, transparent background")
    ap.add_argument("--preview")
    a = ap.parse_args()
    if not (a.mark and a.word) and not a.logo:
        ap.error("give --mark and --word, or --logo")

    master = Image.open(a.logo).convert("RGB") if a.logo else None
    mark = cut_out(a.mark, MARK_H) if a.mark else part(master, MARK_BOX, MARK_H)
    word = cut_out(a.word, WORD_H) if a.word else part(master, WORD_BOX, WORD_H, WORD_BASELINE)

    strip_w = mark.width + GAP + word.width
    strip = Image.new("RGB", (strip_w, STRIP_H))
    strip.paste(mark, (0, (STRIP_H - MARK_H) // 2))
    word_top = STRIP_H - WORD_H              # the descender ends on the strip's last row
    strip.paste(word, (mark.width + GAP, word_top))
    baseline = word_top + baseline_row(word)   # the dab count sits on this row too
    pix = quantise(strip)

    # "DABS" label for the dab counter, a hand-drawn 5 x 7 pixel font so it
    # stays crisp at this size; drawn by the firmware just left of the count
    glyphs = {
        "D": ["11110", "10001", "10001", "10001", "10001", "10001", "11110"],
        "A": ["01110", "10001", "10001", "11111", "10001", "10001", "10001"],
        "B": ["11110", "10001", "10001", "11110", "10001", "10001", "11110"],
        "S": ["01111", "10000", "10000", "01110", "00001", "00001", "11110"],
    }
    label = set()
    for k, ch in enumerate("DABS"):
        for y, row in enumerate(glyphs[ch]):
            for x, bit in enumerate(row):
                if bit == "1":
                    label.add((k * 6 + x, y))
    label_w, label_h = 4 * 6 - 1, 7
    label_runs = []
    for y in range(label_h):
        x = 0
        while x < label_w:
            if (x, y) not in label:
                x += 1
                continue
            n = 1
            while (x + n, y) in label:
                n += 1
            label_runs.append((x, n, y))
            x += n

    lines = [
        "/* logo_strip.h -- generated by tools/make_logo.py from the Terpline logo; do not edit.",
        " * The flame mark + \"Terpline\" wordmark as an RGB565 image, row by row, each",
        " * pixel high byte first (the order the panel takes it). */",
        f"#define LOGO_X      {LEFT}",
        f"#define LOGO_Y      {STRIP_Y}",
        f"#define LOGO_PW     {strip_w}   /* image width, px */",
        f"#define LOGO_H      {STRIP_H}",
        f"#define LOGO_W      {LEFT + strip_w}   /* the logo ends here; the dab count is right of it */",
        f"#define LOGO_BASELINE {baseline}   /* strip row the wordmark's letters stand on */",
        f"static const u8 LOGO_PIX[{strip_w * STRIP_H * 2}] = {{",
    ]
    flat = [v for row in pix for v in row]
    for i in range(0, len(flat), 12):
        lines.append("    " + " ".join(f"0x{v >> 8:02X},0x{v & 0xFF:02X}," for v in flat[i:i + 12]))
    lines.append("};")
    lines += [
        f"#define LABEL_W     {label_w}",
        f"#define LABEL_H     {label_h}",
        f"#define LABEL_RUNS  {len(label_runs)}",
        "static const u8 LABEL_RUN[LABEL_RUNS][3] = {   /* \"DABS\": {x, length, row} */",
    ]
    for i in range(0, len(label_runs), 8):
        lines.append("    " + " ".join(f"{{{x},{n},{y}}}," for x, n, y in label_runs[i:i + 8]))
    lines.append("};")
    (REPO / "carta2" / "logo_strip.h").write_text("\n".join(lines) + "\n", newline="\n")
    print(f"logo_strip.h: {strip_w} x {STRIP_H} px, {strip_w * STRIP_H * 2} bytes; "
          f"mark {mark.width}px + wordmark {word.width}x{WORD_H}px")

    if a.preview:
        img = Image.new("RGB", (strip_w, STRIP_H))
        for y in range(STRIP_H):
            for x in range(strip_w):
                v = pix[y][x]
                img.putpixel((x, y), (((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31))
        img.resize((strip_w * 8, STRIP_H * 8), Image.NEAREST).save(a.preview)


if __name__ == "__main__":
    main()
