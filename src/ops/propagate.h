// ops/propagate.h - propagar colores desde los pixeles "buenos" hacia los que no tienen color util.
//
//   BLEED (sangrado): el RGB de los pixeles 100 % transparentes se rellena con el color del borde
//     mas cercano, sin tocar el alfa. Invisible en pantalla, pero los motores de juego filtran y
//     generan mipmaps mezclando vecinos: con RGB negro bajo el alfa 0 aparecen halos oscuros en los
//     bordes; con sangrado, no. (--bleed)
//   DEFRINGE: el RGB de los pixeles semitransparentes se reemplaza por el del pixel opaco mas
//     cercano, como "Defringe" de Photoshop: quita el ribete del fondo viejo que quedo mezclado en
//     el antialias. (--defringe N)
//
// Tecnica: BFS multi-fuente por capas (transformada de distancia de chebyshev). Cada pixel de la
// capa k toma el promedio de sus vecinos (8-conexos) ya resueltos en capas < k. O(n) total, y cada
// capa se calcula en paralelo porque solo lee lo resuelto antes.
#pragma once
#include "../core/image.h"

typedef enum PropagateMode { PROPAGATE_BLEED, PROPAGATE_DEFRINGE } PropagateMode;

#define PROPAGATE_UNLIMITED UINT32_MAX

// Devuelve en *touched cuantos pixeles cambiaron.
Status propagate_colors(Image *image, PropagateMode mode, u32 max_distance, u64 *touched, Diag *diag);
