// codec/float_image.h - de pixeles flotantes lineales (HDR, PFM, PSD 32 bits, DDS float, BC6H) a la
// imagen RGBA de 16 bits de prisma, con tonemap y codificacion sRGB. Un solo lugar para que todos
// los formatos HDR se vean igual.
#pragma once
#include "codec.h"

// `rgba` = w*h*4 flotantes lineales (alfa en [0,1]). Resuelve TONE_AUTO mirando el pico.
Status float_image_to_rgba16(const float *rgba, u32 width, u32 height, const DecodeOptions *opt,
                             Image *out, ToneOperator *used, Diag *diag);

// Premultiplicado -> directo sobre flotantes lineales (antes del tonemap). O(pixeles).
void float_image_unpremultiply(float *rgba, size_t pixels);

// Nombre para el reporte ("ACES", "Reinhard", "recorte").
const char *tone_name(ToneOperator op);
