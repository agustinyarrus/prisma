// tools/image2png/main.c - image2png: cualquier imagen a PNG, con la transparencia bien tratada.
//
// Orquesta: opciones -> entradas -> nombres de salida -> conversion en paralelo -> resumen.
// El lote se reparte en el pool por orden de TAMAÑO DECRECIENTE (planificacion LPT: los archivos
// grandes arrancan primero y los chicos rellenan los huecos al final, asi ningun hilo queda solo
// con el archivo gigante mientras los demas esperan).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "job.h"
#include "report.h"
#include "../../app/ui.h"
#include "../../codec/wic.h"
#include "../../core/cpu.h"
#include "../../core/fileio.h"
#include "../../core/pool.h"
#include "../../core/text.h"

enum { EXIT_OK = 0, EXIT_SOME_FAILED = 1, EXIT_USAGE = 2, EXIT_CANCELLED = 130 };
static const double PROGRESS_FRAME = 1.0 / 30.0;

static double now(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static BOOL WINAPI on_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT) {
        pool_request_cancel();
        return TRUE;            // se termina ordenado: sin archivos a medias (la escritura es atomica)
    }
    return FALSE;
}

// ---- extensiones de imagen conocidas (propias + WIC) ------------------------------------------------

typedef struct ExtList { Vec items; } ExtList;          // char*

static void ext_add(ExtList *l, const char *ext, size_t len) {
    while (len && *ext == '.') { ext++; len--; }
    if (!len || len > 12) return;
    char tmp[16];
    memcpy(tmp, ext, len);
    tmp[len] = 0;
    str_lower_ascii(tmp);
    for (size_t i = 0; i < l->items.count; i++) if (strcmp(VEC_AT(&l->items, char *, i), tmp) == 0) return;
    char **slot = vec_push(&l->items);
    if (slot) *slot = mem_strdup(tmp);
}

static void ext_add_list(ExtList *l, const char *list, char sep) {
    for (const char *p = list; *p;) {
        while (*p == sep || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != sep && *e != ' ') e++;
        if (e > p) ext_add(l, p, (size_t)(e - p));
        p = e;
    }
}

static void wic_ext_visitor(void *ctx, const char *name, const char *exts) {
    UNUSED(name);
    ext_add_list(ctx, exts, ',');
}

static void collect_extensions(ExtList *l) {
    vec_init(&l->items, sizeof(char *));
    size_t count;
    const Codec *const *codecs = codec_builtin_list(&count);
    for (size_t i = 0; i < count; i++) ext_add_list(l, codecs[i]->extensions, ' ');
    wic_enumerate_decoders(wic_ext_visitor, l);
}

static void ext_free(ExtList *l) {
    for (size_t i = 0; i < l->items.count; i++) mem_free(VEC_AT(&l->items, char *, i));
    vec_free(&l->items);
}

// ---- mascara (se carga una sola vez, en el borde) --------------------------------------------------

static bool load_mask(Config *cfg) {
    wchar_t *path = utf8_to_utf16(cfg->mask_path);
    FileData file;
    Diag d = { 0 };
    bool ok = path && file_read_all(path, &file, &d) == ST_OK;
    mem_free(path);
    if (!ok) { report_error("no se pudo leer la mascara", d.message); return false; }
    DecodeOptions opt = cfg->decode;
    opt.single_frame = true;
    opt.frame_mode = FRAMES_DEFAULT;
    DecodeContext ctx = { file.data, file.size, "", &opt, &d, 0 };
    Decoded doc;
    ok = decode_image(&ctx, &doc) == ST_OK && decoded_frame_count(&doc);
    file_data_free(&file);
    if (ok) {
        cfg->mask = decoded_frame(&doc, 0)->image;
        decoded_frame(&doc, 0)->image = (Image){ 0 };
    } else {
        report_error("no se pudo leer la mascara", d.message);
    }
    decoded_free(&doc);
    return ok;
}

// ---- ejecucion del lote -------------------------------------------------------------------------------

typedef struct RunState {
    Job           *jobs;
    size_t         count;
    const u32     *order;
    const Config  *cfg;
    SRWLOCK        lock;
    u64            done, bytes_done, bytes_total;
    double         start, last_draw;
} RunState;

static void run_task(void *arg, size_t k) {
    RunState *r = arg;
    Job *job = &r->jobs[r->order[k]];
    convert_job(job, r->cfg);
    report_job(r->cfg, job);
    AcquireSRWLockExclusive(&r->lock);
    r->done++;
    r->bytes_done += job->in->size;
    double t = now();
    if (t - r->last_draw >= PROGRESS_FRAME || r->done == r->count) {
        r->last_draw = t;
        if (!r->cfg->quiet && !r->cfg->json) report_progress(r->done, r->count, r->bytes_done, r->bytes_total, t - r->start);
    }
    ReleaseSRWLockExclusive(&r->lock);
}

static const Job *g_sort_jobs;

static int by_size_desc(const void *a, const void *b) {
    u64 x = g_sort_jobs[*(const u32 *)a].in->size, y = g_sort_jobs[*(const u32 *)b].in->size;
    return x > y ? -1 : x < y;
}

static void short_usage(void) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "\n  ");
    buf_bold(&b);
    buf_inked(&b, UI_ACCENT, "image2png");
    buf_inked(&b, UI_DIM, " %s · cualquier imagen a PNG, experto en transparencias\n\n", PRISMA_VERSION);
    buf_inked(&b, UI_SUBTLE, "  uso  ");
    buf_puts(&b, "image2png [opciones] <entrada...>\n\n");
    buf_inked(&b, UI_DIM, "       image2png -h           todas las opciones\n");
    buf_inked(&b, UI_DIM, "       image2png --examples   recetas de uso\n");
    buf_inked(&b, UI_DIM, "       image2png --formats    que formatos puede leer\n\n");
    term_emit(&b);
    buf_free(&b);
}

