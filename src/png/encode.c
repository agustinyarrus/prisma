// png/encode.c - ver encode.h
#include <stdio.h>
#include <stdlib.h>
#include "encode.h"
#include "../codec/zlib.h"
#include "../core/icc.h"
#include "../core/pool.h"

// ---- perfiles por nivel de optimizacion --------------------------------------------------------

typedef struct LevelProfile {
    int  level;            // nivel final de deflate
    int  estimate_level;   // nivel para ordenar estrategias (0 = no hay pruebas)
    u32  plans;            // planes de color a probar
    u32  finalists;        // estrategias que llegan a la compresion final
    bool parallel;         // deflate por tramos (se apaga en -O4: un flujo da la mejor razon)
} LevelProfile;

static const LevelProfile PROFILES[OPTIMIZE_MAX + 1] = {
    { 1, 0, 1, 1, true },
    { 6, 0, 1, 1, true },
    { 9, 0, 1, 1, true },
    { 11, 5, 2, 2, true },
    { 12, 9, 4, 2, false },
};

// Si los buffers filtrados de todas las estrategias entran en este presupuesto, las pruebas
// corren en paralelo entre si; si no, una por vez (cada una ya paraleliza por dentro).
#define TRIAL_MEMORY_BUDGET ((size_t)512 << 20)

static const FilterStrategy SET_O3[] = { FS_NONE, FS_PAETH, FS_MINSUM, FS_ENTROPY };
static const FilterStrategy SET_O3_LOW[] = { FS_NONE, FS_MINSUM, FS_ENTROPY };
static const FilterStrategy SET_O4[] = { FS_NONE, FS_SUB, FS_UP, FS_AVG, FS_PAETH, FS_MINSUM, FS_ENTROPY, FS_BIGRAMS };

static const u8 ADAM7[7][4] = {
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
    { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 },
};

void encode_options_default(EncodeOptions *o) {
    memset(o, 0, sizeof *o);
    o->optimize = OPTIMIZE_DEFAULT;
    o->level = -1;
    o->filter = FS__COUNT;
    o->color = COLOR_AUTO;
    o->reduce = true;
    o->parallel = true;
    o->meta = META_COLOR;
}

static const LevelProfile *profile_of(const EncodeOptions *o) {
    return &PROFILES[CLAMP(o->optimize, 0, OPTIMIZE_MAX)];
}

// ---- conjunto de filas empaquetadas (1 imagen o 7 pasadas de Adam7) ----------------------------

typedef struct PackedSet {
    Packed pass[7];
    u32    count;
} PackedSet;

static void packed_set_free(PackedSet *set) {
    for (u32 i = 0; i < set->count; i++) packed_free(&set->pass[i]);
    set->count = 0;
}

static Status pass_image(const Image *im, int p, Image *out, bool *empty, Diag *diag) {
    u32 w = im->width > ADAM7[p][0] ? (im->width - ADAM7[p][0] + ADAM7[p][2] - 1) / ADAM7[p][2] : 0;
    u32 h = im->height > ADAM7[p][1] ? (im->height - ADAM7[p][1] + ADAM7[p][3] - 1) / ADAM7[p][3] : 0;
    *empty = !w || !h;
    if (*empty) return ST_OK;
    Status s = image_alloc(out, w, h, im->depth, false, 0, diag);
    if (s != ST_OK) return s;
    u32 bpp = image_bytes_per_pixel(im);
    for (u32 y = 0; y < h; y++) {
        const u8 *src = image_row(im, ADAM7[p][1] + y * ADAM7[p][3]);
        u8 *dst = image_row(out, y);
        for (u32 x = 0; x < w; x++)
            memcpy(dst + (size_t)x * bpp, src + (size_t)(ADAM7[p][0] + x * ADAM7[p][2]) * bpp, bpp);
    }
    return ST_OK;
}

static Status pack_set(const Image *im, const PngPlan *plan, bool interlace, PackedSet *set, Diag *diag) {
    memset(set, 0, sizeof *set);
    if (!interlace) {
        Status s = pack_image(im, plan, &set->pass[0], diag);
        if (s == ST_OK) set->count = 1;
        return s;
    }
    for (int p = 0; p < 7; p++) {
        Image sub;
        bool empty;
        Status s = pass_image(im, p, &sub, &empty, diag);
        if (s != ST_OK) { packed_set_free(set); return s; }
        if (empty) continue;
        s = pack_image(&sub, plan, &set->pass[set->count], diag);
        image_free(&sub);
        if (s != ST_OK) { packed_set_free(set); return s; }
        set->count++;
    }
    return ST_OK;
}

