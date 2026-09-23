// core/image.c - ver image.h
#include "image.h"
#include "pool.h"

Status image_alloc(Image *im, u32 width, u32 height, u32 depth, bool zero, u64 max_pixels, Diag *diag) {
    memset(im, 0, sizeof *im);
    if (width == 0 || height == 0)
        return diag_fail(diag, ST_CORRUPT, "dimensiones invalidas (%u x %u)", width, height);
    if (depth != 8 && depth != 16)
        return diag_fail(diag, ST_INVALID, "profundidad interna invalida (%u)", depth);
    u64 pixels = (u64)width * height;
    if (max_pixels && pixels > max_pixels)
        return diag_fail(diag, ST_TOO_LARGE, "%u x %u supera el limite de %llu megapixeles (--max-pixels)",
                         width, height, (unsigned long long)(max_pixels / 1000000));
    size_t stride, bytes;
    if (!mul_ok(width, depth / 2, &stride) || !mul_ok(stride, height, &bytes))
        return diag_fail(diag, ST_TOO_LARGE, "%u x %u no entra en memoria direccionable", width, height);
    im->pixels = zero ? mem_calloc(bytes) : mem_alloc(bytes);
    if (!im->pixels)
        return diag_fail(diag, ST_NO_MEMORY, "sin memoria para %u x %u (%zu MB)", width, height, bytes >> 20);
    im->width = width;
    im->height = height;
    im->depth = depth;
    im->stride = stride;
    return ST_OK;
}

void image_free(Image *im) {
    mem_free(im->pixels);
    memset(im, 0, sizeof *im);
}

Status image_clone(const Image *src, Image *dst, Diag *diag) {
    Status s = image_alloc(dst, src->width, src->height, src->depth, false, 0, diag);
    if (s == ST_OK) memcpy(dst->pixels, src->pixels, src->stride * src->height);
    return s;
}

typedef struct DepthJob {
    const Image *src;
    Image       *dst;
} DepthJob;

static void widen_rows(void *arg, size_t y0, size_t y1) {
    const DepthJob *j = arg;
    for (size_t y = y0; y < y1; y++) {
        const u8 *s = image_row(j->src, (u32)y);
        u16 *d = (u16 *)image_row(j->dst, (u32)y);
        for (size_t i = 0, n = (size_t)j->src->width * 4; i < n; i++) d[i] = u8_to_u16(s[i]);
    }
}

static void narrow_rows(void *arg, size_t y0, size_t y1) {
    const DepthJob *j = arg;
    for (size_t y = y0; y < y1; y++) {
        const u16 *s = (const u16 *)image_row(j->src, (u32)y);
        u8 *d = image_row(j->dst, (u32)y);
        for (size_t i = 0, n = (size_t)j->src->width * 4; i < n; i++) d[i] = u16_to_u8(s[i]);
    }
}

Status image_set_depth(Image *im, u32 depth, Diag *diag) {
    if (im->depth == depth) return ST_OK;
    Image out;
    Status s = image_alloc(&out, im->width, im->height, depth, false, 0, diag);
    if (s != ST_OK) return s;
    DepthJob job = { im, &out };
    parallel_range(im->height, 64, depth == 16 ? widen_rows : narrow_rows, &job);
    image_free(im);
    *im = out;
    return ST_OK;
}

static void unpremultiply_rows(void *arg, size_t y0, size_t y1) {
    Image *im = arg;
    for (size_t y = y0; y < y1; y++) {
        if (im->depth == 8) {
            u8 *p = image_row(im, (u32)y);
            for (u32 x = 0; x < im->width; x++, p += 4) {
                u32 a = p[3];
                if (a == 255) continue;
                if (!a) { p[0] = p[1] = p[2] = 0; continue; }
                for (int c = 0; c < 3; c++) p[c] = (u8)MIN(255u, (p[c] * 255u + a / 2) / a);
            }
        } else {
            u16 *p = (u16 *)image_row(im, (u32)y);
            for (u32 x = 0; x < im->width; x++, p += 4) {
                u64 a = p[3];
                if (a == 65535) continue;
                if (!a) { p[0] = p[1] = p[2] = 0; continue; }
                for (int c = 0; c < 3; c++) p[c] = (u16)MIN(65535u, (p[c] * 65535ull + a / 2) / a);
            }
        }
    }
}

