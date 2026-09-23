// png/plan.c - ver plan.h
#include <stdio.h>
#include <stdlib.h>
#include "plan.h"

enum { CHUNK_OVERHEAD = 12, KEY_CHUNK_BYTES = CHUNK_OVERHEAD + 6 };

static u32 palette_depth(u32 n) { return n <= 2 ? 1 : n <= 4 ? 2 : n <= 16 ? 4 : 8; }

u32 plan_channels(const PngPlan *p) {
    switch (p->color_type) {
    case PNG_CT_GRAY: case PNG_CT_PALETTE: return 1;
    case PNG_CT_GRAY_ALPHA: return 2;
    case PNG_CT_RGB: return 3;
    default: return 4;
    }
}

size_t plan_row_bytes(const PngPlan *p, u32 width) {
    return ((size_t)width * plan_channels(p) * p->bit_depth + 7) / 8;
}

u32 plan_filter_bpp(const PngPlan *p) {
    return MAX(1u, plan_channels(p) * p->bit_depth / 8);
}

const char *plan_describe(const PngPlan *p, char *buf, size_t cap) {
    switch (p->color_type) {
    case PNG_CT_PALETTE:
        snprintf(buf, cap, "paleta %u col · %u bit%s%s", p->palette_size, p->bit_depth, p->bit_depth > 1 ? "s" : "",
                 p->trns_count ? " · tRNS" : "");
        break;
    case PNG_CT_GRAY:
        snprintf(buf, cap, "gris %u bit%s%s", p->bit_depth, p->bit_depth > 1 ? "s" : "", p->key ? " · clave tRNS" : "");
        break;
    case PNG_CT_GRAY_ALPHA: snprintf(buf, cap, "gris+alfa %u bits", p->bit_depth); break;
    case PNG_CT_RGB: snprintf(buf, cap, "RGB %u bits%s", p->bit_depth, p->key ? " · clave tRNS" : ""); break;
    default: snprintf(buf, cap, "RGBA %u bits", p->bit_depth); break;
    }
    return buf;
}

// ---- paleta ---------------------------------------------------------------------------------

typedef struct PaletteEntry {
    u32 rgba;
    u32 count;
} PaletteEntry;

// Translucidos primero (asi el tRNS termina en el ultimo translucido), despues por frecuencia
// y, a igualdad, por valor (orden determinista: la misma entrada da el mismo archivo).
static int entry_order(const void *pa, const void *pb) {
    const PaletteEntry *a = pa, *b = pb;
    bool ta = (a->rgba >> 24) != 255, tb = (b->rgba >> 24) != 255;
    if (ta != tb) return ta ? -1 : 1;
    if (a->count != b->count) return a->count > b->count ? -1 : 1;
    return a->rgba < b->rgba ? -1 : a->rgba > b->rgba;
}

static void build_palette(const ColorStats *s, bool need_transparent_entry, PngPlan *p) {
    PaletteEntry entries[MAX_PALETTE];
    u32 n = s->colors;
    bool has_clear = false;
    for (u32 i = 0; i < n; i++) {
        entries[i].rgba = s->palette[i];
        entries[i].count = s->counts[i];
        has_clear |= (s->palette[i] >> 24) == 0;
    }
    if (need_transparent_entry && !has_clear && n < MAX_PALETTE) {
        entries[n].rgba = 0;
        entries[n].count = 0;
        n++;
    }
    qsort(entries, n, sizeof entries[0], entry_order);
    p->palette_size = n;
    p->trns_count = 0;
    for (u32 i = 0; i < n; i++) {
        p->palette[i] = entries[i].rgba;
        if ((entries[i].rgba >> 24) != 255) p->trns_count = i + 1;
    }
    p->bit_depth = (u8)palette_depth(n);
    p->color_type = PNG_CT_PALETTE;
}

// ---- colores clave para tRNS -----------------------------------------------------------------

static bool rgb_free(const Image *const *images, u32 count, const u16 rgb16[3]) {
    for (u32 i = 0; i < count; i++)
        if (image_uses_rgb_opaque(images[i], rgb16)) return false;
    return true;
}

