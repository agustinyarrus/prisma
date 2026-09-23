// ops/pixelops.c - ver pixelops.h
#include <math.h>
#include "pixelops.h"
#include "../core/colorspace.h"
#include "../core/pool.h"

enum { ROW_CHUNK = 1024 };               // pixeles por tramo de fila (buffer flotante en la pila)
static const float EPS = 1e-6f;

Kernel kernel_simple(KernelKind kind, float p0, float p1, float p2) {
    Kernel k = { kind, { p0, p1, p2 }, NULL, NULL };
    return k;
}

Kernel kernel_color_matrix(const float m[9]) {
    Kernel k = { K_COLOR_MATRIX, { 0 }, NULL, NULL };
    memcpy(k.p, m, 9 * sizeof(float));
    return k;
}

Kernel kernel_gamut_to_srgb(const float m[9], const float *decode_table) {
    Kernel k = kernel_color_matrix(m);
    k.kind = K_GAMUT_TO_SRGB;
    k.table = decode_table;
    return k;
}

// Tabla muestreada pareja con interpolacion lineal: la curva se evalua con powf UNA vez por
// entrada de la tabla, no por pixel.
INLINE float table_lookup(const float *t, float v) {
    float x = saturatef(v) * TRANSFER_LUT_SIZE;
    u32 i = (u32)x;
    if (i >= TRANSFER_LUT_SIZE) return t[TRANSFER_LUT_SIZE];
    float f = x - (float)i;
    return t[i] + (t[i + 1] - t[i]) * f;
}

INLINE void matrix3_apply(const float *m, const float in[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[3 * r] * in[0] + m[3 * r + 1] * in[1] + m[3 * r + 2] * in[2];
}

Kernel kernel_key(Rgba c, float fuzz, float feather) {
    Kernel k = { K_KEY, { c.r, c.g, c.b, fuzz, feather }, NULL, NULL };
    return k;
}

Kernel kernel_color_to_alpha(Rgba c, float t, float o) {
    Kernel k = { K_COLOR_TO_ALPHA, { c.r, c.g, c.b, t, o }, NULL, NULL };
    return k;
}

Kernel kernel_unmatte(Rgba m) {
    Kernel k = { K_UNMATTE, { m.r, m.g, m.b }, NULL, NULL };
    return k;
}

Kernel kernel_flatten(Rgba bg, bool linear) {
    Kernel k = { K_FLATTEN, { bg.r, bg.g, bg.b, linear ? 1.0f : 0.0f }, NULL, NULL };
    if (linear) { k.p[4] = srgb_to_linear(bg.r); k.p[5] = srgb_to_linear(bg.g); k.p[6] = srgb_to_linear(bg.b); }
    return k;
}

// Color a alfa (el algoritmo de GEGL que usa GIMP): el alfa minimo tal que el pixel sea la mezcla
// de un color "puro" con el fondo; el color puro se reconstruye. Conserva el antialias de los
// bordes y las sombras suaves, que un color clave con tolerancia destruiria.
static void color_to_alpha(float *px, const float *p) {
    const float *bg = p;
    float t = p[3], o = p[4];
    float alpha = 0.0f, dist = 0.0f;
    for (int i = 0; i < 3; i++) {
        float d = fabsf(px[i] - bg[i]), a;
        if (d < t + EPS) a = 0.0f;
        else if (d > o - EPS) a = 1.0f;
        else if (px[i] < bg[i]) a = (d - t) / (fminf(o, bg[i]) - t);
        else a = (d - t) / (fminf(o, 1.0f - bg[i]) - t);
        if (a > alpha) { alpha = a; dist = d; }
    }
    if (alpha > EPS) {
        float ratio = dist > EPS ? t / dist : 0.0f, inv = 1.0f / alpha;
        for (int i = 0; i < 3; i++) {
            float c = bg[i] + (px[i] - bg[i]) * ratio;
            px[i] = saturatef(c + (px[i] - c) * inv);
        }
    }
    px[3] *= saturatef(alpha);
}

INLINE float luma709(const float *px) { return 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2]; }

