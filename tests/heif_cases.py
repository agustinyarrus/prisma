"""heif_cases.py - AVIF/HEIF por el codec de Windows y color CICP (chunk cICP de PNG 3.a ed.).

Lo que se prueba es lo que prisma le agrega al codec de Windows, que por su cuenta:
  - ignora el alfa (imagen auxiliar) de AVIF/HEIC,
  - convierte YCbCr con BT.709 aunque el archivo declare otra matriz,
  - no dice nada de los primarios (Display P3, BT.2020).
El oraculo es libavif (via Pillow) o la imagen original. Como AVIF es con perdida y el alfa bajo
amplifica cualquier diferencia de color, el error se mide PREMULTIPLICADO (lo que se ve).
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

import numpy as np
from PIL import Image, features

import writers
from cases import Case, CASES, Expect, base_gradient, base_photo, case, png_chunks, read_rgba, save, within

HAS_AVIF = features.check("avif")


# ---------------------------------------------------------------------------------------------------
# metricas y referencias
# ---------------------------------------------------------------------------------------------------

def within_linear(got, want, max_tol: float, mean_tol: float) -> tuple[bool, str]:
    """Como `within`, pero en luz LINEAL: cerca del negro la curva sRGB tiene pendiente 12,92 y
    multiplica por 13 cualquier diferencia de decodificacion; la conversion de gama es lineal en
    luz lineal y ahi las diferencias se comparan en su tamaño real."""
    ga, wa = got[..., 3:4] / 255.0, want[..., 3:4] / 255.0
    lin_g, lin_w = srgb_to_lin(got[..., :3] / 255.0), srgb_to_lin(want[..., :3] / 255.0)
    err = np.abs(lin_g * ga - lin_w * wa).max(-1) * 255
    ok = err.max() <= max_tol and err.mean() <= mean_tol
    return ok, f"error visible en luz lineal max {err.max():.1f} (tol {max_tol:.1f}) · media {err.mean():.2f} (tol {mean_tol:.2f})"


def libavif(path) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGBA")).astype(np.int64)


def chromaticity_matrix(rgb_xy, white_xy) -> np.ndarray:
    """RGB lineal -> XYZ para unos primarios (implementacion independiente de la del C)."""
    cols = np.array([[x / y, 1.0, (1 - x - y) / y] for x, y in rgb_xy]).T
    wx, wy = white_xy
    s = np.linalg.solve(cols, np.array([wx / wy, 1.0, (1 - wx - wy) / wy]))
    return cols * s


D65 = (0.3127, 0.3290)
SRGB_XY = [(0.64, 0.33), (0.30, 0.60), (0.15, 0.06)]
P3_XY = [(0.680, 0.320), (0.265, 0.690), (0.150, 0.060)]


def srgb_to_lin(v):
    return np.where(v <= 0.04045, v / 12.92, ((v + 0.055) / 1.055) ** 2.4)


def lin_to_srgb(v):
    v = np.clip(v, 0, 1)
    return np.where(v <= 0.0031308, v * 12.92, 1.055 * v ** (1 / 2.4) - 0.055)


def p3_to_srgb(rgba: np.ndarray) -> np.ndarray:
    m = np.linalg.inv(chromaticity_matrix(SRGB_XY, D65)) @ chromaticity_matrix(P3_XY, D65)
    lin = srgb_to_lin(rgba[..., :3].astype(np.float64) / 255) @ m.T
    out = rgba.astype(np.float64).copy()
    out[..., :3] = np.round(lin_to_srgb(lin) * 255)
    return out.astype(np.int64)


def cicp_of(path) -> bytes | None:
    return next((v for k, v in png_chunks(path) if k == b"cICP"), None)


def png_with_cicp(src: Path, dst: Path, cicp: bytes) -> None:
    """Inserta un chunk cICP justo despues de IHDR (tiene que ir antes de PLTE e IDAT)."""
    data = src.read_bytes()
    ihdr_end = 8 + 8 + 13 + 4
    chunk = struct.pack(">I", 4) + b"cICP" + cicp + struct.pack(">I", zlib.crc32(b"cICP" + cicp))
    dst.write_bytes(data[:ihdr_end] + chunk + data[ihdr_end:])


# ---------------------------------------------------------------------------------------------------
# AVIF por WIC
# ---------------------------------------------------------------------------------------------------

def avif_source(d: Path, name: str = "base.avif", **kw) -> Path:
    path = d / name
    Image.fromarray(base_gradient()).save(path, **({"quality": 90} | kw))
    return path


def avif_variant(name: str, *, patch: dict | None = None, check_fn=None, args=(), prem=False):
    def make(d: Path) -> Expect:
        src = avif_source(d)
        if prem:
            s = base_gradient()
            pm = s.copy()
            pm[..., :3] = (s[..., :3].astype(int) * s[..., 3:4] + 127) // 255
            Image.fromarray(pm).save(d / "pm.avif", quality=100, subsampling="4:4:4")
            writers.avif_add_prem(d / "pm.avif", d / "in.avif")
        elif patch:
            writers.avif_patch_nclx(src, d / "in.avif", **patch)
        else:
            src.rename(d / "in.avif")
        return Expect(["in.avif"], check=check_fn)
    CASES.append(Case("heif", name, make, list(args)))


# Windows y libavif interpolan el croma distinto: con BT.709 declarado (que Windows ya decodificaba
# bien) la diferencia medida es max 4,0 / media 0,68 niveles visibles. Ese es el piso; cualquier
# error de matriz, rango o gama da decenas.
AVIF_MAX, AVIF_MEAN = 5.0, 0.9


def vs_libavif(max_tol=AVIF_MAX, mean_tol=AVIF_MEAN):
    def check(outs, cd, res):
        return within(read_rgba(outs[0]).astype(np.int64), libavif(Path(cd) / "in.avif"), max_tol, mean_tol)
    return check


if HAS_AVIF:
    # libavif escribe BT.601 por omision; Windows decodifica como BT.709. Sin correccion: max 34.
    avif_variant("avif-bt601-color-corregido", check_fn=vs_libavif())
    avif_variant("avif-bt709-declarado", patch={"matrix": 1}, check_fn=vs_libavif())
    avif_variant("avif-bt2020-matriz", patch={"matrix": 9}, check_fn=vs_libavif())
    avif_variant("avif-rango-limitado", patch={"full_range": False}, check_fn=vs_libavif())

    def identity_warns(outs, cd, res):
        warned = any("matriz CICP 0" in w for w in res.get("warnings", []))
        return warned, f"aviso de matriz identidad: {warned}"
    avif_variant("avif-matriz-identidad-avisa", patch={"matrix": 0}, check_fn=identity_warns)

    def prem_check(outs, cd, res):
        ok, detail = within(read_rgba(outs[0]).astype(np.int64), base_gradient().astype(np.int64), AVIF_MAX, AVIF_MEAN)
        origin = res.get("alpha_origin", "")
        return ok, f"{detail} · {origin}".rstrip(" ·")
    avif_variant("avif-alfa-premultiplicado", prem=True, check_fn=prem_check)

    def p3_tagged(outs, cd, res):
        c = cicp_of(outs[0])
        ok_px, detail = within(read_rgba(outs[0]).astype(np.int64), libavif(Path(cd) / "in.avif"), AVIF_MAX, AVIF_MEAN)
        return c == bytes([12, 13, 0, 1]) and ok_px, f"cICP {list(c) if c else None} · {detail}"
    avif_variant("avif-display-p3-queda-en-cicp", patch={"primaries": 12}, check_fn=p3_tagged)

    def p3_converted(outs, cd, res):
        c = cicp_of(outs[0])
        srgb = any(k == b"sRGB" for k, _ in png_chunks(outs[0]))
        want = p3_to_srgb(libavif(Path(cd) / "in.avif"))
        ok_px, detail = within_linear(read_rgba(outs[0]).astype(np.int64), want, *AVIF_P3_LINEAR_TOL)
        return c is None and srgb and ok_px, f"sin cICP {c is None} · chunk sRGB {srgb} · {detail}"
    # En luz lineal el piso de decodificacion medido es max 3,5 / media 0,75, y la conversion lo puede
    # agrandar a lo sumo por la norma infinito de la matriz P3 -> sRGB (1,45).
    P3_TO_SRGB = np.linalg.inv(chromaticity_matrix(SRGB_XY, D65)) @ chromaticity_matrix(P3_XY, D65)
    P3_GAIN = float(np.abs(P3_TO_SRGB).sum(1).max())
    AVIF_P3_LINEAR_TOL = (3.5 * P3_GAIN, 0.75 * P3_GAIN)
    avif_variant("avif-display-p3-a-srgb", patch={"primaries": 12}, check_fn=p3_converted, args=["--icc", "srgb"])


# ---------------------------------------------------------------------------------------------------
# PNG con cICP
# ---------------------------------------------------------------------------------------------------

@case("heif", "png-cicp-se-conserva")
def _(d):
    save(base_gradient(), d / "plain.png")
    png_with_cicp(d / "plain.png", d / "in.png", bytes([12, 13, 0, 1]))

    def check(outs, cd, res):
        c = cicp_of(outs[0])
        return c == bytes([12, 13, 0, 1]), f"cICP {list(c) if c else None}"
    return Expect(["in.png"], check=check)


@case("heif", "png-cicp-p3-a-srgb-exacto", "--icc", "srgb")
def _(d):
    s = base_gradient()
    save(s, d / "plain.png")
    png_with_cicp(d / "plain.png", d / "in.png", bytes([12, 13, 0, 1]))
    want = p3_to_srgb(s)

    def check(outs, cd, res):
        got = read_rgba(outs[0]).astype(np.int64)
        diff = int(np.abs(got - want).max())
        return diff <= 1 and cicp_of(outs[0]) is None, f"dif. max {diff} contra la matriz P3->sRGB de numpy (tol 1)"
    return Expect(["in.png"], check=check)


@case("heif", "png-cicp-hdr-pq-no-se-convierte", "--icc", "srgb")
def _(d):
    save(base_photo(20, 30), d / "plain.png")
    png_with_cicp(d / "plain.png", d / "in.png", bytes([9, 16, 0, 1]))

    def check(outs, cd, res):
        c = cicp_of(outs[0])
        warned = any("HDR" in w for w in res.get("warnings", []))
        return c == bytes([9, 16, 0, 1]) and warned, f"cICP conservado {list(c) if c else None} · aviso HDR {warned}"
    return Expect(["in.png"], check=check)
