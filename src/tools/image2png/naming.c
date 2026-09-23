// tools/image2png/naming.c - a donde va cada salida.
//
//   una entrada, -o archivo       -> ese archivo
//   varias entradas, -o carpeta   -> carpeta\nombre.png (con --keep-tree, la subcarpeta relativa)
//   sin -o                        -> al lado de la entrada
//   -o -                          -> stdout (una sola entrada)
// Colisiones: "a.jpg" y "a.gif" irian los dos a "a.png"; se detectan ANTES de convertir (orden por
// ruta sin distinguir mayusculas, O(n log n)) y todo el grupo pasa a "a.jpg.png" / "a.gif.png": el
// resultado no depende de que hilo termina primero.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include "job.h"
#include "../../core/fileio.h"
#include "../../core/text.h"

static bool same_path(const wchar_t *a, const wchar_t *b) {
    return CompareStringOrdinal(a, -1, b, -1, TRUE) == CSTR_EQUAL;
}

static wchar_t *stem_of(const wchar_t *path) {
    const wchar_t *base = path_basename(path), *ext = path_extension(path);
    size_t n = (size_t)(ext - base);
    wchar_t *s = mem_alloc((n + 1) * sizeof(wchar_t));
    if (s) { memcpy(s, base, n * sizeof(wchar_t)); s[n] = 0; }
    return s;
}

static wchar_t *dir_of(const wchar_t *path) {
    const wchar_t *base = path_basename(path);
    size_t n = (size_t)(base - path);
    while (n > 3 && (path[n - 1] == L'\\' || path[n - 1] == L'/')) n--;
    wchar_t *d = mem_alloc((n + 1) * sizeof(wchar_t));
    if (d) { memcpy(d, path, n * sizeof(wchar_t)); d[n] = 0; }
    return d;
}

static wchar_t *join3(const wchar_t *dir, const wchar_t *name, const wchar_t *tail) {
    size_t a = wcslen(dir), b = wcslen(name), c = tail ? wcslen(tail) : 0;
    bool sep = a && dir[a - 1] != L'\\' && dir[a - 1] != L'/';
    wchar_t *out = mem_alloc((a + b + c + 2) * sizeof(wchar_t));
    if (!out) return NULL;
    memcpy(out, dir, a * sizeof(wchar_t));
    size_t at = a;
    if (sep) out[at++] = L'\\';
    memcpy(out + at, name, b * sizeof(wchar_t));
    at += b;
    if (c) { memcpy(out + at, tail, c * sizeof(wchar_t)); at += c; }
    out[at] = 0;
    return out;
}

// nombre + sufijo + [.ext_original] + ".png"
static wchar_t *output_name(const wchar_t *input, const wchar_t *suffix, bool keep_extension) {
    wchar_t *stem = stem_of(input);
    if (!stem) return NULL;
    const wchar_t *ext = path_extension(input);
    size_t n = wcslen(stem) + (suffix ? wcslen(suffix) : 0) + (keep_extension ? wcslen(ext) : 0) + 5;
    wchar_t *out = mem_alloc(n * sizeof(wchar_t));
    if (out) swprintf(out, n, L"%s%s%s.png", stem, suffix ? suffix : L"", keep_extension ? ext : L"");
    mem_free(stem);
    return out;
}

wchar_t *frame_output_path(const wchar_t *base_png, u32 index, u32 total) {
    const wchar_t *ext = path_extension(base_png);
    size_t stem_len = (size_t)(ext - base_png);
    int digits = 3;
    for (u32 t = total ? total - 1 : 0; t >= 1000; t /= 10) digits++;
    size_t n = stem_len + (size_t)digits + 16;
    wchar_t *out = mem_alloc(n * sizeof(wchar_t));
    if (out) swprintf(out, n, L"%.*s-%0*u.png", (int)stem_len, base_png, digits, index);
    return out;
}

static int compare_outputs(void *ctx, const void *a, const void *b) {
    const Job *jobs = ctx;
    const wchar_t *x = jobs[*(const u32 *)a].output, *y = jobs[*(const u32 *)b].output;
    if (!x || !y) return (x != NULL) - (y != NULL);
    return CompareStringOrdinal(x, -1, y, -1, TRUE) - CSTR_EQUAL;
}

