// ops/color.h - colores que escribe el usuario: #rgb, #rgba, #rrggbb, #rrggbbaa, rgb(r,g,b),
// rgba(r,g,b,a), los 148 nombres de CSS (incluido "transparent") y porcentajes en rgb().
#pragma once
#include "../core/base.h"

typedef struct Rgba {
    float r, g, b, a;         // [0, 1], valores codificados (sRGB), alfa directo
} Rgba;

// true si se entendio. En `error` (si no es NULL) queda una pista de que estuvo mal.
bool color_parse(const char *text, Rgba *out, char *error, size_t error_cap);

// "#rrggbbaa" (o "#rrggbb" si es opaco) para mostrar en el reporte.
void color_format(const Rgba *c, char *out, size_t cap);

// Nombres conocidos (para la ayuda y el "¿quisiste decir…?").
const char *const *color_names(size_t *count);