static void apply(const Kernel *k, float *px, size_t index) {
    const float *p = k->p;
    switch (k->kind) {
    case K_ALPHA_FROM_LUMA:     px[3] = saturatef(luma709(px)); break;
    case K_ALPHA_FROM_CHANNEL:  px[3] = px[(int)p[0]]; break;
    case K_ALPHA_INVERT_SOURCE: px[3] = 1.0f - px[3]; break;
    case K_MASK:                px[3] *= k->mask[index]; break;
    case K_UNPREMULTIPLY:
        if (px[3] > EPS) for (int i = 0; i < 3; i++) px[i] = fminf(1.0f, px[i] / px[3]);
        else px[0] = px[1] = px[2] = 0.0f;
        break;
    case K_UNMATTE:
        if (px[3] > EPS && px[3] < 1.0f)
            for (int i = 0; i < 3; i++) px[i] = saturatef((px[i] - (1.0f - px[3]) * p[i]) / px[3]);
        else if (px[3] <= EPS) px[0] = px[1] = px[2] = 0.0f;
        break;
    case K_KEY: {
        // Distancia RGB normalizada a [0,1] (dividida por la diagonal del cubo, raiz de 3).
        float dr = px[0] - p[0], dg = px[1] - p[1], db = px[2] - p[2];
        float d = sqrtf(dr * dr + dg * dg + db * db) * 0.57735027f;
        if (d <= p[3]) px[3] = 0.0f;
        else if (p[4] > 0.0f && d < p[3] + p[4]) px[3] *= (d - p[3]) / p[4];
        break;
    }
    case K_COLOR_TO_ALPHA: color_to_alpha(px, p); break;
    case K_THRESHOLD:      px[3] = px[3] >= p[0] ? 1.0f : 0.0f; break;
    case K_LEVELS: {
        float a = saturatef((px[3] - p[0]) / fmaxf(p[1] - p[0], EPS));
        px[3] = p[2] == 1.0f ? a : powf(a, 1.0f / p[2]);
        break;
    }
    case K_OPACITY:      px[3] *= p[0]; break;
    case K_INVERT_ALPHA: px[3] = 1.0f - px[3]; break;
    case K_FLATTEN: {
        float a = px[3];
        if (p[3] != 0.0f) {                      // en luz lineal: la mezcla fisicamente correcta
            for (int i = 0; i < 3; i++)
                px[i] = linear_to_srgb(srgb_to_linear(px[i]) * a + p[4 + i] * (1.0f - a));
        } else {
            for (int i = 0; i < 3; i++) px[i] = px[i] * a + p[i] * (1.0f - a);
        }
        px[3] = 1.0f;
        break;
    }
    case K_CLEAN_BLACK:  if (px[3] <= 0.0f) px[0] = px[1] = px[2] = 0.0f; break;
    case K_GRAYSCALE: {
        float y = 0.2126f * srgb_to_linear(px[0]) + 0.7152f * srgb_to_linear(px[1]) + 0.0722f * srgb_to_linear(px[2]);
        px[0] = px[1] = px[2] = saturatef(linear_to_srgb(y));
        break;
    }
    case K_PREMULTIPLY:  for (int i = 0; i < 3; i++) px[i] *= px[3]; break;
    case K_COLOR_MATRIX: {
        float in[3] = { px[0], px[1], px[2] };
        matrix3_apply(p, in, px);
        break;
    }
    case K_GAMUT_TO_SRGB: {
        float lin[3] = { table_lookup(k->table, px[0]), table_lookup(k->table, px[1]), table_lookup(k->table, px[2]) }, out[3];
        matrix3_apply(p, lin, out);
        for (int i = 0; i < 3; i++) px[i] = linear_to_srgb_fast(saturatef(out[i]));
        break;
    }
    }
}

typedef struct PixelJob {
    Image        *image;
    const Kernel *kernels;
    u32           count;
} PixelJob;

static void run_rows(void *arg, size_t y0, size_t y1) {
    const PixelJob *j = arg;
    const Image *im = j->image;
    float buf[ROW_CHUNK * 4];
    for (size_t y = y0; y < y1; y++) {
        u8 *row = image_row(im, (u32)y);
        for (u32 x0 = 0; x0 < im->width; x0 += ROW_CHUNK) {
            u32 n = MIN((u32)ROW_CHUNK, im->width - x0);
            if (im->depth == 8) { for (u32 i = 0; i < n * 4; i++) buf[i] = row[(size_t)x0 * 4 + i] * (1.0f / 255.0f); }
            else { const u16 *s = (const u16 *)row + (size_t)x0 * 4; for (u32 i = 0; i < n * 4; i++) buf[i] = s[i] * (1.0f / 65535.0f); }
            for (u32 i = 0; i < n; i++) {
                size_t index = y * im->width + x0 + i;
                for (u32 k = 0; k < j->count; k++) apply(&j->kernels[k], buf + 4 * i, index);
            }
            if (im->depth == 8) { for (u32 i = 0; i < n * 4; i++) row[(size_t)x0 * 4 + i] = (u8)(saturatef(buf[i]) * 255.0f + 0.5f); }
            else { u16 *d = (u16 *)row + (size_t)x0 * 4; for (u32 i = 0; i < n * 4; i++) d[i] = (u16)(saturatef(buf[i]) * 65535.0f + 0.5f); }
        }
    }
}

void pixelops_run(Image *image, const Kernel *kernels, u32 count) {
    if (!count) return;
    colorspace_init();
    PixelJob job = { image, kernels, count };
    parallel_range(image->height, MAX(1u, 32768u / MAX(image->width, 1u)), run_rows, &job);
}
