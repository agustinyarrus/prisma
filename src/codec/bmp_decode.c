// codec/bmp_decode.c - BMP/DIB: encabezados OS/2 1.x/2.x y Windows V1..V5; 1/2/4/8 bits con
// paleta, 16/24/32 bits, BITFIELDS y ALPHABITFIELDS con mascaras de cualquier ancho (10 bits por
// canal sale en 16 bits), RLE4/RLE8 (los pixeles salteados quedan transparentes), RLE24 de OS/2,
// JPEG/PNG embebidos y perfil ICC embebido de V5.
//
// La trampa clasica: 32 bits "BI_RGB" tiene un 4.o byte que la especificacion llama reservado,
// pero media industria guarda ahi el alfa y la otra media lo deja en 0. Automatico: si hay algun
// valor distinto de 0 es alfa; si son todos 0, la imagen es opaca (--bmp-alpha on|off manda).
#include <stdio.h>
#include "bmp.h"
#include "masks.h"
#include "reader.h"
#include "../core/pool.h"

enum {
    BI_RGB = 0, BI_RLE8 = 1, BI_RLE4 = 2, BI_BITFIELDS = 3, BI_JPEG = 4, BI_PNG = 5,
    BI_ALPHABITFIELDS = 6, BI_CMYK = 11, OS2_HUFFMAN = 3, OS2_RLE24 = 4,
    PROFILE_EMBEDDED = 0x4D424544, BMP_MAX_DIMENSION = 1 << 24,
};

typedef struct DibInfo {
    u32       header_size;
    i32       width, height;
    u16       bits;
    u32       compression;
    u32       image_size;
    u32       colors_used;
    u32       masks[4];
    bool      has_masks;
    bool      os2;
    bool      top_down;
    u32       entry_size;          // 3 (OS/2 1.x) o 4
    const u8 *palette;
    u32       palette_count;
    u32       cs_type, profile_offset, profile_size;
} DibInfo;

static Status parse_info(const DecodeContext *ctx, DibRequest *req, DibInfo *info) {
    Diag *diag = ctx->diag;
    Reader r = reader_make(req->dib, req->dib_size);
    memset(info, 0, sizeof *info);
    info->header_size = rd_le32(&r);
    u32 hs = info->header_size;
    if (hs == 12) {
        info->width = rd_le16(&r);
        info->height = rd_le16s(&r);
        rd_skip(&r, 2);
        info->bits = rd_le16(&r);
        info->entry_size = 3;
        info->os2 = true;
    } else if (hs >= 16 && hs <= 1024) {
        info->width = rd_le32s(&r);
        info->height = rd_le32s(&r);
        rd_skip(&r, 2);
        info->bits = rd_le16(&r);
        info->entry_size = 4;
        info->os2 = hs == 16 || hs == 64;
        if (hs >= 20) info->compression = rd_le32(&r);
        if (hs >= 24) info->image_size = rd_le32(&r);
        if (hs >= 36) { rd_skip(&r, 8); info->colors_used = rd_le32(&r); }
        if (!info->os2 && hs >= 52) {
            rd_seek(&r, 40);
            for (int i = 0; i < (hs >= 56 ? 4 : 3); i++) info->masks[i] = rd_le32(&r);
            info->has_masks = true;
        }
        if (!info->os2 && hs >= 108) { rd_seek(&r, 56); info->cs_type = rd_le32(&r); }
        if (!info->os2 && hs >= 124) { rd_seek(&r, 112); info->profile_offset = rd_le32(&r); info->profile_size = rd_le32(&r); }
    } else {
        return diag_fail(diag, ST_UNSUPPORTED, "encabezado DIB de %u bytes desconocido", hs);
    }
    if (r.bad) return diag_fail(diag, ST_TRUNCATED, "encabezado DIB incompleto");
    // Mascaras separadas (encabezado V1 de 40 bytes + BITFIELDS): van justo despues.
    size_t after = hs;
    bool fields = !info->os2 && (info->compression == BI_BITFIELDS || info->compression == BI_ALPHABITFIELDS);
    if (fields && hs == 40) {
        u32 n = info->compression == BI_ALPHABITFIELDS ? 4 : 3;
        rd_seek(&r, hs);
        for (u32 i = 0; i < n; i++) info->masks[i] = rd_le32(&r);
        if (r.bad) return diag_fail(diag, ST_TRUNCATED, "faltan las mascaras BITFIELDS");
        info->has_masks = true;
        after += 4 * (size_t)n;
    }
    if (info->bits <= 8) {
        u32 count = info->colors_used ? info->colors_used : 1u << info->bits;
        size_t avail = after < req->dib_size ? (req->dib_size - after) / info->entry_size : 0;
        if (req->pixels && req->pixels > req->dib + after)
            avail = MIN(avail, (size_t)(req->pixels - req->dib - after) / info->entry_size);
        info->palette_count = (u32)MIN(MIN((size_t)count, avail), (size_t)256);
        info->palette = req->dib + after;
        after += (size_t)info->palette_count * info->entry_size;
    }
    info->top_down = info->height < 0;
    // Sin ubicacion explicita, los pixeles empiezan donde terminan encabezado + mascaras + paleta.
    if (!req->pixels) req->pixels = req->dib + after;
    return ST_OK;
}

