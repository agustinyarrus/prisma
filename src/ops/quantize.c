// ops/quantize.c - ver quantize.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include "quantize.h"
#include "../core/pool.h"

enum {
    MAX_COLORS = 256, HIST_LIMIT = 1 << 21, CACHE_BITS = 12, BAYER = 8, TRANSPARENT_KEY = 0,
    KMEANS_MAX_ITERATIONS = 40,
    KMEANS_WORK_BUDGET = 1 << 26,                 // entradas x iteraciones: acota el tiempo con millones de colores
    KMEANS_MIN_ITERATIONS = 4,
};
static const double KMEANS_MIN_GAIN = 1e-4;      // mejora relativa del error por debajo de la cual se para

// Pesos por canal (sensibilidad relativa al verde) sobre colores premultiplicados.
static const float W[3] = { 0.5f, 1.0f, 0.45f };
static const float W_ALPHA = 0.625f;             // peso del alfa al partir cajas
static const float ERROR_CLAMP = 0.25f;          // tope del error difundido (evita "estelas")

typedef struct Vec4 {
    float p[3];                                  // W[c] * color * alfa
    float a;
} Vec4;

INLINE Vec4 vec_of(u32 rgba) {
    float a = (rgba >> 24) / 255.0f;
    Vec4 v = { { W[0] * (rgba & 255) / 255.0f * a, W[1] * ((rgba >> 8) & 255) / 255.0f * a, W[2] * ((rgba >> 16) & 255) / 255.0f * a }, a };
    return v;
}

INLINE Vec4 vec_straight(float r, float g, float b, float a) {
    Vec4 v = { { W[0] * r * a, W[1] * g * a, W[2] * b * a }, a };
    return v;
}

// Promedio del error al verlos sobre negro y sobre blanco, canal por canal:
//   negro: dp            blanco: dp + W·da          (dp premultiplicado y ponderado)
//   (dp² + (dp + W·da)²) / 2 = dp² + dp·W·da + (W·da)²/2
// Es una forma cuadratica definida positiva, y eso importa: la MEDIA de un grupo es exactamente el
// punto que minimiza la suma de estas distancias, asi que Lloyd (k-means) baja el error en cada
// iteracion sin excepcion. (Con "el peor de los dos" la media no es el optimo y k-means oscila.)
INLINE float distance(const Vec4 *x, const Vec4 *y) {
    float da = y->a - x->a, d = 0.0f;
    for (int c = 0; c < 3; c++) {
        float dp = x->p[c] - y->p[c], wa = W[c] * da;
        d += dp * dp + dp * wa + 0.5f * wa * wa;
    }
    return d;
}

// Cota para podar: cada termino es >= dp²/2 (el minimo de 1 + t + t²/2 es 1/2, en t = -1).
static const float PRUNE_FACTOR = 0.5f;

static u32 rgba_of(const Vec4 *v) {
    float a = saturatef(v->a);
    u32 A = (u32)(a * 255.0f + 0.5f);
    if (!A) return 0;
    u32 out = A << 24;
    for (int c = 0; c < 3; c++) {
        float col = saturatef(v->p[c] / (W[c] * a));
        out |= (u32)(col * 255.0f + 0.5f) << (8 * c);
    }
    return out;
}

INLINE u32 normalize_key(u32 rgba) { return (rgba >> 24) ? rgba : TRANSPARENT_KEY; }

// ---- histograma: direccionamiento abierto con hash de Fibonacci de 64 bits ------------------------

typedef struct Hist {
    u64   *keys;          // UINT64_MAX = vacio
    u32   *values;
    size_t cap, size;
} Hist;

static void hist_free(Hist *h) { mem_free(h->keys); mem_free(h->values); memset(h, 0, sizeof *h); }

static bool hist_init(Hist *h, size_t cap) {
    h->cap = cap;
    h->size = 0;
    h->keys = mem_alloc(cap * sizeof(u64));
    h->values = mem_calloc(cap * sizeof(u32));
    if (!h->keys || !h->values) { hist_free(h); return false; }
    memset(h->keys, 0xFF, cap * sizeof(u64));
    return true;
}

