// png/analyze.h - una pasada sobre la imagen para saber que usa DE VERDAD.
//
// De esta foto sale el formato de salida sin perdida mas chico posible: si no hay alfa real, no
// se guarda canal alfa; si todo es gris, gris; si hay <= 256 colores, paleta (con el alfa en
// tRNS); si una foto de 16 bits en realidad usa solo valores de 8, 8 bits. La pasada es O(n),
// en paralelo por bandas de filas, y cada banda lleva su propio conjunto de colores (tabla hash
// abierta de 512 lugares); al final se unen.
#pragma once
#include "../core/image.h"

enum { MAX_PALETTE = 256 };

typedef struct ColorStats {
    u32  depth;                   // 8 o 16 (de la imagen)
    u64  pixels;
    u64  transparent, partial, opaque;   // alfa == 0 / intermedio / maximo
    bool gray;                    // R == G == B en todos los pixeles
    bool gray_visible;            // R == G == B en los pixeles con alfa > 0
    bool reducible_to_8;          // 16 bits cuyo byte alto == bajo en todas las muestras
    u32  gray_levels[8];          // bitset de niveles de gris usados (8 bits)
    // Colores RGBA8 distintos (colors > 256 = "muchos"). counts = frecuencia de cada uno.
    u32  colors;
    u32  palette[MAX_PALETTE];
    u32  counts[MAX_PALETTE];
    // Pixeles 100 % transparentes: ¿comparten un mismo RGB? (para usar tRNS con color clave)
    bool transparent_same_rgb;
    u16  transparent_rgb[3];      // escala de 16 bits
    // Histograma del alfa en 16 cubetas (para --info).
    u64  alpha_hist[16];
} ColorStats;

INLINE bool stats_opaque(const ColorStats *s)       { return s->transparent == 0 && s->partial == 0; }
INLINE bool stats_binary_alpha(const ColorStats *s) { return s->partial == 0; }
INLINE bool stats_palettable(const ColorStats *s)   { return s->colors <= MAX_PALETTE && (s->depth == 8 || s->reducible_to_8); }

void analyze_image(const Image *image, ColorStats *out);
// Une las estadisticas de varios cuadros (APNG): un solo formato de salida para todos.
void analyze_merge(ColorStats *into, const ColorStats *other);

// ¿Algun pixel con alfa > 0 usa este RGB? (para elegir un color clave libre). 16 bits.
bool image_uses_rgb_opaque(const Image *image, const u16 rgb[3]);

// Minima profundidad de gris (1, 2, 4 u 8) que representa exacto todos los niveles usados.
u32  gray_min_depth(const ColorStats *s);

// Colores RGBA distintos EXACTOS (todo alfa 0 cuenta como uno; 16 bits se cuentan en 8). Radix sort
// LSD de 4 pasadas de un byte: O(n) sin tablas hash. 0 si no hubo memoria. Para --info.
u64  analyze_unique_colors(const Image *image);