// Gris: nivel k de `depth` bits. Primero el gris que ya tienen los transparentes (sin tocar
// nada); si no, con permiso para limpiar, un nivel que no use ningun pixel.
static bool find_gray_key(const ColorStats *s, const Image *const *images, u32 count, u32 depth,
                          bool may_clean, u16 *key, bool *clean) {
    u32 levels = depth >= 16 ? 65536 : 1u << depth;
    if (s->transparent_same_rgb && s->transparent_rgb[0] == s->transparent_rgb[1] &&
        s->transparent_rgb[1] == s->transparent_rgb[2]) {
        u16 v16 = s->transparent_rgb[0];
        bool exact = depth == 16 || v16 % 257 == 0;
        if (exact && depth < 16) {
            u32 v8 = v16 / 257, step = 255 / (levels - 1);
            exact = v8 % step == 0;
            if (exact && rgb_free(images, count, s->transparent_rgb)) { *key = (u16)(v8 / step); *clean = false; return true; }
        } else if (exact && rgb_free(images, count, s->transparent_rgb)) {
            *key = v16; *clean = false; return true;
        }
    }
    if (!may_clean) return false;
    if (depth < 16 || s->depth == 8 || s->reducible_to_8) {
        u32 lv = depth >= 16 ? 256 : levels, step = 255 / (lv - 1);
        for (u32 k = 0; k < lv; k++) {
            u32 v8 = k * step;
            if (!((s->gray_levels[v8 >> 5] >> (v8 & 31)) & 1)) {
                *key = depth >= 16 ? (u16)(v8 * 257) : (u16)k;
                *clean = true;
                return true;
            }
        }
        return false;
    }
    static const u16 CANDIDATES[] = { 1, 65534, 4097, 61439 };
    for (size_t i = 0; i < ARRAY_LEN(CANDIDATES); i++) {
        u16 rgb[3] = { CANDIDATES[i], CANDIDATES[i], CANDIDATES[i] };
        if (rgb_free(images, count, rgb)) { *key = CANDIDATES[i]; *clean = true; return true; }
    }
    return false;
}

static bool find_rgb_key(const ColorStats *s, const Image *const *images, u32 count, bool write8,
                         bool may_clean, u16 key[3], bool *clean) {
    if (s->transparent_same_rgb && rgb_free(images, count, s->transparent_rgb)) {
        bool exact8 = s->transparent_rgb[0] % 257 == 0 && s->transparent_rgb[1] % 257 == 0 && s->transparent_rgb[2] % 257 == 0;
        if (!write8 || exact8) {
            for (int c = 0; c < 3; c++) key[c] = write8 ? (u16)(s->transparent_rgb[c] / 257) : s->transparent_rgb[c];
            *clean = false;
            return true;
        }
    }
    if (!may_clean) return false;
    // Colores improbables en fotos e ilustraciones; se verifican contra la imagen igual.
    static const u8 CANDIDATES[][3] = { { 0, 0, 0 }, { 255, 0, 255 }, { 1, 2, 3 }, { 254, 1, 253 }, { 255, 255, 255 } };
    for (size_t i = 0; i < ARRAY_LEN(CANDIDATES); i++) {
        u16 rgb16[3] = { u8_to_u16(CANDIDATES[i][0]), u8_to_u16(CANDIDATES[i][1]), u8_to_u16(CANDIDATES[i][2]) };
        if (!rgb_free(images, count, rgb16)) continue;
        for (int c = 0; c < 3; c++) key[c] = write8 ? CANDIDATES[i][c] : rgb16[c];
        *clean = true;
        return true;
    }
    return false;
}

// ---- eleccion ----------------------------------------------------------------------------------

static u64 raw_estimate(u64 pixels, u32 channels, u32 depth, u64 overhead) {
    return pixels * channels * depth / 8 + overhead;
}

