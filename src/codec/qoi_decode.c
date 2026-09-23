// codec/qoi_decode.c - QOI ("Quite OK Image"): el formato sin perdida mas simple y rapido que
// existe; el decodificador es la especificacion misma (una pagina) en un solo bucle.
#include "codec.h"

enum { QOI_HEADER = 14, QOI_END = 8, QOI_MAX_PIXELS = 400000000 };
enum { OP_INDEX = 0x00, OP_DIFF = 0x40, OP_LUMA = 0x80, OP_RUN = 0xC0, OP_RGB = 0xFE, OP_RGBA = 0xFF, MASK2 = 0xC0 };

static Status qoi_decode(const DecodeContext *ctx, Decoded *out) {
    const u8 *d = ctx->data;
    size_t n = ctx->size;
    if (n < QOI_HEADER + QOI_END || memcmp(d, "qoif", 4) != 0) return diag_fail(ctx->diag, ST_CORRUPT, "no es QOI");
    u32 w = load_be32(d + 4), h = load_be32(d + 8);
    u8 channels = d[12], colorspace = d[13];
    if (!w || !h || (u64)w * h > QOI_MAX_PIXELS || (channels != 3 && channels != 4) || colorspace > 1)
        return diag_fail(ctx->diag, ST_CORRUPT, "encabezado QOI invalido (%u x %u, %u canales)", w, h, channels);
    Frame *f = decoded_add_frame(out);
    if (!f) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    Status s = image_alloc(&f->image, w, h, 8, false, ctx->opt->max_pixels, ctx->diag);
    if (s != ST_OK) return s;

    u8 index[64][4];
    memset(index, 0, sizeof index);
    u8 px[4] = { 0, 0, 0, 255 };
    const u8 *p = d + QOI_HEADER, *end = d + n - QOI_END;
    u8 *dst = f->image.pixels;
    size_t total = (size_t)w * h, i = 0;
    u32 run = 0;
    for (; i < total; i++, dst += 4) {
        if (run) { run--; memcpy(dst, px, 4); continue; }
        if (p >= end) break;
        u8 b = *p++;
        if (b == OP_RGB) {
            if (end - p < 3) break;
            px[0] = p[0]; px[1] = p[1]; px[2] = p[2]; p += 3;
        } else if (b == OP_RGBA) {
            if (end - p < 4) break;
            memcpy(px, p, 4); p += 4;
        } else if ((b & MASK2) == OP_INDEX) {
            memcpy(px, index[b], 4);
        } else if ((b & MASK2) == OP_DIFF) {
            px[0] += (u8)(((b >> 4) & 3) - 2); px[1] += (u8)(((b >> 2) & 3) - 2); px[2] += (u8)((b & 3) - 2);
        } else if ((b & MASK2) == OP_LUMA) {
            if (p >= end) break;
            u8 b2 = *p++;
            int vg = (b & 0x3F) - 32;
            px[0] += (u8)(vg - 8 + ((b2 >> 4) & 15)); px[1] += (u8)vg; px[2] += (u8)(vg - 8 + (b2 & 15));
        } else {
            run = b & 0x3F;
        }
        memcpy(index[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) & 63], px, 4);
        memcpy(dst, px, 4);
    }
    if (i < total) {
        memset(dst, 0, (total - i) * 4);
        diag_warn(ctx->diag, "QOI cortado: %zu de %zu pixeles (el resto queda transparente)", i, total);
    }
    SourceInfo *src = &out->source;
    source_set(src, "QOI", "propio");
    src->bits = 8;
    src->channels = channels;
    src->has_alpha = channels == 4;
    src->alpha_origin = channels == 4 ? ALPHA_ORIGIN_CHANNEL : ALPHA_ORIGIN_NONE;
    source_detail(src, "%s · %s", channels == 4 ? "RGBA" : "RGB", colorspace ? "todo lineal" : "sRGB + alfa lineal");
    return ST_OK;
}

static u8 qoi_probe(const u8 *d, size_t n, const char *ext) {
    UNUSED(ext);
    return n >= QOI_HEADER && memcmp(d, "qoif", 4) == 0 ? 100 : 0;
}

const Codec CODEC_QOI = {
    .name = "QOI",
    .extensions = "qoi",
    .description = "Quite OK Image, RGB y RGBA",
    .probe = qoi_probe,
    .decode = qoi_decode,
};
