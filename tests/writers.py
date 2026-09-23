"""writers.py - escritores minimos de formatos (y variantes) que Pillow no produce.

Cada funcion escribe un archivo y la prueba conoce de antemano los pixeles que un decodificador
correcto tiene que devolver: el "oraculo" es la imagen con la que se genero el archivo.
"""
from __future__ import annotations

import struct

import numpy as np

# ---------------------------------------------------------------------------------------------
# BMP
# ---------------------------------------------------------------------------------------------


def _bmp_file(dib: bytes, pixel_offset_extra: int = 0) -> bytes:
    """'BM' + encabezado de archivo; dib = encabezado de info + paleta/mascaras + pixeles."""
    return b"BM" + struct.pack("<IHHI", 14 + len(dib), 0, 0, 14 + pixel_offset_extra) + dib


def bmp_v5_argb(path, rgba: np.ndarray, zero_alpha: bool = False) -> None:
    """32 bits BI_BITFIELDS con encabezado V5 y mascara de alfa (asi lo guarda Windows)."""
    h, w, _ = rgba.shape
    px = rgba.copy()
    if zero_alpha:
        px[..., 3] = 0
    bgra = px[::-1, :, [2, 1, 0, 3]].tobytes()
    hdr = struct.pack("<IiiHHIIiiII", 124, w, h, 1, 32, 3, len(bgra), 2835, 2835, 0, 0)
    hdr += struct.pack("<IIII", 0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000)
    hdr += struct.pack("<I", 0x73524742) + bytes(36) + bytes(12) + struct.pack("<IIII", 4, 0, 0, 0)
    assert len(hdr) == 124
    with open(path, "wb") as f:
        f.write(_bmp_file(hdr + bgra, 124))


def bmp_32_bi_rgb(path, rgba: np.ndarray, zero_alpha: bool) -> None:
    """32 bits BI_RGB (4.o byte "reservado"): con alfa real o todo en 0."""
    h, w, _ = rgba.shape
    px = rgba.copy()
    if zero_alpha:
        px[..., 3] = 0
    data = px[::-1, :, [2, 1, 0, 3]].tobytes()
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 32, 0, len(data), 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(_bmp_file(hdr + data, 40))


def bmp_565(path, rgb: np.ndarray) -> np.ndarray:
    """16 bits 5:6:5 con BI_BITFIELDS. Devuelve lo que un decodificador correcto entrega."""
    h, w, _ = rgb.shape
    r, g, b = (rgb[..., 0] >> 3).astype(np.uint16), (rgb[..., 1] >> 2).astype(np.uint16), (rgb[..., 2] >> 3).astype(np.uint16)
    v = (r << 11) | (g << 5) | b
    stride = ((w * 16 + 31) // 32) * 4
    rows = bytearray()
    for y in range(h - 1, -1, -1):
        row = v[y].astype("<u2").tobytes()
        rows += row + bytes(stride - len(row))
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 16, 3, len(rows), 0, 0, 0, 0) + struct.pack("<III", 0xF800, 0x07E0, 0x001F)
    with open(path, "wb") as f:
        f.write(_bmp_file(hdr + bytes(rows), 52))
    out = np.zeros((h, w, 4), np.uint8)
    out[..., 0] = (r << 3) | (r >> 2)
    out[..., 1] = (g << 2) | (g >> 4)
    out[..., 2] = (b << 3) | (b >> 2)
    out[..., 3] = 255
    return out


def bmp_1010102(path, rgb16: np.ndarray) -> np.ndarray:
    """32 bits con mascaras de 10 bits por canal (y 2 de alfa en 0 -> opaco). Sale en 16 bits."""
    h, w, _ = rgb16.shape
    r, g, b = (rgb16[..., 0] >> 6).astype(np.uint32), (rgb16[..., 1] >> 6).astype(np.uint32), (rgb16[..., 2] >> 6).astype(np.uint32)
    v = (r << 20) | (g << 10) | b
    data = v[::-1].astype("<u4").tobytes()
    hdr = struct.pack("<IiiHHIIiiII", 56, w, h, 1, 32, 3, len(data), 0, 0, 0, 0)
    hdr += struct.pack("<IIII", 0x3FF00000, 0x000FFC00, 0x000003FF, 0)
    with open(path, "wb") as f:
        f.write(_bmp_file(hdr + data, 56))
    out = np.zeros((h, w, 4), np.uint16)
    for i, ch in enumerate((r, g, b)):
        out[..., i] = (ch * 65535 + 511) // 1023
    out[..., 3] = 65535
    return out


