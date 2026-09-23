// core/icc.h - lectura minima de perfiles ICC: nombre y "¿es sRGB?".
//
// Casi todas las fotos traen un perfil sRGB de 3 KB que no aporta nada: PNG tiene un chunk sRGB
// de 1 byte que dice lo mismo. Detectarlo ahorra 3 KB por archivo (en un icono de 2 KB, mas de
// la mitad del archivo). Los perfiles que no son sRGB (Display P3, Adobe RGB) se conservan
// siempre: descartarlos cambiaria los colores que se ven.
#pragma once
#include "base.h"

// Descripcion del perfil (tag 'desc', v2 ASCII o v4 'mluc' UTF-16). false si no se pudo leer.
bool icc_description(const u8 *icc, size_t size, char *out, size_t cap);

// Perfil RGB de monitor cuya descripcion es una de las variantes conocidas de sRGB.
bool icc_is_srgb(const u8 *icc, size_t size);

// Espacio de color del perfil ("RGB ", "GRAY", "CMYK"...), 4 caracteres + 0.
bool icc_color_space(const u8 *icc, size_t size, char out[5]);