INLINE size_t hist_slot(const Hist *h, u32 key) {
    return (size_t)(((u64)key * 0x9E3779B97F4A7C15ull) >> (64 - __builtin_ctzll(h->cap)));
}

static u32 *hist_find(Hist *h, u32 key, bool insert);

static bool hist_grow(Hist *h) {
    Hist bigger;
    if (!hist_init(&bigger, h->cap * 2)) return false;
    for (size_t i = 0; i < h->cap; i++)
        if (h->keys[i] != UINT64_MAX) *hist_find(&bigger, (u32)h->keys[i], true) = h->values[i];
    hist_free(h);
    *h = bigger;
    return true;
}

static u32 *hist_find(Hist *h, u32 key, bool insert) {
    size_t i = hist_slot(h, key), mask = h->cap - 1;
    while (h->keys[i] != UINT64_MAX) {
        if (h->keys[i] == key) return &h->values[i];
        i = (i + 1) & mask;
    }
    if (!insert) return NULL;
    if ((h->size + 1) * 2 > h->cap) {
        if (!hist_grow(h)) return NULL;
        return hist_find(h, key, true);
    }
    h->keys[i] = key;
    h->values[i] = 0;
    h->size++;
    return &h->values[i];
}

// ---- median cut ---------------------------------------------------------------------------------

typedef struct Entry {
    u32  rgba;
    u32  count;
    Vec4 v;
} Entry;

typedef struct Box {
    u32    lo, hi;          // rango en el arreglo de entradas
    double sse;             // error cuadratico ponderado por cantidad
    int    axis;            // eje de mayor varianza (0..2 color, 3 alfa)
    Vec4   mean;
} Box;

INLINE float axis_value(const Entry *e, int axis) { return axis < 3 ? e->v.p[axis] : e->v.a * W_ALPHA; }

static void box_measure(const Entry *entries, Box *b) {
    double n = 0, sum[4] = { 0 }, sq[4] = { 0 };
    for (u32 i = b->lo; i < b->hi; i++) {
        const Entry *e = &entries[i];
        for (int d = 0; d < 4; d++) {
            double v = axis_value(e, d);
            sum[d] += v * e->count;
            sq[d] += v * v * e->count;
        }
        n += e->count;
    }
    b->sse = 0;
    double best = -1;
    for (int d = 0; d < 4; d++) {
        double var = sq[d] - sum[d] * sum[d] / n;
        b->sse += var;
        if (var > best) { best = var; b->axis = d; }
    }
    b->mean = (Vec4){ { (float)(sum[0] / n), (float)(sum[1] / n), (float)(sum[2] / n) }, (float)(sum[3] / n / W_ALPHA) };
    if (b->hi - b->lo < 2) b->sse = -1.0;               // una sola entrada: no se puede partir
}

// Cola de prioridad (heap binario de maximos por SSE).
typedef struct BoxHeap { Box *items; u32 size; } BoxHeap;

static void heap_push(BoxHeap *h, Box b) {
    u32 i = h->size++;
    h->items[i] = b;
    while (i && h->items[(i - 1) / 2].sse < h->items[i].sse) {
        Box t = h->items[i]; h->items[i] = h->items[(i - 1) / 2]; h->items[(i - 1) / 2] = t;
        i = (i - 1) / 2;
    }
}

static Box heap_pop(BoxHeap *h) {
    Box top = h->items[0];
    h->items[0] = h->items[--h->size];
    for (u32 i = 0;;) {
        u32 l = 2 * i + 1, r = l + 1, m = i;
        if (l < h->size && h->items[l].sse > h->items[m].sse) m = l;
        if (r < h->size && h->items[r].sse > h->items[m].sse) m = r;
        if (m == i) break;
        Box t = h->items[i]; h->items[i] = h->items[m]; h->items[m] = t;
        i = m;
    }
    return top;
}

static int compare_axis(void *ctx, const void *a, const void *b) {
    int axis = *(const int *)ctx;
    float x = axis_value(a, axis), y = axis_value(b, axis);
    return x < y ? -1 : x > y;
}