def bmp_rle8_with_holes(path) -> np.ndarray:
    """RLE8 de 8x6 con un salto (delta) y un fin de linea temprano: lo salteado queda TRANSPARENTE."""
    w, h = 8, 6
    pal = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    rle = bytearray()
    rle += bytes([8, 1, 0, 0])                 # fila 0 (abajo): 8 rojos, fin de linea
    rle += bytes([3, 2, 0, 2, 2, 1])           # fila 1: 3 verdes, delta (+2, +1) -> fila 2, x=5
    rle += bytes([3, 3, 0, 0])                 # fila 2: 3 azules desde x=5, fin de linea
    rle += bytes([0, 4, 1, 2, 3, 1, 4, 1, 0, 0])   # fila 3: absoluto 4 [1,2,3,1], 4 rojos, fin de linea
    rle += bytes([0, 1])                       # fin del bitmap (filas 4 y 5 vacias)
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 8, 1, len(rle), 0, 0, 4, 0)
    palette = b"".join(bytes([b, g, r, 0]) for r, g, b in pal)
    with open(path, "wb") as f:
        f.write(_bmp_file(hdr + palette + bytes(rle), 40 + len(palette)))
    exp = np.zeros((h, w, 4), np.uint8)             # transparente por defecto

    def put(row_from_bottom, x, idx):
        y = h - 1 - row_from_bottom
        exp[y, x, :3] = pal[idx]
        exp[y, x, 3] = 255
    for x in range(8): put(0, x, 1)
    for x in range(3): put(1, x, 2)
    for x in range(5, 8): put(2, x, 3)
    for x, idx in enumerate([1, 2, 3, 1]): put(3, x, idx)
    for x in range(4, 8): put(3, x, 1)
    return exp


