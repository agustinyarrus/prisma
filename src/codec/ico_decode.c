// codec/ico_decode.c - ICO y CUR: cada archivo trae varios tamanos (16..256 px), en DIB o PNG.
//
// Por defecto se elige la mejor imagen (la mas grande y, a igual tamano, la de mas bits) mirando
// las dimensiones REALES de cada una: el directorio del icono miente seguido (0 = 256, cantidad
// de colores en 0, bits en 0 en las entradas PNG). --ico-size N elige un lado; --frames all
// extrae todas.
//
// Transparencia de un icono DIB: si es de 32 bits y trae alfa, manda el alfa; si no, la mascara
// AND de 1 bit (1 = transparente). Los pixeles con mascara 1 y color XOR no negro son "invertir
// pantalla" (cursores de texto): en PNG no existe, quedan transparentes y se avisa.
#include <stdio.h>
#include <stdlib.h>
#include "bmp.h"
#include "reader.h"

enum { ICO_TYPE = 1, CUR_TYPE = 2, ENTRY_SIZE = 16, MAX_ENTRIES = 1024 };

static const u8 PNG_SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

typedef struct IcoEntry {
    u32       index;
    const u8 *data;
    size_t    size;
    u32       width, height, bits;     // reales (del PNG o del DIB)
    bool      is_png;
    u16       hot_x, hot_y;
} IcoEntry;

static void probe_entry(IcoEntry *e, u32 dir_w, u32 dir_h, u32 dir_bits) {
    e->width = dir_w;
    e->height = dir_h;
    e->bits = dir_bits;
    if (e->size >= 24 && memcmp(e->data, PNG_SIG, 8) == 0) {
        e->is_png = true;
        e->width = load_be32(e->data + 16);
        e->height = load_be32(e->data + 20);
        u8 depth = e->size >= 26 ? e->data[24] : 8, ct = e->size >= 26 ? e->data[25] : 6;
        static const u8 CH[7] = { 1, 0, 3, 1, 2, 0, 4 };
        e->bits = (u32)depth * (ct < 7 ? CH[ct] : 4);
    } else if (e->size >= 16) {
        u32 hs = load_le32(e->data);
        if (hs >= 40) {
            e->width = (u32)abs((i32)load_le32(e->data + 4));
            e->height = (u32)abs((i32)load_le32(e->data + 8)) / 2;
            e->bits = load_le16(e->data + 14);
        }
    }
}

// Mejor = mas pixeles; a igualdad, mas bits; a igualdad, PNG (sin perdida de mascara).
static bool better(const IcoEntry *a, const IcoEntry *b, u32 wanted) {
    if (wanted) {
        bool aw = a->width == wanted || a->height == wanted, bw = b->width == wanted || b->height == wanted;
        if (aw != bw) return aw;
    }
    u64 pa = (u64)a->width * a->height, pb = (u64)b->width * b->height;
    if (pa != pb) return pa > pb;
    if (a->bits != b->bits) return a->bits > b->bits;
    return a->is_png && !b->is_png;
}

static Status decode_entry(const DecodeContext *ctx, const IcoEntry *e, Image *out, u32 *inverted, bool *used_mask, bool *used_alpha) {
    *inverted = 0;
    *used_mask = *used_alpha = false;
    if (e->is_png) {
        Decoded inner;
        Status s = decode_nested(ctx, e->data, e->size, &inner);
        if (s == ST_OK && decoded_frame_count(&inner)) {
            *out = decoded_frame(&inner, 0)->image;
            decoded_frame(&inner, 0)->image = (Image){ 0 };
            *used_alpha = inner.source.has_alpha;
        }
        decoded_free(&inner);
        return s;
    }
    DibRequest req = { e->data, e->size, NULL, true };
    DibResult res;
    Status s = dib_decode(ctx, &req, &res, NULL);
    if (s != ST_OK) return s;
    *out = res.image;
    if (res.bits == 32 && res.alpha_used && ctx->opt->bmp_alpha != TRI_OFF) { *used_alpha = true; return ST_OK; }
    if (!res.and_mask) return ST_OK;
    // Mascara AND: 1 bit por pixel, filas de a 4 bytes, de abajo hacia arriba.
    *used_mask = true;
    u32 h = out->height, w = out->width;
    for (u32 r = 0; r < h; r++) {
        size_t row_at = (size_t)r * res.and_stride;
        if (row_at + (w + 7) / 8 > res.and_mask_size) break;         // mascara cortada: lo que falte queda opaco
        const u8 *mask = res.and_mask + row_at;
        u32 y = h - 1 - r;
        for (u32 x = 0; x < w; x++) {
            if (!((mask[x >> 3] >> (7 - (x & 7))) & 1)) continue;
            if (out->depth == 8) {
                u8 *px = image_row(out, y) + 4 * (size_t)x;
                if (px[0] | px[1] | px[2]) (*inverted)++;
                memset(px, 0, 4);
            } else {
                u16 *px = (u16 *)image_row(out, y) + 4 * (size_t)x;
                if (px[0] | px[1] | px[2]) (*inverted)++;
                memset(px, 0, 8);
            }
        }
    }
    return ST_OK;
}

