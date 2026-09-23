"""make_icon.py - dibuja el icono de image2png: un prisma que abre un haz blanco en un abanico
pastel, sobre fondo transparente.

    python assets/make_icon.py        -> src/tools/image2png/image2png.ico (16 a 256 px)

Cada tamaño se dibuja APARTE a 8x y se reduce con Lanczos: un icono de 16 px no es la version de
256 achicada (quedaria borroso), lleva trazos mas gruesos y menos detalle. Los tamaños chicos
se ajustan a la grilla de pixeles para que los bordes caigan enteros.
"""
from __future__ import annotations

import math
from pathlib import Path

from PIL import Image, ImageDraw

OUT = Path(__file__).resolve().parent.parent / "src" / "tools" / "image2png" / "image2png.ico"
SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
SUPERSAMPLE = 8

# Catppuccin claro: el abanico de rojo a violeta, el vidrio lavanda, el haz casi blanco
FAN = [(243, 139, 168), (250, 179, 135), (249, 226, 175), (166, 227, 161), (137, 220, 235), (137, 180, 250), (203, 166, 247)]
GLASS_EDGE = (205, 214, 244, 255)
GLASS_FILL = (180, 190, 254, 70)
BEAM = (245, 245, 250, 255)


def draw(size: int) -> Image.Image:
    s = size * SUPERSAMPLE
    im = Image.new("RGBA", (s, s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    small = size <= 24
    # trazo: ~1,3 px en los chicos (legible), mas fino en proporcion en los grandes
    edge = (1.3 if small else max(1.0, size / 30)) * SUPERSAMPLE

    # prisma: triangulo equilatero apoyado; en los chicos llena casi todo el lienzo
    cx, cy, r = s * 0.42, s * 0.58, s * (0.46 if small else 0.40)
    tri = [(cx + r * math.cos(math.radians(a)), cy + r * math.sin(math.radians(a))) for a in (-90, 30, 150)]

    # abanico: sale del lado derecho del prisma hacia el borde derecho, abriendose
    exit_pt = ((tri[0][0] + tri[1][0]) / 2, (tri[0][1] + tri[1][1]) / 2)
    bands = FAN if size >= 32 else FAN[::2]
    spread = s * (0.56 if small else 0.46)
    top, bottom = cy - spread / 2 - s * 0.06, cy + spread / 2 - s * 0.06
    step = (bottom - top) / len(bands)
    for i, color in enumerate(bands):
        y0, y1 = top + i * step, top + (i + 1) * step
        d.polygon([exit_pt, (s * 0.98, y0), (s * 0.98, y1 + 0.5 * SUPERSAMPLE)], fill=color + (255,))

    # haz de entrada desde la izquierda hasta la cara izquierda del prisma
    entry = ((tri[0][0] + tri[2][0]) / 2, (tri[0][1] + tri[2][1]) / 2)
    beam_w = max(edge * 1.2, s * 0.045)
    d.line([(s * 0.02, entry[1] + s * 0.10), entry], fill=BEAM, width=int(beam_w))
    # recorrido interno del haz (tenue)
    d.line([entry, exit_pt], fill=(255, 255, 255, 120), width=int(beam_w * 0.7))

    # vidrio encima del abanico: relleno translucido y borde
    d.polygon(tri, fill=GLASS_FILL)
    d.line(tri + [tri[0]], fill=GLASS_EDGE, width=int(edge), joint="curve")

    return im.resize((size, size), Image.LANCZOS)


def main() -> None:
    images = [draw(size) for size in SIZES]
    OUT.parent.mkdir(parents=True, exist_ok=True)
    images[-1].save(OUT, format="ICO", sizes=[(s, s) for s in SIZES], append_images=images[:-1])
    preview = Image.new("RGBA", (sum(SIZES) + 12 * len(SIZES), 256 + 24), (17, 17, 27, 255))
    x = 12
    for im in images:
        preview.alpha_composite(im, (x, 12 + (256 - im.height) // 2))
        x += im.width + 12
    preview.save(Path(__file__).resolve().parent / "icono-muestra.png")
    print(f"{OUT} ({OUT.stat().st_size} bytes, {len(SIZES)} tamaños)")


if __name__ == "__main__":
    main()
