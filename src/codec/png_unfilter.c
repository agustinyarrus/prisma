// codec/png_unfilter.c - ver png_unfilter.h
#include "png_unfilter.h"
#include <immintrin.h>
#include <stdlib.h>

enum { FILTER_NONE = 0, FILTER_SUB = 1, FILTER_UP = 2, FILTER_AVG = 3, FILTER_PAETH = 4 };

// ---- escalares (bpp 1 y 2, y colas) ---------------------------------------------------------
// Desde el byte `from`: los caminos SSE resuelven pixeles enteros y, si la fila no es multiplo del
// pixel (nunca en un PNG valido, pero la funcion no lo asume), terminan la cola aca. Dos lazos
// para que el caso comun no pague un if por byte: el primer pixel no tiene vecino izquierdo.

INLINE u8 paeth_predict(int a, int b, int c) {
    int pa = abs(b - c), pb = abs(a - c), pc = abs(a + b - 2 * c);
    return (u8)(pa <= pb && pa <= pc ? a : pb <= pc ? b : c);
}

static void sub_scalar(u8 *row, size_t from, size_t n, u32 bpp) {
    for (size_t i = MAX(from, (size_t)bpp); i < n; i++) row[i] = (u8)(row[i] + row[i - bpp]);
}

static void avg_scalar(u8 *row, const u8 *prev, size_t from, size_t n, u32 bpp) {
    for (size_t i = from; i < bpp && i < n; i++) row[i] = (u8)(row[i] + (prev[i] >> 1));
    for (size_t i = MAX(from, (size_t)bpp); i < n; i++) row[i] = (u8)(row[i] + ((row[i - bpp] + prev[i]) >> 1));
}

static void paeth_scalar(u8 *row, const u8 *prev, size_t from, size_t n, u32 bpp) {
    for (size_t i = from; i < bpp && i < n; i++) row[i] = (u8)(row[i] + prev[i]);
    for (size_t i = MAX(from, (size_t)bpp); i < n; i++) row[i] = (u8)(row[i] + paeth_predict(row[i - bpp], prev[i], prev[i - bpp]));
}

// Up no tiene dependencias: el compilador lo vectoriza solo (32 bytes por instruccion en AVX2).
static void up_rows(u8 *restrict row, const u8 *restrict prev, size_t n) {
    for (size_t i = 0; i < n; i++) row[i] = (u8)(row[i] + prev[i]);
}

// ---- SSE: un pixel por iteracion, canales en paralelo ---------------------------------------
// Carga/guarda exactamente BPP bytes (3, 4, 6 u 8) sin tocar el byte de filtro de la fila que
// sigue; memcpy de tamano constante se compila a un par de movs.

INLINE __m128i load_px(const u8 *p, const u32 bpp) {
    u64 v = 0;
    memcpy(&v, p, bpp);
    return _mm_unpacklo_epi8(_mm_cvtsi64_si128((long long)v), _mm_setzero_si128());
}

INLINE void store_px(u8 *p, __m128i px16, const u32 bpp) {
    u64 v = (u64)_mm_cvtsi128_si64(_mm_packus_epi16(px16, px16));
    memcpy(p, &v, bpp);
}

INLINE size_t whole_pixels(size_t n, u32 bpp) { return n - n % bpp; }

INLINE void sub_sse(u8 *row, size_t n, const u32 bpp) {
    __m128i a = _mm_setzero_si128();
    const __m128i lowbyte = _mm_set1_epi16(0xFF);
    size_t end = whole_pixels(n, bpp);
    for (size_t i = 0; i < end; i += bpp) {
        a = _mm_and_si128(_mm_add_epi16(load_px(row + i, bpp), a), lowbyte);
        store_px(row + i, a, bpp);
    }
    sub_scalar(row, end, n, bpp);
}

INLINE void avg_sse(u8 *row, const u8 *prev, size_t n, const u32 bpp) {
    __m128i a = _mm_setzero_si128();
    const __m128i lowbyte = _mm_set1_epi16(0xFF);
    size_t end = whole_pixels(n, bpp);
    for (size_t i = 0; i < end; i += bpp) {
        __m128i b = load_px(prev + i, bpp);
        __m128i avg = _mm_srli_epi16(_mm_add_epi16(a, b), 1);
        a = _mm_and_si128(_mm_add_epi16(load_px(row + i, bpp), avg), lowbyte);
        store_px(row + i, a, bpp);
    }
    avg_scalar(row, prev, end, n, bpp);
}

INLINE void paeth_sse(u8 *row, const u8 *prev, size_t n, const u32 bpp) {
    __m128i a = _mm_setzero_si128(), c = _mm_setzero_si128();
    const __m128i lowbyte = _mm_set1_epi16(0xFF);
    size_t end = whole_pixels(n, bpp);
    for (size_t i = 0; i < end; i += bpp) {
        __m128i b = load_px(prev + i, bpp);
        // pa = |b - c|, pb = |a - c|, pc = |a + b - 2c| = |(b - c) + (a - c)|
        __m128i bc = _mm_sub_epi16(b, c), ac = _mm_sub_epi16(a, c);
        __m128i pa = _mm_abs_epi16(bc), pb = _mm_abs_epi16(ac), pc = _mm_abs_epi16(_mm_add_epi16(bc, ac));
        __m128i best = _mm_min_epi16(_mm_min_epi16(pa, pb), pc);
        // Prioridad del estandar: a si pa es minimo, si no b si pb lo es, si no c.
        __m128i pick = _mm_blendv_epi8(c, b, _mm_cmpeq_epi16(best, pb));
        pick = _mm_blendv_epi8(pick, a, _mm_cmpeq_epi16(best, pa));
        a = _mm_and_si128(_mm_add_epi16(load_px(row + i, bpp), pick), lowbyte);
        store_px(row + i, a, bpp);
        c = b;
    }
    paeth_scalar(row, prev, end, n, bpp);
}

#define DISPATCH_BPP(fn, ...)                              \
    switch (bpp) {                                         \
    case 3: fn(__VA_ARGS__, 3); break;                     \
    case 4: fn(__VA_ARGS__, 4); break;                     \
    case 6: fn(__VA_ARGS__, 6); break;                     \
    case 8: fn(__VA_ARGS__, 8); break;                     \
    default: fn##_generic(__VA_ARGS__, bpp); break;        \
    }

#define sub_sse_generic(row, n, bpp)          sub_scalar(row, 0, n, bpp)
#define avg_sse_generic(row, prev, n, bpp)    avg_scalar(row, prev, 0, n, bpp)
#define paeth_sse_generic(row, prev, n, bpp)  paeth_scalar(row, prev, 0, n, bpp)

bool png_unfilter_row(u8 *row, const u8 *prev, size_t rowbytes, u32 bpp, u8 filter) {
    switch (filter) {
    case FILTER_NONE:  return true;
    case FILTER_SUB:   DISPATCH_BPP(sub_sse, row, rowbytes); return true;
    case FILTER_UP:    up_rows(row, prev, rowbytes); return true;
    case FILTER_AVG:   DISPATCH_BPP(avg_sse, row, prev, rowbytes); return true;
    case FILTER_PAETH: DISPATCH_BPP(paeth_sse, row, prev, rowbytes); return true;
    default:           return false;
    }
}
