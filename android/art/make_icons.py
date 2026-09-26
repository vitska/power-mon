"""
Generates the launcher icons from ic_launcher_source.png (a square image on white).

    python android/art/make_icons.py

Writes, under app/src/main/res:
  - mipmap-*/ic_launcher_foreground.png  adaptive-icon foreground (Android 8+), 108 dp
  - mipmap-*/ic_launcher.png             legacy square icon, rounded corners, 48 dp
  - mipmap-*/ic_launcher_round.png       legacy round icon, 48 dp
The adaptive icon's XML and its white background colour are written once, by hand,
in mipmap-anydpi-v26/ and values/.

Adaptive icons are masked by the launcher to any shape inside a 72 dp viewport of a
108 dp layer, and only a 66 dp circle is guaranteed to show. The image is therefore
scaled into that circle, not the full layer -- otherwise a round mask cuts the
batteries off.
"""
import os
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, 'ic_launcher_source.png')
RES = os.path.join(HERE, '..', 'app', 'src', 'main', 'res')

DENSITIES = {'mdpi': 1.0, 'hdpi': 1.5, 'xhdpi': 2.0, 'xxhdpi': 3.0, 'xxxhdpi': 4.0}


def on_white(img):
    bg = Image.new('RGBA', img.size, (255, 255, 255, 255))
    bg.alpha_composite(img)
    return bg


def masked(img, radius_frac):
    """The image with rounded corners (radius as a fraction of the side); 0.5 = circle."""
    size = img.size[0]
    mask = Image.new('L', (size * 4, size * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        (0, 0, size * 4 - 1, size * 4 - 1), radius=int(size * 4 * radius_frac), fill=255)
    mask = mask.resize((size, size), Image.LANCZOS)  # 4x supersampled: smooth edge
    out = img.copy()
    out.putalpha(mask)
    return out


def main():
    src = on_white(Image.open(SRC).convert('RGBA'))
    for name, k in DENSITIES.items():
        d = os.path.join(RES, 'mipmap-' + name)
        os.makedirs(d, exist_ok=True)

        # Adaptive foreground: 108 dp layer, image in the 66 dp safe circle (a square
        # of side 66/sqrt(2) would be the strict inscribed fit; 70 dp keeps the art
        # large while the batteries' white margin absorbs the rest).
        layer = int(108 * k)
        art = int(70 * k)
        fg = Image.new('RGBA', (layer, layer), (255, 255, 255, 0))
        fg.alpha_composite(src.resize((art, art), Image.LANCZOS), ((layer - art) // 2,) * 2)
        fg.save(os.path.join(d, 'ic_launcher_foreground.png'))

        # Legacy icons for launchers that ignore adaptive ones.
        side = int(48 * k)
        small = src.resize((side, side), Image.LANCZOS)
        masked(small, 0.18).save(os.path.join(d, 'ic_launcher.png'))
        masked(small, 0.5).save(os.path.join(d, 'ic_launcher_round.png'))
        print(name, layer, side)


if __name__ == '__main__':
    main()
