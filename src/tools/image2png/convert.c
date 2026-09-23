// tools/image2png/convert.c - un archivo de punta a punta: leer -> decodificar -> procesar ->
// codificar -> escribir, midiendo cada etapa. Todo error queda en el trabajo; una excepcion de
// hardware (un decodificador engañado por un archivo malicioso) tambien: se atrapa con SEH y el
// lote sigue con los demas archivos.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include "job.h"
#include "../../core/fileio.h"
#include "../../core/pool.h"
#include "../../core/text.h"

static const double INCH_METERS = 0.0254;

static double now(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

static void extension_hint(const wchar_t *path, char out[16]) {
    out[0] = 0;
    if (!path) return;
    const wchar_t *ext = path_extension(path);
    size_t n = 0;
    for (const wchar_t *p = *ext ? ext + 1 : ext; *p && n + 1 < 16; p++) {
        if (*p > 127) { n = 0; break; }
        out[n++] = (char)(*p >= L'A' && *p <= L'Z' ? *p + 32 : *p);
    }
    out[n] = 0;
}

// La politica de metadatos, en un solo lugar: que se conserva segun --meta/--icc, mas lo que el
// usuario agrega (--text, --dpi). El resultado es una copia propia.
static void output_metadata(const Decoded *doc, const Config *cfg, Metadata *out) {
    memset(out, 0, sizeof *out);
    vec_init(&out->texts, sizeof(TextEntry));
    const Metadata *m = &doc->meta;
    MetaMode mode = cfg->encode.meta;
    if (mode >= META_COLOR) {
        if (cfg->icc != ICC_STRIP && m->icc.size) {
            blob_set(&out->icc, m->icc.data, m->icc.size);
            memcpy(out->icc_name, m->icc_name, sizeof out->icc_name);
        }
        out->has_srgb = m->has_srgb;          out->srgb_intent = m->srgb_intent;
        out->has_cicp = m->has_cicp && cfg->icc != ICC_STRIP;
        memcpy(out->cicp, m->cicp, sizeof out->cicp);
        out->has_gamma = m->has_gamma;        out->gamma = m->gamma;
        out->has_chrm = m->has_chrm;          memcpy(out->chrm, m->chrm, sizeof out->chrm);
        out->has_phys = m->has_phys;          out->phys_x = m->phys_x; out->phys_y = m->phys_y; out->phys_unit = m->phys_unit;
    }
    if (mode == META_ALL) {
        blob_set(&out->exif, m->exif.data, m->exif.size);
        blob_set(&out->xmp, m->xmp.data, m->xmp.size);
        for (size_t i = 0; i < m->texts.count; i++) {
            const TextEntry *t = &VEC_AT(&m->texts, TextEntry, i);
            metadata_add_text(out, t->keyword, t->text, strlen(t->text));
        }
    }
    if (cfg->dpi) {
        out->has_phys = true;
        out->phys_x = out->phys_y = (u32)(cfg->dpi / INCH_METERS + 0.5);
        out->phys_unit = 1;
    }
    for (size_t i = 0; i < cfg->texts.count; i++) {
        const TextPair *t = &VEC_AT(&cfg->texts, TextPair, i);
        metadata_add_text(out, t->keyword, t->value, strlen(t->value));
    }
}

static Status write_output(Job *job, const Config *cfg, const wchar_t *path, const Encoded *enc) {
    job->out_bytes += enc->size;
    job->out_files++;
    if (cfg->dry_run) return ST_OK;
    if (!path) return stdout_write_all(enc->data, enc->size, &job->diag);
    // La carpeta de destino se crea al escribir (asi --dry-run no deja carpetas vacias).
    const wchar_t *base = path_basename(path);
    size_t dir_len = (size_t)(base - path);
    if (dir_len > 3) {
        wchar_t *dir = mem_alloc((dir_len + 1) * sizeof(wchar_t));
        if (dir) {
            memcpy(dir, path, dir_len * sizeof(wchar_t));
            dir[dir_len - 1] = 0;
            if (!ensure_directory(dir)) { mem_free(dir); return diag_fail(&job->diag, ST_IO, "no se pudo crear la carpeta de salida"); }
            mem_free(dir);
        }
    }
    Status s = file_write_atomic(path, enc->data, enc->size, cfg->no_clobber ? WRITE_NO_CLOBBER : WRITE_REPLACE, &job->diag);
    if (s == ST_OK && cfg->preserve_time && job->in->path) file_set_mtime(path, file_mtime(job->in->path));
    return s;
}

static void convert_inner(Job *job, const Config *cfg) {
    Diag *diag = &job->diag;
    const InputFile *in = job->in;
    double t = now();
    FileData file;
    Status s = in->path ? file_read_all(in->path, &file, diag) : stdin_read_all(&file, diag);
    job->t.read = now() - t;
    if (s != ST_OK) { job->status = s; return; }
    job->in_bytes = file.size;

    // ---- decodificar ------------------------------------------------------------------------
    DecodeOptions dopt = cfg->decode;
    bool want_frames = cfg->apng || cfg->split || dopt.frame_mode == FRAMES_ALL;
    dopt.single_frame = !want_frames;
    char ext[16];
    extension_hint(in->path, ext);
    DecodeContext ctx = { file.data, file.size, ext, &dopt, diag, 0 };
    Decoded doc;
    t = now();
    s = decode_image(&ctx, &doc);
    job->t.decode = now() - t;
    file_data_free(&file);                          // los decodificadores copian: se libera ya
    job->source = doc.source;
    job->frames_in = decoded_frame_count(&doc);
    if (s != ST_OK || pool_cancelled()) {
        job->status = s != ST_OK ? s : diag_fail(diag, ST_CANCELLED, "cancelado");
        decoded_free(&doc);
        return;
    }

    // ---- procesar -----------------------------------------------------------------------------
    PipelineOptions popt = cfg->pipeline;
    popt.mask = cfg->mask.pixels ? &cfg->mask : NULL;
    t = now();
    s = pipeline_run(&doc, &popt, &job->pipe, diag);
    job->t.process = now() - t;
    if (s != ST_OK) { job->status = s; decoded_free(&doc); return; }

    // ---- codificar y escribir ------------------------------------------------------------------
    EncodeOptions eo = cfg->encode;
    eo.orientation_applied = job->pipe.oriented != ORIENT_NONE;
    eo.may_clean = popt.clean != CLEAN_KEEP;
    eo.apng_try_both_blends = eo.optimize >= 3;
    if (popt.alpha == ALPHA_FORCE && eo.color == COLOR_AUTO) eo.color = popt.grayscale ? COLOR_GRAY_ALPHA : COLOR_RGBA;
    Metadata meta;
    output_metadata(&doc, cfg, &meta);

    u32 n = decoded_frame_count(&doc);
    const Frame *frames = doc.frames.items;
    bool animate = doc.animated && n > 1 && cfg->apng && !cfg->split;
    if (!animate && n > 1 && job->to_stdout) {
        s = diag_fail(diag, ST_INVALID, "%u imagenes no entran en stdout: usa -o carpeta", n);
    } else if (animate) {
        Encoded enc;
        u32 loops = cfg->loops_set ? cfg->loops : doc.loop_count;
        t = now();
        s = apng_encode(frames, n, loops, &meta, &eo, &enc, diag);
        job->t.encode = now() - t;
        if (s == ST_OK) {
            job->enc = enc.report;
            t = now();
            s = write_output(job, cfg, job->to_stdout ? NULL : job->output, &enc);
            job->t.write = now() - t;
        }
        encoded_free(&enc);
        job->frames_out = job->enc.frames_out;
    } else {
        // Imagen suelta, o cuadros/paginas/tamaños como archivos separados (base-000.png ...).
        for (u32 i = 0; i < n && s == ST_OK; i++) {
            if (pool_cancelled()) { s = diag_fail(diag, ST_CANCELLED, "cancelado"); break; }
            Encoded enc;
            t = now();
            s = png_encode(&frames[i].image, &meta, &eo, &enc, diag);
            job->t.encode += now() - t;
            if (s != ST_OK) break;
            job->enc = enc.report;
            wchar_t *path = job->to_stdout ? NULL : n == 1 ? NULL : frame_output_path(job->output, frames[i].index, doc.source.total_frames ? doc.source.total_frames : n);
            t = now();
            s = write_output(job, cfg, job->to_stdout ? NULL : n == 1 ? job->output : path, &enc);
            job->t.write += now() - t;
            mem_free(path);
            encoded_free(&enc);
        }
        job->frames_out = n;
    }
    if (n) { job->width = frames[0].image.width; job->height = frames[0].image.height; }
    metadata_free(&meta);
    decoded_free(&doc);
    job->status = s;
}

void convert_job(Job *job, const Config *cfg) {
    diag_reset(&job->diag);
    if (job->skip) {
        job->status = diag_fail(&job->diag, ST_SKIPPED, "%s", job->skip_reason);
        return;
    }
    if (job->in->missing) {
        job->status = diag_fail(&job->diag, ST_IO, "no existe");
        return;
    }
    if (pool_cancelled()) {
        job->status = diag_fail(&job->diag, ST_CANCELLED, "cancelado");
        return;
    }
    __try {
        convert_inner(job, cfg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Un bug propio frente a un archivo hostil no tumba el lote entero.
        job->status = ST_CORRUPT;
        snprintf(job->diag.message, sizeof job->diag.message,
                 "fallo interno 0x%08lX al procesar este archivo (archivo malformado o bug: reportalo)", (unsigned long)GetExceptionCode());
        job->diag.status = ST_CORRUPT;
    }
}
