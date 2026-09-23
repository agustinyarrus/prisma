// png/pack.h - de RGBA interno al formato de fila de PNG que eligio el plan.
//
// Una fila por vez, en paralelo por bandas. La paleta se resuelve con una tabla hash de solo
// lectura (512 lugares) compartida por todos los hilos, con un cache del ultimo color: en
// graficos planos casi nunca se consulta la tabla.
#pragma once
#include "plan.h"

typedef struct Packed {
    u8    *rows;          // height filas de row_bytes (sin byte de filtro)
    size_t row_bytes;
    u32    width, height;
} Packed;

Status pack_image(const Image *image, const PngPlan *plan, Packed *out, Diag *diag);
void   packed_free(Packed *p);