// ---- RLE ------------------------------------------------------------------------------------

// Decodifica RLE4/RLE8 a indices; `defined` marca los pixeles escritos (el resto: transparente).
static bool rle_decode(const u8 *p, const u8 *end, u32 w, u32 h, bool rle4, u8 *idx, u8 *defined) {
    u32 x = 0, y = 0;
    bool holes = false;
    while (p + 2 <= end && y < h) {
        u32 count = p[0], val = p[1];
        p += 2;
        if (count) {
            for (u32 k = 0; k < count; k++, x++)
                if (x < w) { idx[(size_t)y * w + x] = (u8)(rle4 ? ((k & 1) ? val & 15 : val >> 4) : val); defined[(size_t)y * w + x] = 1; }
            continue;
        }
        if (val == 0) { if (x < w) holes = true; x = 0; y++; continue; }
        if (val == 1) break;
        if (val == 2) {
            if (p + 2 > end) break;
            x += p[0];
            y += p[1];
            p += 2;
            holes = true;
            continue;
        }
        u32 n = val;
        size_t bytes = rle4 ? (n + 1) / 2 : n;
        size_t padded = (bytes + 1) & ~(size_t)1;
        for (u32 k = 0; k < n; k++, x++) {
            size_t b = rle4 ? k / 2 : k;
            if (p + b >= end) break;
            u8 v = rle4 ? ((k & 1) ? p[b] & 15 : p[b] >> 4) : p[b];
            if (x < w) { idx[(size_t)y * w + x] = v; defined[(size_t)y * w + x] = 1; }
        }
        p += MIN(padded, (size_t)(end - p));
    }
    if (y < h) holes = true;
    return holes;
}

// OS/2 RLE24: igual que RLE8 pero con pixeles BGR de 3 bytes.
static bool rle24_decode(const u8 *p, const u8 *end, u32 w, u32 h, u8 *bgr, u8 *defined) {
    u32 x = 0, y = 0;
    bool holes = false;
    while (p + 2 <= end && y < h) {
        u32 count = p[0];
        if (count) {
            if (p + 4 > end) break;
            for (u32 k = 0; k < count; k++, x++)
                if (x < w) { memcpy(bgr + 3 * ((size_t)y * w + x), p + 1, 3); defined[(size_t)y * w + x] = 1; }
            p += 4;
            continue;
        }
        u32 val = p[1];
        p += 2;
        if (val == 0) { if (x < w) holes = true; x = 0; y++; continue; }
        if (val == 1) break;
        if (val == 2) { if (p + 2 > end) break; x += p[0]; y += p[1]; p += 2; holes = true; continue; }
        size_t bytes = 3 * (size_t)val, padded = (bytes + 1) & ~(size_t)1;
        for (u32 k = 0; k < val && p + 3 * k + 3 <= end; k++, x++)
            if (x < w) { memcpy(bgr + 3 * ((size_t)y * w + x), p + 3 * k, 3); defined[(size_t)y * w + x] = 1; }
        p += MIN(padded, (size_t)(end - p));
    }
    if (y < h) holes = true;
    return holes;
}

// ---- decodificacion -----------------------------------------------------------------------------