def bmp_os2_core(path, rgb: np.ndarray) -> None:
    """Encabezado OS/2 1.x (BITMAPCOREHEADER de 12 bytes), 24 bits."""
    h, w, _ = rgb.shape
    stride = ((w * 24 + 31) // 32) * 4
    rows = bytearray()
    for y in range(h - 1, -1, -1):
        row = rgb[y, :, ::-1].tobytes()
        rows += row + bytes(stride - len(row))
    hdr = struct.pack("<IHHHH", 12, w, h, 1, 24)
    with open(path, "wb") as f:
        f.write(_bmp_file(hdr + bytes(rows), 12))


def bmp_top_down(path, rgb: np.ndarray) -> None:
    h, w, _ = rgb.shape
    stride = ((w * 24 + 31) // 32) * 4
    rows = bytearray()
    for y in range(h):
        row = rgb[y, :, ::-1].tobytes()
        rows += row + bytes(stride - len(row))
    hdr = struct.pack("<IiiHHIIiiII", 40, w, -h, 1, 24, 0, len(rows), 0, 0, 0, 0)
    with open(path, "wb") as f:
        f.write(_bmp_file(hdr + bytes(rows), 40))


# ---------------------------------------------------------------------------------------------
# TGA
# ---------------------------------------------------------------------------------------------


def tga(path, rgba: np.ndarray, *, rle: bool = False, top_left: bool = True, alpha_type: int | None = None,
        premultiply: bool = False, bits: int = 32) -> None:
    h, w, _ = rgba.shape
    px = rgba.astype(np.uint32)
    if premultiply:
        px = px.copy()
        for c in range(3):
            px[..., c] = (px[..., c] * px[..., 3] + 127) // 255
    rows = px if top_left else px[::-1]
    if bits == 32:
        raw = rows[..., [2, 1, 0, 3]].astype(np.uint8).reshape(-1, 4)
    elif bits == 16:
        r, g, b = rows[..., 0] >> 3, rows[..., 1] >> 3, rows[..., 2] >> 3
        a = (rows[..., 3] >= 128).astype(np.uint32)
        v = (a << 15) | (r << 10) | (g << 5) | b
        raw = v.astype("<u2").view(np.uint8).reshape(-1, 2)
    else:
        raw = rows[..., [2, 1, 0]].astype(np.uint8).reshape(-1, 3)
    body = bytearray()
    if not rle:
        body += raw.tobytes()
    else:
        i, n = 0, len(raw)
        while i < n:
            j = i + 1
            while j < n and j - i < 128 and (raw[j] == raw[i]).all():
                j += 1
            if j - i > 1:
                body += bytes([0x80 | (j - i - 1)]) + raw[i].tobytes()
                i = j
            else:
                j = i + 1
                while j < n and j - i < 128 and not (j + 1 < n and (raw[j] == raw[j + 1]).all()):
                    j += 1
                body += bytes([j - i - 1]) + raw[i:j].tobytes()
                i = j
    desc = (8 if bits == 32 else 1 if bits == 16 else 0) | (0x20 if top_left else 0)
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 10 if rle else 2, 0, 0, 0, 0, 0, w, h, bits, desc)
    out = bytearray(hdr) + body
    if alpha_type is not None:
        ext_offset = len(out)
        ext = bytearray(495)
        struct.pack_into("<H", ext, 0, 495)
        ext[494] = alpha_type
        out += ext
        out += struct.pack("<II", ext_offset, 0) + b"TRUEVISION-XFILE.\x00"
    with open(path, "wb") as f:
        f.write(out)


# ---------------------------------------------------------------------------------------------
# Netpbm, farbfeld, SGI, HDR
# ---------------------------------------------------------------------------------------------


def pam(path, samples: np.ndarray, tupltype: str, maxval: int) -> None:
    h, w, depth = samples.shape
    head = f"P7\n# prueba de prisma\nWIDTH {w}\nHEIGHT {h}\nDEPTH {depth}\nMAXVAL {maxval}\nTUPLTYPE {tupltype}\nENDHDR\n".encode()
    data = samples.astype(">u2" if maxval > 255 else np.uint8).tobytes()
    with open(path, "wb") as f:
        f.write(head + data)


def pnm_ascii(path, kind: str, samples: np.ndarray, maxval: int) -> None:
    h, w = samples.shape[:2]
    lines = [kind, "# comentario con acentos: ñandú", f"{w} {h}"]
    if kind != "P1":
        lines.append(str(maxval))
    body = []
    for y in range(h):
        if kind == "P1":
            body.append("".join(str(int(v)) for v in samples[y].reshape(-1)))   # P1 sin espacios: valido
        else:
            body.append(" ".join(str(int(v)) for v in samples[y].reshape(-1)))
    # El comentario va en UTF-8 a proposito: Netpbm define el comentario como bytes hasta el fin de
    # linea, asi que un lector correcto tiene que saltar bytes no ASCII sin tropezar.
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n" + "\n".join(body) + "\n")


def pfm(path, rgb: np.ndarray, little: bool = True) -> None:
    h, w, _ = rgb.shape
    head = f"PF\n{w} {h}\n{-1.0 if little else 1.0}\n".encode()
    data = rgb[::-1].astype("<f4" if little else ">f4").tobytes()
    with open(path, "wb") as f:
        f.write(head + data)


def farbfeld(path, rgba16: np.ndarray) -> None:
    h, w, _ = rgba16.shape
    with open(path, "wb") as f:
        f.write(b"farbfeld" + struct.pack(">II", w, h) + rgba16.astype(">u2").tobytes())


def sgi_rle(path, rgba: np.ndarray) -> None:
    h, w, z = rgba.shape
    hdr = bytearray(512)
    struct.pack_into(">HBBHHHHii", hdr, 0, 474, 1, 1, 3, w, h, z, 0, 255)
    rows = []
    for c in range(z):
        for y in range(h - 1, -1, -1):                   # SGI: de abajo hacia arriba
            row = rgba[y, :, c]
            out = bytearray()
            i = 0
            while i < w:
                j = i + 1
                while j < w and j - i < 127 and row[j] == row[i]:
                    j += 1
                if j - i >= 3:
                    out += bytes([j - i, int(row[i])])
                    i = j
                else:
                    j = i
                    while j < w and j - i < 127 and not (j + 2 < w and row[j] == row[j + 1] == row[j + 2]):
                        j += 1
                    j = max(j, i + 1)
                    out += bytes([0x80 | (j - i)]) + bytes(int(v) for v in row[i:j])
                    i = j
            out += b"\x00"
            rows.append(bytes(out))
    start = 512 + 8 * h * z
    starts, lens, body = [], [], bytearray()
    for r in rows:
        starts.append(start + len(body))
        lens.append(len(r))
        body += r
    # orden de las tablas: por canal, y dentro de cada canal fila 0 = la de abajo
    table = struct.pack(f">{h * z}I", *starts) + struct.pack(f">{h * z}I", *lens)
    with open(path, "wb") as f:
        f.write(bytes(hdr) + table + bytes(body))


def hdr_rle(path, rgb: np.ndarray) -> np.ndarray:
    """Radiance con RLE "nuevo". Devuelve los flotantes que el formato puede representar."""
    h, w, _ = rgb.shape
    mx = rgb.max(axis=2)
    e = np.where(mx > 1e-32, np.floor(np.log2(np.maximum(mx, 1e-32))) + 1, 0)
    scale = np.where(mx > 1e-32, 256.0 / np.exp2(e), 0)
    mant = np.clip(rgb * scale[..., None], 0, 255).astype(np.uint8)
    ex = np.where(mx > 1e-32, e + 128, 0).astype(np.uint8)
    out = bytearray(b"#?RADIANCE\n# prueba\nFORMAT=32-bit_rle_rgbe\n\n" + f"-Y {h} +X {w}\n".encode())
    for y in range(h):
        out += bytes([2, 2, w >> 8, w & 255])
        for plane in (mant[y, :, 0], mant[y, :, 1], mant[y, :, 2], ex[y]):
            i = 0
            while i < w:
                j = i + 1
                while j < w and j - i < 127 and plane[j] == plane[i]:
                    j += 1
                if j - i > 2:
                    out += bytes([128 + (j - i), int(plane[i])])
                    i = j
                else:
                    j = i
                    while j < w and j - i < 128 and not (j + 2 < w and plane[j] == plane[j + 1] == plane[j + 2]):
                        j += 1
                    j = max(j, i + 1)
                    out += bytes([j - i]) + bytes(int(v) for v in plane[i:j])
                    i = j
    with open(path, "wb") as f:
        f.write(out)
    f_ = np.exp2(ex.astype(np.float64) - (128 + 8))
    return np.where(ex[..., None] > 0, (mant.astype(np.float64) + 0.5) * f_[..., None], 0.0)


# ---------------------------------------------------------------------------------------------
# PCX
# ---------------------------------------------------------------------------------------------

PCX_MAX_RUN = 63                     # el contador vive en los 6 bits bajos de un byte 0xC0 | n
PCX_RUN_FLAG = 0xC0


def _pcx_rle(row: bytes) -> bytes:
    """RLE de ZSoft para UNA fila de un plano (una corrida nunca cruza filas). Un byte >= 0xC0 no
    puede ir literal porque se confundiria con un contador: va como corrida de 1."""
    out, i, n = bytearray(), 0, len(row)
    while i < n:
        j = i + 1
        while j < n and j - i < PCX_MAX_RUN and row[j] == row[i]:
            j += 1
        run = j - i
        if run > 1 or row[i] >= PCX_RUN_FLAG:
            out += bytes([PCX_RUN_FLAG | run, row[i]])
        else:
            out.append(row[i])
        i = j
    return bytes(out)


def pcx(path, pixels: np.ndarray, palette: np.ndarray | None = None) -> None:
    """PCX v5 de 8 bits por plano: RGB en 3 planos, o indices + paleta VGA de 256 al final.
    Escritor de referencia correcto para anchos impares (bytes por linea PAR, relleno en 0),
    que es justo donde el de Pillow trunca el archivo."""
    h, w = pixels.shape[:2]
    planes = 1 if palette is not None else pixels.shape[2]
    bytes_per_line = w + (w & 1)
    hdr = bytearray(128)
    hdr[0:4] = bytes([0x0A, 5, 1, 8])
    struct.pack_into("<4H2H", hdr, 4, 0, 0, w - 1, h - 1, 72, 72)
    hdr[65] = planes
    struct.pack_into("<HH", hdr, 66, bytes_per_line, 1)
    body = bytearray()
    for y in range(h):
        for p in range(planes):
            line = (pixels[y] if planes == 1 else pixels[y, :, p]).astype(np.uint8).tobytes()
            body += _pcx_rle(line + bytes(bytes_per_line - w))
    if palette is not None:
        pal = np.zeros((256, 3), np.uint8)
        pal[:len(palette)] = palette
        body += b"\x0c" + pal.tobytes()
    with open(path, "wb") as f:
        f.write(bytes(hdr) + bytes(body))


# ---------------------------------------------------------------------------------------------
# PSD
# ---------------------------------------------------------------------------------------------


def _packbits(row: bytes) -> bytes:
    out, i, n = bytearray(), 0, len(row)
    while i < n:
        j = i + 1
        while j < n and j - i < 128 and row[j] == row[i]:
            j += 1
        if j - i >= 2:
            out += bytes([(257 - (j - i)) & 0xFF, row[i]])
            i = j
        else:
            j = i
            while j < n and j - i < 128 and not (j + 1 < n and row[j] == row[j + 1]):
                j += 1
            j = max(j, i + 1)
            out += bytes([j - i - 1]) + row[i:j]
            i = j
    return bytes(out)


def psd_rgba(path, rgba: np.ndarray, *, rle: bool, matte_white: bool = True, transparency: bool = True, depth: int = 8) -> None:
    """Compuesto RGBA como lo guarda Photoshop: conteo de capas NEGATIVO = el primer canal extra es la
    transparencia, y el color mateado sobre blanco donde el alfa es parcial."""
    h, w, _ = rgba.shape
    src = rgba.astype(np.float64) / (65535.0 if rgba.dtype == np.uint16 else 255.0)
    a = src[..., 3:4]
    color = src[..., :3] * a + (1.0 - a) if matte_white else src[..., :3]
    planes_f = [color[..., 0], color[..., 1], color[..., 2], src[..., 3]]
    maxv = 65535 if depth == 16 else 255
    planes = [np.round(p * maxv).astype(">u2" if depth == 16 else np.uint8) for p in planes_f]
    out = bytearray(b"8BPS" + struct.pack(">H", 1) + bytes(6) + struct.pack(">HIIHH", 4, h, w, depth, 3))
    out += struct.pack(">I", 0)                              # color mode data
    out += struct.pack(">I", 0)                              # image resources
    layer_info = struct.pack(">h", -1 if transparency else 0) + bytes(2)   # conteo (sin registros)
    lm = struct.pack(">I", len(layer_info)) + layer_info
    out += struct.pack(">I", len(lm)) + lm
    if not rle:
        out += struct.pack(">H", 0) + b"".join(p.tobytes() for p in planes)
    else:
        counts, body = [], bytearray()
        for p in planes:
            for y in range(h):
                packed = _packbits(p[y].tobytes())
                counts.append(len(packed))
                body += packed
        out += struct.pack(">H", 1) + struct.pack(f">{len(counts)}H", *counts) + body
    with open(path, "wb") as f:
        f.write(out)


# ---------------------------------------------------------------------------------------------
# ICO / CUR
# ---------------------------------------------------------------------------------------------


def ico_dib_entry(rgba: np.ndarray, bits: int) -> bytes:
    """Entrada DIB de icono: XOR (32 o 24 bits) + mascara AND (1 = transparente)."""
    h, w, _ = rgba.shape
    xor_stride = ((w * bits + 31) // 32) * 4
    and_stride = ((w + 31) // 32) * 4
    xor = bytearray()
    and_mask = bytearray()
    for y in range(h - 1, -1, -1):
        row = rgba[y][:, [2, 1, 0, 3] if bits == 32 else [2, 1, 0]].tobytes()
        xor += row + bytes(xor_stride - len(row))
        bitsrow = bytearray(and_stride)
        for x in range(w):
            if rgba[y, x, 3] < 128:
                bitsrow[x // 8] |= 0x80 >> (x % 8)
        and_mask += bitsrow
    hdr = struct.pack("<IiiHHIIiiII", 40, w, h * 2, 1, bits, 0, len(xor) + len(and_mask), 0, 0, 0, 0)
    return hdr + bytes(xor) + bytes(and_mask)


def ico(path, entries: list[tuple[int, int, bytes]], cursor: bool = False, hotspot=(0, 0)) -> None:
    """entries: (ancho, alto, datos). Arma el directorio con desplazamientos."""
    out = bytearray(struct.pack("<HHH", 0, 2 if cursor else 1, len(entries)))
    offset = 6 + 16 * len(entries)
    for w, h, data in entries:
        planes, bits = (hotspot if cursor else (1, 32))
        out += struct.pack("<BBBBHHII", w & 255, h & 255, 0, 0, planes, bits, len(data), offset)
        offset += len(data)
    for _, _, data in entries:
        out += data
    with open(path, "wb") as f:
        f.write(out)


# ---------------------------------------------------------------------------------------------
# DDS
# ---------------------------------------------------------------------------------------------


def dds_dx10(path, w: int, h: int, dxgi: int, data: bytes, alpha_mode: int = 1) -> None:
    hdr = bytearray(124)
    struct.pack_into("<IIIIIII", hdr, 0, 124, 0x1 | 0x2 | 0x4 | 0x1000, h, w, 0, 0, 1)
    struct.pack_into("<IIIIIIII", hdr, 72, 32, 0x4, 0x30315844, 0, 0, 0, 0, 0)       # 'DX10'
    struct.pack_into("<I", hdr, 104, 0x1000)
    dx10 = struct.pack("<IIIII", dxgi, 3, 0, 1, alpha_mode)
    with open(path, "wb") as f:
        f.write(b"DDS " + bytes(hdr) + dx10 + data)


def dds_legacy_rgb(path, rgba: np.ndarray, bits: int, masks: tuple[int, int, int, int], flags: int) -> None:
    h, w, _ = rgba.shape
    hdr = bytearray(124)
    struct.pack_into("<IIIIIII", hdr, 0, 124, 0x1 | 0x2 | 0x4 | 0x1000, h, w, w * bits // 8, 0, 1)
    struct.pack_into("<IIIIIIII", hdr, 72, 32, flags, 0, bits, *masks)
    struct.pack_into("<I", hdr, 104, 0x1000)
    px = np.zeros((h, w), np.uint32)
    for c, m in enumerate(masks):
        if not m:
            continue
        shift = (m & -m).bit_length() - 1
        width = bin(m).count("1")
        px |= ((rgba[..., c].astype(np.uint32) >> (8 - width)) << shift) if width <= 8 else (rgba[..., c].astype(np.uint32) << shift)
    raw = px.astype("<u4").view(np.uint8).reshape(h, w, 4)[..., :bits // 8].tobytes()
    with open(path, "wb") as f:
        f.write(b"DDS " + bytes(hdr) + raw)


# ---------------------------------------------------------------------------------------------
# HEIF / AVIF: edicion de cajas ISOBMFF sobre un archivo que ya escribio libavif (via Pillow)
# ---------------------------------------------------------------------------------------------


def _isobmff_boxes(buf: bytes | bytearray, start: int, end: int):
    """(tipo, inicio, inicio del contenido, fin) de cada caja hija del rango."""
    at = start
    while at + 8 <= end:
        size, kind = struct.unpack(">I4s", buf[at:at + 8])
        head = 8
        if size == 1:
            size, head = struct.unpack(">Q", buf[at + 8:at + 16])[0], 16
        elif size == 0:
            size = end - at
        yield kind.decode("latin-1"), at, at + head, at + size
        at += size


def _isobmff_child(buf, start: int, end: int, kind: str):
    return next(b for b in _isobmff_boxes(buf, start, end) if b[0] == kind)


def avif_patch_nclx(src_path, dst_path, *, primaries=None, transfer=None, matrix=None, full_range=None) -> None:
    """Reescribe los codigos CICP de la caja colr 'nclx' (mismo tamaño: se edita en el lugar)."""
    buf = bytearray(open(src_path, "rb").read())
    at = buf.find(b"colrnclx")
    assert at > 0, "sin colr nclx"
    body = at + 8
    old = struct.unpack(">HHHB", buf[body:body + 7])
    new = (old[0] if primaries is None else primaries, old[1] if transfer is None else transfer,
           old[2] if matrix is None else matrix, old[3] if full_range is None else (0x80 if full_range else 0))
    struct.pack_into(">HHHB", buf, body, *new)
    open(dst_path, "wb").write(buf)


def _iloc_shift(buf: bytearray, iloc_body: int, iloc_end: int, after: int, delta: int) -> None:
    """Corre `delta` bytes todo offset absoluto de iloc que apunte a partir de `after`."""
    version = buf[iloc_body]
    at = iloc_body + 4
    sizes = buf[at] << 8 | buf[at + 1]
    at += 2
    offset_size, length_size, base_size = sizes >> 12, (sizes >> 8) & 15, (sizes >> 4) & 15
    index_size = sizes & 15 if version in (1, 2) else 0
    count_fmt = ">I" if version == 2 else ">H"
    count = struct.unpack(count_fmt, buf[at:at + struct.calcsize(count_fmt)])[0]
    at += struct.calcsize(count_fmt)

    def field(n):
        return {0: None, 4: ">I", 8: ">Q"}[n]

    for _ in range(count):
        at += 4 if version == 2 else 2                     # item_ID
        method = 0
        if version in (1, 2):
            method = (buf[at] << 8 | buf[at + 1]) & 15
            at += 2
        at += 2                                            # data_reference_index
        base = 0
        if base_size:
            base = struct.unpack(field(base_size), buf[at:at + base_size])[0]
            if method == 0 and base >= after:
                struct.pack_into(field(base_size), buf, at, base + delta)
            at += base_size
        extents = struct.unpack(">H", buf[at:at + 2])[0]
        at += 2
        for _ in range(extents):
            at += index_size
            if offset_size:
                off = struct.unpack(field(offset_size), buf[at:at + offset_size])[0]
                if method == 0 and base == 0 and off >= after:
                    struct.pack_into(field(offset_size), buf, at, off + delta)
                at += offset_size
            at += length_size
    assert at <= iloc_end


def avif_add_prem(src_path, dst_path) -> None:
    """Agrega la referencia 'prem' (primaria -> alfa): declara el color como premultiplicado.
    Las cajas crecen, asi que se corrigen los tamaños de iref y meta y los offsets de iloc."""
    buf = bytearray(open(src_path, "rb").read())
    _, m_start, m_body, m_end = _isobmff_child(buf, 0, len(buf), "meta")
    children = m_body + 4
    _, _, p_body, _ = _isobmff_child(buf, children, m_end, "pitm")
    primary = struct.unpack(">H", buf[p_body + 4:p_body + 6])[0]
    _, r_start, r_body, r_end = _isobmff_child(buf, children, m_end, "iref")
    alpha = next(struct.unpack(">H", buf[b:b + 2])[0] for k, _, b, _ in _isobmff_boxes(buf, r_body + 4, r_end) if k == "auxl")
    prem = struct.pack(">I4sHHH", 14, b"prem", primary, 1, alpha)
    delta = len(prem)
    _, _, l_body, l_end = _isobmff_child(buf, children, m_end, "iloc")
    _iloc_shift(buf, l_body, l_end, r_end, delta)
    buf[r_end:r_end] = prem
    struct.pack_into(">I", buf, r_start, r_end - r_start + delta)
    struct.pack_into(">I", buf, m_start, m_end - m_start + delta)
    open(dst_path, "wb").write(buf)
