// tools/image2png/report.c - ver report.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include "report.h"
#include "../../app/ui.h"
#include "../../codec/wic.h"
#include "../../core/cpu.h"
#include "../../core/fileio.h"
#include "../../core/icc.h"
#include "../../core/cicp.h"
#include "../../core/text.h"
#include "../../png/analyze.h"
#include "../../png/plan.h"

enum { NAME_COLS_WIDE = 34, NAME_COLS = 26, OUT_COLS = 26, FORMAT_COLS_WIDE = 44, FORMAT_COLS = 36, WIDE_TERMINAL = 150, KEY_COLS = 11, MAX_FORMATS = 32 };

static SRWLOCK g_json_lock = SRWLOCK_INIT;

static double job_seconds(const Job *j) { return j->t.read + j->t.decode + j->t.process + j->t.encode + j->t.write; }

// ---- JSON ------------------------------------------------------------------------------------------

static void json_string(Buf *b, const char *s) {
    buf_puts(b, "\"");
    for (; s && *s; s++) {
        u8 c = (u8)*s;
        if (c == '"' || c == '\\') { buf_puts(b, "\\"); buf_append(b, s, 1); }
        else if (c < 0x20) buf_printf(b, "\\u%04x", c);
        else buf_append(b, s, 1);
    }
    buf_puts(b, "\"");
}

static void json_job(const Job *j) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "{\"input\":");
    json_string(&b, j->in->display);
    buf_puts(&b, ",\"output\":");
    json_string(&b, j->output_display);
    buf_printf(&b, ",\"status\":\"%s\"", j->status == ST_OK ? "ok" : j->status == ST_SKIPPED ? "skipped" : "error");
    if (j->status != ST_OK) { buf_puts(&b, ",\"message\":"); json_string(&b, j->diag.message); }
    buf_puts(&b, ",\"format\":");
    json_string(&b, j->source.format);
    buf_puts(&b, ",\"codec\":");
    json_string(&b, j->source.codec);
    buf_printf(&b, ",\"width\":%u,\"height\":%u,\"frames_in\":%u,\"frames_out\":%u,\"bytes_in\":%llu,\"bytes_out\":%llu,\"files_out\":%u",
               j->width, j->height, j->frames_in, j->frames_out, (unsigned long long)j->in_bytes, (unsigned long long)j->out_bytes, j->out_files);
    if (j->status == ST_OK) {
        buf_puts(&b, ",\"png\":{\"format\":");
        json_string(&b, j->enc.format);
        buf_puts(&b, ",\"filter\":");
        json_string(&b, j->enc.filter);
        buf_printf(&b, ",\"level\":%d,\"color_type\":%u,\"bit_depth\":%u,\"palette\":%u,\"trials\":%u}",
                   j->enc.level, j->enc.color_type, j->enc.bit_depth, j->enc.palette_size, j->enc.trials);
        buf_printf(&b, ",\"alpha\":{\"pixels\":%llu,\"transparent\":%llu,\"partial\":%llu}",
                   (unsigned long long)j->enc.pixels, (unsigned long long)j->enc.transparent, (unsigned long long)j->enc.partial);
        buf_puts(&b, ",\"operations\":");
        json_string(&b, j->pipe.summary);
    }
    buf_printf(&b, ",\"seconds\":{\"read\":%.6f,\"decode\":%.6f,\"process\":%.6f,\"encode\":%.6f,\"write\":%.6f}",
               j->t.read, j->t.decode, j->t.process, j->t.encode, j->t.write);
    buf_puts(&b, ",\"warnings\":[");
    for (u32 i = 0; i < MIN(j->diag.warning_count, (u32)DIAG_MAX_WARNINGS); i++) {
        if (i) buf_puts(&b, ",");
        json_string(&b, j->diag.warnings[i]);
    }
    buf_puts(&b, "]}\n");
    AcquireSRWLockExclusive(&g_json_lock);
    stdout_emit(&b);
    ReleaseSRWLockExclusive(&g_json_lock);
    buf_free(&b);
}

// ---- linea por archivo ------------------------------------------------------------------------------

// Rutas: se cortan por el medio para que siempre se vea el nombre del archivo, y una columna
// menos que el ancho para que quede un espacio antes de la siguiente.
static void padded(Buf *b, const char *text, u32 cols, u32 ink) {
    char cut[512];
    utf8_ellipsize_middle(text, cols > 1 ? cols - 1 : cols, cut, sizeof cut);
    size_t start = b->len;
    buf_inked(b, ink, "%s", cut);
    buf_pad_to(b, start, cols);
}