static void store_px(Image *im, u32 x, u32 y, u16 r, u16 g, u16 b, u16 a) {
    if (im->depth == 8) {
        u8 *p = image_row(im, y) + 4 * (size_t)x;
        p[0] = u16_to_u8(r); p[1] = u16_to_u8(g); p[2] = u16_to_u8(b); p[3] = u16_to_u8(a);
    } else {
        u16 *p = (u16 *)image_row(im, y) + 4 * (size_t)x;
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }
}

static Status decode_indexed_rle(const DibInfo *info, const u8 *pixels, const u8 *end, u32 w, u32 h,
                                 Image *im, bool *holes, Diag *diag) {
    size_t n = (size_t)w * h;
    u8 *idx = mem_calloc(n), *defined = mem_calloc(n);
    if (!idx || !defined) { mem_free(idx); mem_free(defined); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }
    *holes = rle_decode(pixels, end, w, h, info->compression == BI_RLE4, idx, defined);
    for (u32 r = 0; r < h; r++) {
        u32 y = info->top_down ? r : h - 1 - r;
        u8 *row = image_row(im, y);
        for (u32 x = 0; x < w; x++) {
            size_t i = (size_t)r * w + x;
            u8 *px = row + 4 * (size_t)x;
            if (!defined[i]) { memset(px, 0, 4); continue; }
            u32 k = idx[i];
            if (k < info->palette_count) {
                const u8 *e = info->palette + (size_t)k * info->entry_size;
                px[0] = e[2]; px[1] = e[1]; px[2] = e[0];
            } else px[0] = px[1] = px[2] = 0;
            px[3] = 255;
        }
    }
    mem_free(idx);
    mem_free(defined);
    return ST_OK;
}

static Status decode_rle24(const DibInfo *info, const u8 *pixels, const u8 *end, u32 w, u32 h, Image *im, bool *holes, Diag *diag) {
    size_t n = (size_t)w * h;
    u8 *bgr = mem_calloc(3 * n), *defined = mem_calloc(n);
    if (!bgr || !defined) { mem_free(bgr); mem_free(defined); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }
    *holes = rle24_decode(pixels, end, w, h, bgr, defined);
    for (u32 r = 0; r < h; r++) {
        u32 y = info->top_down ? r : h - 1 - r;
        u8 *row = image_row(im, y);
        for (u32 x = 0; x < w; x++) {
            size_t i = (size_t)r * w + x;
            u8 *px = row + 4 * (size_t)x;
            if (!defined[i]) { memset(px, 0, 4); continue; }
            px[0] = bgr[3 * i + 2]; px[1] = bgr[3 * i + 1]; px[2] = bgr[3 * i]; px[3] = 255;
        }
    }
    mem_free(bgr);
    mem_free(defined);
    return ST_OK;
}

