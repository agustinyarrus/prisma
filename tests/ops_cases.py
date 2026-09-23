"""ops_cases.py - pruebas de operaciones de transparencia, geometria, codificador, metadatos,
animaciones y casos borde. Las operaciones pixel a pixel se comparan contra implementaciones de
referencia en numpy (las mismas formulas, escritas de nuevo e independientes del C)."""
from __future__ import annotations

import os
import shutil
import struct
import subprocess
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, ImageSequence

import writers
from cases import (CASES, EXE, Case, Expect, base_binary, base_circle, base_gradient, base_palette, base_photo, case,
                   ihdr, png_chunks, read_rgba, rng, save, srgb_encode, within)

# ---------------------------------------------------------------------------------------------------
# referencias numpy (flotante [0,1], alfa directo)
# ---------------------------------------------------------------------------------------------------


def f32(img):
    return img.astype(np.float64) / 255.0


def u8(img):
    return np.clip(np.round(img * 255.0), 0, 255).astype(np.uint8)


def ref_key(img, key, fuzz, feather=0.0):
    x = f32(img)
    dist = np.sqrt(((x[..., :3] - np.array(key)) ** 2).sum(-1)) / np.sqrt(3)
    a = x[..., 3].copy()
    a = np.where(dist <= fuzz, 0.0, a)
    if feather > 0:
        ramp = (dist > fuzz) & (dist < fuzz + feather)
        a = np.where(ramp, a * (dist - fuzz) / feather, a)
    x[..., 3] = a
    return u8(x)


def ref_c2a(img, bg, t=0.0, o=1.0):
    x = f32(img)
    bg = np.array(bg, np.float64)
    out = x.copy()
    eps = 1e-6
    alpha = np.zeros(x.shape[:2])
    dist = np.zeros(x.shape[:2])
    for c in range(3):
        d = np.abs(x[..., c] - bg[c])
        with np.errstate(divide="ignore", invalid="ignore"):
            a = np.where(d < t + eps, 0.0, np.where(d > o - eps, 1.0,
                         np.where(x[..., c] < bg[c], (d - t) / (min(o, bg[c]) - t + 1e-12), (d - t) / (min(o, 1 - bg[c]) - t + 1e-12))))
        take = a > alpha
        dist = np.where(take, d, dist)
        alpha = np.where(take, a, alpha)
    with np.errstate(divide="ignore", invalid="ignore"):
        ratio = np.where(dist > eps, t / np.maximum(dist, eps), 0.0)
        for c in range(3):
            cc = bg[c] + (x[..., c] - bg[c]) * ratio
            v = cc + (x[..., c] - cc) / np.maximum(alpha, eps)
            out[..., c] = np.where(alpha > eps, np.clip(v, 0, 1), x[..., c])
    out[..., 3] = x[..., 3] * np.clip(alpha, 0, 1)
    return u8(out)


def ref_unmatte(img, m):
    x = f32(img)
    a = x[..., 3:4]
    with np.errstate(divide="ignore", invalid="ignore"):
        c = np.clip((x[..., :3] - (1 - a) * np.array(m)) / a, 0, 1)
    x[..., :3] = np.where((a > 0) & (a < 1), c, np.where(a <= 0, 0, x[..., :3]))
    return u8(x)


def ref_flatten(img, bg, linear=False):
    x = f32(img)
    a = x[..., 3:4]
    bg = np.array(bg, np.float64)
    if linear:
        def lin(v): return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)
        c = srgb_encode(lin(x[..., :3]) * a + lin(bg) * (1 - a))
    else:
        c = x[..., :3] * a + bg * (1 - a)
    x[..., :3] = c
    x[..., 3] = 1
    return u8(x)


def gamma_lin(v):
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


# ---------------------------------------------------------------------------------------------------
# operaciones pixel a pixel
# ---------------------------------------------------------------------------------------------------

def op_case(name, args, src_fn, ref_fn, tol=1):
    def make(d, a=args):
        s = src_fn()
        save(s, d / "in.png")
        return Expect(["in.png"], truth=ref_fn(s), tol=tol)
    CASES.append(Case("ops", name, make, list(args)))


op_case("key-blanco-fuzz-10", ["--key", "white", "--fuzz", "10"], lambda: base_gradient(),
        lambda s: ref_key(s, (1, 1, 1), 0.10))
op_case("key-con-feather", ["--key", "#ff0000", "--fuzz", "20", "--feather", "15"], lambda: base_gradient(),
        lambda s: ref_key(s, (1, 0, 0), 0.20, 0.15))
op_case("color-to-alpha-blanco", ["--color-to-alpha", "white"], lambda: np.dstack([base_photo(40, 50), np.full((40, 50), 255, np.uint8)]),
        lambda s: ref_c2a(s, (1, 1, 1)), tol=2)
op_case("color-to-alpha-umbrales", ["--color-to-alpha", "#808080", "--c2a-thresholds", "5,70"],
        lambda: np.dstack([base_photo(40, 50), np.full((40, 50), 255, np.uint8)]), lambda s: ref_c2a(s, (128 / 255,) * 3, 0.05, 0.70), tol=2)