// Donde cortar una caja ya ordenada por su eje: el punto que minimiza la suma de los errores
// cuadraticos de las dos mitades (sobre los 4 ejes), con sumas prefijas: O(m) por corte. La
// mediana de poblacion (el median cut clasico) parte por la mitad de los PIXELES aunque la
// mitad caiga en medio de un grupo compacto; esto parte donde de verdad hay un hueco.
static u32 best_split(const Entry *entries, u32 lo, u32 hi) {
    double total_n = 0, total_s[4] = { 0 }, total_q = 0;
    for (u32 i = lo; i < hi; i++) {
        double c = entries[i].count;
        total_n += c;
        for (int d = 0; d < 4; d++) { double v = axis_value(&entries[i], d); total_s[d] += v * c; total_q += v * v * c; }
    }
    double left_n = 0, left_s[4] = { 0 }, best = INFINITY;
    u32 split = lo + (hi - lo) / 2;
    for (u32 i = lo; i + 1 < hi; i++) {
        double c = entries[i].count;
        left_n += c;
        for (int d = 0; d < 4; d++) left_s[d] += axis_value(&entries[i], d) * c;
        // SSE(izq) + SSE(der) = sum q - |s_izq|^2/n_izq - |s_der|^2/n_der: la suma de cuadrados
        // total no depende del corte, asi que solo hacen falta las sumas simples de cada lado.
        double right_n = total_n - left_n, sse = total_q;
        double ls = 0, rs = 0;
        for (int d = 0; d < 4; d++) { ls += left_s[d] * left_s[d]; rs += (total_s[d] - left_s[d]) * (total_s[d] - left_s[d]); }
        sse -= ls / left_n + rs / right_n;
        if (sse < best) { best = sse; split = i + 1; }
    }
    return split;
}

static u32 median_cut(Entry *entries, u32 count, u32 target, Vec4 *palette) {
    BoxHeap heap = { mem_alloc((size_t)target * sizeof(Box)), 0 };
    if (!heap.items) return 0;
    Box all = { 0, count, 0, 0, { { 0 } } };
    box_measure(entries, &all);
    heap_push(&heap, all);
    while (heap.size < target && heap.items[0].sse > 0.0) {
        Box b = heap_pop(&heap);
        qsort_s(entries + b.lo, b.hi - b.lo, sizeof(Entry), compare_axis, &b.axis);
        u32 split = best_split(entries, b.lo, b.hi);
        Box left = { b.lo, split, 0, 0, { { 0 } } }, right = { split, b.hi, 0, 0, { { 0 } } };
        box_measure(entries, &left);
        box_measure(entries, &right);
        heap_push(&heap, left);
        heap_push(&heap, right);
    }
    u32 n = heap.size;
    for (u32 i = 0; i < n; i++) palette[i] = heap.items[i].mean;
    mem_free(heap.items);
    return n;
}

// ---- vecino mas cercano con poda por eje ordenado ------------------------------------------------
//
// La paleta se ordena por el canal verde ponderado; cada termino de la distancia es >= (dif. en ese
// canal)^2, asi que al alejarse del punto de consulta por el eje ordenado, en cuanto esa diferencia
// al cuadrado supera la mejor distancia, todo lo que sigue en esa direccion queda descartado.

typedef struct Finder {
    Vec4 pal[MAX_COLORS];
    u32  rgba[MAX_COLORS];
    u32  order[MAX_COLORS];
    float key[MAX_COLORS];
    u32  n;
    int  transparent;           // indice de la entrada (0,0,0,0) o -1
} Finder;

static int finder_order(void *ctx, const void *a, const void *b) {
    const Finder *f = ctx;
    float x = f->pal[*(const u32 *)a].p[1], y = f->pal[*(const u32 *)b].p[1];
    return x < y ? -1 : x > y;
}

static void finder_build(Finder *f) {
    f->transparent = -1;
    for (u32 i = 0; i < f->n; i++) {
        f->pal[i] = vec_of(f->rgba[i]);
        f->order[i] = i;
        if (f->rgba[i] == 0) f->transparent = (int)i;
    }
    qsort_s(f->order, f->n, sizeof(u32), finder_order, f);
    for (u32 i = 0; i < f->n; i++) f->key[i] = f->pal[f->order[i]].p[1];
}

