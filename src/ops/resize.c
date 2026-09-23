// ops/resize.c - ver resize.h
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "resize.h"
#include "../core/colorspace.h"
#include "../core/pool.h"

static const char *const FILTER_NAMES[RF__COUNT] = {
    "lanczos3", "lanczos2", "mitchell", "catmull", "bilinear", "box", "nearest",
};

const char *resize_filter_name(ResizeFilter f) { return (unsigned)f < RF__COUNT ? FILTER_NAMES[f] : "?"; }

bool resize_filter_parse(const char *name, ResizeFilter *out) {
    for (int i = 0; i < RF__COUNT; i++) if (strcmp(name, FILTER_NAMES[i]) == 0) { *out = (ResizeFilter)i; return true; }
    if (strcmp(name, "lanczos") == 0) { *out = RF_LANCZOS3; return true; }
    if (strcmp(name, "bicubic") == 0 || strcmp(name, "cubic") == 0) { *out = RF_CATMULL; return true; }
    if (strcmp(name, "linear") == 0 || strcmp(name, "triangle") == 0) { *out = RF_BILINEAR; return true; }
    return false;
}

// ---- especificacion de tamano ------------------------------------------------------------------

bool resize_spec_parse(const char *text, ResizeSpec *out, char *error, size_t cap) {
    memset(out, 0, sizeof *out);
    char s[64];
    size_t n = strlen(text);
    if (!n || n >= sizeof s) { snprintf(error, cap, "tamano vacio o demasiado largo"); return false; }
    memcpy(s, text, n + 1);
    while (n && (s[n - 1] == '!' || s[n - 1] == '^' || s[n - 1] == '>')) {
        if (s[n - 1] == '!') out->exact = true;
        else if (s[n - 1] == '^') out->cover = true;
        else out->only_shrink = true;
        s[--n] = 0;
    }
    if (n && s[n - 1] == '%') {
        s[n - 1] = 0;
        char *end;
        double p = strtod(s, &end);
        if (*end || p <= 0.0 || p > 10000.0) { snprintf(error, cap, "porcentaje invalido: %s", text); return false; }
        out->percent = (float)p;
        out->active = true;
        return true;
    }
    char *x = strchr(s, 'x');
    if (!x) x = strchr(s, 'X');
    char *end;
    if (!x) {
        long w = strtol(s, &end, 10);
        if (*end || w <= 0) { snprintf(error, cap, "tamano invalido \"%s\": usa ANCHOxALTO, ANCHOx, xALTO o N%%", text); return false; }
        out->width = (u32)w;
    } else {
        *x = 0;
        if (*s) { long w = strtol(s, &end, 10); if (*end || w <= 0) goto bad; out->width = (u32)w; }
        if (x[1]) { long h = strtol(x + 1, &end, 10); if (*end || h <= 0) goto bad; out->height = (u32)h; }
        if (!out->width && !out->height) goto bad;
    }
    if (out->width > 1000000 || out->height > 1000000) { snprintf(error, cap, "tamano absurdo: %s", text); return false; }
    out->active = true;
    return true;
bad:
    snprintf(error, cap, "tamano invalido \"%s\": usa ANCHOxALTO, ANCHOx, xALTO o N%%", text);
    return false;
}

bool resize_target(const ResizeSpec *spec, u32 w, u32 h, u32 *ow, u32 *oh) {
    if (!spec->active) return false;
    double tw, th;
    if (spec->percent > 0.0f) {
        tw = w * spec->percent / 100.0;
        th = h * spec->percent / 100.0;
    } else if (spec->width && spec->height) {
        if (spec->exact) { tw = spec->width; th = spec->height; }
        else {
            double sx = (double)spec->width / w, sy = (double)spec->height / h;
            double s = spec->cover ? MAX(sx, sy) : MIN(sx, sy);
            tw = w * s;
            th = h * s;
        }
    } else if (spec->width) {
        tw = spec->width;
        th = (double)h * spec->width / w;
    } else {
        th = spec->height;
        tw = (double)w * spec->height / h;
    }
    u32 nw = (u32)MAX(1.0, floor(tw + 0.5)), nh = (u32)MAX(1.0, floor(th + 0.5));
    if (spec->only_shrink && nw >= w && nh >= h) return false;
    if (nw == w && nh == h) return false;
    *ow = nw;
    *oh = nh;
    return true;
}