static size_t packed_set_filtered_size(const PackedSet *set) {
    size_t total = 0;
    for (u32 i = 0; i < set->count; i++) total += (set->pass[i].row_bytes + 1) * set->pass[i].height;
    return total;
}

static Status filter_set(const PackedSet *set, u32 bpp, FilterStrategy s, Filtered *out, Diag *diag) {
    if (set->count == 1) return filter_packed(&set->pass[0], bpp, s, out, diag);
    memset(out, 0, sizeof *out);
    out->size = packed_set_filtered_size(set);
    out->data = mem_alloc(out->size ? out->size : 1);
    if (!out->data) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para filtrar");
    size_t at = 0;
    for (u32 i = 0; i < set->count; i++) {
        Filtered part;
        Status st = filter_packed(&set->pass[i], bpp, s, &part, diag);
        if (st != ST_OK) { filtered_free(out); return st; }
        memcpy(out->data + at, part.data, part.size);
        at += part.size;
        for (int f = 0; f < 5; f++) out->usage[f] += part.usage[f];
        filtered_free(&part);
    }
    return ST_OK;
}

// ---- datos de imagen: filtros + deflate, con pruebas ------------------------------------------

typedef struct DataOut {
    u8            *z;
    size_t         size;
    FilterStrategy strategy;
    int            level;                    // el nivel de deflate que de verdad se uso
    u64            usage[5];
    u32            trials;
} DataOut;

static void data_free(DataOut *d) { mem_free(d->z); memset(d, 0, sizeof *d); }

// Paleta o menos de 8 bits: filas de indices o bits empaquetados, donde "none" casi siempre gana.
static bool packed_indices(const PngPlan *plan) { return plan->color_type == PNG_CT_PALETTE || plan->bit_depth < 8; }

static u32 strategies_for(const EncodeOptions *o, const PngPlan *plan, FilterStrategy *out) {
    if (o->filter != FS__COUNT) { out[0] = o->filter; return 1; }
    bool low = packed_indices(plan);
    if (o->optimize <= 2) { out[0] = low ? FS_NONE : FS_MINSUM; return 1; }
    const FilterStrategy *set = o->optimize == 3 ? (low ? SET_O3_LOW : SET_O3) : SET_O4;
    u32 n = o->optimize == 3 ? (low ? (u32)ARRAY_LEN(SET_O3_LOW) : (u32)ARRAY_LEN(SET_O3)) : (u32)ARRAY_LEN(SET_O4);
    memcpy(out, set, n * sizeof *out);
    return n;
}

typedef struct Trial {
    FilterStrategy strategy;
    Filtered       filtered;
    size_t         estimate;
    Status         status;
} Trial;

typedef struct TrialJob {
    const PackedSet *set;
    u32              bpp;
    Trial           *trials;
    int              estimate_level;
    bool             inner_parallel;
} TrialJob;

static void trial_task(void *arg, size_t i) {
    TrialJob *job = arg;
    Trial *t = &job->trials[i];
    Diag local = { 0 };
    t->status = filter_set(job->set, job->bpp, t->strategy, &t->filtered, &local);
    if (t->status == ST_OK)
        t->estimate = zlib_compressed_size(t->filtered.data, t->filtered.size, job->estimate_level, job->inner_parallel);
    else
        t->estimate = SIZE_MAX;
}

static int trial_order(const void *a, const void *b) {
    size_t x = ((const Trial *)a)->estimate, y = ((const Trial *)b)->estimate;
    return x < y ? -1 : x > y;
}

static Status compress_filtered(const Filtered *f, FilterStrategy s, int level, bool parallel, DataOut *out, Diag *diag) {
    Status st = zlib_compress(f->data, f->size, level, parallel, &out->z, &out->size, diag);
    out->strategy = s;
    out->level = level;
    memcpy(out->usage, f->usage, sizeof out->usage);
    return st;
}

// ---- -O2: carrera de filtros sobre una muestra -------------------------------------------------
//
// Medido con deflate 9 sobre fotos, capturas, un logo y una imagen de 16 bits: ninguna estrategia
// gana siempre. minsum (la heuristica de libpng) es la mejor en fotos pero pierde 25 % en un logo
// de colores planos y 6 % en capturas; none gana en graficos planos y pierde 40-60 % en fotos;
// bigrams gana en capturas con texto y en 16 bits. Entonces: unas bandas repartidas por toda la
// imagen se filtran con cada candidata y se comprimen rapido; la ganadora filtra la imagen entera.
// Cuesta unos pocos por ciento del tiempo de -O2.