static u32 finder_nearest(const Finder *f, const Vec4 *q) {
    u32 lo = 0, hi = f->n;
    while (lo < hi) { u32 mid = (lo + hi) / 2; if (f->key[mid] < q->p[1]) lo = mid + 1; else hi = mid; }
    i64 left = (i64)lo - 1, right = lo;
    float best = INFINITY;
    u32 best_i = f->order[MIN(lo, f->n - 1)];
    while (left >= 0 || right < (i64)f->n) {
        if (right < (i64)f->n) {
            float dk = f->key[right] - q->p[1];
            if (PRUNE_FACTOR * dk * dk >= best) right = f->n;
            else {
                float d = distance(q, &f->pal[f->order[right]]);
                if (d < best) { best = d; best_i = f->order[right]; }
                right++;
            }
        }
        if (left >= 0) {
            float dk = q->p[1] - f->key[left];
            if (PRUNE_FACTOR * dk * dk >= best) left = -1;
            else {
                float d = distance(q, &f->pal[f->order[left]]);
                if (d < best) { best = d; best_i = f->order[left]; }
                left--;
            }
        }
    }
    return best_i;
}

// ---- k-means (Lloyd) --------------------------------------------------------------------------------
//
// Cada iteracion: asignar cada color unico a su centro mas cercano (en paralelo, con la busqueda
// podada) y mover cada centro a la media de lo suyo, pesada por cantidad de pixeles. Con la
// metrica cuadratica el error total baja siempre; se para cuando mejora menos de 0,01 %.
// Un centro que se queda sin colores se re-siembra en el color con MAS error (pesado): asi
// ningun lugar de la paleta se desperdicia.

typedef struct AssignJob {
    const Finder *finder;
    const Entry  *entries;
    u16          *assign;
    float        *cost;          // distancia al centro asignado
} AssignJob;

static void assign_range(void *arg, size_t lo, size_t hi) {
    const AssignJob *j = arg;
    for (size_t i = lo; i < hi; i++) {
        u32 k = finder_nearest(j->finder, &j->entries[i].v);
        j->assign[i] = (u16)k;
        j->cost[i] = distance(&j->entries[i].v, &j->finder->pal[k]);
    }
}

// Re-siembra los centros vacios en los colores de mayor error pesado (uno distinto por centro).
static void reseed_empty(const Entry *entries, u32 count, const float *cost, const double *counts, Finder *f) {
    for (u32 k = 0; k < f->n; k++) {
        if (counts[k] || (int)k == f->transparent) continue;
        double worst = 0;
        u32 pick = UINT32_MAX;
        for (u32 i = 0; i < count; i++) {
            double e = cost[i] * (double)entries[i].count;
            if (e > worst) {
                bool taken = false;
                for (u32 m = 0; m < f->n && !taken; m++) taken = f->rgba[m] == entries[i].rgba;
                if (!taken) { worst = e; pick = i; }
            }
        }
        if (pick != UINT32_MAX) f->rgba[k] = entries[pick].rgba;
    }
}

