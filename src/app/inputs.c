// app/inputs.c - ver inputs.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "inputs.h"
#include "../core/fileio.h"
#include "../core/text.h"

enum { SEEN_INITIAL = 1024 };

// ---- conjunto de rutas vistas (hash FNV-1a de la ruta en mayusculas) --------------------------------

typedef struct SeenSet {
    u64   *hashes;
    size_t cap, size;
} SeenSet;

static u64 path_hash(const wchar_t *p) {
    u64 h = 1469598103934665603ull;
    for (; *p; p++) {
        wchar_t c = *p == L'/' ? L'\\' : *p;
        if (c >= L'a' && c <= L'z') c -= 32;
        else if (c > 127) c = (wchar_t)(UINT_PTR)CharUpperW((LPWSTR)(UINT_PTR)c);
        h = (h ^ (u64)c) * 1099511628211ull;
    }
    return h | 1;                                   // 0 = lugar vacio
}

static bool seen_insert(SeenSet *s, u64 h) {
    if ((s->size + 1) * 2 > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : SEEN_INITIAL;
        u64 *t = mem_calloc(cap * sizeof(u64));
        if (!t) return true;                        // sin memoria: no deduplicar antes que fallar
        for (size_t i = 0; i < s->cap; i++) {
            if (!s->hashes[i]) continue;
            size_t j = s->hashes[i] & (cap - 1);
            while (t[j]) j = (j + 1) & (cap - 1);
            t[j] = s->hashes[i];
        }
        mem_free(s->hashes);
        s->hashes = t;
        s->cap = cap;
    }
    size_t j = h & (s->cap - 1);
    while (s->hashes[j]) {
        if (s->hashes[j] == h) return false;
        j = (j + 1) & (s->cap - 1);
    }
    s->hashes[j] = h;
    s->size++;
    return true;
}

// ---- ayudas -------------------------------------------------------------------------------------

static bool has_image_extension(const wchar_t *name, const InputOptions *opt) {
    const wchar_t *ext = path_extension(name);
    if (!*ext) return false;
    char e[16];
    size_t n = 0;
    for (const wchar_t *p = ext + 1; *p && n + 1 < sizeof e; p++) {
        if (*p > 127) return false;
        e[n++] = (char)(*p >= L'A' && *p <= L'Z' ? *p + 32 : *p);
    }
    e[n] = 0;
    for (size_t i = 0; i < opt->extension_count; i++) if (strcmp(e, opt->extensions[i]) == 0) return true;
    return false;
}

static bool add_file(Vec *out, SeenSet *seen, const wchar_t *full, const wchar_t *root, const char *display, u64 size, bool missing) {
    if (full && !seen_insert(seen, path_hash(full))) return true;       // ya estaba
    InputFile *f = vec_push(out);
    if (!f) return false;
    f->path = full ? wstr_dup(full) : NULL;
    f->root = root ? wstr_dup(root) : NULL;
    f->display = mem_strdup(display);
    f->size = size;
    f->missing = missing;
    return f->display && (!full || f->path);
}

static char *display_for(const wchar_t *full, const wchar_t *root, const char *given_root) {
    // Relativa a la carpeta que escribio el usuario: "fotos\viaje\a.jpg", no la ruta absoluta.
    if (root) {
        size_t n = wcslen(root);
        const wchar_t *rel = full + n;
        while (*rel == L'\\' || *rel == L'/') rel++;
        char *r = utf16_to_utf8(rel);
        if (!r) return NULL;
        size_t gl = strlen(given_root), rl = strlen(r);
        bool sep = gl && given_root[gl - 1] != '\\' && given_root[gl - 1] != '/';
        char *out = mem_alloc(gl + rl + 2);
        if (out) snprintf(out, gl + rl + 2, "%s%s%s", given_root, sep ? "\\" : "", r);
        mem_free(r);
        return out;
    }
    return utf16_to_utf8(path_basename(full));
}

static u64 find_data_size(const WIN32_FIND_DATAW *fd) { return (u64)fd->nFileSizeHigh << 32 | fd->nFileSizeLow; }

