// app/cli.c - ver cli.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "cli.h"
#include "../core/fileio.h"
#include "../core/text.h"
#include "../ops/color.h"
#include <stdlib.h>

enum { MAX_SUGGEST_DISTANCE = 3, MAX_RESPONSE_DEPTH = 4 };

const char *cli_suggest(const OptionSpec *table, size_t count, const char *unknown) {
    const char *best = NULL;
    u32 best_d = MAX_SUGGEST_DISTANCE + 1;
    for (size_t i = 0; i < count; i++) {
        u32 d = edit_distance(unknown, table[i].name);
        if (d < best_d) { best_d = d; best = table[i].name; }
    }
    return best;
}

static const OptionSpec *find_long(const OptionSpec *t, size_t n, const char *name, size_t len) {
    for (size_t i = 0; i < n; i++)
        if (strlen(t[i].name) == len && strncmp(t[i].name, name, len) == 0) return &t[i];
    return NULL;
}

static const OptionSpec *find_short(const OptionSpec *t, size_t n, char c) {
    for (size_t i = 0; i < n; i++) if (t[i].letter == c) return &t[i];
    return NULL;
}

// ---- acciones genericas -------------------------------------------------------------------------

#define FIELD(type, cfg, spec) ((type *)((u8 *)(cfg) + (spec)->offset))

void cli_mark(const OptionSpec *s, void *cfg) {
    if (cfg && s->flag) *(bool *)((u8 *)cfg + s->flag - 1) = true;
}

bool cli_parse_number(const char *text, double *out) {
    if (!text || !*text) return false;
    char *end;
    double v = strtod(text, &end);
    if (end == text || *end) return false;
    *out = v;
    return true;
}

static bool out_of_range(const OptionSpec *s, double v, char *err, size_t cap) {
    if (s->min == s->max || (v >= s->min && v <= s->max)) return false;
    if (err) snprintf(err, cap, "tiene que estar entre %g y %g", s->min, s->max);
    return true;
}

bool cli_set_true(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(v); UNUSED(err); UNUSED(cap);
    if (cfg) { *FIELD(bool, cfg, s) = true; cli_mark(s, cfg); }
    return true;
}

bool cli_set_false(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(v); UNUSED(err); UNUSED(cap);
    if (cfg) { *FIELD(bool, cfg, s) = false; cli_mark(s, cfg); }
    return true;
}

static bool integer_value(const OptionSpec *s, const char *v, double *out, char *err, size_t cap) {
    double d;
    if (!cli_parse_number(v, &d) || d != (double)(i64)d) { if (err) snprintf(err, cap, "se esperaba un numero entero, no \"%s\"", v ? v : ""); return false; }
    if (out_of_range(s, d, err, cap)) return false;
    *out = d;
    return true;
}

bool cli_set_u32(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    double d = 0;
    if (!integer_value(s, v, &d, err, cap)) return false;
    if (d < 0) { if (err) snprintf(err, cap, "no puede ser negativo"); return false; }
    if (cfg) { *FIELD(u32, cfg, s) = (u32)d; cli_mark(s, cfg); }
    return true;
}

bool cli_set_int(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    double d;
    if (!integer_value(s, v, &d, err, cap)) return false;
    if (cfg) { *FIELD(int, cfg, s) = (int)d; cli_mark(s, cfg); }
    return true;
}

bool cli_set_float(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    double d;
    if (!cli_parse_number(v, &d)) { if (err) snprintf(err, cap, "se esperaba un numero, no \"%s\"", v ? v : ""); return false; }
    if (out_of_range(s, d, err, cap)) return false;
    if (cfg) { *FIELD(float, cfg, s) = (float)d; cli_mark(s, cfg); }
    return true;
}

