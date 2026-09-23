// png/analyze.c - ver analyze.h
#include "analyze.h"
#include "../core/pool.h"

enum { SET_SLOTS = 512, BAND_GRAIN_ROWS = 32, MAX_BANDS = 1024 };

// ---- conjunto de colores: direccionamiento abierto con hash de Fibonacci --------------------

typedef struct ColorSet {
    u32  keys[SET_SLOTS];
    u32  counts[SET_SLOTS];
    u8   used[SET_SLOTS];
    u32  size;
    bool overflow;              // vio mas de MAX_PALETTE colores: deja de contar
} ColorSet;

INLINE u32 slot_of(u32 key) { return (key * 0x9E3779B1u) >> 23; }    // 9 bits altos

// Devuelve el lugar del color (insertandolo si hace falta) o -1 si se desbordo.
static int colorset_find(ColorSet *s, u32 key) {
    u32 i = slot_of(key);
    for (;;) {
        if (!s->used[i]) {
            if (s->size == MAX_PALETTE) { s->overflow = true; return -1; }
            s->used[i] = 1;
            s->keys[i] = key;
            s->counts[i] = 0;
            s->size++;
            return (int)i;
        }
        if (s->keys[i] == key) return (int)i;
        i = (i + 1) & (SET_SLOTS - 1);
    }
}

// ---- estado por banda ------------------------------------------------------------------------

typedef struct BandStats {
    ColorSet colors;
    u64  transparent, partial, opaque;
    bool gray, gray_visible, reducible;
    u32  gray_levels[8];
    bool have_transparent, transparent_same;
    u16  transparent_rgb[3];
    u64  alpha_hist[16];
} BandStats;

static void band_init(BandStats *b) {
    memset(b, 0, sizeof *b);
    b->gray = b->gray_visible = b->reducible = true;
    b->transparent_same = true;
}

static void note_transparent(BandStats *b, u16 r, u16 g, u16 bl) {
    if (!b->have_transparent) {
        b->have_transparent = true;
        b->transparent_rgb[0] = r; b->transparent_rgb[1] = g; b->transparent_rgb[2] = bl;
    } else if (b->transparent_same && (b->transparent_rgb[0] != r || b->transparent_rgb[1] != g || b->transparent_rgb[2] != bl)) {
        b->transparent_same = false;
    }
}

static void analyze_rows8(const Image *im, u32 y0, u32 y1, BandStats *b) {
    u32 last = 0;
    int last_slot = -1;
    for (u32 y = y0; y < y1; y++) {
        const u8 *row = image_row(im, y);
        for (u32 x = 0; x < im->width; x++) {
            u32 p;
            memcpy(&p, row + 4 * (size_t)x, 4);
            u32 r = p & 0xFF, g = (p >> 8) & 0xFF, bl = (p >> 16) & 0xFF, a = p >> 24;
            b->alpha_hist[a >> 4]++;
            if (a == 0) { b->transparent++; note_transparent(b, (u16)(r * 257), (u16)(g * 257), (u16)(bl * 257)); }
            else if (a == 255) b->opaque++;
            else b->partial++;
            bool is_gray = r == g && g == bl;
            if (is_gray) b->gray_levels[r >> 5] |= 1u << (r & 31);
            else { b->gray = false; if (a) b->gray_visible = false; }
            // Corridas de pixeles iguales (lo tipico en graficos) no tocan la tabla hash.
            if (last_slot >= 0 && p == last) { b->colors.counts[last_slot]++; continue; }
            if (b->colors.overflow) continue;
            last_slot = colorset_find(&b->colors, p);
            last = p;
            if (last_slot >= 0) b->colors.counts[last_slot]++;
        }
    }
}

static void analyze_rows16(const Image *im, u32 y0, u32 y1, BandStats *b) {
    u64 last = 0;
    int last_slot = -1;
    for (u32 y = y0; y < y1; y++) {
        const u16 *row = (const u16 *)image_row(im, y);
        for (u32 x = 0; x < im->width; x++, row += 4) {
            u16 r = row[0], g = row[1], bl = row[2], a = row[3];
            b->alpha_hist[a >> 12]++;
            if (a == 0) { b->transparent++; note_transparent(b, r, g, bl); }
            else if (a == 65535) b->opaque++;
            else b->partial++;
            bool is_gray = r == g && g == bl;
            if (!is_gray) { b->gray = false; if (a) b->gray_visible = false; }
            bool exact8 = (r >> 8) == (r & 0xFF) && (g >> 8) == (g & 0xFF) && (bl >> 8) == (bl & 0xFF) && (a >> 8) == (a & 0xFF);
            if (!exact8) { b->reducible = false; continue; }
            if (is_gray) b->gray_levels[(r & 0xFF) >> 5] |= 1u << (r & 31);
            if (!b->reducible || b->colors.overflow) continue;
            u32 p = (u32)(r & 0xFF) | (u32)(g & 0xFF) << 8 | (u32)(bl & 0xFF) << 16 | (u32)(a & 0xFF) << 24;
            u64 key = (u64)p;
            if (last_slot >= 0 && key == last) { b->colors.counts[last_slot]++; continue; }
            last_slot = colorset_find(&b->colors, p);
            last = key;
            if (last_slot >= 0) b->colors.counts[last_slot]++;
        }
    }
}

