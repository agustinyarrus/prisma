"""refpng.py - escritor PNG de referencia, deliberadamente simple, para probar el DECODIFICADOR.

Pillow no escribe PNG entrelazados, ni de 1/2/4 bits en gris con tRNS, ni con los cinco filtros
alternados por fila, ni con el IDAT partido en pedacitos. Este modulo si: cada variante valida de la
especificacion, para que el decodificador de prisma se pruebe contra todas.
"""
from __future__ import annotations

import struct
import zlib

import numpy as np

ADAM7 = [(0, 0, 8, 8), (4, 0, 8, 8), (0, 4, 4, 8), (2, 0, 4, 4), (0, 2, 2, 4), (1, 0, 2, 2), (0, 1, 1, 2)]
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunk(kind: bytes, data: bytes) -> bytes:
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)


def pack_row(samples: np.ndarray, depth: int) -> bytes:
    """Una fila de muestras (W*C enteros) al formato de bits de PNG."""
    flat = samples.reshape(-1).astype(np.uint32)
    if depth == 16:
        return flat.astype(">u2").tobytes()
    if depth == 8:
        return flat.astype(np.uint8).tobytes()
    per = 8 // depth
    out = bytearray((len(flat) * depth + 7) // 8)
    for i, v in enumerate(flat):
        out[i // per] |= (int(v) & ((1 << depth) - 1)) << (8 - depth * (i % per + 1))
    return bytes(out)


def paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)


def filter_row(kind: int, row: bytes, prev: bytes, bpp: int) -> bytes:
    out = bytearray(len(row))
    for i, x in enumerate(row):
        a = row[i - bpp] if i >= bpp else 0
        b = prev[i]
        c = prev[i - bpp] if i >= bpp else 0
        pred = (0, a, b, (a + b) >> 1, paeth(a, b, c))[kind]
        out[i] = (x - pred) & 0xFF
    return bytes([kind]) + bytes(out)


def encode_image(samples: np.ndarray, depth: int, channels: int, filters: str | int) -> bytes:
    """Filas filtradas (sin comprimir) de una imagen HxWxC."""
    h = samples.shape[0]
    bpp = max(1, channels * depth // 8)
    raw = bytearray()
    prev = None
    for y in range(h):
        row = pack_row(samples[y], depth)
        if prev is None:
            prev = bytes(len(row))
        kind = (y % 5) if filters == "cycle" else int(filters)
        raw += filter_row(kind, row, prev, bpp)
        prev = row
    return bytes(raw)


def write_png(path, samples: np.ndarray, color_type: int, depth: int, *, palette=None, trns: bytes | None = None,
              interlace: bool = False, filters: str | int = "cycle", idat_chunk: int = 0,
              extra_before_idat: list[tuple[bytes, bytes]] | None = None, level: int = 9) -> None:
    """samples: HxWxC con los valores crudos de cada muestra (indices si es paleta)."""
    if samples.ndim == 2:
        samples = samples[:, :, None]
    h, w, c = samples.shape
    assert c == CHANNELS[color_type]
    raw = bytearray()
    if not interlace:
        raw += encode_image(samples, depth, c, filters)
    else:
        for x0, y0, dx, dy in ADAM7:
            sub = samples[y0::dy, x0::dx]
            if sub.shape[0] and sub.shape[1]:
                raw += encode_image(sub, depth, c, filters)
    z = zlib.compress(bytes(raw), level)
    out = bytearray(b"\x89PNG\r\n\x1a\n")
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color_type, 0, 0, 1 if interlace else 0))
    for kind, data in extra_before_idat or []:
        out += chunk(kind, data)
    if palette is not None:
        out += chunk(b"PLTE", bytes(np.asarray(palette, np.uint8).reshape(-1)))
    if trns is not None:
        out += chunk(b"tRNS", trns)
    step = idat_chunk or len(z)
    for i in range(0, len(z), step):
        out += chunk(b"IDAT", z[i:i + step])
    out += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(out)


