// core/term.h - salida de consola con estilo: colores pastel, anchos reales y una linea de estado.
//
// Toda la interfaz sale por stderr (stdout queda limpio para el PNG con `-o -` o el JSON).
// En consola se escribe con WriteConsoleW (UTF-16): los acentos, las cajas redondeadas y los
// bloques de las barras se ven bien sin tocar la pagina de codigos de la consola del usuario
// (SetConsoleOutputCP cambia la de la sesion entera y queda cambiada al salir). Redirigido a
// archivo o pipe, sale UTF-8 plano y sin colores.
//
// La "linea de estado" es la barra de progreso fija abajo: cada term_emit la borra, escribe lo
// suyo y la redibuja, todo bajo un lock, asi los hilos no intercalan medias lineas.
#pragma once
#include "base.h"

typedef enum ColorMode { TERM_COLOR_AUTO, TERM_COLOR_ALWAYS, TERM_COLOR_NEVER } ColorMode;

// Paleta: negro puro de fondo (el de la terminal), acentos Catppuccin claros.
enum {
    INK_TEXT     = 0xcdd6f4,
    INK_SUBTLE   = 0xa6adc8,
    INK_DIM      = 0x7f849c,
    INK_FAINT    = 0x45475a,
    INK_TEAL     = 0x8fd6cc,
    INK_LAVENDER = 0xc4b5fd,
    INK_PEACH    = 0xf6c0a0,
    INK_PINK     = 0xf3b9d2,
    INK_SKY      = 0xa8cff2,
    INK_CREAM    = 0xeedfb8,
    INK_SAGE     = 0xb5dfa8,
    INK_ROSE     = 0xf5a3b5,
};

void term_init(ColorMode mode);
void term_shutdown(void);                  // restaura el modo de la consola
bool term_colors(void);
bool term_interactive(void);               // stderr es una consola de verdad
u32  term_columns(void);

// ---- constructor de texto -----------------------------------------------------------------
typedef struct Buf {
    char  *data;
    size_t len;
    size_t cap;
} Buf;

void buf_init(Buf *b);
void buf_free(Buf *b);
void buf_clear(Buf *b);
void buf_append(Buf *b, const char *s, size_t n);
void buf_puts(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...) PRINTF_LIKE(2, 3);
void buf_repeat(Buf *b, const char *s, size_t times);
void buf_ink(Buf *b, u32 rgb);             // color de texto (no-op sin colores)
void buf_bold(Buf *b);
void buf_reset(Buf *b);
void buf_inked(Buf *b, u32 rgb, const char *fmt, ...) PRINTF_LIKE(3, 4);   // color + texto + reset
// Rellena con espacios hasta que lo escrito desde `line_start` ocupe `cols` columnas visibles.
void buf_pad_to(Buf *b, size_t line_start, size_t cols);
size_t visible_width(const char *s, size_t len);   // columnas, salteando secuencias ANSI
// Bytes del prefijo mas largo que ocupa a lo sumo max_cols columnas visibles: las secuencias ANSI
// no ocupan lugar y nunca se cortan por la mitad, ni tampoco un codepoint.
size_t visible_prefix(const char *s, size_t len, size_t max_cols);

// ---- emision ------------------------------------------------------------------------------
void term_emit(const Buf *b);              // a stderr, respetando la linea de estado
void term_emit_text(const char *s);
void term_status(const Buf *line);         // fija/actualiza la linea de estado (solo consola)
void term_status_clear(void);
void stdout_emit(const Buf *b);            // a stdout, UTF-8 crudo (JSON, --info redirigido)
