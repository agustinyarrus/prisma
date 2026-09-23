// core/term.c - ver term.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include "term.h"
#include "mem.h"
#include "text.h"

static HANDLE  g_err = INVALID_HANDLE_VALUE;
static bool    g_console;
static bool    g_colors;
static DWORD   g_saved_mode;
static bool    g_mode_changed;
static SRWLOCK g_ui_lock = SRWLOCK_INIT;
static Buf     g_status;                  // linea de estado vigente (vacia = no hay)

void term_init(ColorMode mode) {
    g_err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD console_mode = 0;
    g_console = g_err != INVALID_HANDLE_VALUE && GetConsoleMode(g_err, &console_mode);
    bool vt = false;
    if (g_console) {
        g_saved_mode = console_mode;
        vt = (console_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) ||
             SetConsoleMode(g_err, console_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | ENABLE_PROCESSED_OUTPUT);
        g_mode_changed = vt && !(console_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING);
    }
    char no_color[4];
    bool env_off = GetEnvironmentVariableA("NO_COLOR", no_color, sizeof no_color) > 0;
    g_colors = mode == TERM_COLOR_ALWAYS || (mode == TERM_COLOR_AUTO && g_console && vt && !env_off);
    buf_init(&g_status);
}

void term_shutdown(void) {
    term_status_clear();
    if (g_mode_changed) SetConsoleMode(g_err, g_saved_mode);
    buf_free(&g_status);
}

bool term_colors(void) { return g_colors; }
bool term_interactive(void) { return g_console; }

u32 term_columns(void) {
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (g_console && GetConsoleScreenBufferInfo(g_err, &info)) {
        int cols = info.srWindow.Right - info.srWindow.Left + 1;
        if (cols >= 40) return (u32)cols;
    }
    return 120;
}

// ---- Buf ------------------------------------------------------------------------------------

void buf_init(Buf *b) { b->data = NULL; b->len = b->cap = 0; }
void buf_free(Buf *b) { mem_free(b->data); buf_init(b); }
void buf_clear(Buf *b) { b->len = 0; if (b->data) b->data[0] = 0; }

static bool buf_grow(Buf *b, size_t extra) {
    size_t need;
    if (!add_ok(b->len, extra + 1, &need)) return false;
    if (need <= b->cap) return true;
    size_t cap = MAX(need, b->cap < 256 ? 256 : b->cap * 2);
    char *data = mem_realloc(b->data, cap);
    if (!data) return false;
    b->data = data;
    b->cap = cap;
    return true;
}

void buf_append(Buf *b, const char *s, size_t n) {
    if (!n || !buf_grow(b, n)) return;
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}

void buf_puts(Buf *b, const char *s) { buf_append(b, s, strlen(s)); }

static void buf_vprintf(Buf *b, const char *fmt, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n <= 0 || !buf_grow(b, (size_t)n)) return;
    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap);
    b->len += (size_t)n;
}

void buf_printf(Buf *b, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
}

void buf_repeat(Buf *b, const char *s, size_t times) {
    size_t n = strlen(s);
    for (size_t i = 0; i < times; i++) buf_append(b, s, n);
}