static bool finalize_config(Config *cfg) {
    cfg->decode.max_pixels = (u64)cfg->max_megapixels * 1000000ull;
    if (cfg->pipeline.extract_alpha && cfg->pipeline.alpha == ALPHA_REMOVE) {
        report_error("opciones que se contradicen", "--extract-alpha guarda el alfa como imagen; --flatten lo elimina");
        return false;
    }
    if (cfg->pipeline.colors && cfg->encode.depth == 16) {
        report_error("opciones que se contradicen", "--colors produce una paleta de 8 bits: no combina con --depth 16");
        return false;
    }
    if (cfg->decode.force_wic && cfg->decode.no_wic) {
        report_error("opciones que se contradicen", "--wic y --no-wic");
        return false;
    }
    return true;
}

int wmain(int argc, wchar_t **argv) {
    Config cfg;
    config_defaults(&cfg);
    CliResult cli;
    bool parsed = cli_parse(argc, argv, IMAGE2PNG_OPTIONS, IMAGE2PNG_OPTION_COUNT, &cfg, &cli);
    term_init(cfg.plain ? TERM_COLOR_NEVER : TERM_COLOR_AUTO);
    int exit_code = EXIT_OK;
    Vec files;
    vec_init(&files, sizeof(InputFile));
    Job *jobs = NULL;
    u32 *order = NULL;
    ExtList exts = { 0 };
    bool pool_running = false;

    if (!parsed) { report_error("no entendi los argumentos", cli.error); exit_code = EXIT_USAGE; goto done; }
    if (cfg.help) { print_help(); goto done; }
    if (cfg.examples) { print_examples(); goto done; }
    if (cfg.version) { report_version(); goto done; }
    if (cfg.formats) { report_formats(); goto done; }
    if (!cli.positionals.count) { short_usage(); exit_code = EXIT_USAGE; goto done; }
    if (!finalize_config(&cfg)) { exit_code = EXIT_USAGE; goto done; }
    if (cfg.mask_path && !load_mask(&cfg)) { exit_code = EXIT_USAGE; goto done; }

    u32 threads = cfg.threads ? cfg.threads : cpu_info()->logical_cores;
    pool_start(threads);
    pool_running = true;
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    collect_extensions(&exts);
    InputOptions iopt = { cfg.recursive, exts.items.items, exts.items.count };
    Diag expand_diag = { 0 };
    if (inputs_expand(&cli.positionals, &iopt, &files, &expand_diag) != ST_OK) {
        report_error("no se pudieron leer las entradas", expand_diag.message);
        exit_code = EXIT_USAGE;
        goto done;
    }
    for (u32 i = 0; i < MIN(expand_diag.warning_count, (u32)DIAG_MAX_WARNINGS); i++) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "  ");
        buf_inked(&b, UI_WARN, "⚠ %s\n", expand_diag.warnings[i]);
        term_emit(&b);
        buf_free(&b);
    }
    if (!files.count) { report_error("no hay nada para convertir", "ninguna entrada era una imagen legible"); exit_code = EXIT_USAGE; goto done; }

    size_t n = files.count;
    jobs = mem_calloc(n * sizeof *jobs);
    order = mem_alloc(n * sizeof *order);
    if (!jobs || !order) { report_error("sin memoria", "no alcanza para planificar el lote"); exit_code = EXIT_USAGE; goto done; }
    u64 total_bytes = 0;
    for (size_t i = 0; i < n; i++) {
        jobs[i].in = &VEC_AT(&files, InputFile, i);
        order[i] = (u32)i;
        total_bytes += jobs[i].in->size;
    }

    if (cfg.info) {
        // Informe: en el orden en que se dieron (el analisis de cada uno ya es paralelo por dentro).
        for (size_t i = 0; i < n && !pool_cancelled(); i++) if (report_info(&cfg, &jobs[i]) != ST_OK) exit_code = EXIT_SOME_FAILED;
        if (!cfg.json) term_emit_text("\n");
        goto done;
    }

    Diag plan_diag = { 0 };
    if (plan_outputs(jobs, n, &cfg, &plan_diag) != ST_OK) { report_error("salida invalida", plan_diag.message); exit_code = EXIT_USAGE; goto done; }

    report_header(&cfg, n, total_bytes, threads);
    g_sort_jobs = jobs;
    qsort(order, n, sizeof *order, by_size_desc);
    RunState run = { jobs, n, order, &cfg, SRWLOCK_INIT, 0, 0, total_bytes, now(), 0 };
    parallel_for(n, run_task, &run);
    report_summary(&cfg, jobs, n, now() - run.start, threads);

    bool any_failed = false;
    for (size_t i = 0; i < n; i++) any_failed |= jobs[i].status != ST_OK && jobs[i].status != ST_SKIPPED;
    exit_code = pool_cancelled() ? EXIT_CANCELLED : any_failed ? EXIT_SOME_FAILED : EXIT_OK;

done:
    if (jobs) for (size_t i = 0; i < files.count; i++) { mem_free(jobs[i].output); mem_free(jobs[i].output_display); }
    mem_free(jobs);
    mem_free(order);
    inputs_free(&files);
    if (exts.items.item_size) ext_free(&exts);
    cli_result_free(&cli);
    config_free(&cfg);
    if (pool_running) pool_stop();
    term_shutdown();
    return exit_code;
}