static void kmeans(Entry *entries, u32 count, Finder *f, u16 *assign, float *cost) {
    u32 iterations = CLAMP((u32)(KMEANS_WORK_BUDGET / MAX(count, 1u)), (u32)KMEANS_MIN_ITERATIONS, (u32)KMEANS_MAX_ITERATIONS);
    double previous = INFINITY;
    for (u32 iter = 0; iter < iterations; iter++) {
        finder_build(f);
        AssignJob job = { f, entries, assign, cost };
        parallel_range(count, 4096, assign_range, &job);
        double sums[MAX_COLORS][4], counts[MAX_COLORS], total = 0;
        memset(sums, 0, sizeof sums);
        memset(counts, 0, sizeof counts);
        for (u32 i = 0; i < count; i++) {
            u32 k = assign[i];
            double c = entries[i].count;
            for (int d = 0; d < 3; d++) sums[k][d] += entries[i].v.p[d] * c;
            sums[k][3] += entries[i].v.a * c;
            counts[k] += c;
            total += cost[i] * c;
        }
        if (iter && previous - total <= previous * KMEANS_MIN_GAIN) break;   // convergio
        previous = total;
        bool changed = false;
        for (u32 k = 0; k < f->n; k++) {
            if (!counts[k] || (int)k == f->transparent) continue;
            Vec4 m = { { (float)(sums[k][0] / counts[k]), (float)(sums[k][1] / counts[k]), (float)(sums[k][2] / counts[k]) },
                       (float)(sums[k][3] / counts[k]) };
            u32 rgba = rgba_of(&m);
            if (rgba != f->rgba[k]) { f->rgba[k] = rgba; changed = true; }
        }
        u32 before[MAX_COLORS];
        memcpy(before, f->rgba, f->n * sizeof(u32));
        reseed_empty(entries, count, cost, counts, f);
        changed |= memcmp(before, f->rgba, f->n * sizeof(u32)) != 0;
        if (!changed) break;
    }
    finder_build(f);
}

// ---- remapeo ----------------------------------------------------------------------------------------

typedef struct RemapJob {
    Image *const *frames;
    const Finder *finder;
    DitherMode    dither;
    float         level;
    float         amplitude;       // dithering ordenado
    double       *sse;             // por cuadro
    u64          *samples;
} RemapJob;