static const FilterStrategy RACE_O2[] = { FS_MINSUM, FS_NONE, FS_BIGRAMS };
// Recargo por costo: bigrams filtra bastante mas lento (en 16 bits, +35 % de tiempo total por 0,9 %
// de tamaño). En el nivel "equilibrado" tiene que ganar por mas de eso para que valga la pena.
static const double RACE_COST_PENALTY[ARRAY_LEN(RACE_O2)] = { 0.0, 0.0, 0.015 };
enum {
    RACE_BANDS = 16,                         // bandas de la muestra, repartidas parejas
    RACE_BAND_ROWS = 8,                      // filas por banda (los filtros miran la de arriba)
    RACE_ESTIMATE_LEVEL = 3,                 // deflate rapido: alcanza para ordenar
};
#define RACE_MAX_SAMPLE ((size_t)2 << 20)   // bytes de muestra como mucho (imagenes muy anchas)

typedef struct RaceJob {
    const Packed         *sample;
    u32                   bpp;
    const FilterStrategy *candidates;
    size_t               *estimate;
} RaceJob;

static void race_task(void *arg, size_t i) {
    RaceJob *job = arg;
    Filtered f = { 0 };
    Diag local = { 0 };
    job->estimate[i] = filter_packed(job->sample, job->bpp, job->candidates[i], &f, &local) == ST_OK
                     ? zlib_compressed_size(f.data, f.size, RACE_ESTIMATE_LEVEL, false) : SIZE_MAX;
    filtered_free(&f);
}

// 16 bits: el byte bajo de cada muestra suele ser ruido del sensor o del degrade, y ahi el
// emparejamiento perezoso de los niveles 2-12 de libdeflate gasta bits en coincidencias cortas que
// no pagan. Medido en un 16 bits ruidoso de 2048x1536: nivel 1 = 12,98 MB, niveles 2-9 = 13,5 MB,
// nivel 12 = 13,05 MB. En 8 bits pasa lo contrario (el 9 le saca 17-22 % al 1 en fotos), asi que
// solo se compara en 16 bits: la muestra ganadora a nivel 1 contra el nivel del perfil.
typedef struct RaceResult {
    FilterStrategy strategy;
    int            level;
} RaceResult;

typedef struct LevelJob {
    const Filtered *sample;
    const int      *levels;
    size_t         *estimate;
} LevelJob;

// Un cuarto de la muestra alcanza para decidir entre 1 y el nivel alto (la diferencia, cuando la
// hay, es de estructura y no de detalle), y el nivel alto sobre ruido es lento: medido, la
// comprobacion con la muestra entera costaba mas de lo que ahorraba comprimir a nivel 1.
#define LEVEL_SAMPLE_MIN ((size_t)64 << 10)

static void level_task(void *arg, size_t i) {
    LevelJob *job = arg;
    size_t n = MIN(job->sample->size, MAX(job->sample->size / 4, LEVEL_SAMPLE_MIN));
    job->estimate[i] = zlib_compressed_size(job->sample->data, n, job->levels[i], false);
}

static int race_level(const Packed *sample, u32 bpp, FilterStrategy winner, int level) {
    Filtered f = { 0 };
    Diag local = { 0 };
    if (filter_packed(sample, bpp, winner, &f, &local) != ST_OK) { filtered_free(&f); return level; }
    const int levels[2] = { 1, level };
    size_t estimate[2];
    LevelJob job = { &f, levels, estimate };
    parallel_for(2, level_task, &job);
    filtered_free(&f);
    return estimate[0] < estimate[1] ? 1 : level;
}

// La muestra: bandas contiguas de filas copiadas en orden. Con pocas filas, la imagen entera.
// O(filas de la muestra x ancho).
static RaceResult race_filters(const Packed *p, u32 bpp, int level, bool race_levels) {
    u32 band_rows = RACE_BAND_ROWS, bands = RACE_BANDS;
    while (bands > 2 && (size_t)bands * band_rows * p->row_bytes > RACE_MAX_SAMPLE) bands /= 2;
    Packed sample = *p;
    u8 *copy = NULL;
    if ((u64)bands * band_rows < p->height) {
        copy = mem_alloc((size_t)bands * band_rows * p->row_bytes);
        if (!copy) return (RaceResult){ RACE_O2[0], level };
        u32 stride = p->height / bands;
        for (u32 b = 0; b < bands; b++)
            memcpy(copy + (size_t)b * band_rows * p->row_bytes, p->rows + (size_t)b * stride * p->row_bytes,
                   (size_t)band_rows * p->row_bytes);
        sample.rows = copy;
        sample.height = bands * band_rows;
    }
    size_t estimate[ARRAY_LEN(RACE_O2)];
    RaceJob job = { &sample, bpp, RACE_O2, estimate };
    parallel_for(ARRAY_LEN(RACE_O2), race_task, &job);
    u32 best = 0;
    double best_score = (double)estimate[0] * (1.0 + RACE_COST_PENALTY[0]);
    for (u32 i = 1; i < ARRAY_LEN(RACE_O2); i++) {
        double score = (double)estimate[i] * (1.0 + RACE_COST_PENALTY[i]);
        if (score < best_score) { best = i; best_score = score; }
    }
    RaceResult result = { RACE_O2[best], race_levels && level > 1 ? race_level(&sample, bpp, RACE_O2[best], level) : level };
    mem_free(copy);
    return result;
}