static void size_delta(Buf *b, u64 in, u64 out) {
    if (!in) return;
    double ratio = (double)out / (double)in - 1.0;
    char pct[32];
    fmt_percent(pct, sizeof pct, fabs(ratio), fabs(ratio) < 0.1 ? 1 : 0);
    buf_inked(b, ratio <= 0 ? UI_OK : UI_FORMAT, "%s%s", ratio <= 0 ? "−" : "+", pct);
}

static void alpha_badge(Buf *b, const EncodeReport *e) {
    if (e->partial) {
        char pct[32];
        fmt_percent(pct, sizeof pct, e->pixels ? (double)(e->transparent + e->partial) / e->pixels : 0, 0);
        buf_inked(b, UI_ALPHA, "α suave %s", pct);
    } else if (e->transparent) {
        buf_inked(b, UI_ALPHA, "α binaria");
    } else {
        buf_inked(b, UI_DIM, "opaca");
    }
}

// "ORIGEN → PNG" recortado a `cols` columnas, con el origen en durazno y el destino en teal.
static void format_column(Buf *b, const char *src, const char *dst, u32 cols) {
    char plain[192], cut[192];
    snprintf(plain, sizeof plain, "%s → %s", src, dst);
    utf8_ellipsize(plain, cols - 1, cut, sizeof cut);
    size_t start = b->len;
    const char *arrow = strstr(cut, "→");
    if (arrow) {
        buf_ink(b, UI_FORMAT);
        buf_append(b, cut, (size_t)(arrow - cut));
        buf_inked(b, UI_DIM, "→");
        buf_inked(b, UI_TEAL, "%s", arrow + strlen("→"));
    } else {
        buf_inked(b, UI_FORMAT, "%s", cut);
    }
    buf_pad_to(b, start, cols);
}

void report_job(const Config *cfg, const Job *j) {
    if (cfg->json) { json_job(j); return; }
    bool ok = j->status == ST_OK;
    if (cfg->quiet && (ok || j->status == ST_SKIPPED)) return;
    u32 cols = term_columns();
    bool wide = cols >= WIDE_TERMINAL;
    Buf b;
    buf_init(&b);
    buf_puts(&b, "  ");
    switch (j->status) {
    case ST_OK:        buf_inked(&b, UI_OK, "✓"); break;
    case ST_SKIPPED:   buf_inked(&b, UI_WARN, "↷"); break;
    case ST_CANCELLED: buf_inked(&b, UI_DIM, "◌"); break;
    default:           buf_inked(&b, UI_ERROR, "✗"); break;
    }
    buf_puts(&b, " ");
    padded(&b, j->in->display, wide ? NAME_COLS_WIDE : NAME_COLS, ok ? UI_PATH : UI_SUBTLE);
    if (!ok) {
        buf_puts(&b, "  ");
        buf_inked(&b, j->status == ST_SKIPPED ? UI_WARN : UI_ERROR, "%s", j->diag.message[0] ? j->diag.message : status_label(j->status));
        buf_puts(&b, "\n");
        term_emit(&b);
        buf_free(&b);
        return;
    }
    if (wide) {
        buf_inked(&b, UI_DIM, "→ ");
        padded(&b, j->output_display, OUT_COLS, UI_SUBTLE);
    }
    char dims[48], in_sz[24], out_sz[24], secs[24];
    snprintf(dims, sizeof dims, "%u×%u", j->width, j->height);
    format_column(&b, j->source.format, j->enc.format, wide ? FORMAT_COLS_WIDE : FORMAT_COLS);
    size_t start = b.len;
    buf_inked(&b, UI_NUMBER, "%s", dims);
    buf_pad_to(&b, start, 12);
    fmt_bytes(in_sz, sizeof in_sz, j->in_bytes);
    fmt_bytes(out_sz, sizeof out_sz, j->out_bytes);
    start = b.len;
    buf_inked(&b, UI_SUBTLE, "%s", in_sz);
    buf_inked(&b, UI_DIM, " → ");
    buf_inked(&b, UI_NUMBER, "%s", out_sz);
    buf_pad_to(&b, start, 22);
    start = b.len;
    size_delta(&b, j->in_bytes, j->out_bytes);
    buf_pad_to(&b, start, 9);
    start = b.len;
    alpha_badge(&b, &j->enc);
    if (j->frames_out > 1 && j->out_files == 1) buf_inked(&b, UI_ACCENT, " · APNG %u cuadros", j->frames_out);
    else if (j->out_files > 1) buf_inked(&b, UI_ACCENT, " · %u archivos", j->out_files);
    buf_puts(&b, "  ");
    buf_pad_to(&b, start, 16);
    fmt_duration(secs, sizeof secs, job_seconds(j));
    buf_inked(&b, UI_DIM, "%s", secs);
    buf_puts(&b, "\n");
    // Detalle: que se hizo (si hubo operaciones) y los avisos.
    if (j->pipe.steps && (cfg->verbose || !cfg->quiet)) {
        buf_puts(&b, "      ");
        buf_inked(&b, UI_DIM, "└ %s", j->pipe.summary);
        buf_puts(&b, "\n");
    }
    if (cfg->verbose) {
        buf_puts(&b, "      ");
        buf_inked(&b, UI_DIM, "└ filtro %s · deflate %d · %u prueba%s · %s", j->enc.filter, j->enc.level, j->enc.trials,
                  j->enc.trials == 1 ? "" : "s", j->source.codec);
        buf_puts(&b, "\n");
    }
    u32 shown = cfg->verbose ? MIN(j->diag.warning_count, (u32)DIAG_MAX_WARNINGS) : MIN(j->diag.warning_count, 1u);
    for (u32 i = 0; i < shown; i++) {
        buf_puts(&b, "      ");
        buf_inked(&b, UI_WARN, "⚠ %s", j->diag.warnings[i]);
        buf_puts(&b, "\n");
    }
    if (!cfg->verbose && j->diag.warning_count > 1) {
        buf_puts(&b, "      ");
        buf_inked(&b, UI_DIM, "  (+%u aviso%s, -v para verlos)", j->diag.warning_count - 1, j->diag.warning_count == 2 ? "" : "s");
        buf_puts(&b, "\n");
    }
    term_emit(&b);
    buf_free(&b);
}

