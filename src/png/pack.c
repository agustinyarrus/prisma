// png/pack.c - ver pack.h
#include "pack.h"
#include "../core/pool.h"

enum { LUT_SLOTS = 512 };

typedef struct PaletteLut {
    u32 keys[LUT_SLOTS];
    u8  index[LUT_SLOTS];
    u8  used[LUT_SLOTS];
} PaletteLut;

INLINE u32 lut_slot(u32 key) { return (key * 0x9E3779B1u) >> 23; }

static void lut_build(PaletteLut *lut, const PngPlan *plan) {
    memset(lut->used, 0, sizeof lut->used);
    for (u32 i = 0; i < plan->palette_size; i++) {
        u32 s = lut_slot(plan->palette[i]);
        while (lut->used[s]) s = (s + 1) & (LUT_SLOTS - 1);
        lut->used[s] = 1;
        lut->keys[s] = plan->palette[i];
        lut->index[s] = (u8)i;
    }
}

// El color siempre esta (la paleta salio del analisis de esta misma imagen).
INLINE u8 lut_find(const PaletteLut *lut, u32 key) {
    u32 s = lut_slot(key);
    while (lut->used[s] && lut->keys[s] != key) s = (s + 1) & (LUT_SLOTS - 1);
    return lut->used[s] ? lut->index[s] : 0;
}

typedef struct PackJob {
    const Image      *image;
    const PngPlan    *plan;
    const PaletteLut *lut;
    Packed           *out;
} PackJob;

// Lee el pixel x como RGBA8 (desde 8 o 16 bits).
INLINE u32 pixel8(const Image *im, const u8 *row, u32 x) {
    if (im->depth == 8) { u32 p; memcpy(&p, row + 4 * (size_t)x, 4); return p; }
    const u16 *q = (const u16 *)row + 4 * (size_t)x;
    return (u32)u16_to_u8(q[0]) | (u32)u16_to_u8(q[1]) << 8 | (u32)u16_to_u8(q[2]) << 16 | (u32)u16_to_u8(q[3]) << 24;
}

INLINE void pixel16(const Image *im, const u8 *row, u32 x, u16 out[4]) {
    if (im->depth == 16) { memcpy(out, row + 8 * (size_t)x, 8); return; }
    const u8 *p = row + 4 * (size_t)x;
    for (int c = 0; c < 4; c++) out[c] = u8_to_u16(p[c]);
}

static void pack_palette(const PackJob *j, const u8 *src, u8 *dst) {
    const Image *im = j->image;
    u32 depth = j->plan->bit_depth, last = 0;
    u8 last_index = lut_find(j->lut, pixel8(im, src, 0));
    last = pixel8(im, src, 0);
    if (depth == 8) {
        for (u32 x = 0; x < im->width; x++) {
            u32 p = pixel8(im, src, x);
            if (p != last) { last = p; last_index = lut_find(j->lut, p); }
            dst[x] = last_index;
        }
        return;
    }
    memset(dst, 0, j->out->row_bytes);
    u32 per_byte = 8 / depth;
    for (u32 x = 0; x < im->width; x++) {
        u32 p = pixel8(im, src, x);
        if (p != last) { last = p; last_index = lut_find(j->lut, p); }
        u32 shift = 8 - depth * (x % per_byte + 1);
        dst[x / per_byte] |= (u8)(last_index << shift);
    }
}

static void pack_gray(const PackJob *j, const u8 *src, u8 *dst) {
    const Image *im = j->image;
    const PngPlan *pl = j->plan;
    u32 depth = pl->bit_depth;
    if (depth == 16) {
        for (u32 x = 0; x < im->width; x++) {
            u16 px[4];
            pixel16(im, src, x, px);
            store_be16(dst + 2 * (size_t)x, pl->clean_to_key && px[3] == 0 ? pl->key_value[0] : px[0]);
        }
        return;
    }
    if (depth == 8) {
        for (u32 x = 0; x < im->width; x++) {
            u32 p = pixel8(im, src, x);
            dst[x] = pl->clean_to_key && (p >> 24) == 0 ? (u8)pl->key_value[0] : (u8)p;
        }
        return;
    }
    memset(dst, 0, j->out->row_bytes);
    u32 per_byte = 8 / depth, drop = 8 - depth;
    for (u32 x = 0; x < im->width; x++) {
        u32 p = pixel8(im, src, x);
        // Niveles exactos (lo garantiza el plan): v >> (8 - d) recupera el indice sin redondeo.
        u32 v = pl->clean_to_key && (p >> 24) == 0 ? pl->key_value[0] : (p & 0xFF) >> drop;
        dst[x / per_byte] |= (u8)(v << (8 - depth * (x % per_byte + 1)));
    }
}

