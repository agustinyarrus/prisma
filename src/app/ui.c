// app/ui.c - ver ui.h
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "ui.h"
#include "../core/mem.h"
#include "../core/text.h"

enum { MIN_WIDTH = 60, MAX_WIDTH = 160, SIDE_MARGIN = 4, CARD_TITLE_MARGIN = 4 };

u32 ui_width(void) {
    u32 cols = term_columns();
    return CLAMP(cols > SIDE_MARGIN ? cols - SIDE_MARGIN : cols, (u32)MIN_WIDTH, (u32)MAX_WIDTH);
}

static u32 mix(u32 a, u32 b, double t) {
    u32 out = 0;
    for (int s = 16; s >= 0; s -= 8) {
        double ca = (a >> s) & 255, cb = (b >> s) & 255;
        out |= (u32)(ca + (cb - ca) * t + 0.5) << s;
    }
    return out;
}

static u32 gradient(double t) {
    return t < 0.5 ? mix(INK_TEAL, INK_LAVENDER, t * 2.0) : mix(INK_LAVENDER, INK_PINK, (t - 0.5) * 2.0);
}

void ui_bar(Buf *b, double fraction, u32 cells) {
    fraction = CLAMP(fraction, 0.0, 1.0);
    u32 filled = (u32)(fraction * cells + 0.5);
    for (u32 i = 0; i < cells; i++) {
        if (i < filled) {
            if (term_colors()) buf_ink(b, gradient(cells > 1 ? (double)i / (cells - 1) : 0.0));
            buf_puts(b, "━");
        } else {
            if (i == filled) buf_ink(b, INK_FAINT);
            buf_puts(b, "─");
        }
    }
    buf_reset(b);
}

void ui_sparkline(Buf *b, const u64 *values, u32 count, u32 ink) {
    static const char *const LEVELS[8] = { "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
    u64 max = 0;
    for (u32 i = 0; i < count; i++) max = MAX(max, values[i]);
    buf_ink(b, ink);
    for (u32 i = 0; i < count; i++) {
        if (!values[i]) { buf_ink(b, INK_FAINT); buf_puts(b, LEVELS[0]); buf_ink(b, ink); continue; }
        double t = log1p((double)values[i]) / log1p((double)max);
        buf_puts(b, LEVELS[MIN(7, (u32)(t * 7.0 + 0.5))]);
    }
    buf_reset(b);
}

void card_begin(Card *c, u32 width) {
    buf_init(&c->body);
    c->width = width;
}

void card_line(Card *c, const char *fmt, ...) {
    Buf line;
    buf_init(&line);
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        char *tmp = mem_alloc((size_t)n + 1);
        if (tmp) {
            va_start(ap, fmt);
            vsnprintf(tmp, (size_t)n + 1, fmt, ap);
            va_end(ap);
            buf_append(&line, tmp, (size_t)n);
            mem_free(tmp);
        }
    }
    // Cada linea se guarda separada por \n; card_emit pone los bordes y el relleno.
    buf_append(&c->body, line.data ? line.data : "", line.len);
    buf_puts(&c->body, "\n");
    buf_free(&line);
}

void card_rule(Card *c) { buf_puts(&c->body, "\x01\n"); }

void card_emit(Card *c, const char *title, u32 title_ink) {
    Buf out;
    buf_init(&out);
    u32 w = c->width;
    buf_puts(&out, "  ");
    buf_ink(&out, INK_FAINT);
    buf_puts(&out, "╭─ ");
    buf_reset(&out);
    buf_bold(&out);
    // El titulo suele ser una ruta: si no entra en el borde de arriba, se corta por el medio.
    char shown[512];
    utf8_ellipsize_middle(title, w > CARD_TITLE_MARGIN ? w - CARD_TITLE_MARGIN : w, shown, sizeof shown);
    buf_inked(&out, title_ink, "%s", shown);
    buf_ink(&out, INK_FAINT);
    buf_puts(&out, " ");
    size_t used = visible_width(shown, strlen(shown)) + 3;
    if (used < w + 1) buf_repeat(&out, "─", w + 1 - used);
    buf_puts(&out, "╮");
    buf_reset(&out);
    buf_puts(&out, "\n");
    const char *p = c->body.data ? c->body.data : "";
    while (*p) {
        const char *e = strchr(p, '\n');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        buf_puts(&out, "  ");
        buf_ink(&out, INK_FAINT);
        if (n == 1 && p[0] == '\x01') {
            buf_puts(&out, "├");
            buf_repeat(&out, "╌", w + 1);
            buf_puts(&out, "┤");
            buf_reset(&out);
        } else {
            buf_puts(&out, "│ ");
            buf_reset(&out);
            size_t start = out.len;
            if (visible_width(p, n) > w) {                  // nunca pisar el borde derecho
                buf_append(&out, p, visible_prefix(p, n, w - 1));
                buf_reset(&out);
                buf_ink(&out, INK_FAINT);
                buf_puts(&out, "…");
            } else {
                buf_append(&out, p, n);
            }
            buf_reset(&out);
            buf_pad_to(&out, start, w);
            buf_ink(&out, INK_FAINT);
            buf_puts(&out, "│");
            buf_reset(&out);
        }
        buf_puts(&out, "\n");
        p = e ? e + 1 : p + n;
    }
    buf_puts(&out, "  ");
    buf_ink(&out, INK_FAINT);
    buf_puts(&out, "╰");
    buf_repeat(&out, "─", w + 1);
    buf_puts(&out, "╯");
    buf_reset(&out);
    buf_puts(&out, "\n");
    term_emit(&out);
    buf_free(&out);
    buf_free(&c->body);
}

void ui_key(Buf *b, const char *key, u32 width) {
    size_t start = b->len;
    buf_inked(b, UI_SUBTLE, "%s", key);
    buf_pad_to(b, start, width);
}
