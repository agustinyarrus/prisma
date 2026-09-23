// png/encode.h - el codificador PNG/APNG de prisma.
//
// Niveles de optimizacion (-O):
//   0  deflate 1,  un filtro por heuristica              lo mas rapido
//   1  deflate 6,  un filtro por heuristica
//   2  deflate 9,  un filtro por heuristica              (default)
//   3  deflate 11, 4 estrategias de filtro medidas + los 2 mejores planes de color
//   4  deflate 12, las 8 estrategias + todos los planes, un solo flujo (maxima razon)
// Medir antes de decidir: en 3 y 4 cada estrategia se comprime rapido para ordenarlas y solo las
// dos mejores se comprimen con el nivel final.
#pragma once
#include "filter.h"
#include "writer.h"

enum { OPTIMIZE_MAX = 4, OPTIMIZE_DEFAULT = 2 };

typedef struct EncodeOptions {
    int            optimize;             // 0..4
    int            level;                // -1 = el del nivel de optimizacion
    FilterStrategy filter;               // FS__COUNT = automatico
    ColorChoice    color;
    u32            depth;                // 0 = auto, 8, 16
    bool           reduce;
    bool           may_clean;
    bool           interlace;
    bool           parallel;             // deflate por tramos en paralelo
    MetaMode       meta;
    bool           orientation_applied;  // la orientacion EXIF ya se aplico a los pixeles
    bool           apng_try_both_blends; // (O3+) probar OVER y SOURCE por cuadro
} EncodeOptions;

typedef struct EncodeReport {
    char   format[64];                   // "paleta 187 col · 8 bits · tRNS"
    char   filter[48];                   // "minsum (paeth 61 %, sub 22 %…)"
    int    level;
    u32    frames_in, frames_out;        // APNG: cuadros recibidos / escritos (se unen los repetidos)
    u32    trials;                       // combinaciones plan x filtro medidas
    u8     color_type, bit_depth;
    u32    palette_size;
    bool   interlaced;
    // Lo que la imagen escrita usa de verdad (del analisis de color).
    u64    pixels, transparent, partial;   // alfa 0 / intermedio
    u32    colors;                         // distintos (257 = mas de 256)
    bool   gray;
} EncodeReport;

typedef struct Encoded {
    u8          *data;
    size_t       size;
    EncodeReport report;
} Encoded;

void   encode_options_default(EncodeOptions *opt);
Status png_encode(const Image *image, const Metadata *meta, const EncodeOptions *opt, Encoded *out, Diag *diag);
Status apng_encode(const Frame *frames, u32 count, u32 loops, const Metadata *meta,
                   const EncodeOptions *opt, Encoded *out, Diag *diag);
void   encoded_free(Encoded *e);
