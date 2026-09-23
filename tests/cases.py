"""cases.py - catalogo de pruebas de image2png.

Cada caso fabrica su entrada en una carpeta propia y dice que tiene que pasar:
  truth   la imagen RGBA exacta que tiene que salir (el "oraculo": con eso se genero la entrada)
  pillow  comparar con lo que Pillow decodifica de la MISMA entrada (formatos con perdida o ajenos)
  check   una verificacion a medida (propiedades: sin halo, tamaño, chunks, cantidad de archivos...)
  fail    la conversion tiene que fallar con un mensaje que contenga ese texto
Los pixeles con alfa 0 no comparan RGB (es invisible) salvo que el caso lo pida.
"""
from __future__ import annotations

import io
import os
import struct
import zlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable

import numpy as np
from PIL import Image, ImageOps, features

import refpng
import writers

SEED = 20260923

# El exe bajo prueba: el de release, o el que diga PRISMA_EXE (p.ej. build/asan/image2png.exe).
EXE = Path(os.environ.get("PRISMA_EXE", Path(__file__).resolve().parent.parent / "bin" / "image2png.exe"))
ASAN_EXIT = 86                     # ASAN_OPTIONS=exitcode=86: un reporte de ASan no se confunde con un error comun


def rng(k: int = 0) -> np.random.Generator:
    return np.random.default_rng(SEED + k)


# ---------------------------------------------------------------------------------------------------
# imagenes base (deterministicas)
# ---------------------------------------------------------------------------------------------------

def base_gradient(h=72, w=96) -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), np.uint8)
    img[..., 0] = x * 255 // (w - 1)
    img[..., 1] = y * 255 // (h - 1)
    img[..., 2] = (x + y) * 255 // (w + h - 2)
    img[..., 3] = np.clip(((x - w / 2) ** 2 + (y - h / 2) ** 2) ** 0.5 * -5 + 300, 0, 255).astype(np.uint8)
    return img


def base_circle(h=80, w=80, garbage=True, color=(220, 60, 40)) -> np.ndarray:
    """Circulo con borde antialiasado; bajo el alfa 0, RGB basura (invisible)."""
    y, x = np.mgrid[0:h, 0:w]
    d = np.sqrt((x - w / 2 + 0.5) ** 2 + (y - h / 2 + 0.5) ** 2)
    a = np.clip((w * 0.35 - d) * 255 / 1.5, 0, 255).astype(np.uint8)
    img = np.zeros((h, w, 4), np.uint8)
    img[..., 0], img[..., 1], img[..., 2] = color
    img[..., 3] = a
    if garbage:
        noise = rng(1).integers(0, 256, (h, w, 3), dtype=np.uint8)
        img[..., :3] = np.where(a[..., None] == 0, noise, img[..., :3])
    return img


def base_photo(h=90, w=120, k=2) -> np.ndarray:
    y, x = np.mgrid[0:h, 0:w]
    r = rng(k)
    img = np.zeros((h, w, 3), np.float64)
    img[..., 0] = 128 + 80 * np.sin(x / 9.0) + 20 * np.cos(y / 5.0)
    img[..., 1] = 128 + 70 * np.cos((x + y) / 13.0)
    img[..., 2] = 128 + 90 * np.sin(y / 7.0)
    img += r.normal(0, 6, img.shape)
    return np.clip(img, 0, 255).astype(np.uint8)


