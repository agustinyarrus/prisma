// app/ui.h - piezas visuales de las herramientas: barra de progreso con degrade pastel, sparklines,
// tarjetas de bordes redondeados y pares clave/valor alineados. Todo se arma en un Buf y se emite de
// una (sin parpadeo); sin colores (archivo, pipe, NO_COLOR) queda texto prolijo igual.
#pragma once
#include "../core/term.h"

// Colores con significado (una sola tabla: si la paleta cambia, cambia en todos lados).
enum {
    UI_OK = INK_SAGE, UI_ERROR = INK_ROSE, UI_WARN = INK_CREAM, UI_ACCENT = INK_LAVENDER,
    UI_NUMBER = INK_SKY, UI_FORMAT = INK_PEACH, UI_ALPHA = INK_PINK, UI_PATH = INK_TEXT,
    UI_DIM = INK_DIM, UI_SUBTLE = INK_SUBTLE, UI_TEAL = INK_TEAL,
};

// Barra de `cells` columnas llenada a `fraction` (0..1), con degrade teal -> lavanda -> rosa.
void ui_bar(Buf *b, double fraction, u32 cells);

// Mini grafico de barras ▁▂▃▄▅▆▇█ (escala logaritmica: los picos no aplastan el resto).
void ui_sparkline(Buf *b, const u64 *values, u32 count, u32 ink);

// Tarjeta: titulo en el borde de arriba y lineas (pueden traer colores) con relleno a `width`.
typedef struct Card {
    Buf  body;
    u32  width;           // ancho interior en columnas
} Card;

void card_begin(Card *c, u32 width);
void card_line(Card *c, const char *fmt, ...) PRINTF_LIKE(2, 3);   // una linea (sin \n)
void card_rule(Card *c);                                           // separador fino
void card_emit(Card *c, const char *title, u32 title_ink);         // arma bordes, emite y libera

// "clave      valor" con la clave en color tenue y ancho fijo.
void ui_key(Buf *b, const char *key, u32 width);

// Ancho util para tarjetas y tablas: el de la terminal, con margen, entre 60 y 160.
u32 ui_width(void);