// Filas sin comprimir: 1/2/4/8 bits (paleta), 16/32 (mascaras), 24 (BGR).
static u32 decode_rows(const DibInfo *info, const MaskChannel ch[4], const u8 *pixels, size_t avail, u32 w, u32 h, Image *im) {
    size_t stride = (((size_t)w * info->bits + 31) / 32) * 4;
    u32 rows_ok = (u32)MIN((size_t)h, stride ? avail / stride : 0);
    bool fast32 = info->bits == 32 && ch[0].mask == 0x00FF0000 && ch[1].mask == 0x0000FF00 && ch[2].mask == 0x000000FF &&
                  (ch[3].mask == 0xFF000000 || ch[3].mask == 0) && im->depth == 8;
    for (u32 r = 0; r < rows_ok; r++) {
        const u8 *src = pixels + (size_t)r * stride;
        u32 y = info->top_down ? r : h - 1 - r;
        u8 *row = image_row(im, y);
        switch (info->bits) {
        case 1: case 2: case 4: case 8: {
            u32 bits = info->bits, per = 8 / bits, maskv = (1u << bits) - 1;
            for (u32 x = 0; x < w; x++) {
                u32 k = bits == 8 ? src[x] : (src[x / per] >> (8 - bits * (x % per + 1))) & maskv;
                u8 *px = row + 4 * (size_t)x;
                if (k < info->palette_count) {
                    const u8 *e = info->palette + (size_t)k * info->entry_size;
                    px[0] = e[2]; px[1] = e[1]; px[2] = e[0];
                } else px[0] = px[1] = px[2] = 0;
                px[3] = 255;
            }
            break;
        }
        case 24:
            for (u32 x = 0; x < w; x++) {
                const u8 *s = src + 3 * (size_t)x;
                if (im->depth == 8) { u8 *px = row + 4 * (size_t)x; px[0] = s[2]; px[1] = s[1]; px[2] = s[0]; px[3] = 255; }
                else store_px(im, x, y, u8_to_u16(s[2]), u8_to_u16(s[1]), u8_to_u16(s[0]), 65535);
            }
            break;
        default:   // 16 y 32 bits con mascaras
            if (fast32) {
                for (u32 x = 0; x < w; x++) {
                    const u8 *s = src + 4 * (size_t)x;
                    u8 *px = row + 4 * (size_t)x;
                    px[0] = s[2]; px[1] = s[1]; px[2] = s[0]; px[3] = ch[3].mask ? s[3] : 255;
                }
                break;
            }
            for (u32 x = 0; x < w; x++) {
                u32 v = info->bits == 16 ? load_le16(src + 2 * (size_t)x) : load_le32(src + 4 * (size_t)x);
                store_px(im, x, y, mask_channel_value(&ch[0], v), mask_channel_value(&ch[1], v), mask_channel_value(&ch[2], v),
                         ch[3].mask ? mask_channel_value(&ch[3], v) : 65535);
            }
            break;
        }
    }
    // Filas que faltan (archivo cortado): transparentes.
    for (u32 r = rows_ok; r < h; r++) memset(image_row(im, info->top_down ? r : h - 1 - r), 0, im->stride);
    return rows_ok;
}

static bool any_alpha_nonzero(const Image *im) {
    for (u32 y = 0; y < im->height; y++) {
        const u8 *row = image_row(im, y);
        if (im->depth == 8) { for (u32 x = 0; x < im->width; x++) if (row[4 * (size_t)x + 3]) return true; }
        else { const u16 *p = (const u16 *)row; for (u32 x = 0; x < im->width; x++) if (p[4 * (size_t)x + 3]) return true; }
    }
    return false;
}

static void force_opaque(Image *im) {
    for (u32 y = 0; y < im->height; y++) {
        u8 *row = image_row(im, y);
        if (im->depth == 8) for (u32 x = 0; x < im->width; x++) row[4 * (size_t)x + 3] = 255;
        else { u16 *p = (u16 *)row; for (u32 x = 0; x < im->width; x++) p[4 * (size_t)x + 3] = 65535; }
    }
}