// ---- nucleos de filtro --------------------------------------------------------------------------

static double sinc(double x) {
    if (fabs(x) < 1e-8) return 1.0;
    x *= 3.14159265358979323846;
    return sin(x) / x;
}

// Mitchell-Netravali generalizado: (B, C) = (1/3, 1/3) Mitchell; (0, 0.5) Catmull-Rom.
static double cubic(double x, double B, double C) {
    x = fabs(x);
    if (x < 1.0) return ((12 - 9 * B - 6 * C) * x * x * x + (-18 + 12 * B + 6 * C) * x * x + (6 - 2 * B)) / 6.0;
    if (x < 2.0) return ((-B - 6 * C) * x * x * x + (6 * B + 30 * C) * x * x + (-12 * B - 48 * C) * x + (8 * B + 24 * C)) / 6.0;
    return 0.0;
}

static double support_of(ResizeFilter f) {
    switch (f) {
    case RF_LANCZOS3: return 3.0;
    case RF_LANCZOS2: case RF_MITCHELL: case RF_CATMULL: return 2.0;
    case RF_BILINEAR: return 1.0;
    default: return 0.5;
    }
}

static double weight_of(ResizeFilter f, double x) {
    switch (f) {
    case RF_LANCZOS3: return fabs(x) < 3.0 ? sinc(x) * sinc(x / 3.0) : 0.0;
    case RF_LANCZOS2: return fabs(x) < 2.0 ? sinc(x) * sinc(x / 2.0) : 0.0;
    case RF_MITCHELL: return cubic(x, 1.0 / 3.0, 1.0 / 3.0);
    case RF_CATMULL:  return cubic(x, 0.0, 0.5);
    case RF_BILINEAR: return fabs(x) < 1.0 ? 1.0 - fabs(x) : 0.0;
    default:          return fabs(x) <= 0.5 ? 1.0 : 0.0;
    }
}

// Pesos de cada posicion de salida: indices de origen [start, start + count) y sus pesos
// normalizados. Los indices fuera de la imagen se pliegan al borde (se repite el ultimo pixel).
typedef struct Contributions {
    u32   *start;
    u32   *count;
    float *weights;       // count_max por salida
    u32    count_max;
} Contributions;

static void contributions_free(Contributions *c) { mem_free(c->start); mem_free(c->count); mem_free(c->weights); }

static bool contributions_build(u32 src, u32 dst, ResizeFilter f, Contributions *c) {
    double scale = (double)dst / src;
    double stretch = scale < 1.0 ? 1.0 / scale : 1.0;
    double support = support_of(f) * stretch;
    c->count_max = (u32)ceil(support * 2.0) + 2;
    c->start = mem_alloc(dst * sizeof(u32));
    c->count = mem_alloc(dst * sizeof(u32));
    c->weights = mem_calloc((size_t)dst * c->count_max * sizeof(float));
    if (!c->start || !c->count || !c->weights) { contributions_free(c); return false; }
    for (u32 i = 0; i < dst; i++) {
        double center = (i + 0.5) / scale - 0.5;
        i64 lo = (i64)floor(center - support + 1e-9), hi = (i64)ceil(center + support - 1e-9);
        i64 first = MAX(lo, (i64)0), last = MIN(hi, (i64)src - 1);
        if (last < first) first = last = CLAMP((i64)floor(center + 0.5), (i64)0, (i64)src - 1);
        u32 n = (u32)(last - first + 1);
        if (n > c->count_max) n = c->count_max;
        float *w = c->weights + (size_t)i * c->count_max;
        double total = 0.0;
        for (i64 j = lo; j <= hi; j++) {
            double wt = weight_of(f, (j - center) / stretch);
            if (wt == 0.0) continue;
            i64 k = CLAMP(j, first, first + (i64)n - 1) - first;      // plegar al borde
            w[k] += (float)wt;
            total += wt;
        }
        if (fabs(total) < 1e-12) { memset(w, 0, n * sizeof(float)); w[CLAMP((i64)floor(center + 0.5) - first, (i64)0, (i64)n - 1)] = 1.0f; total = 1.0; }
        for (u32 k = 0; k < n; k++) w[k] = (float)(w[k] / total);
        c->start[i] = (u32)first;
        c->count[i] = n;
    }
    return true;
}