static Status encode_data(const Image *im, const PngPlan *plan, const EncodeOptions *o, DataOut *out, Diag *diag) {
    memset(out, 0, sizeof *out);
    const LevelProfile *prof = profile_of(o);
    int level = o->level >= 0 ? o->level : prof->level;
    bool parallel = o->parallel && prof->parallel;
    PackedSet set;
    Status st = pack_set(im, plan, o->interlace, &set, diag);
    if (st != ST_OK) return st;
    FilterStrategy strategies[FS__COUNT];
    u32 n = strategies_for(o, plan, strategies);
    u32 bpp = plan_filter_bpp(plan);
    u32 raced = 0;
    if (o->optimize == 2 && o->filter == FS__COUNT && !packed_indices(plan) && set.count == 1) {
        // El nivel solo se compite en 16 bits y si el usuario no lo fijo con --level.
        bool race_levels = plan->bit_depth == 16 && o->level < 0;
        RaceResult race = race_filters(&set.pass[0], bpp, level, race_levels);
        strategies[0] = race.strategy;
        level = race.level;
        raced = ARRAY_LEN(RACE_O2) + (race_levels ? 2 : 0);
    }

    if (n == 1) {
        Filtered f;
        st = filter_set(&set, bpp, strategies[0], &f, diag);
        packed_set_free(&set);
        if (st != ST_OK) return st;
        st = compress_filtered(&f, strategies[0], level, parallel, out, diag);
        out->trials = MAX(raced, 1u);
        filtered_free(&f);
        return st;
    }

    Trial trials[FS__COUNT];
    memset(trials, 0, sizeof trials);
    for (u32 i = 0; i < n; i++) trials[i].strategy = strategies[i];
    size_t per_trial = packed_set_filtered_size(&set);
    bool fan_out = per_trial <= TRIAL_MEMORY_BUDGET / n;
    TrialJob job = { &set, bpp, trials, prof->estimate_level, !fan_out };
    if (fan_out) parallel_for(n, trial_task, &job);
    else for (u32 i = 0; i < n; i++) trial_task(&job, i);
    packed_set_free(&set);

    qsort(trials, n, sizeof trials[0], trial_order);
    u32 finalists = MIN(n, prof->finalists);
    for (u32 i = finalists; i < n; i++) filtered_free(&trials[i].filtered);
    if (trials[0].status != ST_OK) {
        for (u32 i = 0; i < finalists; i++) filtered_free(&trials[i].filtered);
        return diag_fail(diag, trials[0].status, "no se pudo filtrar la imagen");
    }
    st = ST_OK;
    for (u32 i = 0; i < finalists && st == ST_OK; i++) {
        if (trials[i].status != ST_OK) continue;
        DataOut cand;
        st = compress_filtered(&trials[i].filtered, trials[i].strategy, level, parallel, &cand, diag);
        if (st == ST_OK && (!out->z || cand.size < out->size)) { data_free(out); *out = cand; }
        else if (st == ST_OK) data_free(&cand);
    }
    for (u32 i = 0; i < finalists; i++) filtered_free(&trials[i].filtered);
    out->trials = n;
    return st;
}

// ---- reporte ------------------------------------------------------------------------------------

static void describe_filters(const DataOut *d, char *buf, size_t cap) {
    static const char *const NAMES[5] = { "none", "sub", "up", "avg", "paeth" };
    u64 total = 0;
    for (int f = 0; f < 5; f++) total += d->usage[f];
    if (d->strategy <= FS_PAETH || !total) { snprintf(buf, cap, "%s", filter_strategy_name(d->strategy)); return; }
    // Estrategia adaptativa: mostrar los dos filtros mas usados.
    int first = 0, second = -1;
    for (int f = 1; f < 5; f++) if (d->usage[f] > d->usage[first]) first = f;
    for (int f = 0; f < 5; f++) if (f != first && d->usage[f] && (second < 0 || d->usage[f] > d->usage[second])) second = f;
    int p1 = (int)(d->usage[first] * 100 / total);
    if (second >= 0)
        snprintf(buf, cap, "%s (%s %d %%, %s %d %%)", filter_strategy_name(d->strategy), NAMES[first], p1,
                 NAMES[second], (int)(d->usage[second] * 100 / total));
    else
        snprintf(buf, cap, "%s (%s %d %%)", filter_strategy_name(d->strategy), NAMES[first], p1);
}

