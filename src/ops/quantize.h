// ops/quantize.h - reducir a una paleta de N colores CON ALFA (PNG8 con transparencia suave).
//
// Lo que hace la diferencia con un cuantizador comun es la METRICA: dos colores con alfa se
// comparan como se verian compuestos sobre negro Y sobre blanco, promediando los dos errores por
// canal (sobre colores premultiplicados y ponderados por sensibilidad visual). Asi un borde
// semitransparente no se "redondea" a un color que sobre fondo claro se ve distinto. Es una forma
// cuadratica definida positiva: k-means la minimiza exactamente (medido: 0,99x el costo de un
// k-means++ de 8 semillas y 300 iteraciones).
//
//   1. histograma de colores unicos (todos los alfa 0 son UN color: no gastan paleta)
//   2. median cut con cola de prioridad: siempre se parte la caja con mas error cuadratico, por
//      el punto de su eje que minimiza el error de las dos mitades (no por la mediana)
//   3. k-means (Lloyd) hasta converger, con busqueda del vecino mas cercano podada por un eje
//      ordenado y re-siembra de los centros que se quedan vacios
//   4. remapeo: directo, Floyd-Steinberg serpenteante (el error no se difunde a lo transparente) u
//      ordenado (Bayer 8x8, paralelizable y sin "gusanos" de difusion)
// Si la imagen ya tiene <= N colores no se toca nada (sin perdida).
#pragma once
#include "../core/image.h"

typedef enum DitherMode { DITHER_FS = 0, DITHER_NONE, DITHER_ORDERED } DitherMode;

typedef struct QuantizeOptions {
    u32        colors;          // 2..256
    DitherMode dither;
    float      level;           // 0..1 (intensidad del dithering)
} QuantizeOptions;

typedef struct QuantizeReport {
    u64    unique_in;           // colores unicos de la entrada
    u32    palette;             // colores de la paleta final
    double psnr;                // calidad del resultado (dB); INFINITY si fue sin perdida
    bool   lossless;
} QuantizeReport;

// Una paleta comun para todos los cuadros (animaciones). Las imagenes quedan en 8 bits.
Status quantize_frames(Image *const *frames, u32 count, const QuantizeOptions *opt, QuantizeReport *rep, Diag *diag);