typedef struct AnalyzeJob {
    const Image *image;
    BandStats   *bands;
    u32          band_rows;
} AnalyzeJob;

static void analyze_band(void *arg, size_t index) {
    const AnalyzeJob *job = arg;
    BandStats *b = &job->bands[index];
    band_init(b);
    u32 y0 = (u32)index * job->band_rows;
    u32 y1 = MIN(y0 + job->band_rows, job->image->height);
    if (job->image->depth == 8) analyze_rows8(job->image, y0, y1, b);
    else analyze_rows16(job->image, y0, y1, b);
}

// Une el conjunto de una banda al acumulado. false si el total pasa de 256.
static bool merge_colors(ColorSet *into, const ColorSet *from) {
    if (from->overflow) { into->overflow = true; return false; }
    for (u32 i = 0; i < SET_SLOTS && !into->overflow; i++) {
        if (!from->used[i]) continue;
        int slot = colorset_find(into, from->keys[i]);
        if (slot >= 0) into->counts[slot] += from->counts[i];
    }
    return !into->overflow;
}

static void stats_from_band(ColorStats *out, const BandStats *b, const ColorSet *colors, u32 depth, u64 pixels) {
    memset(out, 0, sizeof *out);
    out->depth = depth;
    out->pixels = pixels;
    out->transparent = b->transparent;
    out->partial = b->partial;
    out->opaque = b->opaque;
    out->gray = b->gray;
    out->gray_visible = b->gray_visible;
    out->reducible_to_8 = depth == 16 && b->reducible;
    memcpy(out->gray_levels, b->gray_levels, sizeof out->gray_levels);
    memcpy(out->alpha_hist, b->alpha_hist, sizeof out->alpha_hist);
    out->transparent_same_rgb = b->have_transparent && b->transparent_same;
    memcpy(out->transparent_rgb, b->transparent_rgb, sizeof out->transparent_rgb);
    if (colors->overflow || (depth == 16 && !b->reducible)) {
        out->colors = MAX_PALETTE + 1;
        return;
    }
    for (u32 i = 0; i < SET_SLOTS; i++) {
        if (!colors->used[i]) continue;
        out->palette[out->colors] = colors->keys[i];
        out->counts[out->colors] = colors->counts[i];
        out->colors++;
    }
}

void analyze_image(const Image *image, ColorStats *out) {
    u32 band_rows = MAX((u32)BAND_GRAIN_ROWS, (image->height + MAX_BANDS - 1) / MAX_BANDS);
    // Imagenes angostas y altas: bandas mas gruesas para que cada tarea valga la pena.
    if ((u64)band_rows * image->width < 65536) band_rows = (u32)MIN((u64)image->height, 65536 / MAX(image->width, 1u) + 1);
    u32 band_count = (image->height + band_rows - 1) / band_rows;
    BandStats *bands = mem_alloc((size_t)band_count * sizeof *bands);
    if (!bands) {   // sin memoria para bandas: una sola, en la pila del hilo
        BandStats single;
        band_init(&single);
        if (image->depth == 8) analyze_rows8(image, 0, image->height, &single); else analyze_rows16(image, 0, image->height, &single);
        stats_from_band(out, &single, &single.colors, image->depth, image_pixel_count(image));
        return;
    }
    AnalyzeJob job = { image, bands, band_rows };
    parallel_for(band_count, analyze_band, &job);

    BandStats total;
    band_init(&total);
    for (u32 i = 0; i < band_count; i++) {
        const BandStats *b = &bands[i];
        total.transparent += b->transparent;
        total.partial += b->partial;
        total.opaque += b->opaque;
        total.gray &= b->gray;
        total.gray_visible &= b->gray_visible;
        total.reducible &= b->reducible;
        for (int k = 0; k < 8; k++) total.gray_levels[k] |= b->gray_levels[k];
        for (int k = 0; k < 16; k++) total.alpha_hist[k] += b->alpha_hist[k];
        if (b->have_transparent) {
            if (!total.have_transparent) {
                total.have_transparent = true;
                total.transparent_same = b->transparent_same;
                memcpy(total.transparent_rgb, b->transparent_rgb, sizeof total.transparent_rgb);
            } else if (!b->transparent_same || memcmp(total.transparent_rgb, b->transparent_rgb, sizeof total.transparent_rgb) != 0) {
                total.transparent_same = false;
            }
        }
        if (!total.colors.overflow) merge_colors(&total.colors, &b->colors);
    }
    stats_from_band(out, &total, &total.colors, image->depth, image_pixel_count(image));
    mem_free(bands);
}

