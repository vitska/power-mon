"""
Generates the launcher icons from ic_launcher_source.png -- a full-bleed scenic/photo
image (glow, gradient background included), not a symbol-on-white mark. Edge to edge,
not inset into the adaptive-icon safe circle: an inset would show the launcher's own
background colour in the margin, which is exactly the seam a full-bleed image has none
of on its own.

    python android/art/make_icons.py

Writes, under app/src/main/res:
  - mipmap-*/ic_launcher_foreground.png  adaptive-icon foreground (Android 8+), 108 dp,
                                          the source scaled to COVER the whole layer
  - mipmap-*/ic_launcher.png             legacy square icon, rounded corners, 48 dp
  - mipmap-*/ic_launcher_round.png       legacy round icon, 48 dp
  - values/ic_launcher_background.xml    solid colour sampled from the source's own
                                          corners, so any sliver the mask exposes
                                          matches instead of showing white or black
The adaptive icon's XML is written once, by hand, in mipmap-anydpi-v26/.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'ic_launcher_source.png')
RES = os.path.join(HERE, '..', 'app', 'src', 'main', 'res')

DENSITIES = {'mdpi': 1.0, 'hdpi': 1.5, 'xhdpi': 2.0, 'xxhdpi': 3.0, 'xxxhdpi': 4.0}


def square_cover(img):
    """Center-cropped to a square: the shorter side, taken from the middle of the
    long side so an off-center subject (like this source's vertically-centered
    battery) is not clipped."""
    w, h = img.size
    side = min(w, h)
    left = (w - side) // 2
    top = (h - side) // 2
    return img.crop((left, top, left + side, top + side))


def masked(img, radius_frac):
    """Rounded corners (fraction of the side as the radius); 0.5 = circle."""
    size = img.size[0]
    mask = Image.new('L', (size * 4, size * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size * 4 - 1, size * 4 - 1), radius=int(size * 4 * radius_frac), fill=255)
    mask = mask.resize((size, size), Image.LANCZOS)  # 4x supersampled: smooth edge
    out = img.convert('RGBA').copy()
    out.putalpha(mask)
    return out


def corner_color(img):
    w, h = img.size
    pts = [(2, 2), (w - 3, 2), (2, h - 3), (w - 3, h - 3)]
    px = img.convert('RGB')
    r = sum(px.getpixel(p)[0] for p in pts) / 4
    g = sum(px.getpixel(p)[1] for p in pts) / 4
    b = sum(px.getpixel(p)[2] for p in pts) / 4
    return '#%02X%02X%02X' % (round(r), round(g), round(b))


def main():
    raw = Image.open(SRC).convert('RGBA')
    bg_hex = corner_color(raw)
    square = square_cover(raw)

    for name, k in DENSITIES.items():
        d = os.path.join(RES, 'mipmap-' + name)
        os.makedirs(d, exist_ok=True)

        # Adaptive foreground: fills the full 108 dp layer edge to edge (a full-bleed
        # image, not a mark that needs the 66 dp safe-circle inset).
        layer = int(108 * k)
        fg = square.resize((layer, layer), Image.LANCZOS)
        fg.save(os.path.join(d, 'ic_launcher_foreground.png'))

        # Legacy icons for launchers that ignore adaptive ones.
        side = int(48 * k)
        small = square.resize((side, side), Image.LANCZOS)
        masked(small, 0.18).save(os.path.join(d, 'ic_launcher.png'))
        masked(small, 0.5).save(os.path.join(d, 'ic_launcher_round.png'))
        print(name, layer, side)

    bg_path = os.path.join(RES, 'values', 'ic_launcher_background.xml')
    with open(bg_path, 'w', encoding='utf-8') as f:
        f.write(
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<resources>\n'
            f'    <color name="ic_launcher_background">{bg_hex}</color>\n'
            '</resources>\n'
        )
    print('background', bg_hex)


if __name__ == '__main__':
    main()