static Status ico_decode(const DecodeContext *ctx, Decoded *out) {
    Diag *diag = ctx->diag;
    Reader r = reader_make(ctx->data, ctx->size);
    rd_skip(&r, 2);
    u16 type = rd_le16(&r), count = rd_le16(&r);
    if (r.bad || !count || count > MAX_ENTRIES) return diag_fail(diag, ST_CORRUPT, "directorio de icono invalido");
    if (6 + (size_t)count * ENTRY_SIZE > ctx->size) return diag_fail(diag, ST_TRUNCATED, "directorio de icono cortado");
    IcoEntry *entries = mem_calloc(count * sizeof *entries);
    if (!entries) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
    u32 valid = 0;
    for (u32 i = 0; i < count; i++) {
        u32 dw = rd_u8(&r), dh = rd_u8(&r);
        rd_skip(&r, 2);
        u16 planes_or_x = rd_le16(&r), bits_or_y = rd_le16(&r);
        u32 size = rd_le32(&r), offset = rd_le32(&r);
        if (offset >= ctx->size || size < 16) continue;
        IcoEntry *e = &entries[valid];
        e->index = i;
        e->data = ctx->data + offset;
        e->size = MIN((size_t)size, ctx->size - offset);
        e->hot_x = planes_or_x;
        e->hot_y = bits_or_y;
        probe_entry(e, dw ? dw : 256, dh ? dh : 256, type == CUR_TYPE ? 0 : bits_or_y);
        valid++;
    }
    if (!valid) { mem_free(entries); return diag_fail(diag, ST_CORRUPT, "el icono no tiene imagenes validas"); }

    const DecodeOptions *opt = ctx->opt;
    u32 best = 0;
    for (u32 i = 1; i < valid; i++) if (better(&entries[i], &entries[best], opt->ico_size)) best = i;
    if (opt->frame_mode == FRAMES_ONE && opt->frame_index >= valid) {
        mem_free(entries);
        return diag_fail(diag, ST_INVALID, "--frame %u: el icono tiene %u imagenes (0..%u)", opt->frame_index, valid, valid - 1);
    }
    if (opt->ico_size && entries[best].width != opt->ico_size && entries[best].height != opt->ico_size)
        diag_warn(diag, "no hay imagen de %u px: se uso la de %ux%u", opt->ico_size, entries[best].width, entries[best].height);

    Status s = ST_OK;
    u32 inverted_total = 0, min_side = UINT32_MAX, max_side = 0;
    bool any_mask = false, any_alpha = false, any_png = false;
    for (u32 i = 0; i < valid; i++) {
        min_side = MIN(min_side, entries[i].width);
        max_side = MAX(max_side, entries[i].width);
        any_png |= entries[i].is_png;
    }
    for (u32 i = 0; i < valid && s == ST_OK; i++) {
        bool wanted = opt->frame_mode == FRAMES_ALL || (opt->frame_mode == FRAMES_ONE ? i == opt->frame_index : i == best);
        if (!wanted) continue;
        Image img = { 0 };
        u32 inverted;
        bool used_mask, used_alpha;
        s = decode_entry(ctx, &entries[i], &img, &inverted, &used_mask, &used_alpha);
        if (s != ST_OK) {
            if (opt->frame_mode == FRAMES_ALL) {           // en "todas", una imagen rota no tumba las demas
                diag_warn(diag, "imagen %u del icono ilegible: %s", i, diag->message);
                diag_reset(diag);
                s = ST_OK;
            }
            continue;
        }
        Frame *f = decoded_add_frame(out);
        if (!f) { image_free(&img); s = diag_fail(diag, ST_NO_MEMORY, "sin memoria"); break; }
        f->image = img;
        f->index = entries[i].index;
        if (type == CUR_TYPE) { f->hotspot_x = entries[i].hot_x; f->hotspot_y = entries[i].hot_y; }
        inverted_total += inverted;
        any_mask |= used_mask;
        any_alpha |= used_alpha;
    }
    if (s == ST_OK && !decoded_frame_count(out)) s = diag_fail(diag, ST_CORRUPT, "ninguna imagen del icono se pudo leer");

    SourceInfo *src = &out->source;
    source_set(src, type == CUR_TYPE ? "CUR" : "ICO", "propio");
    src->bits = 8;
    src->channels = 4;
    src->total_frames = valid;
    src->has_alpha = any_alpha || any_mask;
    src->alpha_origin = any_alpha ? ALPHA_ORIGIN_CHANNEL : any_mask ? ALPHA_ORIGIN_MASK : ALPHA_ORIGIN_NONE;
    source_detail(src, "%u imagen%s (%u–%u px)%s · elegida %ux%u %u bits%s", valid, valid == 1 ? "" : "es", min_side, max_side,
                  any_png ? " · con PNG" : "", entries[best].width, entries[best].height, entries[best].bits,
                  entries[best].is_png ? " PNG" : "");
    if (inverted_total)
        diag_warn(diag, "%u pixeles de \"invertir pantalla\" (cursor): PNG no los tiene, quedan transparentes", inverted_total);
    if (opt->frame_mode == FRAMES_DEFAULT && valid > 1)
        diag_warn(diag, "el icono tiene %u tamaños: se convirtio el mejor (--frames all para todos, --ico-size N para uno)", valid);
    mem_free(entries);
    return s;
}

static u8 ico_probe(const u8 *d, size_t n, const char *ext) {
    if (n < 22 || load_le16(d) != 0) return 0;
    u16 type = load_le16(d + 2), count = load_le16(d + 4);
    if ((type != ICO_TYPE && type != CUR_TYPE) || count == 0 || count > MAX_ENTRIES) return 0;
    // Primera entrada: desplazamiento dentro del archivo y reservado en 0.
    u32 size = load_le32(d + 14), offset = load_le32(d + 18);
    if (d[9] != 0 && d[9] != 255) return 10;
    if (offset < 6 + 16u * count || offset >= n || size < 16) return 10;
    bool named = strcmp(ext, "ico") == 0 || strcmp(ext, "cur") == 0;
    return named ? 95 : 70;
}

const Codec CODEC_ICO = {
    .name = "ICO",
    .extensions = "ico cur",
    .description = "Iconos y cursores de Windows: todos los tamaños, DIB o PNG, mascara AND y alfa de 32 bits",
    .probe = ico_probe,
    .decode = ico_decode,
};