static void fill_report(EncodeReport *r, const PngPlan *plan, const DataOut *d, const EncodeOptions *o, u32 frames_in, u32 frames_out,
                        const ColorStats *stats) {
    plan_describe(plan, r->format, sizeof r->format);
    describe_filters(d, r->filter, sizeof r->filter);
    r->level = d->level ? d->level : o->level >= 0 ? o->level : profile_of(o)->level;
    r->frames_in = frames_in;
    r->frames_out = frames_out;
    r->trials = d->trials;
    r->color_type = plan->color_type;
    r->bit_depth = plan->bit_depth;
    r->palette_size = plan->palette_size;
    r->interlaced = o->interlace;
    r->pixels = stats->pixels;
    r->transparent = stats->transparent;
    r->partial = stats->partial;
    r->colors = stats->colors;
    r->gray = stats->gray;
}

static size_t plan_overhead(const PngPlan *p) {
    if (p->color_type == PNG_CT_PALETTE) return 3 * (size_t)p->palette_size + p->trns_count + 24;
    return p->key ? 18 : 0;
}

// ---- perfil ICC vs tipo de color ------------------------------------------------------------------
//
// PNG exige que el perfil describa el tipo de color: RGB para color/paleta, GRAY para gris. Un
// perfil sRGB se escribe como chunk sRGB (valido en los dos). Si igual no encaja (gris forzado con
// perfil RGB, perfil CMYK de una conversion ingenua), se descarta con aviso: un PNG invalido es peor.

static bool icc_space_is(const Metadata *m, const char *space) {
    char sp[5];
    return m->icc.size && !icc_is_srgb(m->icc.data, m->icc.size) && icc_color_space(m->icc.data, m->icc.size, sp) && strcmp(sp, space) == 0;
}

static bool icc_fits(const Metadata *m, const PngPlan *p) {
    if (!m->icc.size || icc_is_srgb(m->icc.data, m->icc.size)) return true;
    bool gray = p->color_type == PNG_CT_GRAY || p->color_type == PNG_CT_GRAY_ALPHA;
    if (icc_space_is(m, "GRAY")) return gray;
    if (icc_space_is(m, "RGB ")) return !gray;
    return false;
}

// Metadatos a escribir: los mismos, o una copia superficial sin el perfil si no encaja.
static const Metadata *metadata_for(const Metadata *m, const PngPlan *p, Metadata *scratch, Diag *diag) {
    if (icc_fits(m, p)) return m;
    char sp[5] = "?";
    icc_color_space(m->icc.data, m->icc.size, sp);
    diag_warn(diag, "perfil ICC de espacio \"%s\" incompatible con PNG %s: se descarta",
              sp, p->color_type == PNG_CT_GRAY || p->color_type == PNG_CT_GRAY_ALPHA ? "gris" : "color");
    *scratch = *m;
    scratch->icc = (Blob){ 0 };
    scratch->icc_name[0] = 0;
    return scratch;
}

// ---- PNG estatico ----------------------------------------------------------------------------------

Status png_encode(const Image *image, const Metadata *meta, const EncodeOptions *o, Encoded *out, Diag *diag) {
    memset(out, 0, sizeof *out);
    ColorStats stats;
    analyze_image(image, &stats);
    PngPlan plans[4];
    u32 plan_count = 1;
    PlanRequest req = { o->color, o->depth, o->reduce, o->may_clean, false, !icc_space_is(meta, "RGB "), profile_of(o)->plans, &plan_count };
    Status st = plan_choose(&stats, &image, 1, &req, plans, diag);
    if (st != ST_OK) return st;

    DataOut best = { 0 };
    u32 best_plan = 0;
    size_t best_total = SIZE_MAX;
    u32 trials = 0;
    for (u32 i = 0; i < plan_count && st == ST_OK; i++) {
        DataOut d;
        st = encode_data(image, &plans[i], o, &d, diag);
        if (st != ST_OK) break;
        trials += d.trials;
        size_t total = d.size + plan_overhead(&plans[i]);
        if (total < best_total) { data_free(&best); best = d; best_plan = i; best_total = total; }
        else data_free(&d);
    }
    if (st != ST_OK) { data_free(&best); return st; }

    const PngPlan *plan = &plans[best_plan];
    ByteVec v = { 0 };
    png_write_signature(&v);
    png_write_ihdr(&v, image->width, image->height, plan, o->interlace);
    Metadata scratch;
    png_write_metadata(&v, metadata_for(meta, plan, &scratch, diag), o->orientation_applied);
    png_write_palette(&v, plan);
    png_write_data_chunks(&v, "IDAT", best.z, best.size);
    png_write_end(&v);
    best.trials = trials;
    fill_report(&out->report, plan, &best, o, 1, 1, &stats);
    data_free(&best);
    if (v.failed) { bv_free(&v); return diag_fail(diag, ST_NO_MEMORY, "sin memoria para armar el PNG"); }
    out->data = v.data;
    out->size = v.len;
    return ST_OK;
}