op_case("unmatte-blanco", ["--unmatte", "white"], lambda: base_gradient(), lambda s: ref_unmatte(s, (1, 1, 1)), tol=2)
op_case("threshold-128", ["--threshold", "128"], lambda: base_gradient(),
        lambda s: np.dstack([s[..., :3], np.where(s[..., 3] >= 128, 255, 0)]).astype(np.uint8))
op_case("opacity-50", ["--opacity", "50"], lambda: base_gradient(),
        lambda s: np.dstack([s[..., :3], np.round(s[..., 3] * 0.5)]).astype(np.uint8))
op_case("invert-alpha", ["--invert-alpha"], lambda: base_gradient(),
        lambda s: np.dstack([s[..., :3], 255 - s[..., 3]]).astype(np.uint8))
op_case("flatten-azul", ["--flatten", "#336699"], lambda: base_gradient(), lambda s: ref_flatten(s, (0x33 / 255, 0x66 / 255, 0x99 / 255)))
op_case("flatten-lineal", ["--flatten", "#336699", "--linear"], lambda: base_gradient(),
        lambda s: ref_flatten(s, (0x33 / 255, 0x66 / 255, 0x99 / 255), linear=True), tol=2)
op_case("premultiply", ["--premultiply"], lambda: base_gradient(),
        lambda s: u8(np.dstack([f32(s)[..., :3] * f32(s)[..., 3:4], f32(s)[..., 3]])))
op_case("unpremultiply", ["--unpremultiply"], lambda: base_gradient(),
        lambda s: u8(np.dstack([np.where(f32(s)[..., 3:4] > 0, np.minimum(1, f32(s)[..., :3] / np.maximum(f32(s)[..., 3:4], 1e-9)), 0), f32(s)[..., 3]])), tol=2)
op_case("alpha-from-luma", ["--alpha-from", "luma"], lambda: np.dstack([base_photo(30, 40), np.full((30, 40), 255, np.uint8)]),
        lambda s: np.dstack([s[..., :3], u8(0.2126 * f32(s)[..., 0] + 0.7152 * f32(s)[..., 1] + 0.0722 * f32(s)[..., 2])]).astype(np.uint8))
op_case("alpha-from-canal-g", ["--alpha-from", "g"], lambda: np.dstack([base_photo(30, 40), np.full((30, 40), 255, np.uint8)]),
        lambda s: np.dstack([s[..., :3], s[..., 1]]).astype(np.uint8))
op_case("extract-alpha", ["--extract-alpha"], lambda: base_gradient(),
        lambda s: np.dstack([s[..., 3], s[..., 3], s[..., 3], np.full(s.shape[:2], 255, np.uint8)]).astype(np.uint8))
op_case("grayscale", ["--grayscale"], lambda: base_gradient(),
        lambda s: np.dstack([np.repeat(u8(srgb_encode(0.2126 * gamma_lin(f32(s)[..., 0]) + 0.7152 * gamma_lin(f32(s)[..., 1]) + 0.0722 * gamma_lin(f32(s)[..., 2])))[..., None], 3, -1), s[..., 3]]), tol=2)


@case("ops", "mascara-externa-luma")
def _(d):
    s = np.dstack([base_photo(30, 40), np.full((30, 40), 255, np.uint8)])
    save(s, d / "in.png")
    m = base_photo(30, 40, 7)[..., 2]
    Image.fromarray(m, "L").save(d / "mask.png")
    t = s.copy()
    t[..., 3] = np.round(m.astype(np.float64)).astype(np.uint8)
    return Expect(["in.png"], truth=t, tol=1, extra_args=["--mask", str(d / "mask.png")])


@case("ops", "clean-negro-bajo-alfa-0", "--clean")
def _(d):
    s = base_circle()
    save(s, d / "in.png")
    t = s.copy()
    t[s[..., 3] == 0, :3] = 0
    return Expect(["in.png"], truth=t, compare_hidden=True)


def neighbors_average(img, mask_src, y, x):
    h, w = mask_src.shape
    acc, n = np.zeros(3), 0
    for dy in (-1, 0, 1):
        for dx in (-1, 0, 1):
            if dy == dx == 0:
                continue
            yy, xx = y + dy, x + dx
            if 0 <= yy < h and 0 <= xx < w and mask_src[yy, xx]:
                acc += img[yy, xx, :3]
                n += 1
    return acc / n if n else None


@case("ops", "bleed-sangrado", "--bleed")
def _(d):
    s = base_circle()
    save(s, d / "in.png")

    def check(outs, cd, res):
        o = read_rgba(outs[0]).astype(np.int32)
        if (o[..., 3] != s[..., 3]).any():
            return False, "el sangrado cambio el alfa"
        visible = s[..., 3] > 0
        worst = 0
        ys, xs = np.nonzero(~visible)
        checked = 0
        for y, x in zip(ys, xs):
            avg = neighbors_average(s.astype(np.int32), visible, y, x)
            if avg is None:
                continue
            worst = max(worst, np.abs(o[y, x, :3] - avg).max())
            checked += 1
        far = o[0, 0, :3]
        return worst <= 1 and checked > 0, f"{checked} pixeles del borde · error max {worst:.1f} · esquina {tuple(far)}"
    return Expect(["in.png"], check=check)


