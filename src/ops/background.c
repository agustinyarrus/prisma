// ops/background.c - ver background.h
#include <math.h>
#include "background.h"
#include "propagate.h"
#include "../core/pool.h"

enum { BINS = 4096, MIN_COVERAGE_PERCENT = 30, MAX_EDGE_PX = 16 };
static const float MIN_SEPARATION = 0.0025f;       // |F - B|^2 minimo para poder separar sujeto y fondo
static const float MIN_ALPHA_UNMIX = 0.08f;        // por debajo, el color se toma del sujeto (no se amplifica ruido)

INLINE void read_px(const Image *im, size_t i, float px[4]) {
    if (im->depth == 8) { const u8 *p = im->pixels + 4 * i; for (int c = 0; c < 4; c++) px[c] = p[c] / 255.0f; }
    else { const u16 *p = (const u16 *)im->pixels + 4 * i; for (int c = 0; c < 4; c++) px[c] = p[c] / 65535.0f; }
}

INLINE void write_px(Image *im, size_t i, const float px[4]) {
    if (im->depth == 8) { u8 *p = im->pixels + 4 * i; for (int c = 0; c < 4; c++) p[c] = (u8)(saturatef(px[c]) * 255.0f + 0.5f); }
    else { u16 *p = (u16 *)im->pixels + 4 * i; for (int c = 0; c < 4; c++) p[c] = (u16)(saturatef(px[c]) * 65535.0f + 0.5f); }
}

// ---- deteccion del color de fondo -------------------------------------------------------------

typedef struct Votes {
    u32    *hist;
    double (*sums)[3];
    u64     border;
} Votes;

static void vote(Votes *v, const Image *im, u32 x, u32 y) {
    float px[4];
    read_px(im, (size_t)y * im->width + x, px);
    v->border++;
    if (px[3] < 0.5f) return;                       // el borde ya transparente no vota
    u32 bin = (u32)(px[0] * 15.0f + 0.5f) << 8 | (u32)(px[1] * 15.0f + 0.5f) << 4 | (u32)(px[2] * 15.0f + 0.5f);
    v->hist[bin]++;
    for (int c = 0; c < 3; c++) v->sums[bin][c] += px[c];
}

bool background_detect(const Image *im, Rgba *color, float *coverage) {
    Votes v = { mem_calloc(BINS * sizeof(u32)), mem_calloc(BINS * sizeof *v.sums), 0 };
    if (!v.hist || !v.sums) { mem_free(v.hist); mem_free(v.sums); return false; }
    u32 w = im->width, h = im->height;
    for (u32 x = 0; x < w; x++) { vote(&v, im, x, 0); if (h > 1) vote(&v, im, x, h - 1); }
    for (u32 y = 1; y + 1 < h; y++) { vote(&v, im, 0, y); if (w > 1) vote(&v, im, w - 1, y); }
    u32 best = 0;
    for (u32 b = 1; b < BINS; b++) if (v.hist[b] > v.hist[best]) best = b;
    bool ok = v.hist[best] > 0;
    if (ok) {
        double n = v.hist[best];
        *color = (Rgba){ (float)(v.sums[best][0] / n), (float)(v.sums[best][1] / n), (float)(v.sums[best][2] / n), 1.0f };
        *coverage = (float)(n / (double)v.border);
    }
    mem_free(v.hist);
    mem_free(v.sums);
    return ok;
}

// ---- relleno por renglones ---------------------------------------------------------------------

typedef struct Flood {
    const Image *im;
    Rgba         bg;
    float        fuzz;
    u8          *removed;     // 1 = fondo alcanzado desde el borde
    u32         *stack;
    size_t       top, cap;
} Flood;

INLINE bool is_background(const Flood *f, size_t i) {
    float px[4];
    read_px(f->im, i, px);
    if (px[3] <= 0.0f) return true;                // lo ya transparente conecta regiones de fondo
    float dr = px[0] - f->bg.r, dg = px[1] - f->bg.g, db = px[2] - f->bg.b;
    return sqrtf(dr * dr + dg * dg + db * db) * 0.57735027f <= f->fuzz;
}

static bool push(Flood *f, u32 i) {
    if (f->top == f->cap) {
        size_t cap = f->cap ? f->cap * 2 : 4096;
        u32 *grown = mem_realloc(f->stack, cap * sizeof(u32));
        if (!grown) return false;
        f->stack = grown;
        f->cap = cap;
    }
    f->stack[f->top++] = i;
    return true;
}