Status dib_decode(const DecodeContext *ctx, const DibRequest *req_in, DibResult *res, Decoded *meta_out) {
    Diag *diag = ctx->diag;
    memset(res, 0, sizeof *res);
    DibRequest req = *req_in;
    DibInfo info;
    Status s = parse_info(ctx, &req, &info);
    if (s != ST_OK) return s;
    if (info.width <= 0 || info.height == 0 || info.width > BMP_MAX_DIMENSION || info.height > BMP_MAX_DIMENSION || info.height < -BMP_MAX_DIMENSION)
        return diag_fail(diag, ST_CORRUPT, "dimensiones invalidas: %d x %d", info.width, info.height);
    u32 w = (u32)info.width, h = (u32)(info.height < 0 ? -info.height : info.height);
    if (req.icon) { h /= 2; info.top_down = false; if (!h) return diag_fail(diag, ST_CORRUPT, "icono de altura 0"); }
    res->bits = info.bits;
    const u8 *end = req.dib + req.dib_size;
    const u8 *pixels = req.pixels;
    if (pixels < req.dib || pixels > end) return diag_fail(diag, ST_CORRUPT, "desplazamiento de pixeles fuera del archivo");
    size_t avail = (size_t)(end - pixels);

    // Perfil ICC embebido (V5): se desplaza desde el inicio del encabezado.
    if (meta_out && info.cs_type == PROFILE_EMBEDDED && info.profile_size && info.profile_offset < req.dib_size &&
        info.profile_size <= req.dib_size - info.profile_offset)
        blob_set(&meta_out->meta.icc, req.dib + info.profile_offset, info.profile_size);

    if (!info.os2 && (info.compression == BI_JPEG || info.compression == BI_PNG)) {
        if (req.icon) return diag_fail(diag, ST_UNSUPPORTED, "icono DIB con compresion JPEG/PNG");
        size_t n = info.image_size && info.image_size <= avail ? info.image_size : avail;
        Decoded inner;
        s = decode_nested(ctx, pixels, n, &inner);
        if (s == ST_OK && decoded_frame_count(&inner)) {
            res->image = decoded_frame(&inner, 0)->image;
            decoded_frame(&inner, 0)->image = (Image){ 0 };
            res->alpha_used = inner.source.has_alpha;
        }
        decoded_free(&inner);
        return s;
    }
    bool rle = !info.os2 ? (info.compression == BI_RLE8 || info.compression == BI_RLE4) : false;
    bool rle24 = info.os2 && info.compression == OS2_RLE24 && info.bits == 24;
    if (info.os2 && info.compression == OS2_HUFFMAN) return diag_fail(diag, ST_UNSUPPORTED, "BMP OS/2 con Huffman 1D");
    if (info.compression == BI_CMYK || info.compression > BI_ALPHABITFIELDS) return diag_fail(diag, ST_UNSUPPORTED, "BMP con compresion %u", info.compression);
    if (!rle && !rle24 && info.compression != BI_RGB && info.compression != BI_BITFIELDS && info.compression != BI_ALPHABITFIELDS)
        return diag_fail(diag, ST_UNSUPPORTED, "BMP con compresion %u", info.compression);
    switch (info.bits) {
    case 1: case 2: case 4: case 8: case 16: case 24: case 32: break;
    default: return diag_fail(diag, ST_UNSUPPORTED, "BMP de %u bits por pixel", info.bits);
    }
    if (info.bits <= 8 && !info.palette_count) return diag_fail(diag, ST_CORRUPT, "BMP con paleta vacia");

    // Mascaras por defecto para 16/32 bits sin BITFIELDS.
    u32 masks[4] = { info.masks[0], info.masks[1], info.masks[2], info.masks[3] };
    bool explicit_masks = info.has_masks && (info.compression == BI_BITFIELDS || info.compression == BI_ALPHABITFIELDS || info.header_size >= 52);
    if (!explicit_masks || (!masks[0] && !masks[1] && !masks[2])) {
        if (info.bits == 16) { masks[0] = 0x7C00; masks[1] = 0x03E0; masks[2] = 0x001F; masks[3] = 0; }
        else if (info.bits == 32) { masks[0] = 0x00FF0000; masks[1] = 0x0000FF00; masks[2] = 0x000000FF; masks[3] = 0xFF000000; }
    }
    if (info.bits == 32 && info.compression == BI_RGB) masks[3] = 0xFF000000;   // el 4.o byte "reservado"
    MaskChannel ch[4];
    u32 depth = 8;
    for (int i = 0; i < 4; i++) {
        if (!mask_channel_init(&ch[i], info.bits >= 16 ? masks[i] : 0)) { for (int k = 0; k < i; k++) mask_channel_free(&ch[k]); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }
        if (ch[i].bits > 8) depth = 16;
    }

    s = image_alloc(&res->image, w, h, depth, false, ctx->opt->max_pixels, diag);
    if (s == ST_OK) {
        if (rle) {
            s = decode_indexed_rle(&info, pixels, end, w, h, &res->image, &res->rle_holes, diag);
        } else if (rle24) {
            s = decode_rle24(&info, pixels, end, w, h, &res->image, &res->rle_holes, diag);
        } else {
            u32 ok_rows = decode_rows(&info, ch, pixels, avail, w, h, &res->image);
            if (ok_rows < h) diag_warn(diag, "BMP cortado: %u de %u filas (el resto queda transparente)", ok_rows, h);
            size_t stride = (((size_t)w * info.bits + 31) / 32) * 4;
            if (req.icon) {
                res->and_stride = ((w + 31) / 32) * 4;
                size_t xor_bytes = stride * h;
                if (xor_bytes < avail) {
                    res->and_mask = pixels + xor_bytes;
                    res->and_mask_size = avail - xor_bytes;
                }
            }
        }
    }
    for (int i = 0; i < 4; i++) mask_channel_free(&ch[i]);
    if (s != ST_OK) { image_free(&res->image); return s; }

    // Alfa: solo 16/32 bits con canal alfa pueden traerlo.
    bool alpha_channel = info.bits >= 16 && ch[3].mask;
    if (alpha_channel) {
        res->alpha_used = any_alpha_nonzero(&res->image);
        Tristate mode = ctx->opt->bmp_alpha;
        if (mode == TRI_OFF || (mode == TRI_AUTO && !res->alpha_used)) force_opaque(&res->image);
    }
    return ST_OK;
}