// ---- encabezado, progreso y resumen ---------------------------------------------------------------

void report_header(const Config *cfg, size_t jobs, u64 total_bytes, u32 threads) {
    if (cfg->quiet || cfg->json) return;
    Buf b;
    buf_init(&b);
    char size[24];
    fmt_bytes(size, sizeof size, total_bytes);
    buf_puts(&b, "\n  ");
    buf_bold(&b);
    buf_inked(&b, UI_ACCENT, "image2png");
    buf_inked(&b, UI_DIM, " %s  ·  ", PRISMA_VERSION);
    buf_inked(&b, UI_NUMBER, "%zu", jobs);
    buf_inked(&b, UI_DIM, " archivo%s · %s · %u hilo%s · -O%d%s%s\n\n", jobs == 1 ? "" : "s", size, threads, threads == 1 ? "" : "s",
              cfg->encode.optimize, cfg->encode.level >= 0 ? " · deflate " : "", "");
    term_emit(&b);
    buf_free(&b);
}

void report_progress(u64 done, u64 total, u64 bytes_done, u64 bytes_total, double elapsed) {
    if (!term_interactive() || total < 2) return;
    Buf b;
    buf_init(&b);
    u32 cols = term_columns();
    u32 bar = cols >= 120 ? 48 : cols >= 90 ? 32 : 20;
    double fraction = bytes_total ? (double)bytes_done / (double)bytes_total : (double)done / (double)total;
    buf_puts(&b, "  ");
    ui_bar(&b, fraction, bar);
    char pct[16], speed[24], eta[24];
    fmt_percent(pct, sizeof pct, fraction, 0);
    buf_inked(&b, UI_NUMBER, "  %s", pct);
    buf_inked(&b, UI_DIM, "  ·  %llu/%llu", (unsigned long long)done, (unsigned long long)total);
    if (elapsed > 0.5 && bytes_done) {
        fmt_bytes(speed, sizeof speed, (u64)(bytes_done / elapsed));
        buf_inked(&b, UI_DIM, "  ·  %s/s", speed);
        if (fraction > 0.02 && fraction < 1.0) {
            fmt_duration(eta, sizeof eta, elapsed * (1.0 - fraction) / fraction);
            buf_inked(&b, UI_DIM, "  ·  faltan %s", eta);
        }
    }
    term_status(&b);
    buf_free(&b);
}

typedef struct FormatCount { char name[16]; u32 count; } FormatCount;

