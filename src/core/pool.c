// core/pool.c - ver pool.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pool.h"
#include "mem.h"

enum { MAX_POOL_THREADS = 256, RANGES_PER_THREAD = 4 };

typedef struct Job {
    PoolTask    fn;
    void       *ctx;
    size_t      count;
    size_t      next_index;     // proximo indice sin reclamar   (bajo g_lock)
    size_t      finished;       // indices terminados            (bajo g_lock)
    struct Job *older;          // lista de trabajos activos: la cabeza es el mas nuevo
    struct Job *newer;
} Job;

static SRWLOCK            g_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE g_changed = CONDITION_VARIABLE_INIT;   // hay trabajo nuevo o algo termino
static Job               *g_newest;
static bool               g_shutdown;
static HANDLE             g_workers[MAX_POOL_THREADS];
static u32                g_worker_count;
static volatile long      g_cancel;

u32 pool_threads(void) { return g_worker_count + 1; }

void pool_request_cancel(void) { InterlockedExchange(&g_cancel, 1); }
bool pool_cancelled(void) { return InterlockedCompareExchange(&g_cancel, 0, 0) != 0; }

// Bajo lock. `limit` = el trabajo propio de quien busca (no mira trabajos mas viejos que el);
// NULL = cualquier trabajo (hilos ociosos).
static Job *find_claimable(const Job *limit) {
    for (Job *j = g_newest; j; j = j->older) {
        if (j->next_index < j->count) return j;
        if (j == limit) break;
    }
    return NULL;
}

// Bajo lock al entrar y al salir; suelta el lock mientras corre la tarea.
static void run_one(Job *job) {
    size_t index = job->next_index++;
    ReleaseSRWLockExclusive(&g_lock);
    job->fn(job->ctx, index);
    AcquireSRWLockExclusive(&g_lock);
    if (++job->finished == job->count) WakeAllConditionVariable(&g_changed);
}

static DWORD WINAPI worker_main(void *arg) {
    UNUSED(arg);
    AcquireSRWLockExclusive(&g_lock);
    for (;;) {
        Job *job = find_claimable(NULL);
        if (job) { run_one(job); continue; }
        if (g_shutdown) break;
        SleepConditionVariableSRW(&g_changed, &g_lock, INFINITE, 0);
    }
    ReleaseSRWLockExclusive(&g_lock);
    return 0;
}

void pool_start(u32 threads) {
    if (g_worker_count) return;
    threads = CLAMP(threads, 1u, (u32)MAX_POOL_THREADS);
    for (u32 i = 0; i + 1 < threads; i++) {
        // Pila de 4 MB: los decodificadores recursivos (ICO con PNG adentro) y el helping anidado
        // apilan marcos; el default de 1 MB queda justo con imagenes patologicas.
        HANDLE h = CreateThread(NULL, 4u << 20, worker_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (!h) break;
        g_workers[g_worker_count++] = h;
    }
}

void pool_stop(void) {
    AcquireSRWLockExclusive(&g_lock);
    g_shutdown = true;
    WakeAllConditionVariable(&g_changed);
    ReleaseSRWLockExclusive(&g_lock);
    for (u32 i = 0; i < g_worker_count; i++) {
        WaitForSingleObject(g_workers[i], INFINITE);
        CloseHandle(g_workers[i]);
    }
    g_worker_count = 0;
    g_shutdown = false;
}

void parallel_for(size_t count, PoolTask fn, void *ctx) {
    if (count == 0) return;
    if (count == 1 || g_worker_count == 0) {
        for (size_t i = 0; i < count; i++) fn(ctx, i);
        return;
    }
    Job job = { .fn = fn, .ctx = ctx, .count = count };
    AcquireSRWLockExclusive(&g_lock);
    job.older = g_newest;
    if (g_newest) g_newest->newer = &job;
    g_newest = &job;
    WakeAllConditionVariable(&g_changed);
    while (job.finished < job.count) {
        Job *claim = job.next_index < job.count ? &job : find_claimable(&job);
        if (claim) run_one(claim);
        else SleepConditionVariableSRW(&g_changed, &g_lock, INFINITE, 0);
    }
    // Sacar el trabajo de la lista (puede no ser la cabeza: otros hilos apilaron despues).
    if (job.newer) job.newer->older = job.older; else g_newest = job.older;
    if (job.older) job.older->newer = job.newer;
    ReleaseSRWLockExclusive(&g_lock);
}

typedef struct RangeCtx {
    PoolRange fn;
    void     *ctx;
    size_t    total;
    size_t    pieces;
} RangeCtx;

static void range_task(void *arg, size_t index) {
    const RangeCtx *r = arg;
    // Reparto parejo: los primeros (total % pieces) tramos llevan un elemento de mas.
    size_t base = r->total / r->pieces, extra = r->total % r->pieces;
    size_t begin = index * base + MIN(index, extra);
    size_t end = begin + base + (index < extra ? 1 : 0);
    if (begin < end) r->fn(r->ctx, begin, end);
}

void parallel_range(size_t total, size_t grain, PoolRange fn, void *ctx) {
    if (total == 0) return;
    if (grain == 0) grain = 1;
    size_t max_pieces = (size_t)pool_threads() * RANGES_PER_THREAD;
    size_t pieces = MIN((total + grain - 1) / grain, max_pieces);
    if (pieces <= 1) { fn(ctx, 0, total); return; }
    RangeCtx r = { fn, ctx, total, pieces };
    parallel_for(pieces, range_task, &r);
}
