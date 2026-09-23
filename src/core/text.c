// core/text.c - ver text.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "text.h"
#include "mem.h"

char *utf16_to_utf8(const wchar_t *w) {
    if (!w) return NULL;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = mem_alloc((size_t)n);
    if (s && WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL) <= 0) { mem_free(s); return NULL; }
    return s;
}

wchar_t *utf8_to_utf16(const char *s) {
    if (!s) return NULL;
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = mem_alloc((size_t)n * sizeof(wchar_t));
    if (w && MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) <= 0) { mem_free(w); return NULL; }
    return w;
}

wchar_t *wstr_dup(const wchar_t *w) {
    size_t n = (wcslen(w) + 1) * sizeof(wchar_t);
    wchar_t *copy = mem_alloc(n);
    if (copy) memcpy(copy, w, n);
    return copy;
}

wchar_t *wstr_concat(const wchar_t *a, const wchar_t *b) {
    size_t la = wcslen(a), lb = wcslen(b);
    wchar_t *out = mem_alloc((la + lb + 1) * sizeof(wchar_t));
    if (!out) return NULL;
    memcpy(out, a, la * sizeof(wchar_t));
    memcpy(out + la, b, (lb + 1) * sizeof(wchar_t));
    return out;
}

// Decodifica un codepoint UTF-8; devuelve cuantos bytes consumio (>= 1 siempre, para avanzar
// aun sobre bytes invalidos).
static size_t utf8_next(const u8 *p, size_t len, u32 *cp) {
    u8 c = p[0];
    if (c < 0x80) { *cp = c; return 1; }
    size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (need == 1 || need > len) { *cp = 0xFFFD; return 1; }
    u32 v = c & (0x7F >> need);
    for (size_t i = 1; i < need; i++) {
        if ((p[i] & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
        v = v << 6 | (p[i] & 0x3F);
    }
    *cp = v;
    return need;
}

static u32 codepoint_width(u32 cp) {
    if (cp == 0) return 0;
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x200B && cp <= 0x200F) || cp == 0xFE0F) return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0x1F300 && cp <= 0x1FAFF)) return 2;
    return 1;
}

size_t utf8_display_width(const char *s, size_t len) {
    const u8 *p = (const u8 *)s;
    size_t width = 0;
    for (size_t i = 0; i < len;) {
        u32 cp;
        i += utf8_next(p + i, len - i, &cp);
        width += codepoint_width(cp);
    }
    return width;
}

static const char ELLIPSIS[] = "\xE2\x80\xA6";      // …
enum { ELLIPSIS_BYTES = sizeof ELLIPSIS - 1 };

size_t utf8_ellipsize(const char *s, size_t max_cols, char *out, size_t out_cap) {
    size_t len = strlen(s);
    if (out_cap == 0) return 0;
    if (utf8_display_width(s, len) <= max_cols) {
        size_t n = MIN(len, out_cap - 1);
        memcpy(out, s, n);
        out[n] = 0;
        return n;
    }
    const u8 *p = (const u8 *)s;
    size_t cols = 0, i = 0, budget = max_cols > 0 ? max_cols - 1 : 0;
    while (i < len) {
        u32 cp;
        size_t step = utf8_next(p + i, len - i, &cp);
        u32 w = codepoint_width(cp);
        if (cols + w > budget || i + step + sizeof ELLIPSIS > out_cap) break;
        cols += w;
        i += step;
    }
    memcpy(out, s, i);
    memcpy(out + i, ELLIPSIS, sizeof ELLIPSIS);
    return i + ELLIPSIS_BYTES;
}

size_t utf8_prefix_cols(const char *s, size_t len, size_t max_cols, size_t *cols_used) {
    const u8 *p = (const u8 *)s;
    size_t i = 0, cols = 0;
    while (i < len) {
        u32 cp;
        size_t step = utf8_next(p + i, len - i, &cp);
        u32 w = codepoint_width(cp);
        if (cols + w > max_cols) break;
        cols += w;
        i += step;
    }
    if (cols_used) *cols_used = cols;
    return i;
}