static wchar_t *planned_path(const Job *job, const Config *cfg, const wchar_t *out_dir, const wchar_t *suffix, bool keep_ext) {
    const InputFile *in = job->in;
    wchar_t *name = output_name(in->path, suffix, keep_ext);
    if (!name) return NULL;
    wchar_t *dir = NULL;
    if (out_dir) {
        dir = wstr_dup(out_dir);
        // --keep-tree: la subcarpeta de la entrada relativa a la carpeta que dio el usuario.
        if (dir && cfg->keep_tree && in->root) {
            wchar_t *in_dir = dir_of(in->path);
            size_t root_len = wcslen(in->root);
            if (in_dir && wcslen(in_dir) > root_len) {
                const wchar_t *rel = in_dir + root_len;
                while (*rel == L'\\' || *rel == L'/') rel++;
                wchar_t *deeper = join3(dir, rel, NULL);
                mem_free(dir);
                dir = deeper;
            }
            mem_free(in_dir);
        }
    } else {
        dir = dir_of(in->path);
    }
    wchar_t *path = dir ? join3(dir, name, NULL) : NULL;
    mem_free(dir);
    mem_free(name);
    return path;
}

Status plan_outputs(Job *jobs, size_t count, const Config *cfg, Diag *diag) {
    bool out_given = cfg->output != NULL;
    bool out_stdout = out_given && strcmp(cfg->output, "-") == 0;
    wchar_t *out_w = out_given && !out_stdout ? utf8_to_utf16(cfg->output) : NULL;
    wchar_t *suffix = cfg->suffix ? utf8_to_utf16(cfg->suffix) : NULL;
    size_t out_len = out_w ? wcslen(out_w) : 0;
    bool trailing_sep = out_len && (out_w[out_len - 1] == L'\\' || out_w[out_len - 1] == L'/');
    // Con una sola entrada, -o es el nombre de salida aunque salgan varios cuadros (base-000.png...).
    bool several = count > 1;
    bool out_is_dir = out_w && (trailing_sep || path_is_directory(out_w) || several);
    Status s = ST_OK;

    if (out_stdout && count != 1) s = diag_fail(diag, ST_INVALID, "-o - (stdout) admite una sola entrada, y hay %zu", count);
    else if (out_w && several && !trailing_sep && path_exists(out_w) && !path_is_directory(out_w))
        s = diag_fail(diag, ST_INVALID, "-o apunta a un archivo existente pero hay varias salidas: usa una carpeta");
    else if (cfg->in_place && out_given) s = diag_fail(diag, ST_INVALID, "--in-place reescribe cada .png en su lugar: no combina con -o");
    if (s != ST_OK) { mem_free(out_w); mem_free(suffix); return s; }

    for (size_t i = 0; i < count && s == ST_OK; i++) {
        Job *j = &jobs[i];
        if (!j->in->path) {                                           // stdin
            if (out_w && !out_is_dir) j->output = wstr_dup(out_w);
            else if (out_is_dir) j->output = join3(out_w, L"stdin.png", NULL);
            else j->to_stdout = true;
            continue;
        }
        if (out_stdout) { j->to_stdout = true; continue; }
        if (out_w && !out_is_dir) j->output = wstr_dup(out_w);
        else j->output = planned_path(j, cfg, out_is_dir ? out_w : NULL, suffix, false);
        if (!j->output) s = diag_fail(diag, ST_NO_MEMORY, "sin memoria");
    }

    // Colisiones: ordenar por salida y renombrar grupos repetidos con la extension original.
    u32 *order = s == ST_OK ? mem_alloc(count * sizeof(u32)) : NULL;
    if (order) {
        for (u32 i = 0; i < count; i++) order[i] = i;
        qsort_s(order, count, sizeof(u32), compare_outputs, jobs);
        for (size_t a = 0; a < count;) {
            size_t b = a + 1;
            while (b < count && jobs[order[a]].output && jobs[order[b]].output && same_path(jobs[order[a]].output, jobs[order[b]].output)) b++;
            if (b - a > 1)
                for (size_t k = a; k < b; k++) {
                    Job *j = &jobs[order[k]];
                    if (!j->in->path || (out_w && !out_is_dir)) continue;
                    wchar_t *renamed = planned_path(j, cfg, out_is_dir ? out_w : NULL, suffix, true);
                    if (renamed) { mem_free(j->output); j->output = renamed; }
                }
            a = b;
        }
        mem_free(order);
    }

    // La salida no puede pisar la entrada salvo --in-place.
    for (size_t i = 0; i < count && s == ST_OK; i++) {
        Job *j = &jobs[i];
        if (!j->output || !j->in->path || !same_path(j->output, j->in->path) || cfg->in_place) continue;
        j->skip = true;
        snprintf(j->skip_reason, sizeof j->skip_reason, "la salida seria la misma entrada (--in-place para reoptimizarla)");
    }
    for (size_t i = 0; i < count; i++) {
        Job *j = &jobs[i];
        j->output_display = j->to_stdout ? mem_strdup("(stdout)") : j->output ? utf16_to_utf8(path_basename(j->output)) : mem_strdup("?");
    }
    mem_free(out_w);
    mem_free(suffix);
    return s;
}