// Recorrido iterativo con pila de carpetas (sin recursion: arboles profundos no agotan la pila).
static bool walk_directory(Vec *out, SeenSet *seen, const wchar_t *root, const char *given, const InputOptions *opt, u64 *found) {
    Vec stack;
    vec_init(&stack, sizeof(wchar_t *));
    wchar_t **first = vec_push(&stack);
    if (!first || !(*first = wstr_dup(root))) { vec_free(&stack); return false; }
    bool ok = true;
    while (stack.count && ok) {
        wchar_t *dir = VEC_AT(&stack, wchar_t *, stack.count - 1);
        stack.count--;
        wchar_t *pattern = wstr_concat(dir, L"\\*");
        wchar_t *io = pattern ? path_for_io(pattern) : NULL;
        WIN32_FIND_DATAW fd;
        HANDLE h = io ? FindFirstFileExW(io, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH) : INVALID_HANDLE_VALUE;
        mem_free(pattern);
        mem_free(io);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
                wchar_t *sep = wstr_concat(dir, L"\\");
                wchar_t *full = sep ? wstr_concat(sep, fd.cFileName) : NULL;
                mem_free(sep);
                if (!full) { ok = false; break; }
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    // Junctions y links simbolicos a carpetas: no se siguen (pueden formar ciclos).
                    if (opt->recursive && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                        wchar_t **slot = vec_push(&stack);
                        if (slot) { *slot = full; full = NULL; } else ok = false;
                    }
                } else if (has_image_extension(fd.cFileName, opt)) {
                    char *display = display_for(full, root, given);
                    ok = display && add_file(out, seen, full, root, display, find_data_size(&fd), false);
                    mem_free(display);
                    (*found)++;
                }
                mem_free(full);
            } while (ok && FindNextFileW(h, &fd));
            FindClose(h);
        }
        mem_free(dir);
    }
    for (size_t i = 0; i < stack.count; i++) mem_free(VEC_AT(&stack, wchar_t *, i));
    vec_free(&stack);
    return ok;
}

static bool expand_wildcard(Vec *out, SeenSet *seen, const wchar_t *pattern, const char *given, u64 *found) {
    wchar_t *full = path_full(pattern);
    if (!full) return false;
    wchar_t *base = (wchar_t *)path_basename(full);
    wchar_t saved = *base;
    *base = 0;
    wchar_t *dir = wstr_dup(full);                  // carpeta del patron, con su barra final
    *base = saved;
    wchar_t *io = path_for_io(full);
    bool ok = dir != NULL;
    WIN32_FIND_DATAW fd;
    HANDLE h = io && ok ? FindFirstFileExW(io, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH) : INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        // Lo que el usuario escribio antes del patron, para mostrar rutas relativas a eso.
        const char *slash = strrchr(given, '\\');
        const char *fslash = strrchr(given, '/');
        if (fslash > slash) slash = fslash;
        size_t prefix = slash ? (size_t)(slash - given + 1) : 0;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            wchar_t *path = wstr_concat(dir, fd.cFileName);
            char *name = utf16_to_utf8(fd.cFileName);
            char *display = name ? mem_alloc(prefix + strlen(name) + 1) : NULL;
            if (display) { memcpy(display, given, prefix); strcpy(display + prefix, name); }
            ok = path && display && add_file(out, seen, path, NULL, display, find_data_size(&fd), false);
            mem_free(path);
            mem_free(name);
            mem_free(display);
            (*found)++;
        } while (ok && FindNextFileW(h, &fd));
        FindClose(h);
    }
    mem_free(io);
    mem_free(dir);
    mem_free(full);
    return ok;
}

Status inputs_expand(const Vec *positionals, const InputOptions *opt, Vec *out, Diag *diag) {
    SeenSet seen = { 0 };
    Status status = ST_OK;
    for (size_t i = 0; i < positionals->count && status == ST_OK; i++) {
        const char *arg = VEC_AT(positionals, char *, i);
        if (strcmp(arg, "-") == 0) {
            if (!add_file(out, &seen, NULL, NULL, "(stdin)", 0, false)) status = diag_fail(diag, ST_NO_MEMORY, "sin memoria");
            continue;
        }
        wchar_t *w = utf8_to_utf16(arg);
        if (!w) { status = diag_fail(diag, ST_NO_MEMORY, "sin memoria"); break; }
        bool wildcard = strpbrk(arg, "*?") != NULL;
        u64 found = 0;
        if (wildcard) {
            if (!expand_wildcard(out, &seen, w, arg, &found)) status = diag_fail(diag, ST_NO_MEMORY, "sin memoria");
            else if (!found) diag_warn(diag, "\"%s\" no coincide con ningun archivo", arg);
        } else if (path_is_directory(w)) {
            wchar_t *full = path_full(w);
            size_t n = full ? wcslen(full) : 0;
            while (n > 3 && (full[n - 1] == L'\\' || full[n - 1] == L'/')) full[--n] = 0;
            if (!full || !walk_directory(out, &seen, full, arg, opt, &found)) status = diag_fail(diag, ST_NO_MEMORY, "sin memoria");
            else if (!found) diag_warn(diag, "la carpeta \"%s\" no tiene imagenes%s", arg, opt->recursive ? "" : " (-r para incluir subcarpetas)");
            mem_free(full);
        } else {
            wchar_t *full = path_full(w);
            bool exists = full && path_exists(full);
            if (!full || !add_file(out, &seen, full, NULL, arg, exists ? file_size(full) : 0, !exists))
                status = diag_fail(diag, ST_NO_MEMORY, "sin memoria");
            mem_free(full);
        }
        mem_free(w);
    }
    mem_free(seen.hashes);
    return status;
}

void inputs_free(Vec *files) {
    for (size_t i = 0; i < files->count; i++) {
        InputFile *f = &VEC_AT(files, InputFile, i);
        mem_free(f->path);
        mem_free(f->root);
        mem_free(f->display);
    }
    vec_free(files);
}