// ---- pasadas -------------------------------------------------------------------------------------

typedef struct ResizeJob {
    const Image         *src;
    Image               *dst;
    float               *tmp;          // dst.width x src.height x 4 (premultiplicado)
    const Contributions *horizontal, *vertical;
    bool                 linear;
    const float         *lut8;         // 8 bits -> [0,1] (lineal o codificado)
} ResizeJob;

// Fila de origen a flotante premultiplicado.
static void load_row(const ResizeJob *j, u32 y, float *out) {
    const Image *im = j->src;
    const u8 *row = image_row(im, y);
    for (u32 x = 0; x < im->width; x++) {
        float c[4];
        if (im->depth == 8) {
            const u8 *p = row + 4 * (size_t)x;
            c[0] = j->lut8[p[0]]; c[1] = j->lut8[p[1]]; c[2] = j->lut8[p[2]]; c[3] = p[3] / 255.0f;
        } else {
            const u16 *p = (const u16 *)row + 4 * (size_t)x;
            for (int k = 0; k < 3; k++) c[k] = j->linear ? srgb_to_linear(p[k] / 65535.0f) : p[k] / 65535.0f;
            c[3] = p[3] / 65535.0f;
        }
        out[4 * x] = c[0] * c[3]; out[4 * x + 1] = c[1] * c[3]; out[4 * x + 2] = c[2] * c[3]; out[4 * x + 3] = c[3];
    }
}

static void horizontal_pass(void *arg, size_t y0, size_t y1) {
    const ResizeJob *j = arg;
    const Contributions *c = j->horizontal;
    u32 dw = j->dst->width;
    float *row = mem_alloc((size_t)j->src->width * 4 * sizeof(float));
    if (!row) return;
    for (size_t y = y0; y < y1; y++) {
        load_row(j, (u32)y, row);
        float *out = j->tmp + y * dw * 4;
        for (u32 x = 0; x < dw; x++) {
            const float *w = c->weights + (size_t)x * c->count_max;
            const float *s = row + 4 * (size_t)c->start[x];
            float acc[4] = { 0, 0, 0, 0 };
            for (u32 k = 0; k < c->count[x]; k++)
                for (int ch = 0; ch < 4; ch++) acc[ch] += w[k] * s[4 * k + ch];
            memcpy(out + 4 * (size_t)x, acc, sizeof acc);
        }
    }
    mem_free(row);
}