@case("ops", "defringe-2px", "--defringe", "2")
def _(d):
    s = base_circle(garbage=False)
    s = s.copy()
    semi = (s[..., 3] > 0) & (s[..., 3] < 255)
    s[semi, :3] = (250, 250, 250)                      # ribete blanco del fondo viejo
    save(s, d / "in.png")

    def check(outs, cd, res):
        o = read_rgba(outs[0]).astype(np.int32)
        same_alpha = (o[..., 3] == s[..., 3]).all()
        edge = o[semi, :3]
        dist = np.abs(edge - np.array([220, 60, 40])).max() if edge.size else 0
        return same_alpha and dist <= 2, f"alfa igual {same_alpha} · desvio del color interior {dist}"
    return Expect(["in.png"], check=check)


def logo_on_white(h=90, w=120):
    """Anillo azul oscuro antialiasado sobre blanco, con un hueco blanco ENCERRADO adentro."""
    y, x = np.mgrid[0:h, 0:w]
    d = np.sqrt((x - w / 2 + 0.5) ** 2 + (y - h / 2 + 0.5) ** 2)
    outer = np.clip((34 - d) / 1.2, 0, 1)
    inner = np.clip((d - 16) / 1.2, 0, 1)
    cover = outer * inner
    fg = np.array([20, 40, 120], np.float64)
    rgb = fg * cover[..., None] + 255 * (1 - cover[..., None])
    img = np.dstack([np.round(rgb).astype(np.uint8), np.full((h, w), 255, np.uint8)])
    return img, cover, d


@case("ops", "remove-bg-conserva-hueco-interior", "--remove-bg")
def _(d):
    s, cover, dist = logo_on_white()
    save(s, d / "in.png")
    # La salida ideal: afuera del anillo, el azul PURO con alfa = cobertura (sin ribete blanco);
    # adentro (el anillo y el hueco blanco encerrado, que NO es fondo alcanzable), la entrada tal cual.
    ideal = s.copy()
    outer = dist > 25
    ideal[outer, :3] = (20, 40, 120)
    ideal[outer, 3] = np.round(cover[outer] * 255).astype(np.uint8)

    def check(outs, cd, res):
        o = read_rgba(outs[0]).astype(np.int64)
        corners = o[0, 0, 3] == 0 and o[-1, -1, 3] == 0
        hole = bool((o[dist < 12, 3] == 255).all())
        # Tolerancias: la entrada es de 8 bits, y des-mezclar un pixel de alfa a amplifica su
        # redondeo por 1/a; en premultiplicado eso vuelve a ser ~1 nivel.
        ok, detail = within(o, ideal.astype(np.int64), 4.0, 0.3, alpha_tol=6)
        return corners and hole and ok, f"esquinas {corners} · hueco {hole} · {detail}"
    return Expect(["in.png"], check=check)


# ---------------------------------------------------------------------------------------------------
# geometria
# ---------------------------------------------------------------------------------------------------

def geo_case(name, args, ref_fn):
    def make(d, a=args):
        s = base_gradient(30, 44)
        save(s, d / "in.png")
        return Expect(["in.png"], truth=ref_fn(s), compare_hidden=True)
    CASES.append(Case("geo", name, make, list(args)))


geo_case("rotate-90", ["--rotate", "90"], lambda s: np.rot90(s, -1))
geo_case("rotate-180", ["--rotate", "180"], lambda s: np.rot90(s, 2))
geo_case("rotate-270", ["--rotate", "270"], lambda s: np.rot90(s, 1))
geo_case("rotate-menos-90", ["--rotate", "-90"], lambda s: np.rot90(s, 1))
geo_case("flip-h", ["--flip", "h"], lambda s: s[:, ::-1])
geo_case("flip-v", ["--flip", "v"], lambda s: s[::-1])
geo_case("flip-hv", ["--flip", "hv"], lambda s: s[::-1, ::-1])
geo_case("rotate-90-flip-h", ["--rotate", "90", "--flip", "h"], lambda s: np.rot90(s, -1)[:, ::-1])
geo_case("crop", ["--crop", "5,7,20,11"], lambda s: s[7:18, 5:25])
geo_case("pad-4", ["--pad", "4"], lambda s: np.pad(s, ((4, 4), (4, 4), (0, 0))))
# El margen y la imagen no se pisan: se suman la imagen con borde en cero y el borde de color.
geo_case("pad-trbl-color", ["--pad", "1,2,3,4", "--pad-color", "#ff000080"],
         lambda s: np.pad(s, ((1, 3), (4, 2), (0, 0))) + pad_color_layer(s, (1, 2, 3, 4), (255, 0, 0, 128)))


def pad_color_layer(s, trbl, color):
    t, r, b, l = trbl
    h, w, _ = s.shape
    layer = np.zeros((h + t + b, w + l + r, 4), np.uint8)
    layer[:] = color
    layer[t:t + h, l:l + w] = 0
    return layer


@case("geo", "trim-con-margen", "--trim", "3")
def _(d):
    s = np.zeros((50, 60, 4), np.uint8)
    s[10:20, 15:40] = (200, 100, 50, 255)
    s[25, 45] = (1, 2, 3, 128)
    save(s, d / "in.png")
    t = np.pad(s[10:26, 15:46], ((3, 3), (3, 3), (0, 0)))
    return Expect(["in.png"], truth=t)


