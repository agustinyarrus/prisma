// core/cicp.h - codigos CICP (ITU-T H.273): primarios, transferencia y matriz YCbCr.
//
// Es el idioma de color de los formatos de video y sus derivados (AVIF, HEIC, JPEG XL) y, desde la
// 3.a edicion, de PNG (chunk cICP). Aca vive el algebra que hace falta para:
//  - corregir un decodificador que aplico la matriz YCbCr equivocada (el de AVIF de Windows la
//    deduce de los primarios en vez de leerla): C = M_declarada^-1 · M_usada;
//  - convertir otros primarios (Display P3, BT.2020...) a sRGB en luz lineal.
// Todo se calcula una vez por imagen en doble precision; por pixel queda una matriz 3x3.
#pragma once
#include "base.h"

enum {
    CICP_PRIMARIES_BT709 = 1, CICP_UNSPECIFIED = 2, CICP_PRIMARIES_BT601 = 6,
    CICP_TRANSFER_BT709 = 1, CICP_TRANSFER_LINEAR = 8, CICP_TRANSFER_SRGB = 13,
    CICP_TRANSFER_PQ = 16, CICP_TRANSFER_HLG = 18,
    CICP_MATRIX_IDENTITY = 0, CICP_MATRIX_BT709 = 1, CICP_MATRIX_BT601 = 6,
};

typedef struct Cicp {
    u8   primaries, transfer, matrix;
    bool full_range;
} Cicp;

const char *cicp_primaries_name(u32 primaries);        // "Display P3", "BT.2020"... o NULL
const char *cicp_transfer_name(u32 transfer);          // "sRGB", "PQ (HDR)"... o NULL

// ¿Describe exactamente sRGB (o no especifica nada, que en la practica se lee como sRGB)?
bool cicp_is_srgb_like(const Cicp *c);
bool cicp_is_hdr(const Cicp *c);                       // PQ o HLG

// Matriz 3x3 (fila mayor) que corrige RGB decodificado con la matriz YCbCr `used` cuando el
// archivo declaraba `declared`. false si alguna de las dos no es una matriz lineal conocida
// (identidad/GBR, YCgCo, luminancia constante): esos casos no se pueden corregir despues.
bool cicp_ycbcr_correction(u32 used, u32 declared, u32 primaries, float out[9]);

// Matriz lineal (fila mayor) de los primarios `primaries` a los de sRGB/BT.709, mismo blanco
// D65 (o Bradford si el blanco difiere). false si los primarios son desconocidos.
bool cicp_gamut_to_srgb(u32 primaries, float out[9]);

// Transferencias que se saben invertir a luz lineal por pixel (sin HDR). Ver cicp_to_linear.
bool  cicp_transfer_invertible(u32 transfer);
float cicp_to_linear(u32 transfer, float encoded);
