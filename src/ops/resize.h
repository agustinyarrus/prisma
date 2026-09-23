// ops/resize.h - redimensionado de calidad que no ensucia los bordes transparentes.
//
// El error clasico al achicar una imagen con alfa: mezclar pixeles en alfa DIRECTO. Un pixel
// transparente con RGB negro (invisible) se mezcla con su vecino rojo y aparece un borde oscuro.
// Aca se filtra en alfa PREMULTIPLICADO (el color pesa segun su opacidad) y recien al final se
// divide. Con --linear ademas se mezcla en luz lineal (degradados y bordes finos mas fieles).
//
// Filtro separable (horizontal y despues vertical) con pesos precalculados por columna/fila de
// salida; al achicar, el soporte del filtro se estira en proporcion (antialias correcto).
// O(ancho x alto x soporte), en paralelo por filas.
#pragma once
#include "../core/image.h"

typedef enum ResizeFilter {
    RF_LANCZOS3 = 0, RF_LANCZOS2, RF_MITCHELL, RF_CATMULL, RF_BILINEAR, RF_BOX, RF_NEAREST, RF__COUNT
} ResizeFilter;

// Especificacion estilo ImageMagick: "800x600" (entra en la caja), "800x600!" (exacto), "800x"
// (ancho), "x600" (alto), "50%", "800x600^" (cubre la caja), sufijo ">" (solo achica).
typedef struct ResizeSpec {
    bool  active;
    u32   width, height;      // 0 = no fijado
    float percent;            // > 0: escala
    bool  exact, cover, only_shrink;
} ResizeSpec;

bool resize_spec_parse(const char *text, ResizeSpec *out, char *error, size_t cap);
// Dimensiones finales para una imagen de w x h (false si no hay que cambiar nada).
bool resize_target(const ResizeSpec *spec, u32 w, u32 h, u32 *out_w, u32 *out_h);

const char *resize_filter_name(ResizeFilter f);
bool        resize_filter_parse(const char *name, ResizeFilter *out);

Status resize_image(Image *image, u32 width, u32 height, ResizeFilter filter, bool linear_light, Diag *diag);
