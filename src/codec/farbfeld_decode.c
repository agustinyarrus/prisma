// codec/farbfeld_decode.c - farbfeld (suckless): "farbfeld" + ancho + alto + RGBA de 16 bits big
// endian. Lo mas cercano a un volcado crudo que existe; sale directo en 16 bits.
#include "codec.h"

enum { FF_HEADER = 16 };

static Status farbfeld_decode(const DecodeContext *ctx, Decoded *out) {
    const u8 *d = ctx->data;
    if (ctx->size < FF_HEADER || memcmp(d, "farbfeld", 8) != 0) return diag_fail(ctx->diag, ST_CORRUPT, "no es farbfeld");
    u32 w = load_be32(d + 8), h = load_be32(d + 12);
    Frame *f = decoded_add_frame(out);
    if (!f) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    Status s = image_alloc(&f->image, w, h, 16, false, ctx->opt->max_pixels, ctx->diag);
    if (s != ST_OK) return s;
    size_t samples = (size_t)w * h * 4, avail = (ctx->size - FF_HEADER) / 2;
    u16 *dst = (u16 *)f->image.pixels;
    size_t n = MIN(samples, avail);
    for (size_t i = 0; i < n; i++) dst[i] = load_be16(d + FF_HEADER + 2 * i);
    if (n < samples) {
        memset(dst + n, 0, (samples - n) * 2);
        diag_warn(ctx->diag, "farbfeld cortado: faltan %zu muestras (quedan transparentes)", samples - n);
    }
    SourceInfo *src = &out->source;
    source_set(src, "farbfeld", "propio");
    src->bits = 16;
    src->channels = 4;
    src->has_alpha = true;
    src->alpha_origin = ALPHA_ORIGIN_CHANNEL;
    source_detail(src, "RGBA · 16 bits");
    return ST_OK;
}

static u8 farbfeld_probe(const u8 *d, size_t n, const char *ext) {
    UNUSED(ext);
    return n >= FF_HEADER && memcmp(d, "farbfeld", 8) == 0 ? 100 : 0;
}

const Codec CODEC_FARBFELD = {
    .name = "farbfeld",
    .extensions = "ff farbfeld",
    .description = "farbfeld de suckless (RGBA 16 bits)",
    .probe = farbfeld_probe,
    .decode = farbfeld_decode,
};
