// codec/pcx_decode.c - ZSoft PCX: 1 bit (blanco y negro), EGA planar de 2-4 planos, CGA/16 colores
// empaquetados, 256 colores con la paleta al final (marcador 0x0C), RGB de 3 planos y RGBA de 4.
// Los tramos RLE pueden cruzar el fin de fila (varios escritores lo hacen): se decodifica como un
// solo flujo y despues se corta en filas.
#include "codec.h"

enum { PCX_HEADER = 128, PCX_PALETTE_TAIL = 769, PCX_RLE = 0xC0 };

static Status pcx_decode(const DecodeContext *ctx, Decoded *out) {
    const u8 *d = ctx->data;
    size_t n = ctx->size;
    if (n < PCX_HEADER) return diag_fail(ctx->diag, ST_TRUNCATED, "encabezado PCX incompleto");
    u8 encoding = d[2], bpp = d[3], planes = d[65];
    u32 xmin = load_le16(d + 4), ymin = load_le16(d + 6), xmax = load_le16(d + 8), ymax = load_le16(d + 10);
    u32 bpl = load_le16(d + 66);
    if (xmax < xmin || ymax < ymin || !bpl) return diag_fail(ctx->diag, ST_CORRUPT, "ventana PCX invalida");
    u32 w = xmax - xmin + 1, h = ymax - ymin + 1;
    bool mono = bpp == 1 && planes == 1, ega = bpp == 1 && planes >= 2 && planes <= 4;
    bool packed = (bpp == 2 || bpp == 4) && planes == 1, indexed8 = bpp == 8 && planes == 1;
    bool rgb = bpp == 8 && (planes == 3 || planes == 4);
    if (!mono && !ega && !packed && !indexed8 && !rgb) return diag_fail(ctx->diag, ST_UNSUPPORTED, "PCX de %u bits x %u planos", bpp, planes);
    if ((u64)bpl * 8 < (u64)w * bpp) return diag_fail(ctx->diag, ST_CORRUPT, "bytes por linea PCX insuficientes");

    size_t line = (size_t)bpl * planes, total;
    if (!mul_ok(line, h, &total)) return diag_fail(ctx->diag, ST_TOO_LARGE, "PCX demasiado grande");
    u8 *raw = mem_calloc(total);
    if (!raw) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    const u8 *p = d + PCX_HEADER, *end = d + n;
    if (indexed8 && n >= PCX_HEADER + PCX_PALETTE_TAIL && d[n - PCX_PALETTE_TAIL] == 0x0C) end = d + n - PCX_PALETTE_TAIL;
    size_t o = 0;
    if (encoding == 1) {
        while (o < total && p < end) {
            u8 b = *p++;
            if ((b & PCX_RLE) == PCX_RLE) {
                u32 count = b & 0x3F;
                if (p >= end) break;
                u8 v = *p++;
                for (u32 k = 0; k < count && o < total; k++) raw[o++] = v;
            } else {
                raw[o++] = b;
            }
        }
    } else {
        o = MIN(total, (size_t)(end - p));
        memcpy(raw, p, o);
    }
    if (o < total) diag_warn(ctx->diag, "PCX cortado: %zu de %zu bytes de pixeles", o, total);

    // Paleta: 256 colores al final, o los 16 del encabezado.
    u8 palette[256][3];
    if (indexed8) {
        if (n >= PCX_HEADER + PCX_PALETTE_TAIL && d[n - PCX_PALETTE_TAIL] == 0x0C) memcpy(palette, d + n - 768, 768);
        else for (int i = 0; i < 256; i++) palette[i][0] = palette[i][1] = palette[i][2] = (u8)i;
    } else {
        memcpy(palette, d + 16, 48);
    }

    Frame *f = decoded_add_frame(out);
    if (!f) { mem_free(raw); return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria"); }
    Status s = image_alloc(&f->image, w, h, 8, false, ctx->opt->max_pixels, ctx->diag);
    if (s != ST_OK) { mem_free(raw); return s; }
    for (u32 y = 0; y < h; y++) {
        const u8 *src = raw + (size_t)y * line;
        u8 *row = image_row(&f->image, y);
        for (u32 x = 0; x < w; x++) {
            u8 *px = row + 4 * (size_t)x;
            px[3] = 255;
            if (rgb) {
                px[0] = src[x]; px[1] = src[bpl + x]; px[2] = src[2 * (size_t)bpl + x];
                if (planes == 4) px[3] = src[3 * (size_t)bpl + x];
                continue;
            }
            u32 idx;
            if (mono) { px[0] = px[1] = px[2] = (src[x >> 3] >> (7 - (x & 7))) & 1 ? 255 : 0; continue; }
            if (ega) {
                idx = 0;
                for (u32 pl = 0; pl < planes; pl++) idx |= (u32)((src[(size_t)pl * bpl + (x >> 3)] >> (7 - (x & 7))) & 1) << pl;
            } else if (packed) {
                u32 per = 8 / bpp;
                idx = (src[x / per] >> (8 - bpp * (x % per + 1))) & ((1u << bpp) - 1);
            } else {
                idx = src[x];
            }
            px[0] = palette[idx][0]; px[1] = palette[idx][1]; px[2] = palette[idx][2];
        }
    }
    mem_free(raw);
    SourceInfo *src = &out->source;
    source_set(src, "PCX", "propio");
    src->bits = bpp;
    src->channels = rgb ? planes : 1;
    src->has_alpha = rgb && planes == 4;
    src->alpha_origin = src->has_alpha ? ALPHA_ORIGIN_CHANNEL : ALPHA_ORIGIN_NONE;
    source_detail(src, "v%u · %u bits x %u plano%s%s", d[1], bpp, planes, planes > 1 ? "s" : "", encoding == 1 ? " · RLE" : "");
    return ST_OK;
}

static u8 pcx_probe(const u8 *d, size_t n, const char *ext) {
    if (n < PCX_HEADER || d[0] != 0x0A || d[1] > 5 || d[2] > 1) return 0;
    if (d[3] != 1 && d[3] != 2 && d[3] != 4 && d[3] != 8) return 0;
    if (d[65] < 1 || d[65] > 4 || load_le16(d + 8) < load_le16(d + 4) || load_le16(d + 10) < load_le16(d + 6)) return 0;
    return strcmp(ext, "pcx") == 0 || strcmp(ext, "pcc") == 0 ? 90 : 60;
}

const Codec CODEC_PCX = {
    .name = "PCX",
    .extensions = "pcx pcc",
    .description = "ZSoft PCX: 1/2/4/8 bits, EGA planar, 256 colores, RGB y RGBA",
    .probe = pcx_probe,
    .decode = pcx_decode,
};
