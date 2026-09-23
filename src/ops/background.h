// ops/background.h - borrar el fondo CONECTADO a los bordes (--remove-bg), sin tocar el sujeto.
//
// A diferencia de --key (que borra ese color en toda la imagen, incluido el blanco de los ojos de
// una persona sobre fondo blanco), aca solo se borra lo que se alcanza desde el borde de la imagen
// caminando por pixeles parecidos al fondo: relleno por semillas con recorrido por renglones
// (scanline flood fill, O(n)). Despues, un anillo de `edge_px` pixeles alrededor de lo borrado pasa
// por color-a-alfa contra el color del fondo: el antialias del contorno queda translucido y sin
// el halo del fondo viejo.
//
// Color del fondo "auto": el color mas frecuente del borde (histograma de 4096 cubetas, 4 bits por
// canal) promediado dentro de su cubeta. Si ese color cubre menos del 30 % del borde, se avisa.
#pragma once
#include "../core/image.h"
#include "color.h"

typedef struct BgRemoveOptions {
    bool  auto_color;
    Rgba  color;
    float fuzz;              // tolerancia (distancia RGB normalizada 0..1)
    u32   edge_px;           // ancho del anillo de limpieza de halo (0 = sin limpieza)
} BgRemoveOptions;

typedef struct BgRemoveReport {
    Rgba   color;            // el color usado (detectado o dado)
    float  border_coverage;  // fraccion del borde que tenia ese color
    u64    removed;          // pixeles borrados
    u64    softened;         // pixeles del anillo tratados
} BgRemoveReport;

// Detecta el color de fondo (para reutilizarlo en todos los cuadros de una animacion).
bool   background_detect(const Image *image, Rgba *color, float *coverage);
Status background_remove(Image *image, const BgRemoveOptions *opt, BgRemoveReport *report, Diag *diag);