static const u8 BAYER8[BAYER][BAYER] = {
    { 0, 32, 8, 40, 2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
    { 12, 44, 4, 36, 14, 46, 6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
    { 3, 35, 11, 43, 1, 33, 9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
    { 15, 47, 7, 39, 13, 45, 5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

INLINE double pixel_sse(u32 from, u32 to) {
    double s = 0;
    u32 fa = from >> 24, ta = to >> 24;
    s += ((double)fa - ta) * ((double)fa - ta);
    if (fa || ta)
        for (int c = 0; c < 3; c++) { double d = (double)((from >> (8 * c)) & 255) - ((to >> (8 * c)) & 255); s += d * d; }
    return s;
}

// Sin dithering u ordenado: cada pixel es independiente (paralelo por filas), con un cache
// directo de 4096 lugares por hilo (los graficos repiten colores).
typedef struct RowJob {
    RemapJob *remap;
    Image    *image;
    double    sse;
    SRWLOCK   lock;
} RowJob;

static void remap_rows(void *arg, size_t y0, size_t y1) {
    RowJob *rj = arg;
    const RemapJob *j = rj->remap;
    Image *im = rj->image;
    u32 cache_key[1 << CACHE_BITS];
    u16 cache_val[1 << CACHE_BITS];
    memset(cache_key, 0xFF, sizeof cache_key);
    double sse = 0;
    for (size_t y = y0; y < y1; y++) {
        u32 *row = (u32 *)image_row(im, (u32)y);
        for (u32 x = 0; x < im->width; x++) {
            u32 px = row[x], key = normalize_key(px), idx;
            if (j->dither == DITHER_ORDERED && (px >> 24)) {
                float off = (BAYER8[y % BAYER][x % BAYER] / 64.0f - 0.5f) * j->amplitude;
                float a = (px >> 24) / 255.0f;
                Vec4 q = vec_straight(saturatef((px & 255) / 255.0f + off), saturatef(((px >> 8) & 255) / 255.0f + off),
                                      saturatef(((px >> 16) & 255) / 255.0f + off), a);
                idx = finder_nearest(j->finder, &q);
            } else if (key == TRANSPARENT_KEY && j->finder->transparent >= 0) {
                idx = (u32)j->finder->transparent;
            } else {
                u32 slot = (u32)((key * 0x9E3779B1u) >> (32 - CACHE_BITS));
                if (cache_key[slot] == key) idx = cache_val[slot];
                else {
                    Vec4 q = vec_of(key);
                    idx = finder_nearest(j->finder, &q);
                    cache_key[slot] = key;
                    cache_val[slot] = (u16)idx;
                }
            }
            u32 out = j->finder->rgba[idx];
            sse += pixel_sse(px, out);
            row[x] = out;
        }
    }
    AcquireSRWLockExclusive(&rj->lock);
    rj->sse += sse;
    ReleaseSRWLockExclusive(&rj->lock);
}

// Floyd-Steinberg serpenteante, en color directo; el alfa no se difunde (se respeta el del pixel)
// y los pixeles 100 % transparentes ni reciben ni reparten error.
static double remap_floyd(Image *im, const Finder *f, float level) {
    u32 w = im->width;
    float *cur = mem_calloc((size_t)(w + 2) * 3 * sizeof(float)), *next = mem_calloc((size_t)(w + 2) * 3 * sizeof(float));
    double sse = 0;
    if (!cur || !next) { mem_free(cur); mem_free(next); return -1.0; }
    for (u32 y = 0; y < im->height; y++) {
        u32 *row = (u32 *)image_row(im, y);
        bool reverse = y & 1;
        memset(next, 0, (size_t)(w + 2) * 3 * sizeof(float));
        for (u32 k = 0; k < w; k++) {
            u32 x = reverse ? w - 1 - k : k;
            u32 px = row[x];
            u32 a8 = px >> 24;
            if (!a8 && f->transparent >= 0) { row[x] = 0; continue; }
            float *e = cur + 3 * (size_t)(x + 1);
            float c[3], a = a8 / 255.0f;
            for (int ch = 0; ch < 3; ch++) c[ch] = saturatef(((px >> (8 * ch)) & 255) / 255.0f + e[ch]);
            Vec4 q = vec_straight(c[0], c[1], c[2], a);
            u32 out = f->rgba[finder_nearest(f, &q)];
            sse += pixel_sse(px, out);
            row[x] = out;
            int dir = reverse ? -1 : 1;
            for (int ch = 0; ch < 3; ch++) {
                float err = clampf((c[ch] - ((out >> (8 * ch)) & 255) / 255.0f) * level * a, -ERROR_CLAMP, ERROR_CLAMP);
                cur[3 * (size_t)(x + 1 + dir) + ch] += err * (7.0f / 16.0f);
                next[3 * (size_t)(x + 1 - dir) + ch] += err * (3.0f / 16.0f);
                next[3 * (size_t)(x + 1) + ch] += err * (5.0f / 16.0f);
                next[3 * (size_t)(x + 1 + dir) + ch] += err * (1.0f / 16.0f);
            }
        }
        float *t = cur; cur = next; next = t;
    }
    mem_free(cur);
    mem_free(next);
    return sse;
}

static void remap_frame(void *arg, size_t i) {
    RemapJob *j = arg;
    Image *im = j->frames[i];
    j->samples[i] = (u64)im->width * im->height * 4;
    if (j->dither == DITHER_FS) {
        j->sse[i] = remap_floyd(im, j->finder, j->level);
        return;
    }
    RowJob rj = { j, im, 0.0, SRWLOCK_INIT };
    parallel_range(im->height, 32, remap_rows, &rj);
    j->sse[i] = rj.sse;
}

// ---- entrada ---------------------------------------------------------------------------------------

Status quantize_frames(Image *const *frames, u32 count, const QuantizeOptions *opt, QuantizeReport *rep, Diag *diag) {
    memset(rep, 0, sizeof *rep);
    u32 target = CLAMP(opt->colors, 2u, (u32)MAX_COLORS);
    for (u32 i = 0; i < count; i++) {
        Status s = image_set_depth(frames[i], 8, diag);
        if (s != ST_OK) return s;
    }
    // 1) Histograma. Si hay demasiados colores unicos se posteriza SOLO para armar la paleta
    //    (el remapeo final usa los pixeles exactos).
    Hist h = { 0 };
    u32 mask = 0xFFFFFFFFu;
    for (int posterize = 0; posterize <= 3; posterize++) {
        hist_free(&h);
        if (!hist_init(&h, 1 << 16)) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para el histograma");
        u8 m = (u8)(0xFF << posterize);
        mask = (u32)m | (u32)m << 8 | (u32)m << 16 | 0xFF000000u;
        bool overflow = false;
        for (u32 f = 0; f < count && !overflow; f++) {
            const Image *im = frames[f];
            u32 last = 0xFFFFFFFFu;
            u32 *last_slot = NULL;
            for (u32 y = 0; y < im->height && !overflow; y++) {
                const u32 *row = (const u32 *)image_row(im, y);
                for (u32 x = 0; x < im->width; x++) {
                    u32 key = normalize_key(row[x]) & mask;
                    if (key == last && last_slot) { (*last_slot)++; continue; }
                    u32 *v = hist_find(&h, key, true);
                    if (!v) { hist_free(&h); return diag_fail(diag, ST_NO_MEMORY, "sin memoria para el histograma"); }
                    (*v)++;
                    last = key;
                    last_slot = v;
                    // hist_find pudo crecer la tabla y mover los valores: el puntero se renueva
                    // en cada insercion nueva, asi que el cache sigue siendo valido.
                    if (h.size > HIST_LIMIT && posterize < 3) { overflow = true; break; }
                }
            }
        }
        if (!overflow) break;
    }
    rep->unique_in = h.size;

    // Sin perdida: ya entra en la paleta pedida (y no hubo que posterizar).
    if (h.size <= target && mask == 0xFFFFFFFFu) {
        rep->palette = (u32)h.size;
        rep->lossless = true;
        rep->psnr = INFINITY;
        hist_free(&h);
        return ST_OK;
    }

    // 2) Entradas (el transparente aparte: tiene su lugar reservado en la paleta).
    Entry *entries = mem_alloc(h.size * sizeof(Entry));
    if (!entries) { hist_free(&h); return diag_fail(diag, ST_NO_MEMORY, "sin memoria para cuantizar"); }
    u32 n = 0;
    bool has_transparent = false;
    for (size_t i = 0; i < h.cap; i++) {
        if (h.keys[i] == UINT64_MAX) continue;
        u32 key = (u32)h.keys[i];
        if (key == TRANSPARENT_KEY) { has_transparent = true; continue; }
        entries[n].rgba = key;
        entries[n].count = h.values[i];
        entries[n].v = vec_of(key);
        n++;
    }
    hist_free(&h);

    Finder *finder = mem_calloc(sizeof *finder);
    u16 *assign = mem_alloc(MAX(n, 1u) * sizeof(u16));
    float *cost = mem_alloc(MAX(n, 1u) * sizeof(float));
    if (!finder || !assign || !cost) {
        mem_free(entries); mem_free(finder); mem_free(assign); mem_free(cost);
        return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
    }
    u32 colors = target - (has_transparent ? 1 : 0);
    Vec4 means[MAX_COLORS];
    u32 made = n ? median_cut(entries, n, colors, means) : 0;
    for (u32 i = 0; i < made; i++) finder->rgba[finder->n++] = rgba_of(&means[i]);
    if (has_transparent) finder->rgba[finder->n++] = 0;
    // 3) Refinar con k-means (sobre las entradas, pesadas por cantidad).
    if (n) kmeans(entries, n, finder, assign, cost);
    else finder_build(finder);
    mem_free(entries);
    mem_free(assign);
    mem_free(cost);

    // 4) Remapear todos los cuadros (en paralelo entre cuadros).
    double *sse = mem_calloc(count * sizeof(double));
    u64 *samples = mem_calloc(count * sizeof(u64));
    if (!sse || !samples) { mem_free(sse); mem_free(samples); mem_free(finder); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }
    RemapJob job = { frames, finder, opt->dither, CLAMP(opt->level, 0.0f, 1.0f),
                     CLAMP(opt->level, 0.0f, 1.0f) / cbrtf((float)finder->n), sse, samples };
    parallel_for(count, remap_frame, &job);
    double total_sse = 0;
    u64 total_samples = 0;
    for (u32 i = 0; i < count; i++) { total_sse += sse[i] < 0 ? 0 : sse[i]; total_samples += samples[i]; }
    rep->palette = finder->n;
    rep->psnr = total_sse > 0 ? 10.0 * log10(255.0 * 255.0 / (total_sse / (double)total_samples)) : INFINITY;
    mem_free(sse);
    mem_free(samples);
    mem_free(finder);
    return ST_OK;
}