@case("geo", "resize-50-sin-halo-oscuro", "--resize", "50%")
def _(d):
    s = base_circle(80, 80, garbage=False).copy()
    s[s[..., 3] == 0, :3] = 0                            # negro invisible: el que produce halos si se mezcla mal
    save(s, d / "in.png")

    def check(outs, cd, res):
        o = read_rgba(outs[0]).astype(np.int32)
        size_ok = o.shape[:2] == (40, 40)
        edge = (o[..., 3] > 20) & (o[..., 3] < 235)
        dev = np.abs(o[edge, :3] - np.array([220, 60, 40])).max() if edge.any() else 0
        return size_ok and dev <= 6, f"40x40 {size_ok} · desvio de color en el borde {dev} (sin premultiplicar seria ~100)"
    return Expect(["in.png"], check=check)


@case("geo", "resize-caja-wxh", "--resize", "50x50")
def _(d):
    save(base_gradient(40, 100), d / "in.png")
    def check(outs, cd, res):
        w, h, *_ = ihdr(outs[0])
        return (w, h) == (50, 20), f"{w}x{h} (esperado 50x20: entra en la caja)"
    return Expect(["in.png"], check=check)


@case("geo", "resize-solo-achica", "--resize", "500x500>")
def _(d):
    save(base_gradient(40, 60), d / "in.png")
    def check(outs, cd, res):
        w, h, *_ = ihdr(outs[0])
        return (w, h) == (60, 40), f"{w}x{h} (no agranda)"
    return Expect(["in.png"], check=check)


# ---------------------------------------------------------------------------------------------------
# paleta con perdida
# ---------------------------------------------------------------------------------------------------

# La metrica del cuantizador (quantize.h): promedio del error compuesto sobre negro y sobre blanco,
# en premultiplicado y ponderado por canal. Blanco: p + W(1 - a), asi que la diferencia es
# dp - W·da y el termino cruzado va con signo menos. Es euclidea despues de una transformacion
# lineal (Cholesky de Q): el k-means de referencia corre ahi.
QUANT_W = np.array([0.5, 1.0, 0.45])


def quant_space(img: np.ndarray) -> np.ndarray:
    a = img[..., 3:4] / 255.0
    x = np.dstack([QUANT_W * img[..., :3] / 255.0 * a, a])
    q = np.zeros((4, 4))
    q[:3, :3] = np.eye(3)
    q[:3, 3] = q[3, :3] = -QUANT_W / 2
    q[3, 3] = (QUANT_W ** 2).sum() / 2
    return x @ np.linalg.cholesky(q)


def kmeans_reference(x: np.ndarray, k: int, seeds: int = 4, iterations: int = 200) -> float:
    """k-means++ de referencia (independiente del C): el mejor costo medio de varias semillas."""
    best = np.inf
    for seed in range(seeds):
        r = np.random.default_rng(seed)
        centers = x[r.choice(len(x), 1)]
        for _ in range(k - 1):
            d = ((x[:, None] - centers[None]) ** 2).sum(-1).min(1)
            centers = np.vstack([centers, x[r.choice(len(x), p=d / d.sum())]])
        for _ in range(iterations):
            labels = ((x[:, None] - centers[None]) ** 2).sum(-1).argmin(1)
            moved = np.array([x[labels == j].mean(0) if (labels == j).any() else centers[j] for j in range(k)])
            if np.allclose(moved, centers):
                break
            centers = moved
        best = min(best, float(((x - centers[labels]) ** 2).sum(-1).mean()))
    return best


def gaussian_blur(img: np.ndarray) -> np.ndarray:
    k = np.array([1, 4, 6, 4, 1], np.float64) / 16
    out = img.astype(np.float64)
    for axis in (0, 1):
        out = np.apply_along_axis(lambda v: np.convolve(np.pad(v, 2, mode="edge"), k, "valid"), axis, out)
    return out


def premultiplied(img: np.ndarray) -> np.ndarray:
    return np.dstack([img[..., :3] * img[..., 3:4] / 255.0, img[..., 3:4]])


@case("paleta", "colors-16-con-alfa-casi-optimo", "--colors", "16", "--dither", "none")
def _(d):
    s = base_gradient()
    save(s, d / "in.png")

    def check(outs, cd, res):
        o = read_rgba(outs[0]).astype(np.int64)
        uniq = len(np.unique(o.reshape(-1, 4), axis=0))
        ct = ihdr(outs[0])[3]
        clear_ok = bool((o[s[..., 3] == 0, 3] == 0).all())
        vis = s[..., 3] > 0
        ours = float(((quant_space(s)[vis] - quant_space(o)[vis]) ** 2).sum(-1).mean())
        ref = kmeans_reference(quant_space(s)[vis], 15)          # 15 + el transparente reservado
        ratio = ours / ref
        ok = uniq <= 16 and ct == 3 and clear_ok and ratio <= 1.05
        return ok, f"{uniq} colores · tipo {ct} · costo {ratio:.3f}x el de k-means++ (tol 1,05) · transparentes intactos {clear_ok}"
    return Expect(["in.png"], check=check)