void buf_ink(Buf *b, u32 rgb) {
    if (g_colors) buf_printf(b, "\x1b[38;2;%u;%u;%um", (rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
}

void buf_bold(Buf *b)  { if (g_colors) buf_puts(b, "\x1b[1m"); }
void buf_reset(Buf *b) { if (g_colors) buf_puts(b, "\x1b[0m"); }

void buf_inked(Buf *b, u32 rgb, const char *fmt, ...) {
    buf_ink(b, rgb);
    va_list ap;
    va_start(ap, fmt);
    buf_vprintf(b, fmt, ap);
    va_end(ap);
    buf_reset(b);
}

size_t visible_width(const char *s, size_t len) {
    size_t cols = 0, run_start = 0, i = 0;
    while (i < len) {
        if (s[i] == 0x1b && i + 1 < len && s[i + 1] == '[') {
            cols += utf8_display_width(s + run_start, i - run_start);
            i += 2;
            while (i < len && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) i++;
            i++;
            run_start = i;
        } else {
            i++;
        }
    }
    if (run_start < len) cols += utf8_display_width(s + run_start, len - run_start);
    return cols;
}

INLINE bool is_csi_final(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

size_t visible_prefix(const char *s, size_t len, size_t max_cols) {
    size_t cols = 0, i = 0;
    while (i < len) {
        if (s[i] == 0x1b && i + 1 < len && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < len && !is_csi_final(s[j])) j++;
            i = MIN(j + 1, len);
            continue;
        }
        size_t run_end = i;
        while (run_end < len && s[run_end] != 0x1b) run_end++;
        size_t used;
        size_t take = utf8_prefix_cols(s + i, run_end - i, max_cols - cols, &used);
        cols += used;
        i += take;
        if (i < run_end) return i;                          // el tramo no entro entero: aca se corta
    }
    return i;
}

void buf_pad_to(Buf *b, size_t line_start, size_t cols) {
    size_t used = visible_width(b->data + line_start, b->len - line_start);
    if (used < cols) buf_repeat(b, " ", cols - used);
}

// ---- emision --------------------------------------------------------------------------------

static void write_raw(HANDLE h, bool console, const char *s, size_t len) {
    if (!len) return;
    if (console) {
        int wn = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, NULL, 0);
        if (wn <= 0) return;
        wchar_t stack[2048];
        wchar_t *w = wn <= (int)ARRAY_LEN(stack) ? stack : mem_alloc((size_t)wn * sizeof(wchar_t));
        if (!w) return;
        MultiByteToWideChar(CP_UTF8, 0, s, (int)len, w, wn);
        DWORD done;
        WriteConsoleW(h, w, (DWORD)wn, &done, NULL);
        if (w != stack) mem_free(w);
    } else {
        DWORD done;
        WriteFile(h, s, (DWORD)len, &done, NULL);
    }
}

// Sin colores (archivo, pipe, NO_COLOR) las secuencias ANSI no se escriben: se filtran aca, asi
// quien arma el texto no tiene que preguntar en cada linea.
static void write_ui(const char *s, size_t len) {
    if (g_colors) { write_raw(g_err, g_console, s, len); return; }
    size_t start = 0, i = 0;
    while (i < len) {
        if (s[i] == 0x1b && i + 1 < len && s[i + 1] == '[') {
            write_raw(g_err, g_console, s + start, i - start);
            i += 2;
            while (i < len && !((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z'))) i++;
            start = ++i;
        } else i++;
    }
    write_raw(g_err, g_console, s + start, len - start);
}

static const char CLEAR_LINE[] = "\r\x1b[2K";

void term_emit(const Buf *b) {
    if (!b->len) return;
    AcquireSRWLockExclusive(&g_ui_lock);
    bool status = g_console && g_status.len;
    if (status) write_raw(g_err, true, CLEAR_LINE, sizeof CLEAR_LINE - 1);
    write_ui(b->data, b->len);
    if (status) write_ui(g_status.data, g_status.len);
    ReleaseSRWLockExclusive(&g_ui_lock);
}

void term_emit_text(const char *s) {
    Buf b = { (char *)s, strlen(s), 0 };
    term_emit(&b);
}

void term_status(const Buf *line) {
    if (!g_console) return;
    AcquireSRWLockExclusive(&g_ui_lock);
    buf_clear(&g_status);
    buf_append(&g_status, line->data, line->len);
    write_raw(g_err, true, CLEAR_LINE, sizeof CLEAR_LINE - 1);
    write_ui(g_status.data, g_status.len);
    ReleaseSRWLockExclusive(&g_ui_lock);
}

void term_status_clear(void) {
    if (!g_console) return;
    AcquireSRWLockExclusive(&g_ui_lock);
    if (g_status.len) write_raw(g_err, true, CLEAR_LINE, sizeof CLEAR_LINE - 1);
    buf_clear(&g_status);
    ReleaseSRWLockExclusive(&g_ui_lock);
}

void stdout_emit(const Buf *b) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    bool console = GetConsoleMode(out, &mode);
    write_raw(out, console, b->data, b->len);
}