// Cada tramo horizontal se marca de una y solo se apilan semillas de los tramos nuevos de arriba
// y de abajo: cada pixel se evalua O(1) veces.
static bool flood_fill(Flood *f) {
    u32 w = f->im->width, h = f->im->height;
    while (f->top) {
        u32 seed = f->stack[--f->top];
        if (f->removed[seed]) continue;
        u32 y = seed / w, x = seed % w;
        size_t row = (size_t)y * w;
        u32 xl = x, xr = x;
        while (xl > 0 && !f->removed[row + xl - 1] && is_background(f, row + xl - 1)) xl--;
        while (xr + 1 < w && !f->removed[row + xr + 1] && is_background(f, row + xr + 1)) xr++;
        memset(f->removed + row + xl, 1, xr - xl + 1);
        for (int dir = -1; dir <= 1; dir += 2) {
            if ((dir < 0 && y == 0) || (dir > 0 && y + 1 >= h)) continue;
            size_t nrow = (size_t)(y + dir) * w;
            bool in_run = false;
            for (u32 nx = xl; nx <= xr; nx++) {
                bool bg = !f->removed[nrow + nx] && is_background(f, nrow + nx);
                if (bg && !in_run && !push(f, (u32)(nrow + nx))) return false;
                in_run = bg;
            }
        }
    }
    return true;
}

// ---- banda de matting: sujeto y fondo conocidos, a los dos lados de la frontera --------------------
//
// Un pixel de borde antialiasado es la mezcla P = a·F + (1 - a)·B del color puro del sujeto F con
// el fondo B. B es conocido. F sale de propagar hacia la banda el color de los pixeles INTERIORES
// mas cercanos (BFS por capas de propagate.c: O(n), sin ventanas). Con F y B, el alfa es la
// proyeccion de P - B sobre F - B (minimos cuadrados) y el color real se despeja de la mezcla.
//
// La banda cubre los dos lados de la frontera que dejo el relleno:
//   ADENTRO  (conservados a <= edge de lo borrado): el antialias fuerte; se des-mezcla.
//   AFUERA   (borrados a <= edge de lo conservado): el antialias TENUE que la tolerancia se trago.
//            Si la mezcla lo explica (proyeccion clara y casi nada de residuo perpendicular, que
//            es lo que distingue antialias de ruido de JPEG), vuelve como translucido.
// Resultado: bordes suaves de punta a punta, sin el ribete del fondo viejo.

// Etiqueta de cada pixel (un byte): de que lado de la frontera esta y a que distancia.
enum {
    LABEL_INTERIOR = 0,
    LABEL_INSIDE = 1,            // 1..MAX_EDGE_PX: distancia a lo borrado
    LABEL_OUTSIDE = 64,          // 65..64+MAX_EDGE_PX: distancia a lo conservado
    LABEL_REMOVED = 255,
};
_Static_assert(LABEL_OUTSIDE + MAX_EDGE_PX < LABEL_REMOVED, "las etiquetas de afuera no pueden pisar LABEL_REMOVED");
enum { BAND_GRAIN = 2048 };      // pixeles de banda por tramo paralelo
static const float MIN_RESTORE_ALPHA = 0.03f;       // ~8/255: por debajo, un borrado no vuelve (es ruido)
static const float MAX_RESIDUAL_RATIO = 0.5f;       // residuo perpendicular / parte explicada por la mezcla
static const float NOISE_FLOOR = 2.0f / 255.0f;     // ruido de cuantizacion que siempre se tolera

INLINE bool is_outside_label(u8 label) { return label > LABEL_OUTSIDE && label != LABEL_REMOVED; }

typedef struct Band {
    u32   *pixels;               // indices de los pixeles de la banda
    float *original;             // su RGBA original (4 por pixel)
    size_t count;
} Band;