def write_apng(path, canvas_w: int, canvas_h: int, frames: list[dict], plays: int = 0) -> list[np.ndarray]:
    """APNG RGBA de 8 bits con cuadros explicitos: dict(img, x, y, dispose, blend, delay_ms).
    Devuelve los lienzos compuestos segun la especificacion (el oraculo)."""
    out = bytearray(b"\x89PNG\r\n\x1a\n")
    out += chunk(b"IHDR", struct.pack(">IIBBBBB", canvas_w, canvas_h, 8, 6, 0, 0, 0))
    out += chunk(b"acTL", struct.pack(">II", len(frames), plays))
    seq = 0
    for i, f in enumerate(frames):
        img = f["img"]
        h, w, _ = img.shape
        out += chunk(b"fcTL", struct.pack(">IIIIIHHBB", seq, w, h, f["x"], f["y"], f["delay_ms"], 1000, f["dispose"], f["blend"]))
        seq += 1
        z = zlib.compress(encode_image(img, 8, 4, "cycle"), 9)
        if i == 0:
            out += chunk(b"IDAT", z)
        else:
            out += chunk(b"fdAT", struct.pack(">I", seq) + z)
            seq += 1
    out += chunk(b"IEND", b"")
    with open(path, "wb") as fh:
        fh.write(out)
    return read_png(path)


# ---------------------------------------------------------------------------------------------------
# Lector de referencia (independiente del C): lo usan las pruebas donde Pillow se equivoca
#   - gris de 1/2/4 bits con tRNS (Pillow compara la clave cruda contra el valor ya expandido)
#   - APNG con blend OVER sobre destino translucido (Pillow usa paste con mascara, no Porter-Duff)
# ---------------------------------------------------------------------------------------------------


def _chunks(data: bytes):
    at = 8
    while at + 8 <= len(data):
        n = struct.unpack(">I", data[at:at + 4])[0]
        yield data[at + 4:at + 8], data[at + 8:at + 8 + n]
        at += 12 + n


