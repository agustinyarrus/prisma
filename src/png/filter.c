// png/filter.c - ver filter.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdlib.h>
#include "filter.h"
#include "../core/cpu.h"
#include "../core/pool.h"
#include "../codec/zlib.h"

enum { ENTROPY_TABLE = 4096, BRUTE_CONTEXT_ROWS = 3, BRUTE_LEVEL = 2, FILTER_TYPES = 5 };

static const char *const NAMES[FS__COUNT] = {
    [FS_NONE] = "none", [FS_SUB] = "sub", [FS_UP] = "up", [FS_AVG] = "avg", [FS_PAETH] = "paeth",
    [FS_MINSUM] = "minsum", [FS_ENTROPY] = "entropy", [FS_BIGRAMS] = "bigrams", [FS_BRUTE] = "brute",
};

const char *filter_strategy_name(FilterStrategy s) { return (unsigned)s < FS__COUNT ? NAMES[s] : "?"; }

bool filter_strategy_parse(const char *name, FilterStrategy *out) {
    for (int i = 0; i < FS__COUNT; i++)
        if (strcmp(name, NAMES[i]) == 0) { *out = (FilterStrategy)i; return true; }
    return false;
}

// ---- nucleos ----------------------------------------------------------------------------------

INLINE u8 paeth_predict(u8 a, u8 b, u8 c) {
    i16 pa = (i16)abs((i16)b - (i16)c), pb = (i16)abs((i16)a - (i16)c), pc = (i16)abs((i16)a + (i16)b - 2 * (i16)c);
    return (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
}

// Escribe en `out` la fila filtrada con el tipo f. Solo lee cur/prev: el compilador la vectoriza
// entera (no hay dependencia con la salida, a diferencia de deshacer el filtro).
INLINE void apply_filter(u32 f, const u8 *restrict cur, const u8 *restrict prev, size_t n, u32 bpp, u8 *restrict out) {
    size_t head = MIN((size_t)bpp, n);
    switch (f) {
    case FS_NONE:
        memcpy(out, cur, n);
        break;
    case FS_SUB:
        for (size_t i = 0; i < head; i++) out[i] = cur[i];
        for (size_t i = bpp; i < n; i++) out[i] = (u8)(cur[i] - cur[i - bpp]);
        break;
    case FS_UP:
        for (size_t i = 0; i < n; i++) out[i] = (u8)(cur[i] - prev[i]);
        break;
    case FS_AVG:
        for (size_t i = 0; i < head; i++) out[i] = (u8)(cur[i] - (prev[i] >> 1));
        for (size_t i = bpp; i < n; i++) out[i] = (u8)(cur[i] - (u8)(((u32)cur[i - bpp] + prev[i]) >> 1));
        break;
    default:
        for (size_t i = 0; i < head; i++) out[i] = (u8)(cur[i] - prev[i]);
        for (size_t i = bpp; i < n; i++) out[i] = (u8)(cur[i] - paeth_predict(cur[i - bpp], prev[i], prev[i - bpp]));
        break;
    }
}

// Suma de |byte como entero con signo|: 0 y 255 cuestan lo mismo (±1).
INLINE u64 score_minsum(const u8 *row, size_t n) {
    u64 sum = 0;
    for (size_t i = 0; i < n; i++) { u32 v = row[i]; sum += v < 128 ? v : 256 - v; }
    return sum;
}

static float g_clog2c[ENTROPY_TABLE];            // c * log2(c)
static INIT_ONCE g_entropy_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK entropy_table_init(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    UNUSED(once); UNUSED(param); UNUSED(ctx);
    for (int c = 1; c < ENTROPY_TABLE; c++) g_clog2c[c] = (float)c * log2f((float)c);
    return TRUE;
}

// Minimizar la entropia de la fila equivale a maximizar sum(c log2 c): se devuelve el negativo.
static double score_entropy(const u8 *row, size_t n) {
    u32 counts[256] = { 0 };
    for (size_t i = 0; i < n; i++) counts[row[i]]++;
    double s = 0.0;
    for (int b = 0; b < 256; b++) {
        u32 c = counts[b];
        s += c < ENTROPY_TABLE ? g_clog2c[c] : (double)c * log2((double)c);
    }
    return -s;
}

// Pares de bytes distintos (bitset de 64K): menos pares = mas patrones que deflate encuentra.
static u32 score_bigrams(const u8 *row, size_t n, u64 *bitset) {
    memset(bitset, 0, 65536 / 8);
    u32 distinct = 0;
    for (size_t i = 1; i < n; i++) {
        u32 pair = (u32)row[i - 1] << 8 | row[i];
        u64 bit = 1ull << (pair & 63);
        if (!(bitset[pair >> 6] & bit)) { bitset[pair >> 6] |= bit; distinct++; }
    }
    return distinct;
}

// ---- banda de filas -------------------------------------------------------------------------

typedef struct FilterJob {
    const Packed  *packed;
    u32            bpp;
    FilterStrategy strategy;
    Filtered      *out;
    const u8      *zero_row;
    size_t         band_rows;
    u64           *usage;       // [bandas][FILTER_TYPES]
    volatile long  failed;
} FilterJob;

INLINE void filter_band_impl(FilterJob *job, size_t band) {
    const Packed *p = job->packed;
    size_t n = p->row_bytes, stride = n + 1;
    size_t y0 = band * job->band_rows, y1 = MIN(y0 + job->band_rows, (size_t)p->height);
    u64 *usage = job->usage + band * FILTER_TYPES;
    FilterStrategy s = job->strategy;

    if (s <= FS_PAETH) {                                         // filtro fijo
        for (size_t y = y0; y < y1; y++) {
            const u8 *cur = p->rows + y * n, *prev = y ? cur - n : job->zero_row;
            u8 *dst = job->out->data + y * stride;
            dst[0] = (u8)s;
            apply_filter((u32)s, cur, prev, n, job->bpp, dst + 1);
        }
        usage[s] += y1 - y0;
        return;
    }

    u8 *scratch = mem_alloc(FILTER_TYPES * n + 1);
    u64 *bigram_bits = s == FS_BIGRAMS ? mem_alloc(65536 / 8) : NULL;
    u8 *brute_buf = s == FS_BRUTE ? mem_alloc((BRUTE_CONTEXT_ROWS + 1) * stride) : NULL;
    if (!scratch || (s == FS_BIGRAMS && !bigram_bits) || (s == FS_BRUTE && !brute_buf)) {
        InterlockedExchange(&job->failed, 1);
        mem_free(scratch);
        mem_free(bigram_bits);
        mem_free(brute_buf);
        return;
    }
    for (size_t y = y0; y < y1; y++) {
        const u8 *cur = p->rows + y * n, *prev = y ? cur - n : job->zero_row;
        u8 *dst = job->out->data + y * stride;
        u32 best = 0;
        double best_score = INFINITY;
        for (u32 f = 0; f < FILTER_TYPES; f++) {
            u8 *cand = scratch + (size_t)f * n;
            apply_filter(f, cur, prev, n, job->bpp, cand);
            double score;
            if (s == FS_MINSUM) score = (double)score_minsum(cand, n);
            else if (s == FS_ENTROPY) score = score_entropy(cand, n);
            else if (s == FS_BIGRAMS) score = score_bigrams(cand, n, bigram_bits);
            else {
                // Brute: comprimir las ultimas filas ya decididas de la banda + la candidata.
                size_t ctx_rows = MIN((size_t)BRUTE_CONTEXT_ROWS, y - y0);
                memcpy(brute_buf, job->out->data + (y - ctx_rows) * stride, ctx_rows * stride);
                brute_buf[ctx_rows * stride] = (u8)f;
                memcpy(brute_buf + ctx_rows * stride + 1, cand, n);
                score = (double)zlib_quick_size(brute_buf, (ctx_rows + 1) * stride, BRUTE_LEVEL);
            }
            if (score < best_score) { best_score = score; best = f; }
        }
        dst[0] = (u8)best;
        memcpy(dst + 1, scratch + (size_t)best * n, n);
        usage[best]++;
    }
    mem_free(scratch);
    mem_free(bigram_bits);
    mem_free(brute_buf);
}

// La misma fuente compilada dos veces: base (SSE4) y AVX2 (vectores de 32 bytes).
static void filter_band_base(void *arg, size_t band) { filter_band_impl(arg, band); }
TARGET_AVX2 static void filter_band_avx2(void *arg, size_t band) { filter_band_impl(arg, band); }

Status filter_packed(const Packed *packed, u32 bpp, FilterStrategy strategy, Filtered *out, Diag *diag) {
    memset(out, 0, sizeof *out);
    InitOnceExecuteOnce(&g_entropy_once, entropy_table_init, NULL, NULL);
    size_t stride = packed->row_bytes + 1;
    if (!mul_ok(stride, packed->height, &out->size)) return diag_fail(diag, ST_TOO_LARGE, "imagen demasiado grande para filtrar");
    out->data = mem_alloc(out->size);
    u8 *zero_row = mem_calloc(packed->row_bytes + 1);
    // Bandas de ~256 KB: suficientemente grandes para amortizar la reserva de scratch, y bastantes
    // para repartir entre hilos.
    size_t band_rows = MAX((size_t)1, ((size_t)256 << 10) / stride);
    if (strategy == FS_BRUTE) band_rows = MAX((size_t)1, band_rows / 4);
    size_t bands = (packed->height + band_rows - 1) / band_rows;
    u64 *usage = mem_calloc(bands * FILTER_TYPES * sizeof(u64));
    if (!out->data || !zero_row || !usage) {
        mem_free(zero_row);
        mem_free(usage);
        filtered_free(out);
        return diag_fail(diag, ST_NO_MEMORY, "sin memoria para filtrar");
    }
    FilterJob job = { packed, bpp, strategy, out, zero_row, band_rows, usage, 0 };
    parallel_for(bands, cpu_info()->avx2 ? filter_band_avx2 : filter_band_base, &job);
    for (size_t b = 0; b < bands; b++)
        for (int f = 0; f < FILTER_TYPES; f++) out->usage[f] += usage[b * FILTER_TYPES + f];
    mem_free(zero_row);
    mem_free(usage);
    if (job.failed) { filtered_free(out); return diag_fail(diag, ST_NO_MEMORY, "sin memoria para filtrar"); }
    return ST_OK;
}

void filtered_free(Filtered *f) {
    mem_free(f->data);
    memset(f, 0, sizeof *f);
}