void encoded_free(Encoded *e) {
    mem_free(e->data);
    memset(e, 0, sizeof *e);
}

// =============================================================================================
// APNG
// =============================================================================================

enum { BLEND_SOURCE = 0, BLEND_OVER = 1 };

typedef struct OutFrame {
    u32     x, y, w, h;
    u32     delay_num, delay_den;
    u8      blend;
    Image   source_sub;        // region del lienzo nuevo (BLEND_SOURCE)
    Image   over_sub;          // igual, con lo que no cambio en transparente (BLEND_OVER)
    bool    has_over;
    DataOut data;
    Status  status;
    char    error[DIAG_MESSAGE_CAP];
} OutFrame;

// Rectangulo minimo que contiene los pixeles distintos entre dos cuadros. false si son iguales.
static bool diff_bbox(const Image *a, const Image *b, u32 *x0, u32 *y0, u32 *x1, u32 *y1) {
    u32 bpp = image_bytes_per_pixel(a);
    u32 min_x = a->width, max_x = 0, min_y = a->height, max_y = 0;
    for (u32 y = 0; y < a->height; y++) {
        const u8 *ra = image_row(a, y), *rb = image_row(b, y);
        if (memcmp(ra, rb, a->stride) == 0) continue;
        u32 l = 0, r = a->width - 1;
        while (memcmp(ra + (size_t)l * bpp, rb + (size_t)l * bpp, bpp) == 0) l++;
        while (memcmp(ra + (size_t)r * bpp, rb + (size_t)r * bpp, bpp) == 0) r--;
        min_x = MIN(min_x, l);
        max_x = MAX(max_x, r);
        min_y = MIN(min_y, y);
        max_y = y;
    }
    if (min_y > max_y) return false;
    *x0 = min_x; *y0 = min_y; *x1 = max_x + 1; *y1 = max_y + 1;
    return true;
}

static Status crop_region(const Image *src, u32 x, u32 y, u32 w, u32 h, Image *out, Diag *diag) {
    Status s = image_alloc(out, w, h, src->depth, false, 0, diag);
    if (s != ST_OK) return s;
    u32 bpp = image_bytes_per_pixel(src);
    for (u32 r = 0; r < h; r++) memcpy(image_row(out, r), image_row(src, y + r) + (size_t)x * bpp, (size_t)w * bpp);
    return ST_OK;
}

// OVER con los pixeles que no cambiaron en transparente es valido solo si cada pixel que SI
// cambio es opaco en el cuadro nuevo o era transparente en el viejo: OVER sobre un pixel que
// quedo en el lienzo mezclaria colores que no tienen que mezclarse.
static bool over_valid(const Image *prev, const Image *cur, u32 x0, u32 y0, u32 x1, u32 y1) {
    u32 bpp = image_bytes_per_pixel(cur);
    for (u32 y = y0; y < y1; y++) {
        const u8 *rp = image_row(prev, y), *rc = image_row(cur, y);
        for (u32 x = x0; x < x1; x++) {
            const u8 *p = rp + (size_t)x * bpp, *c = rc + (size_t)x * bpp;
            if (memcmp(p, c, bpp) == 0) continue;
            bool new_opaque = cur->depth == 8 ? c[3] == 255 : ((const u16 *)c)[3] == 65535;
            bool old_clear = cur->depth == 8 ? p[3] == 0 : ((const u16 *)p)[3] == 0;
            if (!new_opaque && !old_clear) return false;
        }
    }
    return true;
}

static void make_over(const Image *prev, const Image *cur, const OutFrame *f, const u8 *clear_px, Image *sub) {
    u32 bpp = image_bytes_per_pixel(cur);
    for (u32 r = 0; r < f->h; r++) {
        const u8 *rp = image_row(prev, f->y + r) + (size_t)f->x * bpp, *rc = image_row(cur, f->y + r) + (size_t)f->x * bpp;
        u8 *dst = image_row(sub, r);
        for (u32 x = 0; x < f->w; x++, rp += bpp, rc += bpp, dst += bpp)
            memcpy(dst, memcmp(rp, rc, bpp) == 0 ? clear_px : rc, bpp);
    }
}