@case("paleta", "dithering-mejora-el-promedio-local", "--colors", "16")
def _(d):
    # El dithering empeora el error pixel a pixel a proposito: lo que tiene que mejorar es el
    # color PROMEDIO de cada zona (lo que ve el ojo), medido con un desenfoque gaussiano.
    s = base_gradient()
    save(s, d / "in.png")

    def check(outs, cd, res):
        flat = Path(cd) / "sin-dithering.png"
        subprocess.run([str(EXE), str(Path(cd) / "in.png"), "-o", str(flat), "--colors", "16", "--dither", "none"],
                       capture_output=True, check=True)
        want = gaussian_blur(premultiplied(s.astype(np.float64)))
        err = {name: float(np.abs(gaussian_blur(premultiplied(read_rgba(path).astype(np.float64))) - want)[..., :3].mean())
               for name, path in (("fs", outs[0]), ("none", flat))}
        return err["fs"] < 0.92 * err["none"], f"error del promedio local: FS {err['fs']:.2f} vs sin dithering {err['none']:.2f} (tiene que bajar >= 8 %)"
    return Expect(["in.png"], check=check)


@case("paleta", "colors-sin-perdida-si-ya-entra", "--colors", "64")
def _(d):
    s = base_palette(40, 50, 30)
    save(s, d / "in.png")
    return Expect(["in.png"], truth=s)


@case("paleta", "dither-ordenado", "--colors", "8", "--dither", "ordered")
def _(d):
    save(base_gradient(), d / "in.png")
    def check(outs, cd, res):
        o = read_rgba(outs[0]).reshape(-1, 4)
        uniq = len({tuple(p) for p in o})
        return uniq <= 8, f"{uniq} colores"
    return Expect(["in.png"], check=check)


# ---------------------------------------------------------------------------------------------------
# codificador
# ---------------------------------------------------------------------------------------------------

def photo_rgba(h=96, w=128):
    return np.dstack([base_photo(h, w), (np.arange(w)[None, :] * 2 % 256).repeat(h, 0).astype(np.uint8)])


for _level in range(5):
    def _mk(d, lv=_level):
        s = photo_rgba()
        save(s, d / "in.bmp")
        return Expect(["in.bmp"], truth=s, compare_hidden=True)
    CASES.append(Case("encoder", f"optimize-O{_level}", _mk, [f"-O{_level}"]))

for _flt in ("none", "sub", "up", "avg", "paeth", "minsum", "entropy", "bigrams", "brute"):
    def _mkf(d):
        s = photo_rgba(48, 64)
        save(s, d / "in.bmp")
        return Expect(["in.bmp"], truth=s, compare_hidden=True)
    CASES.append(Case("encoder", f"filtro-{_flt}", _mkf, ["--filter", _flt]))


def flat_graphic(h=300, w=400) -> np.ndarray:
    """Grafico de colores planos con bordes antialiasados: donde 'none' le gana a minsum."""
    y, x = np.mgrid[0:h, 0:w]
    img = np.zeros((h, w, 4), np.uint8)
    img[..., :3] = (245, 245, 250)
    img[..., 3] = 255
    for k, (cx, cy, r, color) in enumerate([(120, 150, 90, (36, 99, 235)), (280, 120, 70, (250, 204, 21)), (250, 220, 60, (220, 60, 90))]):
        cover = np.clip(r - np.sqrt((x - cx) ** 2 + (y - cy) ** 2) + 0.5, 0, 1)[..., None]
        img[..., :3] = np.round(img[..., :3] * (1 - cover) + np.array(color) * cover).astype(np.uint8)
    return img


def filter_race_case(name: str, make_image):
    """-O2 elige el filtro con una carrera sobre una muestra: tiene que quedar a menos de 2 % de la
    mejor de las dos estrategias principales, sea la imagen que sea."""
    def make(d):
        s = make_image()
        save(s, d / "in.png")

        def check(outs, cd, res):
            sizes = {}
            for flt in ("minsum", "none"):
                dst = Path(cd) / f"fijo-{flt}.png"
                subprocess.run([str(EXE), str(Path(cd) / "in.png"), "-o", str(dst), "--filter", flt], capture_output=True, check=True)
                sizes[flt] = dst.stat().st_size
            ours = Path(outs[0]).stat().st_size
            best = min(sizes.values())
            chosen = res.get("png", {}).get("filter", "?").split(" ")[0]
            exact = (read_rgba(outs[0]) == s).all()
            return exact and ours <= best * 1.02, (f"eligio {chosen}: {ours:,} bytes · minsum {sizes['minsum']:,} · none {sizes['none']:,}"
                                                  f" · {(ours / best - 1) * 100:+.1f} % del mejor").replace(",", ".")
        return Expect(["in.png"], check=check)
    CASES.append(Case("encoder", name, make))


filter_race_case("carrera-de-filtros-grafico-plano", flat_graphic)
filter_race_case("carrera-de-filtros-foto", lambda: np.dstack([base_photo(240, 320), np.full((240, 320), 255, np.uint8)]))