def base_palette(h=40, w=50, n=30, k=3) -> np.ndarray:
    """<= n colores, algunos translucidos."""
    r = rng(k)
    pal = r.integers(0, 256, (n, 4), dtype=np.uint8)
    pal[: n // 3, 3] = 255
    pal[n // 3: n // 2, 3] = 0
    pal[n // 3: n // 2, :3] = 0                      # los transparentes son todos (0,0,0,0)
    idx = (np.add.outer(np.arange(h) // 5, np.arange(w) // 7) * 7 + r.integers(0, 2, (h, w))) % n
    return pal[idx]


def base_binary(h=48, w=64) -> np.ndarray:
    """Foto con alfa binario (0/255) y los transparentes con UN mismo color."""
    img = np.zeros((h, w, 4), np.uint8)
    img[..., :3] = base_photo(h, w, 5)
    y, x = np.mgrid[0:h, 0:w]
    hole = ((x // 8 + y // 8) % 3) == 0
    img[..., 3] = np.where(hole, 0, 255)
    img[hole, :3] = (12, 34, 56)
    return img


def rgba_of(im: Image.Image) -> np.ndarray:
    im.load()
    return np.asarray(im.convert("RGBA"))


# ---------------------------------------------------------------------------------------------------
# estructura de los casos
# ---------------------------------------------------------------------------------------------------

@dataclass
class Expect:
    inputs: list[str]
    truth: np.ndarray | None = None
    frames: list[np.ndarray] | None = None         # animacion esperada (APNG)
    tol: int = 0
    pillow: bool = False                           # comparar con Pillow decodificando la entrada
    pillow_transform: Callable | None = None       # p.ej. ImageOps.exif_transpose
    check: Callable | None = None                  # check(outputs, case_dir, result) -> (ok, detalle)
    fail: str | None = None
    compare_hidden: bool = False                   # comparar tambien el RGB bajo alfa 0
    extra_args: list[str] = field(default_factory=list)
    expect_files: int | None = None
    exit_code: int | None = None


@dataclass
class Case:
    group: str
    name: str
    make: Callable[[Path], Expect]
    args: list[str] = field(default_factory=list)


CASES: list[Case] = []


def case(group: str, name: str, *args: str):
    def register(fn):
        CASES.append(Case(group, name, fn, list(args)))
        return fn
    return register


def save(img: np.ndarray, path: Path, **kw) -> None:
    mode = {1: "L", 3: "RGB", 4: "RGBA"}[img.shape[2] if img.ndim == 3 else 1]
    Image.fromarray(img if img.ndim == 2 or img.shape[2] > 1 else img[..., 0], mode).save(path, **kw)


def png_chunks(path: Path) -> list[tuple[bytes, bytes]]:
    data = Path(path).read_bytes()
    out, at = [], 8
    while at + 8 <= len(data):
        n = struct.unpack(">I", data[at:at + 4])[0]
        out.append((data[at + 4:at + 8], data[at + 8:at + 8 + n]))
        at += 12 + n
    return out


def ihdr(path: Path) -> tuple[int, int, int, int, int]:
    d = dict(png_chunks(path))[b"IHDR"]
    w, h, depth, ct, _, _, il = struct.unpack(">IIBBBBB", d)
    return w, h, depth, ct, il


# ===================================================================================================
# PNG: todas las combinaciones validas de tipo de color y profundidad, con y sin Adam7 y tRNS
# ===================================================================================================

VALID_DEPTHS = {0: [1, 2, 4, 8, 16], 2: [8, 16], 3: [1, 2, 4, 8], 4: [8, 16], 6: [8, 16]}


def make_png_variant(ct: int, depth: int, interlace: bool, with_trns: bool, h=13, w=11):
    def make(d: Path) -> Expect:
        r = rng(ct * 100 + depth)
        maxv = (1 << depth) - 1
        palette, trns = None, None
        if ct == 3:
            n = 1 << depth
            palette = r.integers(0, 256, (n, 3), dtype=np.uint8)
            samples = r.integers(0, n, (h, w, 1))
            if with_trns:
                trns = bytes(r.integers(0, 256, max(1, n // 2), dtype=np.uint8))
        else:
            samples = r.integers(0, maxv + 1, (h, w, refpng.CHANNELS[ct]))
            if with_trns and ct == 0:
                key = int(samples[0, 0, 0])
                trns = struct.pack(">H", key)
            if with_trns and ct == 2:
                trns = struct.pack(">HHH", *[int(v) for v in samples[1, 1, :3]])
        refpng.write_png(d / "in.png", samples, ct, depth, palette=palette, trns=trns, interlace=interlace)
        truth = refpng.expected_rgba(samples, ct, depth, palette=palette, trns=trns)
        return Expect(["in.png"], truth=truth)
    return make


for _ct, _depths in VALID_DEPTHS.items():
    for _depth in _depths:
        for _il in (False, True):
            for _t in ((False, True) if _ct in (0, 2, 3) else (False,)):
                _name = f"ct{_ct}-{_depth}bit{'-adam7' if _il else ''}{'-trns' if _t else ''}"
                CASES.append(Case("png", _name, make_png_variant(_ct, _depth, _il, _t)))


@case("png", "1x1-rgba")
def _(d):
    s = np.array([[[10, 20, 30, 40]]])
    refpng.write_png(d / "in.png", s, 6, 8)
    return Expect(["in.png"], truth=s.astype(np.uint8))


@case("png", "idat-partido-en-7-bytes")
def _(d):
    s = base_gradient()
    refpng.write_png(d / "in.png", s, 6, 8, idat_chunk=7)
    return Expect(["in.png"], truth=s)


for _f in range(5):
    def _mk(d, f=_f):
        s = base_gradient()
        refpng.write_png(d / "in.png", s, 6, 8, filters=f)
        return Expect(["in.png"], truth=s)
    CASES.append(Case("png", f"filtro-fijo-{_f}", _mk))


@case("png", "truncado-rescata-filas")
def _(d):
    s = base_gradient(64, 64)
    refpng.write_png(d / "full.png", s, 6, 8, filters=0, level=0)
    data = (d / "full.png").read_bytes()
    (d / "in.png").write_bytes(data[: len(data) // 2])

    def check(outs, cd, res):
        o = read_rgba(outs[0])
        good = (o[:20] == s[:20]).all()
        tail_clear = (o[-5:, :, 3] == 0).all()
        warned = any("cortado" in w or "recuperaron" in w for w in res.get("warnings", []))
        return good and tail_clear and warned, f"filas buenas {'ok' if good else 'MAL'} · final transparente {tail_clear} · aviso {warned}"
    return Expect(["in.png"], check=check)


@case("png", "crc-roto-falla")
def _(d):
    s = base_gradient(16, 16)
    refpng.write_png(d / "ok.png", s, 6, 8)
    data = bytearray((d / "ok.png").read_bytes())
    data[-20] ^= 0x55                                  # un byte del IDAT: el CRC ya no coincide
    (d / "in.png").write_bytes(bytes(data))
    return Expect(["in.png"], fail="CRC")


@case("png", "apng-dispose-blend-over-translucido")
def _(d):
    base = base_circle(48, 48, garbage=False)
    frames = [dict(img=base, x=0, y=0, dispose=0, blend=0, delay_ms=50)]
    for k in range(1, 5):
        img = np.zeros((20, 30, 4), np.uint8)
        img[..., :] = (40 * k, 200, 255 - 40 * k, 60 + 40 * k)       # translucido: OVER sobre translucido
        img[5:10, 5:12, 3] = 0
        frames.append(dict(img=img, x=3 * k, y=2 * k, dispose=(0, 1, 2, 0)[k - 1], blend=k % 2, delay_ms=40 + 10 * k))
    oracle = refpng.write_apng(d / "in.png", 48, 48, frames, plays=2)
    return Expect(["in.png"], frames=oracle, tol=1)


# ===================================================================================================
# GIF
# ===================================================================================================

@case("gif", "estatico-transparente")
def _(d):
    im = Image.fromarray(base_palette(40, 50, 30), "RGBA").convert("P", palette=Image.ADAPTIVE, colors=64)
    im.save(d / "in.gif", transparency=0)
    return Expect(["in.gif"], pillow=True)


for _disp in (0, 1, 2, 3):
    def _mkgif(d, disp=_disp):
        frames = []
        for k in range(5):
            img = np.zeros((40, 60, 4), np.uint8)
            img[..., 3] = 0
            img[5 + k * 4:20 + k * 4, 5 + k * 8:25 + k * 8] = (255, 40 * k, 100, 255)
            frames.append(Image.fromarray(img, "RGBA"))
        frames[0].save(d / "in.gif", save_all=True, append_images=frames[1:], duration=100, loop=0, disposal=disp, optimize=False)
        return Expect(["in.gif"], pillow=True)
    CASES.append(Case("gif", f"animado-descarte-{_disp}", _mkgif))


# ===================================================================================================
# BMP
# ===================================================================================================

@case("bmp", "24bits-pillow")
def _(d):
    s = base_photo(31, 45)
    save(s, d / "in.bmp")
    return Expect(["in.bmp"], truth=np.dstack([s, np.full(s.shape[:2], 255, np.uint8)]))


@case("bmp", "8bits-paleta-pillow")
def _(d):
    im = Image.fromarray(base_photo(30, 40)).convert("P", palette=Image.ADAPTIVE, colors=200)
    im.save(d / "in.bmp")
    return Expect(["in.bmp"], pillow=True)


@case("bmp", "1bit-pillow")
def _(d):
    im = Image.fromarray((base_photo(30, 41)[..., 0] > 128).astype(np.uint8) * 255).convert("1")
    im.save(d / "in.bmp")
    return Expect(["in.bmp"], pillow=True)


@case("bmp", "v5-argb")
def _(d):
    s = base_gradient()
    writers.bmp_v5_argb(d / "in.bmp", s)
    return Expect(["in.bmp"], truth=s, compare_hidden=True)


@case("bmp", "32bits-bi_rgb-con-alfa")
def _(d):
    s = base_gradient()
    writers.bmp_32_bi_rgb(d / "in.bmp", s, zero_alpha=False)
    return Expect(["in.bmp"], truth=s, compare_hidden=True)


@case("bmp", "32bits-alfa-en-cero-es-opaco")
def _(d):
    s = base_gradient()
    writers.bmp_32_bi_rgb(d / "in.bmp", s, zero_alpha=True)
    t = s.copy()
    t[..., 3] = 255
    return Expect(["in.bmp"], truth=t)


@case("bmp", "16bits-565")
def _(d):
    # prisma escala 5/6 bits con redondeo exacto (v*255/31); el oraculo replica bits: difieren a lo sumo en 1
    return Expect(["in.bmp"], truth=writers.bmp_565(d / "in.bmp", base_photo(21, 33)), tol=1)


@case("bmp", "10-10-10-2-sale-en-16-bits")
def _(d):
    s16 = (base_photo(20, 30).astype(np.uint16) * 257)
    return Expect(["in.bmp"], truth=writers.bmp_1010102(d / "in.bmp", s16))


@case("bmp", "rle8-saltos-transparentes")
def _(d):
    return Expect(["in.bmp"], truth=writers.bmp_rle8_with_holes(d / "in.bmp"), compare_hidden=False)


@case("bmp", "os2-core")
def _(d):
    s = base_photo(17, 23)
    writers.bmp_os2_core(d / "in.bmp", s)
    return Expect(["in.bmp"], truth=np.dstack([s, np.full(s.shape[:2], 255, np.uint8)]))


@case("bmp", "top-down")
def _(d):
    s = base_photo(19, 29)
    writers.bmp_top_down(d / "in.bmp", s)
    return Expect(["in.bmp"], truth=np.dstack([s, np.full(s.shape[:2], 255, np.uint8)]))


# ===================================================================================================
# ICO / CUR
# ===================================================================================================

@case("ico", "pillow-4-tamanos-elige-el-mayor")
def _(d):
    Image.fromarray(base_circle(256, 256, garbage=False)).save(d / "in.ico", sizes=[(16, 16), (32, 32), (48, 48), (256, 256)])
    def check(outs, cd, res):
        w, h, *_ = ihdr(outs[0])
        return (w, h) == (256, 256), f"{w}x{h}"
    return Expect(["in.ico"], check=check)


@case("ico", "dib-32bits-con-alfa")
def _(d):
    s = base_circle(32, 32, garbage=False)
    writers.ico(d / "in.ico", [(32, 32, writers.ico_dib_entry(s, 32))])
    return Expect(["in.ico"], truth=s)


@case("ico", "dib-24bits-mascara-and")
def _(d):
    s = base_circle(32, 32, garbage=False)
    writers.ico(d / "in.ico", [(32, 32, writers.ico_dib_entry(s, 24))])
    t = s.copy()
    t[..., 3] = np.where(s[..., 3] < 128, 0, 255)
    return Expect(["in.ico"], truth=t)


@case("ico", "frames-all-saca-todos", "--frames", "all")
def _(d):
    Image.fromarray(base_circle(64, 64, garbage=False)).save(d / "in.ico", sizes=[(16, 16), (32, 32), (64, 64)])
    return Expect(["in.ico"], expect_files=3)


@case("ico", "ico-size-32", "--ico-size", "32")
def _(d):
    Image.fromarray(base_circle(64, 64, garbage=False)).save(d / "in.ico", sizes=[(16, 16), (32, 32), (64, 64)])
    def check(outs, cd, res):
        w, h, *_ = ihdr(outs[0])
        return (w, h) == (32, 32), f"{w}x{h}"
    return Expect(["in.ico"], check=check)


@case("ico", "cursor-cur")
def _(d):
    s = base_circle(32, 32, garbage=False)
    writers.ico(d / "in.cur", [(32, 32, writers.ico_dib_entry(s, 32))], cursor=True, hotspot=(5, 7))
    return Expect(["in.cur"], truth=s)


# ===================================================================================================
# TGA
# ===================================================================================================

@case("tga", "rgba-pillow")
def _(d):
    s = base_gradient()
    save(s, d / "in.tga")
    return Expect(["in.tga"], truth=s, compare_hidden=True)


@case("tga", "rgba-rle-pillow")
def _(d):
    s = base_gradient()
    save(s, d / "in.tga", compression="tga_rle")
    return Expect(["in.tga"], truth=s, compare_hidden=True)


@case("tga", "rle-abajo-arriba")
def _(d):
    s = base_palette(30, 40)
    writers.tga(d / "in.tga", s, rle=True, top_left=False)
    return Expect(["in.tga"], truth=s)


def unpremultiply_stored(s: np.ndarray) -> np.ndarray:
    """Lo que un decodificador correcto recupera de un RGBA8 premultiplicado (el redondeo al guardar
    pierde informacion en alfas bajos: el oraculo parte de lo guardado, no del original)."""
    pm = s.astype(np.uint32)
    for c in range(3):
        pm[..., c] = (pm[..., c] * pm[..., 3] + 127) // 255
    out = pm.copy()
    a = pm[..., 3]
    for c in range(3):
        out[..., c] = np.where(a > 0, np.minimum(255, (pm[..., c] * 255 + a // 2) // np.maximum(a, 1)), 0)
    return out.astype(np.uint8)


@case("tga", "premultiplicado-pie-2.0")
def _(d):
    s = base_gradient()
    writers.tga(d / "in.tga", s, alpha_type=4, premultiply=True)
    return Expect(["in.tga"], truth=unpremultiply_stored(s), tol=1)


@case("tga", "alfa-en-cero-sin-pie-es-opaco")
def _(d):
    s = base_photo(20, 30)
    rgba = np.dstack([s, np.zeros(s.shape[:2], np.uint8)])
    writers.tga(d / "in.tga", rgba)
    return Expect(["in.tga"], truth=np.dstack([s, np.full(s.shape[:2], 255, np.uint8)]))


@case("tga", "16bits-a1r5g5b5")
def _(d):
    s = base_binary(24, 32)
    writers.tga(d / "in.tga", s, bits=16)
    t = s.copy()
    for c in range(3):
        v = s[..., c] >> 3
        t[..., c] = (v << 3) | (v >> 2)
    return Expect(["in.tga"], truth=t)


# ===================================================================================================
# QOI, Netpbm, farbfeld, SGI, PCX
# ===================================================================================================

@case("qoi", "rgba")
def _(d):
    s = base_gradient()
    save(s, d / "in.qoi")
    return Expect(["in.qoi"], truth=s, compare_hidden=True)


@case("qoi", "rgb")
def _(d):
    s = base_photo()
    save(s, d / "in.qoi")
    return Expect(["in.qoi"], truth=np.dstack([s, np.full(s.shape[:2], 255, np.uint8)]))


@case("pnm", "p6-pillow")
def _(d):
    s = base_photo(25, 35)
    save(s, d / "in.ppm")
    return Expect(["in.ppm"], truth=np.dstack([s, np.full(s.shape[:2], 255, np.uint8)]))


@case("pnm", "p5-16bits-pillow")
def _(d):
    g = (base_photo(20, 30)[..., 0].astype(np.uint16) * 250)
    Image.fromarray(g, "I;16").save(d / "in.pgm")
    t = np.dstack([g, g, g, np.full(g.shape, 65535, np.uint16)])
    return Expect(["in.pgm"], truth=t)


@case("pnm", "p4-pbm-pillow")
def _(d):
    b = base_photo(20, 37)[..., 1] > 128
    Image.fromarray(b.astype(np.uint8) * 255).convert("1").save(d / "in.pbm")
    return Expect(["in.pbm"], pillow=True)


@case("pnm", "p1-ascii-1-es-negro")
def _(d):
    b = (base_photo(9, 13)[..., 0] > 128).astype(np.uint8)
    writers.pnm_ascii(d / "in.pbm", "P1", b, 1)
    v = np.where(b == 1, 0, 255).astype(np.uint8)
    return Expect(["in.pbm"], truth=np.dstack([v, v, v, np.full(v.shape, 255, np.uint8)]))


@case("pnm", "p3-ascii-maxval-1023")
def _(d):
    s = rng(9).integers(0, 1024, (7, 9, 3))
    writers.pnm_ascii(d / "in.ppm", "P3", s, 1023)
    t = np.zeros((7, 9, 4), np.uint16)
    t[..., :3] = (s * 65535 + 511) // 1023
    t[..., 3] = 65535
    return Expect(["in.ppm"], truth=t)


@case("pnm", "pam-rgb_alpha-16")
def _(d):
    s = rng(10).integers(0, 65536, (11, 13, 4))
    writers.pam(d / "in.pam", s, "RGB_ALPHA", 65535)
    return Expect(["in.pam"], truth=s.astype(np.uint16), compare_hidden=True)


@case("pnm", "pam-grayscale_alpha-8")
def _(d):
    s = rng(11).integers(0, 256, (11, 13, 2))
    writers.pam(d / "in.pam", s, "GRAYSCALE_ALPHA", 255)
    t = np.dstack([s[..., 0], s[..., 0], s[..., 0], s[..., 1]]).astype(np.uint8)
    return Expect(["in.pam"], truth=t, compare_hidden=True)


def srgb_encode(v):
    v = np.clip(v, 0, 1)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * np.power(v, 1 / 2.4) - 0.055)


@case("pnm", "pfm-flotante-sin-hdr-recorta")
def _(d):
    lin = rng(12).random((9, 12, 3)).astype(np.float32)
    writers.pfm(d / "in.pfm", lin)
    t = np.zeros((9, 12, 4), np.uint16)
    t[..., :3] = np.round(srgb_encode(lin) * 65535)
    t[..., 3] = 65535
    return Expect(["in.pfm"], truth=t, tol=24)


@case("farbfeld", "rgba16")
def _(d):
    s = rng(13).integers(0, 65536, (10, 14, 4)).astype(np.uint16)
    writers.farbfeld(d / "in.ff", s)
    return Expect(["in.ff"], truth=s, compare_hidden=True)


@case("sgi", "rgba-pillow")
def _(d):
    s = base_gradient()
    save(s, d / "in.sgi")
    return Expect(["in.sgi"], truth=s, compare_hidden=True)


@case("sgi", "rgba-rle")
def _(d):
    s = base_palette(20, 30)
    writers.sgi_rle(d / "in.sgi", s)
    return Expect(["in.sgi"], truth=s, compare_hidden=True)


@case("pcx", "rgb-pillow")
def _(d):
    s = base_photo(23, 37)
    save(s, d / "in.pcx")
    return Expect(["in.pcx"], truth=np.dstack([s, np.full(s.shape[:2], 255, np.uint8)]))


@case("pcx", "paleta-pillow")
def _(d):
    Image.fromarray(base_photo(23, 37)).convert("P", palette=Image.ADAPTIVE, colors=100).save(d / "in.pcx")
    return Expect(["in.pcx"], pillow=True)


def opaque(rgb: np.ndarray) -> np.ndarray:
    return np.dstack([rgb, np.full(rgb.shape[:2], 255, np.uint8)])


@case("pcx", "rgb-anchos-impares-y-corridas-largas")
def _(d):
    # Anchos 1, 3 y 131: relleno de la linea par, bytes >= 0xC0 (van siempre como corrida) y
    # corridas de mas de 63 (el maximo del contador) en la misma fila.
    truth = {}
    for w in (1, 3, 131):
        s = base_photo(5, w, 3)
        s[:, w // 3:] = (250, 193, 0xC0)
        writers.pcx(d / f"w{w}.pcx", s)
        truth[f"w{w}"] = opaque(s)

    def check(outs, cd, res):
        got = {Path(o).name.split(".")[0]: o for o in outs}
        if got.keys() != truth.keys():
            return False, f"salidas {sorted(got)}"
        bad = [f"{k}: {detail}" for k, t in truth.items()
               for ok, detail in [compare_rgba(read_rgba(got[k]), t)] if not ok]
        return not bad, "anchos 1, 3 y 131 exactos" if not bad else "; ".join(bad)
    return Expect([f"{k}.pcx" for k in truth], check=check)


@case("pcx", "paleta-256-ancho-impar")
def _(d):
    r = rng(7)
    pal = r.integers(0, 256, (256, 3), dtype=np.uint8)
    idx = r.integers(0, 256, (9, 13), dtype=np.uint8)
    idx[2, :] = 255                                       # corrida de un indice >= 0xC0
    writers.pcx(d / "in.pcx", idx, palette=pal)
    return Expect(["in.pcx"], truth=opaque(pal[idx]))


# ===================================================================================================
# PSD y HDR
# ===================================================================================================

def unmatte_stored(s: np.ndarray, depth: int = 8) -> np.ndarray:
    """Mate blanco como lo guarda Photoshop y despues des-mateado desde lo GUARDADO."""
    maxv = 65535.0 if depth == 16 else 255.0
    src = s.astype(np.float64) / (65535.0 if s.dtype == np.uint16 else 255.0)
    a = src[..., 3:4]
    stored = np.round((src[..., :3] * a + (1 - a)) * maxv) / maxv
    alpha = np.round(src[..., 3:4] * maxv) / maxv
    with np.errstate(invalid="ignore", divide="ignore"):
        c = np.where((alpha > 0) & (alpha < 1), np.clip((stored - (1 - alpha)) / alpha, 0, 1), np.where(alpha <= 0, 0, stored))
    out = np.dstack([c, alpha])
    return np.round(out * maxv).astype(np.uint16 if depth == 16 else np.uint8)


@case("psd", "rgba-mate-blanco-raw")
def _(d):
    s = base_gradient()
    writers.psd_rgba(d / "in.psd", s, rle=False)
    return Expect(["in.psd"], truth=unmatte_stored(s), tol=1)


@case("psd", "rgba-mate-blanco-rle")
def _(d):
    s = base_circle(40, 40, garbage=False)
    writers.psd_rgba(d / "in.psd", s, rle=True)
    return Expect(["in.psd"], truth=unmatte_stored(s), tol=1)


@case("psd", "16bits")
def _(d):
    s = base_gradient().astype(np.uint16) * 257
    writers.psd_rgba(d / "in.psd", s, rle=False, depth=16)
    return Expect(["in.psd"], truth=unmatte_stored(s, 16), tol=2)


@case("psd", "canal-extra-no-es-alfa")
def _(d):
    s = base_gradient()
    writers.psd_rgba(d / "in.psd", s, rle=True, matte_white=False, transparency=False)
    t = s.copy()
    t[..., 3] = 255
    return Expect(["in.psd"], truth=t)


@case("hdr", "rgbe-rle-sin-luces-altas")
def _(d):
    lin = rng(14).random((16, 24, 3)) * 0.95
    stored = writers.hdr_rle(d / "in.hdr", lin)
    t = np.zeros((16, 24, 4), np.uint16)
    t[..., :3] = np.round(srgb_encode(stored) * 65535)
    t[..., 3] = 65535
    return Expect(["in.hdr"], truth=t, tol=40)


@case("hdr", "rgbe-con-luces-altas-usa-aces")
def _(d):
    lin = rng(15).random((16, 24, 3)) * 8.0
    writers.hdr_rle(d / "in.hdr", lin)
    def check(outs, cd, res):
        o = read_rgba(outs[0])
        ok = o.dtype == np.uint16 and o[..., :3].max() < 65535 and o[..., :3].min() >= 0
        tone = any("ACES" in w for w in res.get("warnings", [])) or "ACES" in res.get("operations", "")
        return ok, f"16 bits {o.dtype == np.uint16} · sin saturar {o[..., :3].max() < 65535}"
    return Expect(["in.hdr"], check=check)


# ===================================================================================================
# DDS
# ===================================================================================================

def bc7_random_blocks(bw, bh, k=16) -> bytes:
    r = rng(k)
    blocks = r.integers(0, 256, (bw * bh, 16), dtype=np.uint8)
    blocks[:, 0] |= (1 << r.integers(0, 8, bw * bh)).astype(np.uint8)      # nunca el modo reservado (byte 0 en cero)
    return blocks.tobytes()


@case("dds", "bc7-bloques-al-azar")
def _(d):
    writers.dds_dx10(d / "in.dds", 32, 24, 98, bc7_random_blocks(8, 6))
    return Expect(["in.dds"], pillow=True)


for _fmt in ("DXT1", "DXT3", "DXT5"):
    def _mkdds(d, fmt=_fmt):
        im = Image.fromarray(base_circle(32, 32, garbage=False))
        im.save(d / "in.dds", pixel_format=fmt)
        return Expect(["in.dds"], pillow=True, tol=1)
    CASES.append(Case("dds", f"{_fmt.lower()}-pillow", _mkdds))


@case("dds", "dx10-r8g8b8a8")
def _(d):
    s = base_gradient(20, 28)
    writers.dds_dx10(d / "in.dds", 28, 20, 28, s.tobytes())
    return Expect(["in.dds"], truth=s, compare_hidden=True)


@case("dds", "dx10-b8g8r8a8-premultiplicado")
def _(d):
    s = base_gradient(20, 28)
    pm = s.astype(np.uint32)
    for c in range(3):
        pm[..., c] = (pm[..., c] * pm[..., 3] + 127) // 255
    writers.dds_dx10(d / "in.dds", 28, 20, 87, pm[..., [2, 1, 0, 3]].astype(np.uint8).tobytes(), alpha_mode=2)
    return Expect(["in.dds"], truth=unpremultiply_stored(s), tol=1)


DXGI_R16G16B16A16_UNORM = 11
DXGI_R32G32B32A32_FLOAT = 2


@case("dds", "dx10-rgba16-premultiplicado")
def _(d):
    # 16 bits: el mismo redondeo exacto que en 8, con 65535 como maximo.
    s = base_gradient(20, 28).astype(np.uint64) * 257
    s[..., 0] += np.arange(28, dtype=np.uint64)[None, :] * 3          # que no todos sean multiplos de 257
    a = s[..., 3:4]
    pm = s.copy()
    pm[..., :3] = (s[..., :3] * a + 32767) // 65535
    writers.dds_dx10(d / "in.dds", 28, 20, DXGI_R16G16B16A16_UNORM, pm.astype("<u2").tobytes(), alpha_mode=2)
    with np.errstate(divide="ignore", invalid="ignore"):
        back = np.where(a > 0, np.minimum(65535, (pm[..., :3] * 65535 + a // 2) // np.maximum(a, 1)), 0)
    truth = np.concatenate([back, a], axis=-1).astype(np.uint16)
    return Expect(["in.dds"], truth=truth)


@case("dds", "dx10-float-premultiplicado-lineal")
def _(d):
    # Flotante premultiplicado: se divide en luz LINEAL y despues se codifica sRGB (16 bits).
    s = base_gradient(20, 28).astype(np.float64) / 255
    lin, a = s[..., :3], s[..., 3:4]
    pm = np.concatenate([lin * a, a], axis=-1).astype("<f4")
    writers.dds_dx10(d / "in.dds", 28, 20, DXGI_R32G32B32A32_FLOAT, pm.tobytes(), alpha_mode=2)
    color = np.where(a > 0, np.round(srgb_encode(lin) * 65535), 0)
    truth = np.concatenate([color, np.round(a * 65535)], axis=-1).astype(np.uint16)
    return Expect(["in.dds"], truth=truth, tol=40)


@case("dds", "legacy-a8r8g8b8")
def _(d):
    s = base_gradient(18, 26)
    writers.dds_legacy_rgb(d / "in.dds", s, 32, (0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000), 0x41)
    return Expect(["in.dds"], truth=s, compare_hidden=True)


# ===================================================================================================
# Via WIC: TIFF, WebP, AVIF, JPEG
# ===================================================================================================

@case("wic", "tiff-rgba")
def _(d):
    s = base_gradient()
    save(s, d / "in.tif")
    return Expect(["in.tif"], truth=s)


@case("wic", "tiff-rgb-16bits")
def _(d):
    import cv2
    s = (base_photo(20, 30).astype(np.uint16) * 257 + 3).clip(0, 65535).astype(np.uint16)
    cv2.imwrite(str(d / "in.tif"), s[..., ::-1])
    t = np.dstack([s, np.full(s.shape[:2], 65535, np.uint16)])
    return Expect(["in.tif"], truth=t)


@case("wic", "webp-sin-perdida-rgba")
def _(d):
    s = base_gradient()
    save(s, d / "in.webp", lossless=True)
    return Expect(["in.webp"], truth=s)


@case("wic", "webp-con-perdida")
def _(d):
    save(base_gradient(), d / "in.webp", quality=80)
    return Expect(["in.webp"], pillow=True, tol=40)


@case("wic", "jpeg")
def _(d):
    save(base_photo(), d / "in.jpg", quality=95)
    return Expect(["in.jpg"], pillow=True, tol=8)


for _o in range(1, 9):
    def _mkexif(d, o=_o):
        im = Image.fromarray(base_photo(40, 64))
        exif = Image.Exif()
        exif[0x0112] = o
        im.save(d / "in.jpg", quality=95, exif=exif)
        return Expect(["in.jpg"], pillow=True, pillow_transform=ImageOps.exif_transpose, tol=8)
    CASES.append(Case("wic", f"jpeg-orientacion-exif-{_o}", _mkexif))


@case("wic", "jpeg2000-no-soportado")
def _(d):
    save(base_photo(16, 16), d / "in.jp2")
    return Expect(["in.jp2"], fail="JPEG 2000")


# ===================================================================================================
# helpers de lectura y comparacion (los usa suite.py y algunos checks)
# ===================================================================================================

def compare_rgba(got: np.ndarray, want: np.ndarray, tol: int = 0, hidden: bool = True) -> tuple[bool, str]:
    """Diferencia maxima por canal. Con hidden=False el RGB bajo alfa 0 no cuenta: es invisible y
    un conversor tiene derecho a normalizarlo."""
    if got.shape != want.shape:
        return False, f"tamaño {got.shape} != esperado {want.shape}"
    if got.dtype != want.dtype:
        return False, f"profundidad {got.dtype} != esperada {want.dtype}"
    gi, wi = got.astype(np.int64), want.astype(np.int64)
    visible = np.ones(want.shape[:2], bool) if hidden else (wi[..., 3] > 0)
    diff = np.abs(gi - wi)
    color = int(diff[..., :3][visible].max()) if visible.any() else 0
    alpha = int(diff[..., 3].max())
    ok = color <= tol and alpha <= tol
    return ok, "exacto" if color == 0 and alpha == 0 else f"dif. max color {color} alfa {alpha} (tol {tol})"


def visible_error(got: np.ndarray, want: np.ndarray) -> tuple[float, float, int]:
    """(max, media) del error de color PREMULTIPLICADO en niveles de 8 bits, y el error de alfa.
    Es lo que se ve: una diferencia de color bajo alfa 5 pesa 5/255 de lo que pesa en un opaco."""
    ga, wa = got[..., 3:4].astype(np.float64) / 255, want[..., 3:4].astype(np.float64) / 255
    err = np.abs(got[..., :3] * ga - want[..., :3] * wa).max(-1)
    return float(err.max()), float(err.mean()), int(np.abs(got[..., 3].astype(int) - want[..., 3].astype(int)).max())


def within(got, want, max_tol: float, mean_tol: float, alpha_tol: int = 0) -> tuple[bool, str]:
    mx, mean, alpha = visible_error(got, want)
    ok = mx <= max_tol and mean <= mean_tol and alpha <= alpha_tol
    return ok, f"error visible max {mx:.1f} (tol {max_tol}) · media {mean:.2f} (tol {mean_tol}) · alfa {alpha} (tol {alpha_tol})"


def read_rgba(path) -> np.ndarray:
    """RGBA exacto de un PNG: OpenCV para 16 bits (Pillow los baja a 8), Pillow para 8."""
    w, h, depth, ct, _ = ihdr(path)
    if depth == 16:
        import cv2
        a = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
        if a.ndim == 2:
            out = np.dstack([a, a, a, np.full(a.shape, 65535, np.uint16)])
        elif a.shape[2] == 3:
            out = np.dstack([a[..., ::-1], np.full(a.shape[:2], 65535, np.uint16)])
        else:
            out = a[..., [2, 1, 0, 3]]
        chunks = dict(png_chunks(path))
        if b"tRNS" in chunks:                       # OpenCV no aplica tRNS en 16 bits
            t = chunks[b"tRNS"]
            if ct == 0:
                key = struct.unpack(">H", t[:2])[0]
                out[..., 3] = np.where(out[..., 0] == key, 0, 65535)
            elif ct == 2:
                k = struct.unpack(">HHH", t[:6])
                hit = (out[..., 0] == k[0]) & (out[..., 1] == k[1]) & (out[..., 2] == k[2])
                out[..., 3] = np.where(hit, 0, 65535)
        return np.ascontiguousarray(out)
    if depth < 8 and ct == 0 and any(k == b"tRNS" for k, _ in png_chunks(path)):
        return refpng.read_png(path)[0]            # Pillow compara la clave cruda con el gris ya expandido
    return rgba_of(Image.open(path))


import ops_cases  # noqa: E402,F401  (registra los casos de operaciones, codificador y bordes)
import heif_cases  # noqa: E402,F401  (AVIF/HEIF por WIC y color CICP)