void report_summary(const Config *cfg, const Job *jobs, size_t n, double elapsed, u32 threads) {
    term_status_clear();
    if (cfg->json) {
        u64 ok = 0, failed = 0;
        for (size_t i = 0; i < n; i++) { if (jobs[i].status == ST_OK) ok++; else if (jobs[i].status != ST_SKIPPED) failed++; }
        Buf b;
        buf_init(&b);
        buf_printf(&b, "{\"summary\":{\"files\":%zu,\"ok\":%llu,\"failed\":%llu,\"seconds\":%.6f,\"threads\":%u}}\n", n,
                   (unsigned long long)ok, (unsigned long long)failed, elapsed, threads);
        stdout_emit(&b);
        buf_free(&b);
        return;
    }
    u64 ok = 0, failed = 0, skipped = 0, in_bytes = 0, out_bytes = 0, with_alpha = 0, soft = 0, binary = 0, files_out = 0;
    JobTimes total = { 0 };
    FormatCount formats[MAX_FORMATS];
    u32 nformats = 0;
    for (size_t i = 0; i < n; i++) {
        const Job *j = &jobs[i];
        if (j->status == ST_OK) ok++; else if (j->status == ST_SKIPPED) skipped++; else failed++;
        if (j->status != ST_OK) continue;
        in_bytes += j->in_bytes;
        out_bytes += j->out_bytes;
        files_out += j->out_files;
        if (j->enc.partial) { with_alpha++; soft++; } else if (j->enc.transparent) { with_alpha++; binary++; }
        total.read += j->t.read; total.decode += j->t.decode; total.process += j->t.process;
        total.encode += j->t.encode; total.write += j->t.write;
        u32 k = 0;
        while (k < nformats && strcmp(formats[k].name, j->source.format) != 0) k++;
        if (k == nformats && nformats < MAX_FORMATS) { snprintf(formats[k].name, sizeof formats[k].name, "%s", j->source.format); formats[k].count = 0; nformats++; }
        if (k < nformats) formats[k].count++;
    }
    if (cfg->quiet && !failed) return;
    // Una sola imagen sin problemas: la linea ya dijo todo.
    if (n == 1 && !cfg->bench && !failed) { term_emit_text("\n"); return; }

    Card c;
    u32 w = MIN(ui_width(), 100u) - 4;
    card_begin(&c, w);
    char a[32], b2[32], s[32];
    Buf line;
    buf_init(&line);
    buf_inked(&line, UI_OK, "✓ %llu convertido%s", (unsigned long long)ok, ok == 1 ? "" : "s");
    if (failed) buf_inked(&line, UI_ERROR, "   ✗ %llu con error", (unsigned long long)failed);
    if (skipped) buf_inked(&line, UI_WARN, "   ↷ %llu omitido%s", (unsigned long long)skipped, skipped == 1 ? "" : "s");
    if (files_out != ok) buf_inked(&line, UI_DIM, "   ·  %llu PNG escritos", (unsigned long long)files_out);
    card_line(&c, "%s", line.data ? line.data : "");
    buf_clear(&line);
    if (ok) {
        fmt_bytes(a, sizeof a, in_bytes);
        fmt_bytes(b2, sizeof b2, out_bytes);
        ui_key(&line, "entrada", KEY_COLS);
        buf_inked(&line, UI_SUBTLE, "%s", a);
        buf_inked(&line, UI_DIM, "   →   ");
        buf_inked(&line, UI_DIM, "salida ");
        buf_inked(&line, UI_NUMBER, "%s", b2);
        buf_puts(&line, "   ");
        size_delta(&line, in_bytes, out_bytes);
        card_line(&c, "%s", line.data);
        buf_clear(&line);
    }
    fmt_duration(s, sizeof s, elapsed);
    ui_key(&line, "tiempo", KEY_COLS);
    buf_inked(&line, UI_NUMBER, "%s", s);
    if (elapsed > 0 && in_bytes) {
        fmt_bytes(a, sizeof a, (u64)(in_bytes / elapsed));
        char rate[24];
        fmt_decimal(rate, sizeof rate, (double)(ok + failed) / elapsed, 1);
        buf_inked(&line, UI_DIM, "   ·   %s/s   ·   %s archivos/s", a, rate);
    }
    buf_inked(&line, UI_DIM, "   ·   %u hilo%s", threads, threads == 1 ? "" : "s");
    card_line(&c, "%s", line.data);
    buf_clear(&line);
    if (ok) {
        ui_key(&line, "alfa", KEY_COLS);
        if (with_alpha) buf_inked(&line, UI_ALPHA, "%llu con transparencia", (unsigned long long)with_alpha);
        else buf_inked(&line, UI_DIM, "ninguna con transparencia");
        if (with_alpha) buf_inked(&line, UI_DIM, ":  %llu binaria · %llu suave", (unsigned long long)binary, (unsigned long long)soft);
        card_line(&c, "%s", line.data);
        buf_clear(&line);
        if (nformats) {
            ui_key(&line, "formatos", KEY_COLS);
            for (u32 k = 0; k < nformats; k++) {
                u32 best = k;
                for (u32 m = k + 1; m < nformats; m++) if (formats[m].count > formats[best].count) best = m;
                FormatCount t = formats[k]; formats[k] = formats[best]; formats[best] = t;
                buf_inked(&line, UI_FORMAT, "%s%s", k ? " · " : "", formats[k].name);
                buf_inked(&line, UI_NUMBER, " %u", formats[k].count);
            }
            card_line(&c, "%s", line.data);
            buf_clear(&line);
        }
    }
    if (cfg->bench && ok) {
        card_rule(&c);
        const char *names[5] = { "leer", "decodificar", "procesar", "codificar", "escribir" };
        double values[5] = { total.read, total.decode, total.process, total.encode, total.write };
        double sum = values[0] + values[1] + values[2] + values[3] + values[4];
        for (int k = 0; k < 5; k++) {
            ui_key(&line, names[k], KEY_COLS + 2);
            ui_bar(&line, sum > 0 ? values[k] / sum : 0, 30);
            fmt_duration(s, sizeof s, values[k]);
            buf_inked(&line, UI_NUMBER, "  %s", s);
            fmt_percent(a, sizeof a, sum > 0 ? values[k] / sum : 0, 0);
            buf_inked(&line, UI_DIM, "  %s", a);
            card_line(&c, "%s", line.data);
            buf_clear(&line);
        }
        buf_inked(&line, UI_DIM, "tiempos sumados de todos los hilos (el reloj de pared es el de arriba)");
        card_line(&c, "%s", line.data);
        buf_clear(&line);
    }
    if (failed) {
        card_rule(&c);
        u32 shown = 0;
        for (size_t i = 0; i < n && shown < 12; i++) {
            const Job *j = &jobs[i];
            if (j->status == ST_OK || j->status == ST_SKIPPED) continue;
            size_t st = line.len;
            buf_inked(&line, UI_ERROR, "✗ ");
            char cut[64];
            utf8_ellipsize_middle(j->in->display, 28, cut, sizeof cut);
            buf_inked(&line, UI_SUBTLE, "%s", cut);
            buf_pad_to(&line, st, 32);
            utf8_ellipsize(j->diag.message, w > 36 ? w - 34 : 20, cut, sizeof cut);
            buf_inked(&line, UI_ERROR, "%s", cut);
            card_line(&c, "%s", line.data);
            buf_clear(&line);
            shown++;
        }
        if (failed > shown) { buf_inked(&line, UI_DIM, "… y %llu mas", (unsigned long long)(failed - shown)); card_line(&c, "%s", line.data); buf_clear(&line); }
    }
    buf_free(&line);
    term_emit_text("\n");
    card_emit(&c, failed ? "image2png · con errores" : "image2png · listo", failed ? UI_ERROR : UI_ACCENT);
    term_emit_text("\n");
}