@case("encoder", "adam7", "--interlace")
def _(d):
    s = photo_rgba(37, 41)
    save(s, d / "in.bmp")
    def check(outs, cd, res):
        o = read_rgba(outs[0])
        return (o == s).all() and ihdr(outs[0])[4] == 1, f"exacto {(o == s).all()} · entrelazado {ihdr(outs[0])[4] == 1}"
    return Expect(["in.bmp"], check=check)


@case("encoder", "deflate-paralelo-imagen-grande")
def _(d):
    r = rng(30)
    s = np.dstack([base_photo(1200, 1600, 31), r.integers(0, 256, (1200, 1600), dtype=np.uint8)])
    save(s, d / "in.bmp")
    return Expect(["in.bmp"], truth=s, compare_hidden=True)


@case("encoder", "flujo-unico", "--single-stream", "-O3")
def _(d):
    s = photo_rgba()
    save(s, d / "in.bmp")
    return Expect(["in.bmp"], truth=s, compare_hidden=True)


def expect_ihdr(depth=None, ct=None, trns=None, exact=True):
    def check_factory(s):
        def check(outs, cd, res):
            w, h, dd, cc, il = ihdr(outs[0])
            has_trns = any(k == b"tRNS" for k, _ in png_chunks(outs[0]))
            ok = (depth is None or dd == depth) and (ct is None or cc == ct) and (trns is None or has_trns == trns)
            if exact:
                o = read_rgba(outs[0])
                vis = s[..., 3] > 0 if s.shape[2] == 4 else np.ones(s.shape[:2], bool)
                same = (o[vis] == s[vis]).all() and (o[..., 3][~vis] == 0).all()
                ok = ok and same
            else:
                same = True
            return ok, f"profundidad {dd} · tipo {cc} · tRNS {has_trns} · exacto {same}"
        return check
    return check_factory