// Por fila de salida: acumulador de fila completa y una suma "a*x + y" por cada fila de origen que
// contribuye. Recorre memoria contigua (el compilador lo vectoriza) en vez de saltar por columnas.
static void vertical_pass(void *arg, size_t y0, size_t y1) {
    const ResizeJob *j = arg;
    const Contributions *c = j->vertical;
    Image *dst = j->dst;
    u32 dw = dst->width;
    size_t row_floats = (size_t)dw * 4;
    float *acc_row = mem_alloc(row_floats * sizeof(float));
    if (!acc_row) return;
    for (size_t y = y0; y < y1; y++) {
        const float *w = c->weights + y * c->count_max;
        memset(acc_row, 0, row_floats * sizeof(float));
        for (u32 k = 0; k < c->count[y]; k++) {
            const float *restrict s = j->tmp + (size_t)(c->start[y] + k) * row_floats;
            float wk = w[k];
            for (size_t i = 0; i < row_floats; i++) acc_row[i] += wk * s[i];
        }
        u8 *out = image_row(dst, (u32)y);
        for (u32 x = 0; x < dw; x++) {
            const float *acc = acc_row + 4 * (size_t)x;
            // Lanczos sobrepasa: alfa a [0,1] y color a [0, alfa] (invariante del premultiplicado)
            // ANTES de dividir; si no, aparecen bordes brillantes en el contorno.
            float a = saturatef(acc[3]), px[4];
            for (int ch = 0; ch < 3; ch++) {
                float v = a > 1e-6f ? clampf(acc[ch], 0.0f, a) / a : 0.0f;
                px[ch] = j->linear ? linear_to_srgb_fast(v) : v;
            }
            px[3] = a;
            if (dst->depth == 8) { for (int ch = 0; ch < 4; ch++) out[4 * (size_t)x + ch] = (u8)(saturatef(px[ch]) * 255.0f + 0.5f); }
            else { u16 *o = (u16 *)out; for (int ch = 0; ch < 4; ch++) o[4 * (size_t)x + ch] = (u16)(saturatef(px[ch]) * 65535.0f + 0.5f); }
        }
    }
    mem_free(acc_row);
}

typedef struct NearestJob { const Image *src; Image *dst; } NearestJob;

static void nearest_rows(void *arg, size_t y0, size_t y1) {
    const NearestJob *j = arg;
    u32 bpp = image_bytes_per_pixel(j->src);
    for (size_t y = y0; y < y1; y++) {
        u32 sy = (u32)MIN(((double)y + 0.5) * j->src->height / j->dst->height, (double)(j->src->height - 1));
        const u8 *s = image_row(j->src, sy);
        u8 *d = image_row(j->dst, (u32)y);
        for (u32 x = 0; x < j->dst->width; x++) {
            u32 sx = (u32)MIN(((double)x + 0.5) * j->src->width / j->dst->width, (double)(j->src->width - 1));
            memcpy(d + (size_t)x * bpp, s + (size_t)sx * bpp, bpp);
        }
    }
}

Status resize_image(Image *im, u32 width, u32 height, ResizeFilter filter, bool linear, Diag *diag) {
    if (width == im->width && height == im->height) return ST_OK;
    Image out;
    Status s = image_alloc(&out, width, height, im->depth, false, 0, diag);
    if (s != ST_OK) return s;
    if (filter == RF_NEAREST) {
        NearestJob job = { im, &out };
        parallel_range(height, 64, nearest_rows, &job);
        image_free(im);
        *im = out;
        return ST_OK;
    }
    colorspace_init();
    float lut8[256];
    for (int i = 0; i < 256; i++) lut8[i] = linear ? srgb8_to_linear((u8)i) : i / 255.0f;
    Contributions hc = { 0 }, vc = { 0 };
    size_t tmp_bytes;
    float *tmp = NULL;
    if (!contributions_build(im->width, width, filter, &hc) || !contributions_build(im->height, height, filter, &vc) ||
        !mul_ok((size_t)width * im->height, 16, &tmp_bytes) || !(tmp = mem_alloc(tmp_bytes))) {
        contributions_free(&hc);
        contributions_free(&vc);
        image_free(&out);
        return diag_fail(diag, ST_NO_MEMORY, "sin memoria para redimensionar");
    }
    ResizeJob job = { im, &out, tmp, &hc, &vc, linear, lut8 };
    parallel_range(im->height, 16, horizontal_pass, &job);
    parallel_range(height, 16, vertical_pass, &job);
    mem_free(tmp);
    contributions_free(&hc);
    contributions_free(&vc);
    image_free(im);
    *im = out;
    return ST_OK;
}
