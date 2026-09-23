// core/text.h - UTF-8 <-> UTF-16 y utilidades de cadenas.
//
// Adentro todo es UTF-8 (lo que se imprime, las opciones, los mensajes); las rutas se guardan
// en UTF-16 porque es lo unico que las APIs *W de Windows aceptan sin perder nada: una carpeta
// "Diseño/Ñandú 🎨" tiene que funcionar igual que "C:\temp".
#pragma once
#include "base.h"
#include <wchar.h>

char    *utf16_to_utf8(const wchar_t *w);              // mem_alloc; NULL si falla
wchar_t *utf8_to_utf16(const char *s);                 // mem_alloc; NULL si falla
wchar_t *wstr_dup(const wchar_t *w);
wchar_t *wstr_concat(const wchar_t *a, const wchar_t *b);

// Columnas de terminal que ocupa un texto UTF-8 (sin secuencias ANSI): los combinantes no
// ocupan, los CJK y emoji ocupan 2. Suficiente para alinear tablas con nombres con acentos.
size_t utf8_display_width(const char *s, size_t len);

// Corta s para que ocupe a lo sumo max_cols columnas; si recorta, termina en "…".
// Devuelve bytes escritos en out (sin contar el 0 final).
size_t utf8_ellipsize(const char *s, size_t max_cols, char *out, size_t out_cap);
// Igual, pero corta por el MEDIO ("C:/fotos/…/playa.png"): en una ruta lo que importa es el
// principio (la unidad) y sobre todo el final (el nombre); la cola se lleva dos tercios del lugar.
enum { MIDDLE_MIN_COLS = 8 };
// Bytes del prefijo mas largo de s[0..len) que ocupa a lo sumo max_cols columnas (sin partir un
// codepoint). En *cols_used, las columnas que ocupa (puede ser NULL).
size_t utf8_prefix_cols(const char *s, size_t len, size_t max_cols, size_t *cols_used);
size_t utf8_ellipsize_middle(const char *s, size_t max_cols, char *out, size_t out_cap);

bool   str_ieq(const char *a, const char *b);          // compara ignorando mayusculas ASCII
bool   str_starts_with(const char *s, const char *prefix);
void   str_lower_ascii(char *s);

// Formatos para humanos en castellano rioplatense: coma decimal, "1,5 MB", "142 ms", "3,2 s".
void   fmt_bytes(char *out, size_t cap, u64 bytes);
void   fmt_duration(char *out, size_t cap, double seconds);
void   fmt_count(char *out, size_t cap, u64 n);        // 1.234.567
void   fmt_percent(char *out, size_t cap, double ratio_0_1, int decimals);
void   fmt_decimal(char *out, size_t cap, double value, int decimals);     // 13,7

// Distancia de Damerau-Levenshtein (programacion dinamica O(n*m)) para "¿quisiste decir…?".
u32    edit_distance(const char *a, const char *b);