void analyze_merge(ColorStats *into, const ColorStats *other) {
    into->pixels += other->pixels;
    into->transparent += other->transparent;
    into->partial += other->partial;
    into->opaque += other->opaque;
    into->gray &= other->gray;
    into->gray_visible &= other->gray_visible;
    into->reducible_to_8 &= other->reducible_to_8;
    for (int k = 0; k < 8; k++) into->gray_levels[k] |= other->gray_levels[k];
    for (int k = 0; k < 16; k++) into->alpha_hist[k] += other->alpha_hist[k];
    if (other->transparent) {
        if (!into->transparent) {
            into->transparent_same_rgb = other->transparent_same_rgb;
            memcpy(into->transparent_rgb, other->transparent_rgb, sizeof into->transparent_rgb);
        } else if (!other->transparent_same_rgb || memcmp(into->transparent_rgb, other->transparent_rgb, sizeof into->transparent_rgb) != 0) {
            into->transparent_same_rgb = false;
        }
    }
    if (into->colors > MAX_PALETTE) return;
    if (other->colors > MAX_PALETTE) { into->colors = MAX_PALETTE + 1; return; }
    for (u32 i = 0; i < other->colors; i++) {
        u32 j = 0;
        while (j < into->colors && into->palette[j] != other->palette[i]) j++;
        if (j == into->colors) {
            if (into->colors == MAX_PALETTE) { into->colors = MAX_PALETTE + 1; return; }
            into->palette[j] = other->palette[i];
            into->counts[j] = 0;
            into->colors++;
        }
        into->counts[j] += other->counts[i];
    }
}

bool image_uses_rgb_opaque(const Image *image, const u16 rgb[3]) {
    for (u32 y = 0; y < image->height; y++) {
        const u8 *row = image_row(image, y);
        if (image->depth == 8) {
            u8 r = u16_to_u8(rgb[0]), g = u16_to_u8(rgb[1]), b = u16_to_u8(rgb[2]);
            for (u32 x = 0; x < image->width; x++, row += 4)
                if (row[3] && row[0] == r && row[1] == g && row[2] == b) return true;
        } else {
            const u16 *p = (const u16 *)row;
            for (u32 x = 0; x < image->width; x++, p += 4)
                if (p[3] && p[0] == rgb[0] && p[1] == rgb[1] && p[2] == rgb[2]) return true;
        }
    }
    return false;
}

u64 analyze_unique_colors(const Image *im) {
    size_t n = (size_t)im->width * im->height;
    u32 *a = mem_alloc(n * sizeof(u32)), *b = mem_alloc(n * sizeof(u32));
    if (!a || !b) { mem_free(a); mem_free(b); return 0; }
    size_t k = 0;
    for (u32 y = 0; y < im->height; y++) {
        const u8 *row = image_row(im, y);
        for (u32 x = 0; x < im->width; x++) {
            u32 p;
            if (im->depth == 8) memcpy(&p, row + 4 * (size_t)x, 4);
            else {
                const u16 *q = (const u16 *)row + 4 * (size_t)x;
                p = (u32)u16_to_u8(q[0]) | (u32)u16_to_u8(q[1]) << 8 | (u32)u16_to_u8(q[2]) << 16 | (u32)u16_to_u8(q[3]) << 24;
            }
            a[k++] = (p >> 24) ? p : 0;
        }
    }
    for (u32 shift = 0; shift < 32; shift += 8) {
        size_t count[257] = { 0 };
        for (size_t i = 0; i < n; i++) count[((a[i] >> shift) & 255) + 1]++;
        if (count[((a[0] >> shift) & 255) + 1] == n) continue;          // todos en la misma cubeta
        for (int c = 0; c < 256; c++) count[c + 1] += count[c];
        for (size_t i = 0; i < n; i++) b[count[(a[i] >> shift) & 255]++] = a[i];
        u32 *t = a; a = b; b = t;
    }
    u64 distinct = n ? 1 : 0;
    for (size_t i = 1; i < n; i++) distinct += a[i] != a[i - 1];
    mem_free(a);
    mem_free(b);
    return distinct;
}

u32 gray_min_depth(const ColorStats *s) {
    // Un nivel v es exacto con d bits si es multiplo de 255/(2^d - 1): 255 (1 bit), 85 (2), 17 (4).
    static const u32 STEP[3] = { 255, 85, 17 };
    static const u32 DEPTH[3] = { 1, 2, 4 };
    for (int k = 0; k < 3; k++) {
        bool ok = true;
        for (u32 v = 0; v < 256 && ok; v++)
            if ((s->gray_levels[v >> 5] >> (v & 31)) & 1) ok = v % STEP[k] == 0;
        if (ok) return DEPTH[k];
    }
    return 8;
}