Status plan_choose(const ColorStats *s, const Image *const *images, u32 image_count,
                   const PlanRequest *req, PngPlan *out, Diag *diag) {
    memset(out, 0, sizeof *out);
    if (req->alternatives_found) *req->alternatives_found = 1;
    out->source16 = s->depth == 16;
    bool write8 = !out->source16 || req->depth == 8 || (req->depth == 0 && req->reduce && s->reducible_to_8);
    if (req->depth == 16) write8 = false;
    out->write8 = write8;
    u32 sample_depth = write8 ? 8 : 16;
    bool alpha = !stats_opaque(s), binary = stats_binary_alpha(s);
    bool gray_ok = s->gray || (req->may_clean && s->gray_visible);
    bool auto_gray_ok = gray_ok && req->allow_gray;
    bool pal_ok = write8 && stats_palettable(s);
    ColorChoice choice = req->color;
    u64 px = s->pixels;

    if (choice == COLOR_AUTO && !req->reduce) choice = alpha ? COLOR_RGBA : COLOR_RGB;

    if (choice == COLOR_PALETTE) {
        if (!write8) return diag_fail(diag, ST_INVALID, "una paleta PNG es de 8 bits: no combina con --depth 16");
        if (!pal_ok)
            return diag_fail(diag, ST_INVALID, "la imagen tiene mas de 256 colores: usa --colors 256 para cuantizar a paleta");
        build_palette(s, req->for_animation, out);
        return ST_OK;
    }
    if (choice == COLOR_GRAY || choice == COLOR_GRAY_ALPHA) {
        if (!gray_ok) return diag_fail(diag, ST_INVALID, "la imagen tiene color (interno: debio pasarse a gris antes)");
        if (choice == COLOR_GRAY_ALPHA) {
            out->color_type = PNG_CT_GRAY_ALPHA;
            out->bit_depth = (u8)sample_depth;
            return ST_OK;
        }
        out->color_type = PNG_CT_GRAY;
        out->bit_depth = (u8)(write8 && req->reduce ? gray_min_depth(s) : sample_depth);
        if (!alpha) return ST_OK;
        for (u32 d = out->bit_depth; binary && d <= sample_depth; d = d == 8 ? 16 : d * 2) {
            if (find_gray_key(s, images, image_count, d, req->may_clean, &out->key_value[0], &out->clean_to_key)) {
                out->bit_depth = (u8)d;
                out->key = true;
                return ST_OK;
            }
            if (d >= sample_depth) break;
        }
        return diag_fail(diag, ST_INVALID, "gris sin canal alfa no representa esta transparencia: usa --color graya o --flatten");
    }
    if (choice == COLOR_RGB) {
        out->color_type = PNG_CT_RGB;
        out->bit_depth = (u8)sample_depth;
        if (!alpha) return ST_OK;
        if (binary && find_rgb_key(s, images, image_count, write8, req->may_clean, out->key_value, &out->clean_to_key)) {
            out->key = true;
            return ST_OK;
        }
        return diag_fail(diag, ST_INVALID, "RGB sin canal alfa no representa esta transparencia: usa --flatten o --color rgba");
    }
    if (choice == COLOR_RGBA) {
        out->color_type = PNG_CT_RGBA;
        out->bit_depth = (u8)sample_depth;
        return ST_OK;
    }

    // ---- AUTO: todos los candidatos validos, ordenados por tamano crudo estimado ---------------
    PngPlan found[4];
    u64 found_bytes[4];
    u32 found_count = 0;
    // Insercion en un arreglo ordenado de a lo sumo 4 (si esta lleno, se cae el peor).
#define CONSIDER(plan_expr, bytes_expr)                                                  \
    do {                                                                                 \
        PngPlan cand = *out;                                                             \
        plan_expr;                                                                       \
        u64 bytes = (bytes_expr);                                                        \
        if (found_count < 4 || bytes < found_bytes[3]) {                                 \
            u32 at = found_count < 4 ? found_count++ : 3;                                \
            while (at > 0 && found_bytes[at - 1] > bytes) {                              \
                found[at] = found[at - 1];                                               \
                found_bytes[at] = found_bytes[at - 1];                                   \
                at--;                                                                    \
            }                                                                            \
            found[at] = cand;                                                            \
            found_bytes[at] = bytes;                                                     \
        }                                                                                \
    } while (0)

    if (pal_ok) {
        CONSIDER(build_palette(s, req->for_animation, &cand),
                 raw_estimate(px, 1, cand.bit_depth, 3ull * cand.palette_size + cand.trns_count + 2 * CHUNK_OVERHEAD));
    }
    if (auto_gray_ok) {
        u32 gd = write8 ? gray_min_depth(s) : 16;
        if (!alpha) {
            CONSIDER((cand.color_type = PNG_CT_GRAY, cand.bit_depth = (u8)gd), raw_estimate(px, 1, gd, 0));
        } else {
            if (binary) {
                for (u32 d = gd; d <= sample_depth; d = d == 8 ? 16 : d * 2) {
                    u16 k;
                    bool clean;
                    if (find_gray_key(s, images, image_count, d, req->may_clean, &k, &clean)) {
                        CONSIDER((cand.color_type = PNG_CT_GRAY, cand.bit_depth = (u8)d, cand.key = true,
                                  cand.key_value[0] = k, cand.clean_to_key = clean),
                                 raw_estimate(px, 1, d, KEY_CHUNK_BYTES));
                        break;
                    }
                    if (d >= sample_depth) break;
                }
            }
            CONSIDER((cand.color_type = PNG_CT_GRAY_ALPHA, cand.bit_depth = (u8)sample_depth), raw_estimate(px, 2, sample_depth, 0));
        }
    } else {
        if (!alpha) {
            CONSIDER((cand.color_type = PNG_CT_RGB, cand.bit_depth = (u8)sample_depth), raw_estimate(px, 3, sample_depth, 0));
        } else {
            u16 key[3];
            bool clean;
            if (binary && find_rgb_key(s, images, image_count, write8, req->may_clean, key, &clean)) {
                CONSIDER((cand.color_type = PNG_CT_RGB, cand.bit_depth = (u8)sample_depth, cand.key = true,
                          memcpy(cand.key_value, key, sizeof key), cand.clean_to_key = clean),
                         raw_estimate(px, 3, sample_depth, KEY_CHUNK_BYTES));
            }
            CONSIDER((cand.color_type = PNG_CT_RGBA, cand.bit_depth = (u8)sample_depth), raw_estimate(px, 4, sample_depth, 0));
        }
    }
#undef CONSIDER
    if (!found_count) return diag_fail(diag, ST_INVALID, "no hay formato PNG valido para esta imagen (interno)");
    u32 keep = MIN(found_count, MAX(req->alternatives, 1u));
    for (u32 i = 0; i < keep; i++) out[i] = found[i];
    if (req->alternatives_found) *req->alternatives_found = keep;
    return ST_OK;
}