def _unfilter(raw: bytes, w: int, h: int, depth: int, channels: int) -> np.ndarray:
    bpp = max(1, channels * depth // 8)
    rb = (w * channels * depth + 7) // 8
    out = np.zeros((h, rb), np.uint8)
    prev = np.zeros(rb, np.int32)
    pos = 0
    for y in range(h):
        kind = raw[pos]
        row = np.frombuffer(raw, np.uint8, rb, pos + 1).astype(np.int32)
        pos += rb + 1
        cur = np.zeros(rb, np.int32)
        for i in range(rb):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            pred = (0, a, b, (a + b) >> 1, paeth(a, b, c))[kind]
            cur[i] = (row[i] + pred) & 0xFF
        out[y] = cur
        prev = cur
    return out


def _samples(rows: np.ndarray, w: int, depth: int, channels: int) -> np.ndarray:
    h = rows.shape[0]
    if depth == 16:
        return rows.view(">u2").reshape(h, -1)[:, : w * channels].reshape(h, w, channels).astype(np.uint32)
    if depth == 8:
        return rows[:, : w * channels].reshape(h, w, channels).astype(np.uint32)
    bits = np.unpackbits(rows, axis=1)[:, : w * depth].reshape(h, w, depth)
    weights = 1 << np.arange(depth - 1, -1, -1)
    return (bits * weights).sum(-1).reshape(h, w, 1).astype(np.uint32)


def _decode_stream(z: bytes, w: int, h: int, depth: int, ct: int, interlace: int) -> np.ndarray:
    raw = zlib.decompress(z)
    ch = CHANNELS[ct]
    if not interlace:
        return _samples(_unfilter(raw, w, h, depth, ch), w, depth, ch)
    out = np.zeros((h, w, ch), np.uint32)
    pos = 0
    for x0, y0, dx, dy in ADAM7:
        pw, ph = (w - x0 + dx - 1) // dx if w > x0 else 0, (h - y0 + dy - 1) // dy if h > y0 else 0
        if not pw or not ph:
            continue
        rb = (pw * ch * depth + 7) // 8
        n = ph * (rb + 1)
        out[y0::dy, x0::dx] = _samples(_unfilter(raw[pos:pos + n], pw, ph, depth, ch), pw, depth, ch)
        pos += n
    return out


def _over(dst: np.ndarray, src: np.ndarray) -> np.ndarray:
    """Porter-Duff OVER con alfa directo, en flotante (lo que pide la especificacion de APNG)."""
    d, s = dst.astype(np.float64), src.astype(np.float64)
    full = 65535.0 if dst.dtype == np.uint16 else 255.0
    sa, da = s[..., 3:4] / full, d[..., 3:4] / full
    oa = sa + da * (1 - sa)
    with np.errstate(invalid="ignore", divide="ignore"):
        oc = np.where(oa > 0, (s[..., :3] * sa + d[..., :3] * da * (1 - sa)) / oa, 0)
    return np.dstack([np.round(oc), np.round(oa * full)]).astype(dst.dtype)


def read_png(path) -> list[np.ndarray]:
    """Todos los cuadros (compuestos) de un PNG/APNG como RGBA de 8 o 16 bits."""
    data = open(path, "rb").read()
    info, frames_ctl, idat, fdat, cur = {}, [], [], {}, None
    for kind, body in _chunks(data):
        if kind == b"IHDR":
            w, h, depth, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            info.update(w=w, h=h, depth=depth, ct=ct, il=il)
        elif kind == b"PLTE":
            info["plte"] = body
        elif kind == b"tRNS":
            info["trns"] = body
        elif kind == b"fcTL":
            seq, fw, fh, x, y, dn, dd, dop, bop = struct.unpack(">IIIIIHHBB", body)
            cur = dict(w=fw, h=fh, x=x, y=y, dispose=dop, blend=bop, data=bytearray(), idat=not idat)
            frames_ctl.append(cur)
        elif kind == b"IDAT":
            idat.append(body)
            if cur is not None and cur["idat"]:
                cur["data"] += body
        elif kind == b"fdAT":
            cur["data"] += body[4:]
    w, h, depth, ct, il = info["w"], info["h"], info["depth"], info["ct"], info["il"]

    def to_rgba(samples):
        pal = None
        if ct == 3:
            pal = np.frombuffer(info["plte"], np.uint8).reshape(-1, 3)
        return expected_rgba(samples, ct, depth, palette=pal, trns=info.get("trns"))

    if not frames_ctl:
        return [to_rgba(_decode_stream(b"".join(idat), w, h, depth, ct, il))]
    canvas = None
    out = []
    for i, f in enumerate(frames_ctl):
        img = to_rgba(_decode_stream(bytes(f["data"]), f["w"], f["h"], depth, ct, il))
        if canvas is None:
            canvas = np.zeros((h, w, 4), img.dtype)
        region = canvas[f["y"]:f["y"] + f["h"], f["x"]:f["x"] + f["w"]]
        saved = region.copy()
        dispose = 1 if (i == 0 and f["dispose"] == 2) else f["dispose"]
        region[...] = _over(region, img) if f["blend"] == 1 else img
        out.append(canvas.copy())
        if dispose == 1:
            region[...] = 0
        elif dispose == 2:
            region[...] = saved
    return out


def expected_rgba(samples: np.ndarray, color_type: int, depth: int, *, palette=None, trns=None) -> np.ndarray:
    """La imagen RGBA que un decodificador correcto tiene que entregar (8 o 16 bits)."""
    if samples.ndim == 2:
        samples = samples[:, :, None]
    h, w, _ = samples.shape
    big = depth == 16
    maxv = (1 << depth) - 1
    out = np.zeros((h, w, 4), np.uint16 if big else np.uint8)
    full = 65535 if big else 255
    if color_type == 3:
        pal = np.asarray(palette, np.uint8).reshape(-1, 3)
        alphas = np.full(len(pal), 255, np.uint8)
        if trns:
            alphas[:len(trns)] = np.frombuffer(trns, np.uint8)[:len(pal)]
        idx = samples[..., 0]
        out[..., :3] = pal[idx]
        out[..., 3] = alphas[idx]
        return out

    def scale(v):
        v = v.astype(np.uint32)
        return v if depth >= 8 else (v * (255 // maxv)).astype(np.uint32)

    if color_type in (0, 4):
        g = scale(samples[..., 0])
        out[..., 0] = out[..., 1] = out[..., 2] = g
        out[..., 3] = samples[..., 1] if color_type == 4 else full
        if color_type == 0 and trns:
            key = struct.unpack(">H", trns[:2])[0]
            out[..., 3] = np.where(samples[..., 0] == key, 0, full)
    else:
        out[..., :3] = samples[..., :3]
        out[..., 3] = samples[..., 3] if color_type == 6 else full
        if color_type == 2 and trns:
            key = struct.unpack(">HHH", trns[:6])
            hit = (samples[..., 0] == key[0]) & (samples[..., 1] == key[1]) & (samples[..., 2] == key[2])
            out[..., 3] = np.where(hit, 0, full)
    return out