// ---- BMP ---------------------------------------------------------------------------------------

static Status bmp_decode(const DecodeContext *ctx, Decoded *out) {
    Reader r = reader_make(ctx->data, ctx->size);
    if (!rd_match(&r, "BM", 2)) return diag_fail(ctx->diag, ST_UNSUPPORTED, "BMP de OS/2 (arreglo o icono): no soportado");
    rd_skip(&r, 8);
    u32 offset = rd_le32(&r);
    if (r.bad) return diag_fail(ctx->diag, ST_TRUNCATED, "encabezado BMP incompleto");
    DibRequest req = { ctx->data + 14, ctx->size - 14, NULL, false };
    if (offset >= 14 + 12 && offset < ctx->size) req.pixels = ctx->data + offset;
    DibResult res;
    Status s = dib_decode(ctx, &req, &res, out);
    if (s != ST_OK) return s;
    Frame *f = decoded_add_frame(out);
    if (!f) { image_free(&res.image); return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria"); }
    f->image = res.image;

    SourceInfo *src = &out->source;
    source_set(src, "BMP", "propio");
    src->bits = res.bits <= 8 ? 8 : res.bits == 16 ? 5 : 8;
    src->channels = res.bits == 32 ? 4 : 3;
    bool alpha = (res.bits == 32 || res.bits == 16) && res.alpha_used && ctx->opt->bmp_alpha != TRI_OFF;
    src->has_alpha = alpha || res.rle_holes;
    src->alpha_origin = alpha ? ALPHA_ORIGIN_CHANNEL : res.rle_holes ? ALPHA_ORIGIN_MASK : ALPHA_ORIGIN_NONE;
    u32 hs = load_le32(ctx->data + 14);
    const char *version = hs == 12 ? "OS/2 1.x" : hs == 16 || hs == 64 ? "OS/2 2.x" : hs == 40 ? "V3" : hs == 52 ? "V3+RGB"
                        : hs == 56 ? "V3+RGBA" : hs == 108 ? "V4" : hs == 124 ? "V5" : "?";
    source_detail(src, "%s · %u bits%s%s", version, res.bits, res.rle_holes ? " · RLE con saltos (transparentes)" : "",
                  (res.bits == 32 && !res.alpha_used) ? " · 4.o byte en 0 (opaco)" : "");
    if (res.rle_holes) diag_warn(ctx->diag, "BMP RLE con pixeles salteados: quedan transparentes (como en los navegadores)");
    if (res.bits == 32 && !res.alpha_used && ctx->opt->bmp_alpha == TRI_AUTO)
        diag_warn(ctx->diag, "BMP de 32 bits con alfa todo en 0: se trata como opaco (--bmp-alpha on lo respeta)");
    return ST_OK;
}

static u8 bmp_probe(const u8 *data, size_t size, const char *ext) {
    UNUSED(ext);
    if (size < 18 || data[0] != 'B' || data[1] != 'M') return 0;
    u32 hs = load_le32(data + 14);
    return (hs == 12 || hs == 16 || hs == 40 || hs == 52 || hs == 56 || hs == 64 || hs == 108 || hs == 124) ? 95 : 40;
}

const Codec CODEC_BMP = {
    .name = "BMP",
    .extensions = "bmp dib rle",
    .description = "BMP/DIB OS/2 y V1-V5: paleta, 16/24/32 bits, BITFIELDS/ALPHABITFIELDS, RLE4/8/24, ICC",
    .probe = bmp_probe,
    .decode = bmp_decode,
};