bool cli_set_percent(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    char tmp[64];
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= sizeof tmp) { if (err) snprintf(err, cap, "se esperaba un porcentaje (p.ej. 10 o 10%%)"); return false; }
    memcpy(tmp, v, n + 1);
    if (tmp[n - 1] == '%') tmp[n - 1] = 0;
    double d;
    if (!cli_parse_number(tmp, &d)) { if (err) snprintf(err, cap, "se esperaba un porcentaje (p.ej. 10 o 10%%), no \"%s\"", v); return false; }
    if (out_of_range(s, d, err, cap)) return false;
    if (cfg) { *FIELD(float, cfg, s) = (float)(d / 100.0); cli_mark(s, cfg); }
    return true;
}

bool cli_set_enum(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    if (!v) { if (err) snprintf(err, cap, "falta el valor (%s)", s->choices); return false; }
    int index = 0;
    for (const char *p = s->choices; *p; index++) {
        const char *bar = strchr(p, '|');
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        if (strlen(v) == len && _strnicmp(v, p, len) == 0) {
            if (cfg) { *FIELD(int, cfg, s) = index; cli_mark(s, cfg); }
            return true;
        }
        if (!bar) break;
        p = bar + 1;
    }
    if (err) snprintf(err, cap, "\"%s\" no es una opcion valida (%s)", v, s->choices);
    return false;
}

bool cli_set_string(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    if (!v || !*v) { if (err) snprintf(err, cap, "no puede estar vacio"); return false; }
    if (cfg) {
        char **field = FIELD(char *, cfg, s);
        mem_free(*field);
        *field = mem_strdup(v);
        cli_mark(s, cfg);
    }
    return true;
}

bool cli_set_color(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    Rgba c;
    if (!v || !color_parse(v, &c, err, cap)) return false;
    if (cfg) { *FIELD(Rgba, cfg, s) = c; cli_mark(s, cfg); }
    return true;
}

// ---- argumentos: UTF-8 + @archivos ----------------------------------------------------------------

static bool push_arg(Vec *args, const char *s, size_t len) {
    char **slot = vec_push(args);
    if (!slot) return false;
    *slot = mem_alloc(len + 1);
    if (!*slot) return false;
    memcpy(*slot, s, len);
    (*slot)[len] = 0;
    return true;
}

static bool expand_response(Vec *args, const char *path_utf8, int depth, char *error, size_t cap) {
    if (depth > MAX_RESPONSE_DEPTH) { snprintf(error, cap, "@archivos anidados demasiado profundo"); return false; }
    wchar_t *wpath = utf8_to_utf16(path_utf8);
    FileData f;
    Diag d = { 0 };
    bool ok = wpath && file_read_all(wpath, &f, &d) == ST_OK;
    mem_free(wpath);
    if (!ok) { snprintf(error, cap, "no se pudo leer @%s: %s", path_utf8, d.message); return false; }
    const char *p = (const char *)f.data, *end = p + f.size;
    if (end - p >= 3 && memcmp(p, "\xEF\xBB\xBF", 3) == 0) p += 3;          // BOM de UTF-8
    while (p < end && ok) {
        const char *line = p;
        while (p < end && *p != '\n') p++;
        const char *e = p;
        if (p < end) p++;
        while (e > line && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) e--;
        while (line < e && (*line == ' ' || *line == '\t')) line++;
        if (line == e || *line == '#') continue;                           // vacias y comentarios
        if (e - line >= 2 && *line == '"' && e[-1] == '"') { line++; e--; }
        if (*line == '@') {
            char nested[1024];
            size_t n = MIN((size_t)(e - line - 1), sizeof nested - 1);
            memcpy(nested, line + 1, n);
            nested[n] = 0;
            ok = expand_response(args, nested, depth + 1, error, cap);
        } else {
            ok = push_arg(args, line, (size_t)(e - line));
        }
    }
    file_data_free(&f);
    return ok;
}

