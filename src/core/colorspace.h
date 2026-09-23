// core/colorspace.h - sRGB <-> lineal y tonemapping de HDR a rango visible.
//
// PNG guarda valores codificados en sRGB (con "gamma"). Las cuentas que mezclan luz (redimensionar,
// componer sobre un fondo con --linear, HDR) se hacen en luz LINEAL; si se hacen sobre los
// valores codificados, los bordes antialiasados salen oscuros y los degradados se ensucian.
//
// Para el sentido 8 bits -> lineal hay tabla de 256 flotantes (exacta). Para volver, una tabla de
// 4096 entradas indexada por el valor lineal con interpolacion lineal: error maximo < 0,02 niveles
// de 16 bits (lo mide tests/unit), cien veces mas rapido que powf.
#pragma once
#include "base.h"

// AUTO: si la imagen no pasa de 1,0 (contenido SDR guardado en flotante, algo comun en JPEG XL y
// JPEG XR) se recorta sin curva, para no cambiarle el contraste; si tiene luces por encima de
// 1,0 (HDR de verdad), ACES.
typedef enum ToneOperator { TONE_AUTO = 0, TONE_ACES, TONE_REINHARD, TONE_CLIP } ToneOperator;

void  colorspace_init(void);                 // arma las tablas (idempotente)
float srgb_to_linear(float encoded);         // exacta (powf)
float linear_to_srgb(float linear);          // exacta (powf)
float srgb8_to_linear(u8 v);                 // tabla
float linear_to_srgb_fast(float linear);     // tabla + interpolacion, entrada en [0, 1]

// HDR lineal (escena) -> [0,1] lineal visible. `exposure_scale` = 2^EV.
void  tonemap_rgb(float rgb[3], ToneOperator op, float exposure_scale);

// Resuelve TONE_AUTO mirando el pico de la imagen (RGBA flotante lineal).
ToneOperator tonemap_resolve(ToneOperator op, const float *rgba, size_t pixels, float exposure_scale);

// Fila RGBA flotante lineal (escena) -> RGBA16 codificado en sRGB, con tonemap. Alfa se satura.
void  tonemap_row_rgba16(const float *rgba, u32 width, ToneOperator op, float exposure_scale, u16 *out);

// Medio flotante (IEEE 754 binary16) -> float. Para DDS, BC6H, EXR-like.
float half_to_float(u16 h);