// Pixel transparente que el plan sabe escribir (en la escala de la imagen). false si el plan no
// tiene transparencia (entonces no hay OVER).
static bool transparent_pixel(const PngPlan *plan, u32 depth, u8 out[8]) {
    memset(out, 0, 8);
    u16 rgb[3] = { 0, 0, 0 };
    if (plan->color_type == PNG_CT_PALETTE) {
        u32 i = 0;
        while (i < plan->palette_size && (plan->palette[i] >> 24) != 0) i++;
        if (i == plan->palette_size) return false;
        for (int c = 0; c < 3; c++) rgb[c] = u8_to_u16((u8)(plan->palette[i] >> (8 * c)));
    } else if (plan->color_type == PNG_CT_GRAY || plan->color_type == PNG_CT_RGB) {
        if (!plan->key) return false;
        for (int c = 0; c < 3; c++) {
            u16 k = plan->color_type == PNG_CT_GRAY ? plan->key_value[0] : plan->key_value[c];
            if (plan->write8 && plan->color_type == PNG_CT_RGB) k = u8_to_u16((u8)k);
            else if (plan->color_type == PNG_CT_GRAY && plan->bit_depth < 16)
                k = (u16)(k * (65535u / ((1u << plan->bit_depth) - 1)));
            rgb[c] = k;
        }
    }
    if (depth == 8) { for (int c = 0; c < 3; c++) out[c] = u16_to_u8(rgb[c]); }
    else memcpy(out, rgb, 6);
    return true;
}

static void add_delay(u32 *num, u32 *den, u32 add_num, u32 add_den) {
    double seconds = (double)*num / (*den ? *den : 100) + (double)add_num / (add_den ? add_den : 100);
    static const u32 DENS[] = { 1000, 100, 10, 1 };
    for (size_t i = 0; i < ARRAY_LEN(DENS); i++) {
        double n = seconds * DENS[i];
        if (n <= 65535.0) { *num = (u32)(n + 0.5); *den = DENS[i]; return; }
    }
    *num = 65535;
    *den = 1;
}

typedef struct ApngJob {
    OutFrame           *frames;
    const PngPlan      *plan;
    const EncodeOptions *opt;
} ApngJob;

static void apng_frame_task(void *arg, size_t i) {
    ApngJob *job = arg;
    OutFrame *f = &job->frames[i];
    Diag local = { 0 };
    const Image *first = f->has_over ? &f->over_sub : &f->source_sub;
    f->blend = f->has_over ? BLEND_OVER : BLEND_SOURCE;
    f->status = encode_data(first, job->plan, job->opt, &f->data, &local);
    if (f->status == ST_OK && f->has_over && job->opt->apng_try_both_blends) {
        DataOut alt;
        if (encode_data(&f->source_sub, job->plan, job->opt, &alt, &local) == ST_OK) {
            if (alt.size < f->data.size) { data_free(&f->data); f->data = alt; f->blend = BLEND_SOURCE; }
            else data_free(&alt);
        }
    }
    if (f->status != ST_OK) memcpy(f->error, local.message, sizeof f->error);
}

static void write_fctl(ByteVec *v, u32 seq, const OutFrame *f) {
    u8 c[26];
    store_be32(c, seq);
    store_be32(c + 4, f->w);
    store_be32(c + 8, f->h);
    store_be32(c + 12, f->x);
    store_be32(c + 16, f->y);
    store_be16(c + 20, (u16)f->delay_num);
    store_be16(c + 22, (u16)f->delay_den);
    c[24] = 0;                     // dispose NONE: el lienzo queda, el proximo cuadro solo trae la diferencia
    c[25] = f->blend;
    png_write_chunk(v, "fcTL", c, sizeof c);
}

// fdAT = numero de secuencia + datos; el CRC se calcula sin copiar los datos.
static void write_fdat(ByteVec *v, u32 *seq, const u8 *z, size_t size) {
    do {
        size_t n = MIN(size, (size_t)0x3FFFFFFF);
        u8 head[12];
        store_be32(head, (u32)n + 4);
        memcpy(head + 4, "fdAT", 4);
        store_be32(head + 8, (*seq)++);
        bv_put(v, head, 12);
        bv_put(v, z, n);
        u32 crc = crc32_update(crc32_update(0, head + 4, 8), z, n);
        u8 tail[4];
        store_be32(tail, crc);
        bv_put(v, tail, 4);
        z += n;
        size -= n;
    } while (size);
}