// Distancia de chebyshev a la frontera, de un solo lado: BFS multi-fuente desde los pixeles de un
// lado que tocan el otro, avanzando solo por el otro. Marca label = base + d. O(n).
//   from_removed: fuentes = borrados, se avanza por los interiores (banda de ADENTRO).
//   si no:        fuentes = conservados (interiores o ya de la banda), se avanza por los borrados.
static void band_side(u8 *label, u32 w, u32 h, bool from_removed, u8 base, u32 edge, u32 *cur, u32 *nxt) {
    size_t n = (size_t)w * h, count = 0;
    for (size_t i = 0; i < n; i++) {
        bool source = from_removed ? label[i] == LABEL_REMOVED : label[i] != LABEL_REMOVED && !is_outside_label(label[i]);
        if (source) {
            i64 x = (i64)(i % w), y = (i64)(i / w);
            bool touches = false;
            for (int dy = -1; dy <= 1 && !touches; dy++)
                for (int dx = -1; dx <= 1 && !touches; dx++) {
                    i64 nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    u8 l = label[ny * w + nx];
                    touches = from_removed ? l == LABEL_INTERIOR : l == LABEL_REMOVED;
                }
            if (touches) cur[count++] = (u32)i;
        }
    }
    // Las fuentes del lado de afuera son pixeles conservados: solo sirven de semilla.
    for (u32 d = 1; d <= edge && count; d++) {
        size_t next_count = 0;
        for (size_t k = 0; k < count; k++) {
            i64 x = cur[k] % w, y = cur[k] / w;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    i64 nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    size_t ni = (size_t)(ny * w + nx);
                    bool target = from_removed ? label[ni] == LABEL_INTERIOR : label[ni] == LABEL_REMOVED;
                    if (!target) continue;
                    label[ni] = (u8)(base + d);
                    nxt[next_count++] = (u32)ni;
                }
        }
        u32 *t = cur; cur = nxt; nxt = t;
        count = next_count;
    }
}

typedef struct MatteJob {
    Image       *im;
    const Band  *band;
    const u8    *label;
    Rgba         bg;
    u64          changed;        // contadores compartidos: un suma atomica por tramo, no por pixel
    u64          restored;
} MatteJob;

static void matte_range(void *arg, size_t k0, size_t k1) {
    MatteJob *j = arg;
    const float B[3] = { j->bg.r, j->bg.g, j->bg.b };
    u64 changed = 0, restored = 0;
    for (size_t k = k0; k < k1; k++) {
        size_t i = j->band->pixels[k];
        const float *P = j->band->original + 4 * k;
        float F[4], out[4] = { 0, 0, 0, 0 };
        read_px(j->im, i, F);                               // la propagacion dejo F en el RGB
        bool outside = is_outside_label(j->label[i]);
        float fb[3], pb[3], den = 0.0f, num = 0.0f;
        for (int c = 0; c < 3; c++) {
            fb[c] = F[c] - B[c];
            pb[c] = P[c] - B[c];
            den += fb[c] * fb[c];
            num += pb[c] * fb[c];
        }
        float a = den >= MIN_SEPARATION ? saturatef(num / den) : outside ? 0.0f : 1.0f;
        if (outside) {
            // ¿La mezcla explica el pixel? residuo perpendicular chico frente a lo explicado.
            float residual = 0.0f;
            for (int c = 0; c < 3; c++) { float r = pb[c] - a * fb[c]; residual += r * r; }
            float explained = a * sqrtf(den);
            if (a >= MIN_RESTORE_ALPHA && sqrtf(residual) <= MAX_RESIDUAL_RATIO * explained + NOISE_FLOOR) {
                for (int c = 0; c < 3; c++)
                    out[c] = a >= MIN_ALPHA_UNMIX ? saturatef((P[c] - (1.0f - a) * B[c]) / a) : F[c];
                out[3] = P[3] * a;
                restored++;
            }
        } else if (a >= 0.999f || den < MIN_SEPARATION) {
            memcpy(out, P, sizeof out);                      // opaco, o no separable: queda como estaba
        } else {
            for (int c = 0; c < 3; c++)
                out[c] = a >= MIN_ALPHA_UNMIX ? saturatef((P[c] - (1.0f - a) * B[c]) / a) : F[c];
            out[3] = P[3] * a;
            changed++;
        }
        write_px(j->im, i, out);
    }
    __atomic_fetch_add(&j->changed, changed, __ATOMIC_RELAXED);
    __atomic_fetch_add(&j->restored, restored, __ATOMIC_RELAXED);
}

// Guarda el RGBA original de la banda y la deja transparente: asi la propagacion la trata como
// destino y le escribe F. Lo borrado de afuera de la banda tambien queda transparente.
static bool band_collect(Image *im, const u8 *label, Band *band) {
    size_t n = (size_t)im->width * im->height, count = 0;
    for (size_t i = 0; i < n; i++) count += label[i] != LABEL_INTERIOR && label[i] != LABEL_REMOVED;
    band->count = count;
    band->pixels = mem_alloc(MAX(count, (size_t)1) * sizeof(u32));
    band->original = mem_alloc(MAX(count, (size_t)1) * 4 * sizeof(float));
    if (!band->pixels || !band->original) return false;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        if (label[i] == LABEL_INTERIOR) continue;
        if (label[i] != LABEL_REMOVED) {
            band->pixels[k] = (u32)i;
            read_px(im, i, band->original + 4 * k);
            k++;
        }
        if (im->depth == 8) im->pixels[4 * i + 3] = 0; else ((u16 *)im->pixels)[4 * i + 3] = 0;
    }
    return true;
}