def reduction_case(name, image_fn, **kw):
    def make(d):
        s = image_fn()
        if s.dtype == np.uint16:
            import cv2
            cv2.imwrite(str(d / "in.png"), s[..., [2, 1, 0, 3]])
        else:
            save(s, d / "in.png")
        return Expect(["in.png"], check=expect_ihdr(**kw)(s if s.dtype == np.uint8 else (s // 257).astype(np.uint8)))
    CASES.append(Case("encoder", name, make))


reduction_case("rgba-opaca-baja-a-rgb", lambda: np.dstack([base_photo(), np.full((90, 120), 255, np.uint8)]), ct=2)
reduction_case("gris-detectado", lambda: np.dstack([base_photo()[..., :1].repeat(3, -1), np.full((90, 120), 255, np.uint8)]), ct=0, depth=8)
reduction_case("gris-1-bit", lambda: np.dstack([((base_photo()[..., :1] > 128) * 255).astype(np.uint8).repeat(3, -1), np.full((90, 120), 255, np.uint8)]), ct=0, depth=1)
reduction_case("paleta-con-trns", lambda: base_palette(), ct=3, trns=True)
reduction_case("clave-trns-en-rgb", lambda: base_binary(), ct=2, trns=True)
reduction_case("16-bits-falsos-bajan-a-8", lambda: (np.dstack([base_photo(), np.full((90, 120), 200, np.uint8)]).astype(np.uint16) * 257), depth=8, ct=6)


@case("encoder", "alpha-force-en-opaca", "--alpha", "force")
def _(d):
    save(base_photo(), d / "in.png")
    def check(outs, cd, res):
        ct = ihdr(outs[0])[3]
        return ct in (4, 6), f"tipo {ct}"
    return Expect(["in.png"], check=check)


@case("encoder", "color-rgb-aplana-sobre-blanco", "--color", "rgb")
def _(d):
    s = base_gradient()
    save(s, d / "in.png")
    t = ref_flatten(s, (1, 1, 1))
    def check(outs, cd, res):
        o = read_rgba(outs[0]).astype(int)
        ct = ihdr(outs[0])[3]
        dev = np.abs(o - t.astype(int)).max()
        return ct == 2 and dev <= 1, f"tipo {ct} · desvio {dev}"
    return Expect(["in.png"], check=check)


@case("encoder", "depth-16-forzado", "--depth", "16")
def _(d):
    s = base_gradient()
    save(s, d / "in.png")
    def check(outs, cd, res):
        o = read_rgba(outs[0])
        return o.dtype == np.uint16 and (o[s[..., 3] > 0] == s[s[..., 3] > 0].astype(np.uint16) * 257).all(), f"{o.dtype}"
    return Expect(["in.png"], check=check)


@case("encoder", "no-reduce-deja-rgba", "--no-reduce")
def _(d):
    s = base_palette()
    save(s, d / "in.png")
    return Expect(["in.png"], check=expect_ihdr(ct=6, depth=8)(s))


# ---------------------------------------------------------------------------------------------------
# metadatos
# ---------------------------------------------------------------------------------------------------

def tiny_icc(description: str, space: bytes = b"RGB ") -> bytes:
    """Perfil ICC v2 minimo (encabezado + tag 'desc'): suficiente para probar que se conserva."""
    text = description.encode("ascii") + b"\0"
    desc = b"desc" + bytes(4) + struct.pack(">I", len(text)) + text + bytes(12 + 67)
    header = bytearray(128)
    size = 128 + 4 + 12 + len(desc)
    struct.pack_into(">I", header, 0, size)
    header[8:12] = bytes([2, 0x10, 0, 0])
    header[12:16] = b"mntr"
    header[16:20] = space
    header[20:24] = b"XYZ "
    header[36:40] = b"acsp"
    table = struct.pack(">I", 1) + b"desc" + struct.pack(">II", 128 + 4 + 12, len(desc))
    return bytes(header) + table + desc


@case("meta", "perfil-srgb-pasa-a-chunk-srgb")
def _(d):
    icc = Path(os.environ.get("WINDIR", "C:/Windows")) / "System32/spool/drivers/color/sRGB Color Space Profile.icm"
    Image.fromarray(base_photo()).save(d / "in.png", icc_profile=icc.read_bytes())
    def check(outs, cd, res):
        kinds = [k for k, _ in png_chunks(outs[0])]
        return b"sRGB" in kinds and b"iCCP" not in kinds, f"chunks {[k.decode() for k in kinds]}"
    return Expect(["in.png"], check=check)


@case("meta", "perfil-propio-se-conserva")
def _(d):
    profile = tiny_icc("Perfil de prueba prisma")
    Image.fromarray(base_photo()).save(d / "in.png", icc_profile=profile)

    def check(outs, cd, res):
        # Lo que importa es el PERFIL (bytes descomprimidos), no el nombre del chunk: Pillow ya lo
        # guarda como "ICC Profile" en la entrada, asi que el nombre no dice nada.
        ch = dict(png_chunks(outs[0]))
        if b"iCCP" not in ch:
            return False, f"sin iCCP: {[k.decode() for k in ch]}"
        name, rest = ch[b"iCCP"].split(b"\0", 1)
        same = zlib.decompress(rest[1:]) == profile
        return same, f"iCCP \"{name.decode('latin-1')}\" · perfil identico byte a byte: {same}"
    return Expect(["in.png"], check=check)


@case("meta", "perfil-rgb-impide-gris")
def _(d):
    g = base_photo()[..., :1].repeat(3, -1)
    Image.fromarray(g).save(d / "in.png", icc_profile=tiny_icc("RGB no sRGB"))
    def check(outs, cd, res):
        # Con un perfil RGB el PNG no puede ser gris (tipos 0 y 4); RGB, paleta o RGBA son validos.
        ct = ihdr(outs[0])[3]
        has = any(k == b"iCCP" for k, _ in png_chunks(outs[0]))
        return ct in (2, 3, 6) and has, f"tipo {ct} (nunca gris con perfil RGB) · iCCP {has}"
    return Expect(["in.png"], check=check)


@case("meta", "strip-no-deja-nada", "--strip")
def _(d):
    Image.fromarray(base_photo()).save(d / "in.png", icc_profile=tiny_icc("x"), dpi=(300, 300))
    def check(outs, cd, res):
        kinds = {k for k, _ in png_chunks(outs[0])}
        extra = kinds - {b"IHDR", b"PLTE", b"tRNS", b"IDAT", b"IEND"}
        return not extra, f"chunks auxiliares: {sorted(k.decode() for k in extra) or 'ninguno'}"
    return Expect(["in.png"], check=check)


@case("meta", "texto-utf8-y-dpi", "--text", "Author=Ñandú 🎨", "--dpi", "300")
def _(d):
    save(base_photo(), d / "in.png")
    def check(outs, cd, res):
        ch = png_chunks(outs[0])
        itxt = [v for k, v in ch if k == b"iTXt"]
        phys = [v for k, v in ch if k == b"pHYs"]
        ok_text = bool(itxt) and "Ñandú 🎨" in itxt[0].split(b"\0")[-1].decode("utf-8")
        ok_dpi = bool(phys) and struct.unpack(">IIB", phys[0])[0] == 11811
        return ok_text and ok_dpi, f"iTXt {ok_text} · pHYs 11811 {ok_dpi}"
    return Expect(["in.png"], check=check)


@case("meta", "exif-orientacion-queda-en-1", "--keep-meta")
def _(d):
    exif = Image.Exif()
    exif[0x0112] = 6
    exif[0x010F] = "prisma"
    Image.fromarray(base_photo(40, 64)).save(d / "in.jpg", quality=95, exif=exif)
    def check(outs, cd, res):
        w, h, *_ = ihdr(outs[0])
        ex = [v for k, v in png_chunks(outs[0]) if k == b"eXIf"]
        orient = Image.Exif()
        if ex:
            orient.load(b"Exif\0\0" + ex[0])
        o = orient.get(0x0112)
        return (w, h) == (40, 64) and o == 1, f"{w}x{h} (rotada) · orientacion guardada {o}"
    return Expect(["in.jpg"], check=check)


# ---------------------------------------------------------------------------------------------------
# animaciones
# ---------------------------------------------------------------------------------------------------

def make_anim_gif(d, frames=6, duplicate=False):
    ims = []
    for k in range(frames):
        img = np.zeros((36, 48, 4), np.uint8)
        kk = 0 if duplicate and k == 1 else k
        img[4 + kk * 3:18 + kk * 3, 4 + kk * 5:20 + kk * 5] = (40 * kk, 255 - 30 * kk, 200, 255)
        ims.append(Image.fromarray(img, "RGBA"))
    ims[0].save(d / "in.gif", save_all=True, append_images=ims[1:], duration=[90] * frames, loop=0, disposal=2, optimize=False)


@case("anim", "gif-a-apng")
def _(d):
    make_anim_gif(d)
    return Expect(["in.gif"], pillow=True)


@case("anim", "split-archivos-sueltos", "--split")
def _(d):
    make_anim_gif(d)
    return Expect(["in.gif"], expect_files=6)


@case("anim", "no-apng-primer-cuadro", "--no-apng")
def _(d):
    make_anim_gif(d)
    first = np.asarray(Image.open(d / "in.gif").convert("RGBA"))
    return Expect(["in.gif"], truth=first)


@case("anim", "frame-2", "--frame", "2")
def _(d):
    make_anim_gif(d)
    im = Image.open(d / "in.gif")
    im.seek(2)
    return Expect(["in.gif"], truth=np.asarray(im.convert("RGBA")))


@case("anim", "loops-3", "--loops", "3")
def _(d):
    make_anim_gif(d)
    def check(outs, cd, res):
        actl = dict(png_chunks(outs[0])).get(b"acTL")
        plays = struct.unpack(">II", actl)[1] if actl else None
        return plays == 3, f"num_plays {plays}"
    return Expect(["in.gif"], check=check)


@case("anim", "cuadros-repetidos-se-funden")
def _(d):
    make_anim_gif(d, frames=5, duplicate=True)
    def check(outs, cd, res):
        actl = dict(png_chunks(outs[0])).get(b"acTL")
        frames = struct.unpack(">II", actl)[0] if actl else 0
        im = Image.open(outs[0])
        durs = [f.info.get("duration") for f in ImageSequence.Iterator(im)]
        return frames == 4 and durs[0] == 180, f"cuadros {frames} (de 5) · duraciones {durs}"
    return Expect(["in.gif"], check=check)


# ---------------------------------------------------------------------------------------------------
# casos borde
# ---------------------------------------------------------------------------------------------------

@case("borde", "archivo-vacio")
def _(d):
    (d / "in.png").write_bytes(b"")
    return Expect(["in.png"], fail="vacio")


@case("borde", "basura")
def _(d):
    (d / "in.xyz").write_bytes(bytes(rng(40).integers(0, 256, 5000, dtype=np.uint8)))
    return Expect(["in.xyz"], fail="formato")


@case("borde", "no-existe")
def _(d):
    return Expect(["no-existe.png"], fail="no existe")


@case("borde", "ruta-unicode")
def _(d):
    sub = d / "ñandú 🎨 carpeta"
    sub.mkdir()
    s = base_gradient(10, 12)
    save(s, sub / "foto é ü.tga")
    return Expect([str(Path("ñandú 🎨 carpeta") / "foto é ü.tga")], truth=s, compare_hidden=True)


@case("borde", "1x1-en-varios-formatos")
def _(d):
    # Un pixel translucido en todo formato que lleva alfa; PCX no tiene alfa y va con el escritor
    # propio porque el de Pillow trunca las lineas de 1 pixel de ancho (ni Pillow las relee).
    px = np.array([[[10, 200, 30, 77]]], np.uint8)
    opaque_px = np.array([[[10, 200, 30, 255]]], np.uint8)
    lossless = {"webp": {"lossless": True, "exact": True}}
    expected = {}
    for ext in ("bmp", "tga", "qoi", "sgi", "tif", "webp"):
        save(px, d / f"one.{ext}", **lossless.get(ext, {}))
        expected[ext] = px
    writers.pcx(d / "one.pcx", opaque_px[..., :3])
    expected["pcx"] = opaque_px

    def check(outs, cd, res):
        got = {Path(o).name.split(".")[1]: o for o in outs}          # one.<ext>.png
        if got.keys() != expected.keys():
            return False, f"salidas {sorted(got)}"
        bad = [f"{ext}: {tuple(int(v) for v in read_rgba(o)[0, 0])}" for ext, o in got.items()
               if ihdr(o)[:2] != (1, 1) or not np.array_equal(read_rgba(o), expected[ext])]
        return not bad, f"{len(outs)} salidas 1x1 exactas (alfa 77 donde el formato lo lleva)" if not bad else "; ".join(bad)
    return Expect([f"one.{e}" for e in expected], check=check)


@case("borde", "limite-de-pixeles", "--max-pixels", "1")
def _(d):
    save(base_photo(1200, 1000), d / "in.bmp")
    return Expect(["in.bmp"], fail="supera")


@case("borde", "colision-de-nombres")
def _(d):
    save(base_photo(10, 10), d / "a.bmp")
    save(base_photo(10, 10, 3), d / "a.tga")
    def check(outs, cd, res):
        names = sorted(Path(o).name for o in outs)
        return names == ["a.bmp.png", "a.tga.png"], f"{names}"
    return Expect(["a.bmp", "a.tga"], check=check)


@case("borde", "misma-entrada-dos-veces")
def _(d):
    save(base_photo(10, 10), d / "a.bmp")
    return Expect(["a.bmp", str(Path(".") / "a.bmp")], expect_files=1)
