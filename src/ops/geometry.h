// ops/geometry.h - orientacion (EXIF), rotacion, espejo, recorte, trim y margenes.
#pragma once
#include "../core/image.h"

typedef enum Orientation {
    ORIENT_NONE = 1, ORIENT_FLIP_H = 2, ORIENT_ROT180 = 3, ORIENT_FLIP_V = 4,
    ORIENT_TRANSPOSE = 5, ORIENT_ROT90 = 6, ORIENT_TRANSVERSE = 7, ORIENT_ROT270 = 8,
} Orientation;       // mismos valores que el tag de orientacion de EXIF

// Aplica la transformacion (las que cruzan ejes recorren por bloques de 32x32: caben en cache).
Status geometry_orient(Image *image, Orientation op, Diag *diag);
// Combina rotacion (0/90/180/270, sentido horario) y espejos en una sola transformacion.
Orientation geometry_compose(int rotate_degrees, bool flip_h, bool flip_v);
// Algebra del grupo D4: `first` y despues `second`; y la transformacion que deshace `o`.
Orientation geometry_then(Orientation first, Orientation second);
Orientation geometry_inverse(Orientation o);

Status geometry_crop(Image *image, u32 x, u32 y, u32 w, u32 h, Diag *diag);

// Caja minima con alfa > umbral (umbral en escala 0..255). false si la imagen es toda transparente.
bool   geometry_content_box(const Image *image, u32 threshold8, u32 *x0, u32 *y0, u32 *x1, u32 *y1);

Status geometry_pad(Image *image, u32 left, u32 top, u32 right, u32 bottom, const u16 color16[4], Diag *diag);