void image_unpremultiply(Image *im) {
    parallel_range(im->height, 64, unpremultiply_rows, im);
}

void image_fill(Image *im, const u16 rgba16[4]) {
    for (u32 y = 0; y < im->height; y++) {
        u8 *row = image_row(im, y);
        if (im->depth == 8) {
            u8 px[4] = { u16_to_u8(rgba16[0]), u16_to_u8(rgba16[1]), u16_to_u8(rgba16[2]), u16_to_u8(rgba16[3]) };
            for (u32 x = 0; x < im->width; x++) memcpy(row + 4 * (size_t)x, px, 4);
        } else {
            for (u32 x = 0; x < im->width; x++) memcpy(row + 8 * (size_t)x, rgba16, 8);
        }
    }
}

bool blob_set(Blob *b, const void *data, size_t size) {
    blob_free(b);
    if (!size) return true;
    b->data = mem_alloc(size);
    if (!b->data) return false;
    memcpy(b->data, data, size);
    b->size = size;
    return true;
}

void blob_free(Blob *b) {
    mem_free(b->data);
    b->data = NULL;
    b->size = 0;
}

bool metadata_add_text(Metadata *m, const char *keyword, const char *text, size_t text_len) {
    if (!m->texts.item_size) vec_init(&m->texts, sizeof(TextEntry));
    TextEntry *e = vec_push(&m->texts);
    if (!e) return false;
    size_t kl = MIN(strlen(keyword), sizeof e->keyword - 1);
    memcpy(e->keyword, keyword, kl);
    e->keyword[kl] = 0;
    e->text = mem_alloc(text_len + 1);
    if (!e->text) { m->texts.count--; return false; }
    memcpy(e->text, text, text_len);
    e->text[text_len] = 0;
    return true;
}

void metadata_free(Metadata *m) {
    blob_free(&m->icc);
    blob_free(&m->exif);
    blob_free(&m->xmp);
    for (size_t i = 0; i < m->texts.count; i++) mem_free(VEC_AT(&m->texts, TextEntry, i).text);
    vec_free(&m->texts);
    memset(m, 0, sizeof *m);
}

const char *alpha_origin_label(AlphaOrigin origin) {
    switch (origin) {
    case ALPHA_ORIGIN_NONE:          return "sin transparencia";
    case ALPHA_ORIGIN_CHANNEL:       return "canal alfa";
    case ALPHA_ORIGIN_KEY:           return "color clave";
    case ALPHA_ORIGIN_PALETTE:       return "alfa en la paleta";
    case ALPHA_ORIGIN_MASK:          return "máscara de 1 bit";
    case ALPHA_ORIGIN_PREMULTIPLIED: return "premultiplicado → directo";
    case ALPHA_ORIGIN_MATTED:        return "mate removido";
    }
    return "?";
}

void decoded_init(Decoded *d) {
    memset(d, 0, sizeof *d);
    vec_init(&d->frames, sizeof(Frame));
}

Frame *decoded_add_frame(Decoded *d) {
    Frame *f = vec_push(&d->frames);
    if (f) {
        f->index = (u32)d->frames.count - 1;
        f->hotspot_x = f->hotspot_y = -1;
        f->delay_den = 100;
    }
    return f;
}

void decoded_free(Decoded *d) {
    for (size_t i = 0; i < d->frames.count; i++) image_free(&decoded_frame(d, i)->image);
    vec_free(&d->frames);
    metadata_free(&d->meta);
}