static void zero_removed(Image *im, const u8 *label) {
    size_t n = (size_t)im->width * im->height;
    for (size_t i = 0; i < n; i++)
        if (label[i] == LABEL_REMOVED) { if (im->depth == 8) memset(im->pixels + 4 * i, 0, 4); else memset(im->pixels + 8 * i, 0, 8); }
}

static Status matte_band(Image *im, u8 *label, u32 edge, Rgba bg, BgRemoveReport *rep, Diag *diag) {
    size_t n = (size_t)im->width * im->height;
    u32 w = im->width, h = im->height;
    u32 *cur = mem_alloc(n * sizeof(u32)), *nxt = mem_alloc(n * sizeof(u32));
    Band band = { 0 };
    Status st = ST_OK;
    if (!cur || !nxt) { st = diag_fail(diag, ST_NO_MEMORY, "sin memoria para --remove-bg"); goto done; }
    band_side(label, w, h, true, LABEL_INSIDE - 1, edge, cur, nxt);     // adentro: 1..edge
    band_side(label, w, h, false, LABEL_OUTSIDE, edge, cur, nxt);       // afuera: 65..64+edge
    mem_free(cur); cur = NULL;
    mem_free(nxt); nxt = NULL;
    if (!band_collect(im, label, &band)) { st = diag_fail(diag, ST_NO_MEMORY, "sin memoria para --remove-bg"); goto done; }
    if (band.count) {
        u64 touched;
        // F llega hasta el borde externo de la banda: 2·edge capas desde el interior.
        st = propagate_colors(im, PROPAGATE_BLEED, 2 * edge + 1, &touched, diag);
        if (st != ST_OK) goto done;
        MatteJob job = { im, &band, label, bg, 0, 0 };
        parallel_range(band.count, BAND_GRAIN, matte_range, &job);
        rep->softened += job.changed + job.restored;
        rep->removed -= job.restored;
    }
done:
    mem_free(cur);
    mem_free(nxt);
    mem_free(band.pixels);
    mem_free(band.original);
    return st;
}

Status background_remove(Image *im, const BgRemoveOptions *opt, BgRemoveReport *rep, Diag *diag) {
    memset(rep, 0, sizeof *rep);
    Rgba bg = opt->color;
    float coverage = 1.0f;
    if (opt->auto_color && !background_detect(im, &bg, &coverage)) {
        diag_warn(diag, "--remove-bg: el borde ya es transparente, no hay fondo que borrar");
        return ST_OK;
    }
    rep->color = bg;
    rep->border_coverage = coverage;
    if (opt->auto_color && coverage * 100.0f < MIN_COVERAGE_PERCENT)
        diag_warn(diag, "--remove-bg: el borde no es de un color parejo (%.0f %%): revisa el resultado o da el color", coverage * 100.0f);

    size_t n = (size_t)im->width * im->height;
    Flood f = { im, bg, opt->fuzz, mem_calloc(n), NULL, 0, 0 };
    if (!f.removed) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para --remove-bg");
    u32 w = im->width, h = im->height;
    bool ok = true;
    for (u32 x = 0; x < w && ok; x++) {
        if (is_background(&f, x)) ok = push(&f, x);
        if (ok && h > 1 && is_background(&f, (size_t)(h - 1) * w + x)) ok = push(&f, (u32)((size_t)(h - 1) * w + x));
    }
    for (u32 y = 1; y + 1 < h && ok; y++) {
        if (is_background(&f, (size_t)y * w)) ok = push(&f, y * w);
        if (ok && w > 1 && is_background(&f, (size_t)y * w + w - 1)) ok = push(&f, y * w + w - 1);
    }
    if (ok) ok = flood_fill(&f);
    mem_free(f.stack);
    if (!ok) { mem_free(f.removed); return diag_fail(diag, ST_NO_MEMORY, "sin memoria para --remove-bg"); }

    u8 *label = f.removed;                                  // 1 -> LABEL_REMOVED, 0 -> LABEL_INTERIOR
    for (size_t i = 0; i < n; i++) if (label[i]) { label[i] = LABEL_REMOVED; rep->removed++; }
    u32 edge = MIN(opt->edge_px, (u32)MAX_EDGE_PX);
    Status st = edge && rep->removed && rep->removed < n ? matte_band(im, label, edge, bg, rep, diag) : ST_OK;
    zero_removed(im, label);
    mem_free(label);
    return st;
}