void report_error(const char *title, const char *message) {
    Buf b;
    buf_init(&b);
    buf_puts(&b, "\n  ");
    buf_inked(&b, UI_ERROR, "✗ %s", title);
    buf_puts(&b, "\n    ");
    buf_inked(&b, UI_SUBTLE, "%s", message);
    buf_puts(&b, "\n\n");
    term_emit(&b);
    buf_free(&b);
}

// ---- --info ------------------------------------------------------------------------------------------

Status report_info(const Config *cfg, Job *job) {
    Diag *diag = &job->diag;
    diag_reset(diag);
    if (job->in->missing) return job->status = diag_fail(diag, ST_IO, "no existe");
    FileData file;
    Status s = job->in->path ? file_read_all(job->in->path, &file, diag) : stdin_read_all(&file, diag);
    if (s != ST_OK) return job->status = s;
    DecodeOptions dopt = cfg->decode;
    dopt.single_frame = true;
    char ext[16] = "";
    if (job->in->path) {
        const wchar_t *e = path_extension(job->in->path);
        size_t k = 0;
        for (const wchar_t *p = *e ? e + 1 : e; *p && k + 1 < sizeof ext; p++) ext[k++] = (char)(*p < 128 ? (*p >= L'A' && *p <= L'Z' ? *p + 32 : *p) : '?');
        ext[k] = 0;
    }
    DecodeContext ctx = { file.data, file.size, ext, &dopt, diag, 0 };
    Decoded doc;
    s = decode_image(&ctx, &doc);
    u64 file_bytes = file.size;
    file_data_free(&file);
    if (s != ST_OK) { decoded_free(&doc); report_job(cfg, job); return job->status = s; }
    const Image *im = &decoded_frame(&doc, 0)->image;
    ColorStats st;
    analyze_image(im, &st);
    u64 unique = analyze_unique_colors(im);
    PngPlan plan;
    char icc_desc[96] = "", space[5] = "";
    bool icc_rgb_nonsrgb = doc.meta.icc.size && !icc_is_srgb(doc.meta.icc.data, doc.meta.icc.size) &&
                           icc_color_space(doc.meta.icc.data, doc.meta.icc.size, space) && strcmp(space, "RGB ") == 0;
    PlanRequest req = { COLOR_AUTO, 0, true, false, false, !icc_rgb_nonsrgb, 1, NULL };
    Diag plan_diag = { 0 };
    bool planned = plan_choose(&st, &im, 1, &req, &plan, &plan_diag) == ST_OK;
    job->status = ST_OK;

    if (cfg->json) {
        Buf b;
        buf_init(&b);
        buf_puts(&b, "{\"input\":");
        json_string(&b, job->in->display);
        buf_puts(&b, ",\"format\":");
        json_string(&b, doc.source.format);
        buf_puts(&b, ",\"detail\":");
        json_string(&b, doc.source.detail);
        buf_puts(&b, ",\"codec\":");
        json_string(&b, doc.source.codec);
        buf_printf(&b, ",\"width\":%u,\"height\":%u,\"frames\":%u,\"bits\":%u,\"bytes\":%llu,\"unique_colors\":%llu,\"gray\":%s",
                   im->width, im->height, doc.source.total_frames, doc.source.bits, (unsigned long long)file_bytes,
                   (unsigned long long)unique, st.gray ? "true" : "false");
        buf_printf(&b, ",\"alpha\":{\"origin\":\"%s\",\"transparent\":%llu,\"partial\":%llu,\"opaque\":%llu}",
                   alpha_origin_label(doc.source.alpha_origin), (unsigned long long)st.transparent, (unsigned long long)st.partial,
                   (unsigned long long)st.opaque);
        char pd[64] = "";
        if (planned) plan_describe(&plan, pd, sizeof pd);
        buf_puts(&b, ",\"best_png\":");
        json_string(&b, pd);
        buf_puts(&b, "}\n");
        stdout_emit(&b);
        buf_free(&b);
        decoded_free(&doc);
        return ST_OK;
    }

    Card c;
    u32 w = MIN(ui_width(), 110u) - 4;
    card_begin(&c, w);
    Buf line;
    buf_init(&line);
    char t1[48], t2[48];
#define EMIT_LINE() do { card_line(&c, "%s", line.data ? line.data : ""); buf_clear(&line); } while (0)
    ui_key(&line, "formato", KEY_COLS);
    buf_inked(&line, UI_FORMAT, "%s", doc.source.format);
    buf_inked(&line, UI_SUBTLE, " · %s", doc.source.detail);
    buf_inked(&line, UI_DIM, "   ·  %s", doc.source.codec);
    EMIT_LINE();
    ui_key(&line, "tamaño", KEY_COLS);
    fmt_bytes(t1, sizeof t1, file_bytes);
    buf_inked(&line, UI_NUMBER, "%u × %u", im->width, im->height);
    char mp[32];
    fmt_decimal(mp, sizeof mp, im->width * (double)im->height / 1e6, 2);
    buf_inked(&line, UI_DIM, " · %s MP · %s", mp, t1);
    if (doc.source.total_frames > 1) buf_inked(&line, UI_ACCENT, " · %u %s", doc.source.total_frames, doc.animated || !strcmp(doc.source.format, "GIF") || !strcmp(doc.source.format, "APNG") ? "cuadros" : "imágenes");
    EMIT_LINE();
    ui_key(&line, "alfa", KEY_COLS);
    if (stats_opaque(&st)) buf_inked(&line, UI_DIM, "opaca (%s)", alpha_origin_label(doc.source.alpha_origin));
    else {
        fmt_percent(t1, sizeof t1, (double)st.transparent / st.pixels, 1);
        fmt_percent(t2, sizeof t2, (double)st.partial / st.pixels, 1);
        buf_inked(&line, UI_ALPHA, "%s", alpha_origin_label(doc.source.alpha_origin));
        buf_inked(&line, UI_DIM, " · %s transparente · %s suave · %s", t1, t2, stats_binary_alpha(&st) ? "binaria" : "con semitransparencias");
    }
    EMIT_LINE();
    if (!stats_opaque(&st)) {
        ui_key(&line, "", KEY_COLS);
        ui_sparkline(&line, st.alpha_hist, 16, UI_ALPHA);
        buf_inked(&line, UI_DIM, "  histograma del alfa (0 → 255)");
        EMIT_LINE();
    }
    ui_key(&line, "colores", KEY_COLS);
    fmt_count(t1, sizeof t1, unique);
    buf_inked(&line, UI_NUMBER, "%s", unique ? t1 : "?");
    buf_inked(&line, UI_DIM, " distintos · %s%s", st.gray ? "escala de grises" : "color",
              im->depth == 16 ? (st.reducible_to_8 ? " · 16 bits que caben en 8" : " · 16 bits reales") : "");
    EMIT_LINE();
    ui_key(&line, "perfil", KEY_COLS);
    if (doc.meta.has_cicp) {
        const char *prim = cicp_primaries_name(doc.meta.cicp[0]), *tf = cicp_transfer_name(doc.meta.cicp[1]);
        buf_inked(&line, UI_SUBTLE, "CICP %s · %s", prim ? prim : "?", tf ? tf : "?");
        buf_inked(&line, UI_DIM, " · %u/%u → chunk cICP%s", doc.meta.cicp[0], doc.meta.cicp[1], doc.meta.icc.size ? " (+ iCCP)" : "");
    } else if (doc.meta.icc.size) {
        if (!icc_description(doc.meta.icc.data, doc.meta.icc.size, icc_desc, sizeof icc_desc)) snprintf(icc_desc, sizeof icc_desc, "(sin nombre)");
        fmt_bytes(t1, sizeof t1, doc.meta.icc.size);
        buf_inked(&line, UI_SUBTLE, "%s", icc_desc);
        buf_inked(&line, UI_DIM, " · %s · %s", t1, icc_is_srgb(doc.meta.icc.data, doc.meta.icc.size) ? "es sRGB: se escribe como chunk sRGB (13 bytes)" : "se conserva en iCCP");
    } else if (doc.meta.has_srgb) buf_inked(&line, UI_SUBTLE, "sRGB (chunk)");
    else if (doc.meta.has_gamma) buf_inked(&line, UI_SUBTLE, "gamma %.5f%s", doc.meta.gamma, doc.meta.has_chrm ? " + cromaticidades" : "");
    else buf_inked(&line, UI_DIM, "ninguno (se asume sRGB)");
    EMIT_LINE();
    if (doc.meta.orientation > 1 || doc.meta.exif.size || doc.meta.xmp.size || doc.meta.texts.count) {
        ui_key(&line, "metadatos", KEY_COLS);
        if (doc.meta.orientation > 1) buf_inked(&line, UI_WARN, "orientación EXIF %u (se rota al convertir) ", doc.meta.orientation);
        if (doc.meta.exif.size) { fmt_bytes(t1, sizeof t1, doc.meta.exif.size); buf_inked(&line, UI_DIM, "· EXIF %s ", t1); }
        if (doc.meta.xmp.size) { fmt_bytes(t1, sizeof t1, doc.meta.xmp.size); buf_inked(&line, UI_DIM, "· XMP %s ", t1); }
        if (doc.meta.texts.count) buf_inked(&line, UI_DIM, "· %zu texto%s", doc.meta.texts.count, doc.meta.texts.count == 1 ? "" : "s");
        EMIT_LINE();
    }
    ui_key(&line, "PNG", KEY_COLS);
    if (planned) {
        char pd[64];
        plan_describe(&plan, pd, sizeof pd);
        buf_inked(&line, UI_TEAL, "%s", pd);
        buf_inked(&line, UI_DIM, "  (sin pérdida, lo más compacto posible)");
    } else buf_inked(&line, UI_ERROR, "%s", plan_diag.message);
    EMIT_LINE();
    for (u32 i = 0; i < MIN(diag->warning_count, (u32)DIAG_MAX_WARNINGS); i++) {
        ui_key(&line, i ? "" : "avisos", KEY_COLS);
        buf_inked(&line, UI_WARN, "%s", diag->warnings[i]);
        EMIT_LINE();
    }
#undef EMIT_LINE
    buf_free(&line);
    term_emit_text("\n");
    card_emit(&c, job->in->display, UI_ACCENT);
    decoded_free(&doc);
    return ST_OK;
}