bool cli_parse(int argc, wchar_t **argv, const OptionSpec *table, size_t count, void *config, CliResult *out) {
    memset(out, 0, sizeof *out);
    vec_init(&out->positionals, sizeof(char *));
    Vec args;
    vec_init(&args, sizeof(char *));
    bool ok = true;
    for (int i = 1; i < argc && ok; i++) {
        char *s = utf16_to_utf8(argv[i]);
        if (!s) { snprintf(out->error, sizeof out->error, "argumento %d no es texto valido", i); ok = false; break; }
        if (s[0] == '@' && s[1]) ok = expand_response(&args, s + 1, 1, out->error, sizeof out->error);
        else ok = push_arg(&args, s, strlen(s));
        mem_free(s);
    }
    char **a = args.items;
    bool options_done = false;
    for (size_t i = 0; i < args.count && ok; i++) {
        const char *arg = a[i];
        if (options_done || arg[0] != '-' || strcmp(arg, "-") == 0) {
            ok = push_arg(&out->positionals, arg, strlen(arg));
            continue;
        }
        if (strcmp(arg, "--") == 0) { options_done = true; continue; }
        char err[256] = "";
        if (arg[1] == '-') {
            const char *name = arg + 2, *eq = strchr(name, '=');
            size_t len = eq ? (size_t)(eq - name) : strlen(name);
            const OptionSpec *spec = find_long(table, count, name, len);
            if (!spec) {
                char unknown[64];
                snprintf(unknown, sizeof unknown, "%.*s", (int)MIN(len, sizeof unknown - 1), name);
                const char *guess = cli_suggest(table, count, unknown);
                if (guess) snprintf(out->error, sizeof out->error, "opcion desconocida --%s (¿quisiste decir --%s?)", unknown, guess);
                else snprintf(out->error, sizeof out->error, "opcion desconocida --%s (image2png --help lista todas)", unknown);
                ok = false;
                break;
            }
            const char *value = NULL;
            if (eq) {
                if (spec->arg == ARG_NONE) { snprintf(out->error, sizeof out->error, "--%s no lleva valor", spec->name); ok = false; break; }
                value = eq + 1;
            } else if (spec->arg == ARG_REQUIRED) {
                if (i + 1 >= args.count) { snprintf(out->error, sizeof out->error, "--%s necesita un valor (%s)", spec->name, spec->metavar); ok = false; break; }
                value = a[++i];
            } else if (spec->arg == ARG_OPTIONAL && i + 1 < args.count && a[i + 1][0] != '-' && spec->action(spec, NULL, a[i + 1], NULL, 0)) {
                // Valor opcional: se toma el siguiente argumento solo si la accion (en modo sondeo,
                // config = NULL) lo reconoce como valor valido: "--trim foto.png" no se come el archivo.
                value = a[++i];
            }
            if (!spec->action(spec, config, value, err, sizeof err)) {
                snprintf(out->error, sizeof out->error, "--%s: %s", spec->name, err);
                ok = false;
            }
            continue;
        }
        // Cortas: -q, -qr, -o salida, -osalida, -j8
        for (const char *c = arg + 1; *c && ok; c++) {
            const OptionSpec *spec = find_short(table, count, *c);
            if (!spec) { snprintf(out->error, sizeof out->error, "opcion desconocida -%c", *c); ok = false; break; }
            const char *value = NULL;
            if (spec->arg != ARG_NONE) {
                if (c[1]) value = c + 1;
                else if (spec->arg == ARG_REQUIRED) {
                    if (i + 1 >= args.count) { snprintf(out->error, sizeof out->error, "-%c necesita un valor (%s)", *c, spec->metavar); ok = false; break; }
                    value = a[++i];
                }
            }
            if (!spec->action(spec, config, value, err, sizeof err)) {
                snprintf(out->error, sizeof out->error, "-%c (--%s): %s", *c, spec->name, err);
                ok = false;
            }
            if (value) break;
        }
    }
    for (size_t i = 0; i < args.count; i++) mem_free(a[i]);
    vec_free(&args);
    return ok;
}

void cli_result_free(CliResult *r) {
    for (size_t i = 0; i < r->positionals.count; i++) mem_free(VEC_AT(&r->positionals, char *, i));
    vec_free(&r->positionals);
}