static void pack_gray_alpha(const PackJob *j, const u8 *src, u8 *dst) {
    const Image *im = j->image;
    if (j->plan->bit_depth == 8) {
        for (u32 x = 0; x < im->width; x++) {
            u32 p = pixel8(im, src, x);
            dst[2 * x] = (u8)p;
            dst[2 * x + 1] = (u8)(p >> 24);
        }
        return;
    }
    for (u32 x = 0; x < im->width; x++) {
        u16 px[4];
        pixel16(im, src, x, px);
        store_be16(dst + 4 * (size_t)x, px[0]);
        store_be16(dst + 4 * (size_t)x + 2, px[3]);
    }
}

static void pack_rgb(const PackJob *j, const u8 *src, u8 *dst) {
    const Image *im = j->image;
    const PngPlan *pl = j->plan;
    if (pl->bit_depth == 8) {
        if (im->depth == 8 && !pl->clean_to_key) {
            for (u32 x = 0; x < im->width; x++, src += 4, dst += 3) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; }
            return;
        }
        for (u32 x = 0; x < im->width; x++, dst += 3) {
            u32 p = pixel8(im, src, x);
            if (pl->clean_to_key && (p >> 24) == 0) { dst[0] = (u8)pl->key_value[0]; dst[1] = (u8)pl->key_value[1]; dst[2] = (u8)pl->key_value[2]; }
            else { dst[0] = (u8)p; dst[1] = (u8)(p >> 8); dst[2] = (u8)(p >> 16); }
        }
        return;
    }
    for (u32 x = 0; x < im->width; x++, dst += 6) {
        u16 px[4];
        pixel16(im, src, x, px);
        const u16 *c = pl->clean_to_key && px[3] == 0 ? pl->key_value : px;
        store_be16(dst, c[0]); store_be16(dst + 2, c[1]); store_be16(dst + 4, c[2]);
    }
}

static void pack_rgba(const PackJob *j, const u8 *src, u8 *dst) {
    const Image *im = j->image;
    if (j->plan->bit_depth == 8) {
        if (im->depth == 8) { memcpy(dst, src, (size_t)im->width * 4); return; }
        for (u32 x = 0; x < im->width; x++) { u32 p = pixel8(im, src, x); memcpy(dst + 4 * (size_t)x, &p, 4); }
        return;
    }
    for (u32 x = 0; x < im->width; x++) {
        u16 px[4];
        pixel16(im, src, x, px);
        for (int c = 0; c < 4; c++) store_be16(dst + 8 * (size_t)x + 2 * c, px[c]);
    }
}

static void pack_rows(void *arg, size_t y0, size_t y1) {
    const PackJob *j = arg;
    for (size_t y = y0; y < y1; y++) {
        const u8 *src = image_row(j->image, (u32)y);
        u8 *dst = j->out->rows + y * j->out->row_bytes;
        switch (j->plan->color_type) {
        case PNG_CT_PALETTE:    pack_palette(j, src, dst); break;
        case PNG_CT_GRAY:       pack_gray(j, src, dst); break;
        case PNG_CT_GRAY_ALPHA: pack_gray_alpha(j, src, dst); break;
        case PNG_CT_RGB:        pack_rgb(j, src, dst); break;
        default:                pack_rgba(j, src, dst); break;
        }
    }
}

Status pack_image(const Image *image, const PngPlan *plan, Packed *out, Diag *diag) {
    memset(out, 0, sizeof *out);
    out->width = image->width;
    out->height = image->height;
    out->row_bytes = plan_row_bytes(plan, image->width);
    size_t bytes;
    if (!mul_ok(out->row_bytes, image->height, &bytes)) return diag_fail(diag, ST_TOO_LARGE, "imagen demasiado grande para empaquetar");
    out->rows = mem_alloc(bytes);
    if (!out->rows) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para empaquetar filas");
    PaletteLut *lut = NULL;
    if (plan->color_type == PNG_CT_PALETTE) {
        lut = mem_alloc(sizeof *lut);
        if (!lut) { packed_free(out); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }
        lut_build(lut, plan);
    }
    PackJob job = { image, plan, lut, out };
    parallel_range(image->height, MAX(1u, (u32)(65536 / MAX(out->row_bytes, (size_t)1))), pack_rows, &job);
    mem_free(lut);
    return ST_OK;
}

void packed_free(Packed *p) {
    mem_free(p->rows);
    memset(p, 0, sizeof *p);
}