size_t utf8_ellipsize_middle(const char *s, size_t max_cols, char *out, size_t out_cap) {
    size_t len = strlen(s);
    if (out_cap == 0) return 0;
    if (utf8_display_width(s, len) <= max_cols || max_cols < MIDDLE_MIN_COLS) return utf8_ellipsize(s, max_cols, out, out_cap);
    size_t budget = max_cols - 1, tail_cols = budget * 2 / 3, head_cols = budget - tail_cols;
    const u8 *p = (const u8 *)s;
    size_t head = 0, cols = 0;
    while (head < len) {                                    // cabeza: de a codepoint, mientras entre
        u32 cp;
        size_t step = utf8_next(p + head, len - head, &cp);
        u32 w = codepoint_width(cp);
        if (cols + w > head_cols) break;
        cols += w;
        head += step;
    }
    size_t tail = len;
    cols = 0;
    while (tail > head) {                                   // cola: hacia atras, al inicio de cada codepoint
        size_t start = tail - 1;
        while (start > head && (p[start] & 0xC0) == 0x80) start--;
        u32 cp;
        utf8_next(p + start, tail - start, &cp);
        u32 w = codepoint_width(cp);
        if (cols + w > tail_cols) break;
        cols += w;
        tail = start;
    }
    size_t total = head + ELLIPSIS_BYTES + (len - tail);
    if (total + 1 > out_cap) return utf8_ellipsize(s, max_cols, out, out_cap);
    memcpy(out, s, head);
    memcpy(out + head, ELLIPSIS, ELLIPSIS_BYTES);
    memcpy(out + head + ELLIPSIS_BYTES, s + tail, len - tail);
    out[total] = 0;
    return total;
}

bool str_ieq(const char *a, const char *b) {
    for (;; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
        if (!ca) return true;
    }
}

bool str_starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

void str_lower_ascii(char *s) {
    for (; *s; s++) if (*s >= 'A' && *s <= 'Z') *s += 32;
}

// Numero con coma decimal: snprintf imprime punto, lo cambiamos (el locale del CRT no se toca:
// es global al proceso y no queremos afectar el JSON, que sí va con punto).
static void decimal_comma(char *s) {
    for (; *s; s++) if (*s == '.') { *s = ','; return; }
}

void fmt_bytes(char *out, size_t cap, u64 bytes) {
    static const char *const UNITS[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)bytes;
    int unit = 0;
    while (v >= 1024.0 && unit < 4) { v /= 1024.0; unit++; }
    if (unit == 0) snprintf(out, cap, "%llu B", (unsigned long long)bytes);
    else snprintf(out, cap, v >= 100.0 ? "%.0f %s" : v >= 10.0 ? "%.1f %s" : "%.2f %s", v, UNITS[unit]);
    decimal_comma(out);
}

void fmt_duration(char *out, size_t cap, double seconds) {
    if (seconds < 0.001) snprintf(out, cap, "%.0f µs", seconds * 1e6);
    else if (seconds < 1.0) snprintf(out, cap, "%.0f ms", seconds * 1e3);
    else if (seconds < 60.0) snprintf(out, cap, "%.2f s", seconds);
    else snprintf(out, cap, "%d min %02d s", (int)(seconds / 60), (int)seconds % 60);
    decimal_comma(out);
}

void fmt_count(char *out, size_t cap, u64 n) {
    char digits[32];
    int len = snprintf(digits, sizeof digits, "%llu", (unsigned long long)n);
    size_t o = 0;
    for (int i = 0; i < len && o + 2 < cap; i++) {
        if (i > 0 && (len - i) % 3 == 0) out[o++] = '.';
        out[o++] = digits[i];
    }
    out[o] = 0;
}

void fmt_percent(char *out, size_t cap, double ratio, int decimals) {
    snprintf(out, cap, "%.*f %%", decimals, ratio * 100.0);
    decimal_comma(out);
}

void fmt_decimal(char *out, size_t cap, double value, int decimals) {
    snprintf(out, cap, "%.*f", decimals, value);
    decimal_comma(out);
}

u32 edit_distance(const char *a, const char *b) {
    enum { MAXLEN = 48 };
    size_t n = MIN(strlen(a), (size_t)MAXLEN), m = MIN(strlen(b), (size_t)MAXLEN);
    // DP de Damerau-Levenshtein (variante "optimal string alignment"): tres filas bastan.
    u32 rows[3][MAXLEN + 1];
    u32 *prev2 = rows[0], *prev = rows[1], *cur = rows[2];
    for (size_t j = 0; j <= m; j++) prev[j] = (u32)j;
    for (size_t i = 1; i <= n; i++) {
        cur[0] = (u32)i;
        for (size_t j = 1; j <= m; j++) {
            u32 cost = a[i - 1] == b[j - 1] ? 0 : 1;
            u32 best = MIN(prev[j] + 1, cur[j - 1] + 1);
            best = MIN(best, prev[j - 1] + cost);
            if (i > 1 && j > 1 && a[i - 1] == b[j - 2] && a[i - 2] == b[j - 1])
                best = MIN(best, prev2[j - 2] + 1);
            cur[j] = best;
        }
        u32 *t = prev2; prev2 = prev; prev = cur; cur = t;
    }
    return prev[m];
}
