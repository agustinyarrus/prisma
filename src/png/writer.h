// png/writer.h - arma el archivo: firma, chunks con CRC y metadatos.
//
// Orden de chunks: IHDR, color (sRGB | iCCP | gAMA+cHRM), pHYs, eXIf/XMP/textos, PLTE, tRNS,
// acTL, [fcTL] IDAT, [fcTL fdAT]..., IEND. Todo lo auxiliar va ANTES de los datos de imagen:
// un lector que corta temprano (miniaturas, previsualizaciones) igual ve el perfil de color.
#pragma once
#include "plan.h"

typedef enum MetaMode {
    META_NONE = 0,     // nada: ni perfil de color (¡cambia como se ven los colores si no es sRGB!)
    META_COLOR,        // default: perfil/sRGB/gamma + densidad (pHYs)
    META_ALL,          // + EXIF (con orientacion 1), XMP y textos
} MetaMode;

typedef struct ByteVec {
    u8    *data;
    size_t len;
    size_t cap;
    bool   failed;
} ByteVec;

void bv_put(ByteVec *v, const void *data, size_t size);
void bv_free(ByteVec *v);

void png_write_signature(ByteVec *v);
void png_write_chunk(ByteVec *v, const char type[4], const void *data, size_t size);
void png_write_ihdr(ByteVec *v, u32 width, u32 height, const PngPlan *plan, bool interlace);
void png_write_palette(ByteVec *v, const PngPlan *plan);          // PLTE + tRNS (o tRNS de clave)
// Escribe TODO lo que traiga `meta` (la politica de que conservar se decide antes, en un solo lugar).
void png_write_metadata(ByteVec *v, const Metadata *meta, bool orientation_applied);
void png_write_data_chunks(ByteVec *v, const char type[4], const u8 *zdata, size_t size);   // IDAT
void png_write_end(ByteVec *v);
