// ops/propagate.c - ver propagate.h
#include "propagate.h"
#include "../core/pool.h"

enum { STATE_TARGET = 0, STATE_RESOLVED = 1, STATE_IGNORED = 2, STATE_QUEUED = 3 };

static const int DX[8] = { -1, 0, 1, -1, 1, -1, 0, 1 };
static const int DY[8] = { -1, -1, -1, 0, 0, 1, 1, 1 };

INLINE u32 alpha_at(const Image *im, size_t i) {
    return im->depth == 8 ? im->pixels[4 * i + 3] : ((const u16 *)im->pixels)[4 * i + 3];
}

typedef struct LayerJob {
    Image      *image;
    const u8   *state;
    const u32  *frontier;
    u16        *colors;          // 3 por pixel de la frontera (escala de 16 bits)
} LayerJob;

static void resolve_layer(void *arg, size_t begin, size_t end) {
    const LayerJob *j = arg;
    const Image *im = j->image;
    i64 w = im->width, h = im->height;
    for (size_t k = begin; k < end; k++) {
        u32 i = j->frontier[k];
        i64 x = i % w, y = i / w;
        u64 sum[3] = { 0, 0, 0 };
        u32 n = 0;
        for (int d = 0; d < 8; d++) {
            i64 nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            size_t ni = (size_t)(ny * w + nx);
            if (j->state[ni] != STATE_RESOLVED) continue;
            if (im->depth == 8) { const u8 *p = im->pixels + 4 * ni; for (int c = 0; c < 3; c++) sum[c] += p[c] * 257u; }
            else { const u16 *p = (const u16 *)im->pixels + 4 * ni; for (int c = 0; c < 3; c++) sum[c] += p[c]; }
            n++;
        }
        for (int c = 0; c < 3; c++) j->colors[3 * k + c] = n ? (u16)((sum[c] + n / 2) / n) : 0;
    }
}

Status propagate_colors(Image *im, PropagateMode mode, u32 max_distance, u64 *touched, Diag *diag) {
    *touched = 0;
    size_t n = (size_t)im->width * im->height;
    u32 max_alpha = im->depth == 8 ? 255 : 65535;
    u8 *state = mem_alloc(n);
    u32 *frontier = mem_alloc(n * sizeof(u32)), *next = mem_alloc(n * sizeof(u32));
    if (!state || !frontier || !next) {
        mem_free(state); mem_free(frontier); mem_free(next);
        return diag_fail(diag, ST_NO_MEMORY, "sin memoria para propagar colores");
    }
    size_t sources = 0, targets = 0;
    for (size_t i = 0; i < n; i++) {
        u32 a = alpha_at(im, i);
        bool source = mode == PROPAGATE_BLEED ? a > 0 : a == max_alpha;
        bool target = mode == PROPAGATE_BLEED ? a == 0 : (a > 0 && a < max_alpha);
        state[i] = source ? STATE_RESOLVED : target ? STATE_TARGET : STATE_IGNORED;
        sources += source;
        targets += target;
    }
    Status status = ST_OK;
    if (!sources || !targets) goto done;

    // Capa 1: objetivos con algun vecino fuente.
    i64 w = im->width, h = im->height;
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        if (state[i] != STATE_TARGET) continue;
        i64 x = (i64)(i % (size_t)w), y = (i64)(i / (size_t)w);
        for (int d = 0; d < 8; d++) {
            i64 nx = x + DX[d], ny = y + DY[d];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h || state[ny * w + nx] != STATE_RESOLVED) continue;
            state[i] = STATE_QUEUED;
            frontier[count++] = (u32)i;
            break;
        }
    }
    u16 *colors = mem_alloc(MAX(count, (size_t)1) * 3 * sizeof(u16));
    size_t colors_cap = MAX(count, (size_t)1);
    for (u32 layer = 1; count && layer <= max_distance; layer++) {
        if (pool_cancelled()) { status = diag_fail(diag, ST_CANCELLED, "cancelado"); break; }
        if (count > colors_cap) {
            mem_free(colors);
            colors_cap = count;
            colors = mem_alloc(colors_cap * 3 * sizeof(u16));
        }
        if (!colors) { status = diag_fail(diag, ST_NO_MEMORY, "sin memoria para propagar colores"); break; }
        LayerJob job = { im, state, frontier, colors };
        parallel_range(count, 4096, resolve_layer, &job);
        for (size_t k = 0; k < count; k++) {
            size_t i = frontier[k];
            if (im->depth == 8) { u8 *p = im->pixels + 4 * i; for (int c = 0; c < 3; c++) p[c] = u16_to_u8(colors[3 * k + c]); }
            else { u16 *p = (u16 *)im->pixels + 4 * i; for (int c = 0; c < 3; c++) p[c] = colors[3 * k + c]; }
            state[i] = STATE_RESOLVED;
        }
        *touched += count;
        size_t next_count = 0;
        for (size_t k = 0; k < count; k++) {
            i64 x = frontier[k] % (u32)w, y = frontier[k] / (u32)w;
            for (int d = 0; d < 8; d++) {
                i64 nx = x + DX[d], ny = y + DY[d];
                if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                size_t ni = (size_t)(ny * w + nx);
                if (state[ni] != STATE_TARGET) continue;
                state[ni] = STATE_QUEUED;
                next[next_count++] = (u32)ni;
            }
        }
        u32 *t = frontier; frontier = next; next = t;
        count = next_count;
    }
    mem_free(colors);
done:
    mem_free(state);
    mem_free(frontier);
    mem_free(next);
    return status;
}
