// ops/pixelops.h - operaciones de transparencia pixel a pixel, FUSIONADAS en una sola pasada.
//
// Cada operacion es un "nucleo" (tipo + parametros). Una cadena de nucleos se aplica fila por
// fila: la fila se carga a flotante [0,1] una vez, pasa por todos los nucleos y se guarda una vez.
// Encadenar --key, --threshold y --opacity cuesta lo mismo que una sola operacion (la memoria se
// recorre una vez), y como todo se calcula en flotante, 8 y 16 bits comparten el mismo codigo.
// Un pixel que ningun nucleo toca vuelve EXACTO (x/255 -> *255 redondeado es la identidad).
#pragma once
#include "../core/image.h"
#include "color.h"

typedef enum KernelKind {
    K_ALPHA_FROM_LUMA,        // alfa = luma (Rec.709 sobre valores codificados)
    K_ALPHA_FROM_CHANNEL,     // alfa = canal p[0] (0 R, 1 G, 2 B)
    K_ALPHA_INVERT_SOURCE,    // (auxiliar de alpha-from: alfa = 1 - alfa)
    K_MASK,                   // alfa *= mascara (buffer externo, mismo tamano)
    K_UNPREMULTIPLY,
    K_UNMATTE,                // deshace mate: c = (c - (1-a)·m) / a
    K_KEY,                    // color clave con tolerancia y borde suave
    K_COLOR_TO_ALPHA,         // algoritmo de GIMP/GEGL con umbrales
    K_THRESHOLD,
    K_LEVELS,
    K_OPACITY,
    K_INVERT_ALPHA,
    K_FLATTEN,                // compone sobre un fondo (opcionalmente en luz lineal)
    K_CLEAN_BLACK,            // RGB de alfa 0 -> 0
    K_GRAYSCALE,              // luminancia en luz lineal
    K_PREMULTIPLY,
    K_COLOR_MATRIX,           // RGB <- M·RGB (M 3x3 en p[0..8], sobre valores codificados)
    K_GAMUT_TO_SRGB,          // decodificar con `table`, M·RGB en luz lineal, codificar sRGB
} KernelKind;

enum { TRANSFER_LUT_SIZE = 4096 };    // K_GAMUT_TO_SRGB: `table` tiene TRANSFER_LUT_SIZE + 1 valores

typedef struct Kernel {
    KernelKind   kind;
    float        p[12];
    const float *mask;        // K_MASK: w*h valores [0,1]
    const float *table;       // K_GAMUT_TO_SRGB: codificado [0,1] -> lineal, muestreado parejo
} Kernel;

// Aplica la cadena a toda la imagen (en paralelo por bandas). O(pixeles x nucleos).
void pixelops_run(Image *image, const Kernel *kernels, u32 count);

// Constructores legibles.
Kernel kernel_key(Rgba color, float fuzz, float feather);
Kernel kernel_color_to_alpha(Rgba color, float transparency_threshold, float opacity_threshold);
Kernel kernel_unmatte(Rgba matte);
Kernel kernel_flatten(Rgba background, bool linear_light);
Kernel kernel_simple(KernelKind kind, float p0, float p1, float p2);
Kernel kernel_color_matrix(const float m[9]);
Kernel kernel_gamut_to_srgb(const float m[9], const float *decode_table);