Status apng_encode(const Frame *frames, u32 count, u32 loops, const Metadata *meta,
                   const EncodeOptions *o, Encoded *out, Diag *diag) {
    memset(out, 0, sizeof *out);
    if (count == 1) return png_encode(&frames[0].image, meta, o, out, diag);
    const Image *first = &frames[0].image;
    for (u32 i = 1; i < count; i++)
        if (frames[i].image.width != first->width || frames[i].image.height != first->height || frames[i].image.depth != first->depth)
            return diag_fail(diag, ST_INVALID, "los cuadros de la animacion no tienen el mismo tamano (interno)");

    // 1) Un solo formato para todos los cuadros.
    ColorStats total;
    analyze_image(first, &total);
    for (u32 i = 1; i < count; i++) {
        ColorStats s;
        analyze_image(&frames[i].image, &s);
        analyze_merge(&total, &s);
    }
    const Image **images = mem_alloc(count * sizeof *images);
    OutFrame *of = mem_calloc(count * sizeof *of);
    if (!images || !of) { mem_free(images); mem_free(of); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }
    for (u32 i = 0; i < count; i++) images[i] = &frames[i].image;
    PngPlan plan;
    PlanRequest req = { o->color, o->depth, o->reduce, o->may_clean, true, !icc_space_is(meta, "RGB "), 1, NULL };
    Status st = plan_choose(&total, images, count, &req, &plan, diag);
    mem_free(images);

    // 2) Cada cuadro guarda solo el rectangulo que cambio; los identicos se funden con el anterior.
    u8 clear_px[8];
    bool can_clear = st == ST_OK && transparent_pixel(&plan, first->depth, clear_px);
    u32 n = 0;
    for (u32 i = 0; i < count && st == ST_OK; i++) {
        const Frame *fr = &frames[i];
        if (i == 0) {
            of[0] = (OutFrame){ 0, 0, first->width, first->height, fr->delay_num, fr->delay_den ? fr->delay_den : 100 };
            st = image_clone(first, &of[0].source_sub, diag);
            n = 1;
            continue;
        }
        u32 x0, y0, x1, y1;
        if (!diff_bbox(&frames[i - 1].image, &fr->image, &x0, &y0, &x1, &y1)) {
            add_delay(&of[n - 1].delay_num, &of[n - 1].delay_den, fr->delay_num, fr->delay_den);
            continue;
        }
        OutFrame *f = &of[n];
        *f = (OutFrame){ x0, y0, x1 - x0, y1 - y0, fr->delay_num, fr->delay_den ? fr->delay_den : 100 };
        st = crop_region(&fr->image, x0, y0, f->w, f->h, &f->source_sub, diag);
        if (st == ST_OK && can_clear && over_valid(&frames[i - 1].image, &fr->image, x0, y0, x1, y1)) {
            st = image_alloc(&f->over_sub, f->w, f->h, first->depth, false, 0, diag);
            if (st == ST_OK) { make_over(&frames[i - 1].image, &fr->image, f, clear_px, &f->over_sub); f->has_over = true; }
        }
        n++;
    }

    // 3) Los cuadros se codifican en paralelo (cada uno ademas paraleliza por dentro).
    if (st == ST_OK) {
        ApngJob job = { of, &plan, o };
        parallel_for(n, apng_frame_task, &job);
        for (u32 i = 0; i < n && st == ST_OK; i++)
            if (of[i].status != ST_OK) st = diag_fail(diag, of[i].status, "cuadro %u: %s", i, of[i].error);
    }

    // 4) Armado: la imagen por defecto es el cuadro 0 (lo que ve un visor sin soporte de APNG).
    if (st == ST_OK) {
        ByteVec v = { 0 };
        png_write_signature(&v);
        png_write_ihdr(&v, first->width, first->height, &plan, o->interlace);
        Metadata scratch;
        png_write_metadata(&v, metadata_for(meta, &plan, &scratch, diag), o->orientation_applied);
        png_write_palette(&v, &plan);
        u8 actl[8];
        store_be32(actl, n);
        store_be32(actl + 4, loops);
        png_write_chunk(&v, "acTL", actl, 8);
        u32 seq = 0;
        u32 trials = 0;
        for (u32 i = 0; i < n; i++) {
            write_fctl(&v, seq++, &of[i]);
            if (i == 0) png_write_data_chunks(&v, "IDAT", of[i].data.z, of[i].data.size);
            else write_fdat(&v, &seq, of[i].data.z, of[i].data.size);
            trials += of[i].data.trials;
        }
        png_write_end(&v);
        DataOut summary = of[0].data;
        summary.trials = trials;
        fill_report(&out->report, &plan, &summary, o, count, n, &total);
        if (v.failed) { bv_free(&v); st = diag_fail(diag, ST_NO_MEMORY, "sin memoria para armar el APNG"); }
        else { out->data = v.data; out->size = v.len; }
    }
    for (u32 i = 0; i < count; i++) {
        image_free(&of[i].source_sub);
        image_free(&of[i].over_sub);
        data_free(&of[i].data);
    }
    mem_free(of);
    return st;
}