// ---- --formats y --version ------------------------------------------------------------------------------

typedef struct WicList { Card *card; u32 count; } WicList;

static void add_wic_codec(void *ctx, const char *name, const char *exts) {
    WicList *l = ctx;
    Buf line;
    buf_init(&line);
    char cut[96];
    const char *shown = str_starts_with(name, "Microsoft ") ? name + 10 : name;
    utf8_ellipsize(shown, 30, cut, sizeof cut);
    size_t st = line.len;
    buf_inked(&line, UI_FORMAT, "%s", cut);
    buf_pad_to(&line, st, 32);
    char lower[256];
    snprintf(lower, sizeof lower, "%s", exts);
    str_lower_ascii(lower);
    for (char *p = lower; *p; p++) if (*p == ',') *p = ' ';
    utf8_ellipsize(lower, l->card->width > 36 ? l->card->width - 34 : 20, cut, sizeof cut);
    buf_inked(&line, UI_SUBTLE, "%s", cut);
    card_line(l->card, "%s", line.data);
    buf_free(&line);
    l->count++;
}

void report_formats(void) {
    Card c;
    u32 w = MIN(ui_width(), 120u) - 4;
    card_begin(&c, w);
    Buf line;
    buf_init(&line);
    buf_inked(&line, UI_TEAL, "decodificadores propios");
    buf_inked(&line, UI_DIM, "  (rapidos, sin dependencias, con el manejo de alfa de cada formato)");
    card_line(&c, "%s", line.data);
    buf_clear(&line);
    size_t count;
    const Codec *const *list = codec_builtin_list(&count);
    for (size_t i = 0; i < count; i++) {
        size_t st = line.len;
        buf_inked(&line, UI_FORMAT, "%s", list[i]->name);
        buf_pad_to(&line, st, 10);
        st = line.len;
        buf_inked(&line, UI_SUBTLE, "%s", list[i]->extensions);
        buf_pad_to(&line, st, 26);
        char cut[160];
        utf8_ellipsize(list[i]->description, w > 38 ? w - 37 : 30, cut, sizeof cut);
        buf_inked(&line, UI_DIM, "%s", cut);
        card_line(&c, "%s", line.data);
        buf_clear(&line);
    }
    card_rule(&c);
    buf_inked(&line, UI_TEAL, "codecs de Windows (WIC) instalados en este equipo");
    buf_inked(&line, UI_DIM, "  (JPEG, TIFF, HEIF, AVIF, WebP, JPEG XL, RAW...)");
    card_line(&c, "%s", line.data);
    buf_clear(&line);
    WicList l = { &c, 0 };
    wic_enumerate_decoders(add_wic_codec, &l);
    if (!l.count) { buf_inked(&line, UI_DIM, "(WIC no disponible)"); card_line(&c, "%s", line.data); }
    buf_free(&line);
    term_emit_text("\n");
    card_emit(&c, "formatos que lee image2png", UI_ACCENT);
    term_emit_text("\n");
}

void report_version(void) {
    const CpuInfo *cpu = cpu_info();
    Buf b;
    buf_init(&b);
    buf_puts(&b, "\n  ");
    buf_bold(&b);
    buf_inked(&b, UI_ACCENT, "image2png");
    buf_inked(&b, UI_SUBTLE, " %s", PRISMA_VERSION);
    buf_inked(&b, UI_DIM, "  ·  suite prisma  ·  C17 + clang %d.%d  ·  libdeflate 1.26 (con tramos paralelos)\n", __clang_major__, __clang_minor__);
    buf_inked(&b, UI_DIM, "  %s · %u núcleos / %u hilos · AVX2 %s · AVX-512 %s\n\n", cpu->brand, cpu->physical_cores, cpu->logical_cores,
              cpu->avx2 ? "sí" : "no", cpu->avx512 ? "sí" : "no");
    term_emit(&b);
    buf_free(&b);
}
