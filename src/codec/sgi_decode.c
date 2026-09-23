// codec/sgi_decode.c - Silicon Graphics RGB (.sgi/.rgb/.rgba/.bw): 1 a 4 canales, 8 o 16 bits,
// sin comprimir o RLE por fila y canal. Las filas van de abajo hacia arriba.
#include "codec.h"

enum { SGI_MAGIC = 474, SGI_HEADER = 512, SGI_MAX_CHANNELS = 4 };

static bool rle_row(const u8 *p, const u8 *end, u32 bpc, u32 w, u16 *out) {
    u32 x = 0;
    for (;;) {
        if (p + bpc > end) return false;
        u32 v = bpc == 1 ? p[0] : load_be16(p);
        p += bpc;
        u32 count = v & 0x7F;
        if (!count) return x == w;
        if (x + count > w) return false;
        if (v & 0x80) {
            for (u32 k = 0; k < count; k++, p += bpc) {
                if (p + bpc > end) return false;
                out[x++] = (u16)(bpc == 1 ? p[0] : load_be16(p));
            }
        } else {
            if (p + bpc > end) return false;
            u16 val = (u16)(bpc == 1 ? p[0] : load_be16(p));
            p += bpc;
            for (u32 k = 0; k < count; k++) out[x++] = val;
        }
    }
}

static Status sgi_decode(const DecodeContext *ctx, Decoded *out) {
    const u8 *d = ctx->data;
    size_t n = ctx->size;
    if (n < SGI_HEADER || load_be16(d) != SGI_MAGIC) return diag_fail(ctx->diag, ST_CORRUPT, "no es SGI");
    u8 storage = d[2], bpc = d[3];
    u16 dims = load_be16(d + 4);
    u32 w = load_be16(d + 6), h = dims >= 2 ? load_be16(d + 8) : 1, z = dims >= 3 ? load_be16(d + 10) : 1;
    u32 colormap = load_be32(d + 104);
    if ((bpc != 1 && bpc != 2) || storage > 1 || !w || !h || !z || dims < 1 || dims > 3)
        return diag_fail(ctx->diag, ST_CORRUPT, "encabezado SGI invalido");
    if (colormap != 0) return diag_fail(ctx->diag, ST_UNSUPPORTED, "SGI con mapa de color %u", colormap);
    u32 channels = MIN(z, (u32)SGI_MAX_CHANNELS);

    Frame *f = decoded_add_frame(out);
    if (!f) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    Status s = image_alloc(&f->image, w, h, bpc == 2 ? 16 : 8, false, ctx->opt->max_pixels, ctx->diag);
    if (s != ST_OK) return s;
    u16 *row = mem_alloc((size_t)w * sizeof(u16));
    if (!row) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    const u8 *starts = d + SGI_HEADER, *lens = starts + 4 * (size_t)h * z;
    bool rle_tables_ok = storage == 0 || (size_t)(SGI_HEADER + 8 * (size_t)h * z) <= n;
    u32 bad = 0;
    for (u32 c = 0; c < SGI_MAX_CHANNELS; c++) {
        for (u32 y = 0; y < h; y++) {
            u32 dy = h - 1 - y;
            if (c >= channels) {
                // Canales ausentes: gris replica el canal 0; alfa ausente = opaco.
                for (u32 x = 0; x < w; x++) {
                    if (f->image.depth == 8) {
                        u8 *px = image_row(&f->image, dy) + 4 * (size_t)x;
                        px[c] = c == 3 ? 255 : (channels <= 2 ? px[0] : 0);
                    } else {
                        u16 *px = (u16 *)image_row(&f->image, dy) + 4 * (size_t)x;
                        px[c] = c == 3 ? 65535 : (channels <= 2 ? px[0] : 0);
                    }
                }
                continue;
            }
            bool ok;
            if (storage == 0) {
                size_t at = SGI_HEADER + (((size_t)c * h + y) * w) * bpc;
                ok = at + (size_t)w * bpc <= n;
                for (u32 x = 0; x < w; x++) row[x] = ok ? (u16)(bpc == 1 ? d[at + x] : load_be16(d + at + 2 * (size_t)x)) : 0;
            } else {
                size_t idx = (size_t)c * h + y;
                u32 start = rle_tables_ok ? load_be32(starts + 4 * idx) : 0, len = rle_tables_ok ? load_be32(lens + 4 * idx) : 0;
                ok = rle_tables_ok && start < n && len <= n - start && rle_row(d + start, d + start + len, bpc, w, row);
                if (!ok) memset(row, 0, (size_t)w * sizeof(u16));
            }
            bad += !ok;
            // Gris + alfa: el canal 1 es alfa, no verde.
            u32 target = (channels == 2 && c == 1) ? 3 : c;
            for (u32 x = 0; x < w; x++) {
                if (f->image.depth == 8) {
                    u8 *px = image_row(&f->image, dy) + 4 * (size_t)x;
                    px[target] = (u8)row[x];
                    if (channels <= 2 && c == 0) px[1] = px[2] = (u8)row[x];
                } else {
                    u16 *px = (u16 *)image_row(&f->image, dy) + 4 * (size_t)x;
                    px[target] = row[x];
                    if (channels <= 2 && c == 0) px[1] = px[2] = row[x];
                }
            }
        }
        if (channels == 2 && c == 1) break;          // gris+alfa: ya se lleno todo
    }
    mem_free(row);
    if (bad) diag_warn(ctx->diag, "SGI: %u filas dañadas o faltantes (quedan en 0)", bad);
    SourceInfo *src = &out->source;
    source_set(src, "SGI", "propio");
    src->bits = bpc * 8;
    src->channels = channels;
    src->has_alpha = channels == 2 || channels == 4;
    src->alpha_origin = src->has_alpha ? ALPHA_ORIGIN_CHANNEL : ALPHA_ORIGIN_NONE;
    source_detail(src, "%u canal%s · %u bits%s", channels, channels > 1 ? "es" : "", bpc * 8, storage ? " · RLE" : "");
    return ST_OK;
}

static u8 sgi_probe(const u8 *d, size_t n, const char *ext) {
    UNUSED(ext);
    if (n < SGI_HEADER || load_be16(d) != SGI_MAGIC || d[2] > 1 || (d[3] != 1 && d[3] != 2)) return 0;
    return 90;
}

const Codec CODEC_SGI = {
    .name = "SGI",
    .extensions = "sgi rgb rgba bw int inta",
    .description = "Silicon Graphics RGB: 1-4 canales, 8/16 bits, RLE",
    .probe = sgi_probe,
    .decode = sgi_decode,
};
