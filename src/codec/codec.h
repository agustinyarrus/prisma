// codec/codec.h - registro de decodificadores y opciones de lectura.
//
// Cada formato es una tabla {nombre, extensiones, probe, decode}. El formato se decide por el
// CONTENIDO (numeros magicos), nunca solo por la extension: un .png que en realidad es un JPEG
// renombrado (pasa todo el tiempo con descargas) se lee igual. La extension solo desempata los
// formatos sin firma (TGA). Lo que ningun decodificador propio reconoce va a WIC, el sistema de
// codecs de Windows (JPEG, TIFF, HEIF, AVIF, WebP, JPEG XL, RAW de camaras... y cualquier codec
// que el usuario instale en el futuro).
#pragma once
#include "../core/image.h"
#include "../core/colorspace.h"

typedef enum FrameMode {
    FRAMES_DEFAULT,        // animacion: todos los cuadros; paginas/tamanos: el primero/mejor
    FRAMES_ONE,            // solo frame_index
    FRAMES_ALL,            // todo lo que haya
} FrameMode;

typedef enum Tristate { TRI_AUTO = 0, TRI_ON, TRI_OFF } Tristate;

typedef struct DecodeOptions {
    FrameMode frame_mode;
    u32       frame_index;
    bool      single_frame;      // la salida es un PNG estatico: no componer toda la animacion
    u64       max_pixels;
    Tristate  bmp_alpha;         // BMP de 32 bits: usar el 4.o byte como alfa
    Tristate  tga_alpha;
    bool      psd_keep_matte;    // no quitar el mate blanco del compuesto de PSD
    ToneOperator tonemap;
    float     exposure;          // EV
    bool      raw_gif_delays;    // no estirar retardos <= 10 ms a 100 ms como los navegadores
    bool      bc5_reconstruct_z; // normal maps BC5: reconstruir Z en el canal azul
    bool      force_wic;         // decodificar con WIC aunque haya decodificador propio
    bool      no_wic;
    bool      to_srgb;           // --icc srgb: convertir pixeles con el perfil embebido
    bool      ignore_crc;        // PNG con CRC roto: leerlo igual
    u32       ico_size;          // ICO/CUR: preferir este lado (0 = el mas grande)
} DecodeOptions;

typedef struct DecodeContext {
    const u8            *data;
    size_t               size;
    const char          *ext;         // extension en minusculas sin punto ("tga") o ""
    const DecodeOptions *opt;
    Diag                *diag;
    u32                  nesting;     // ICO con PNG adentro, BMP con JPEG adentro...
} DecodeContext;

typedef struct Codec {
    const char *name;                 // "PNG"
    const char *extensions;           // "png apng" (separadas por espacio)
    const char *description;
    u8     (*probe)(const u8 *data, size_t size, const char *ext);     // 0..100
    Status (*decode)(const DecodeContext *ctx, Decoded *out);
} Codec;

// Formatos propios.
extern const Codec CODEC_PNG, CODEC_GIF, CODEC_BMP, CODEC_ICO, CODEC_TGA, CODEC_QOI, CODEC_PNM,
                   CODEC_FARBFELD, CODEC_PSD, CODEC_HDR, CODEC_PCX, CODEC_DDS, CODEC_SGI;

const Codec *const *codec_builtin_list(size_t *count);

// Decodifica cualquier cosa: prueba los propios por firma y cae a WIC. `out` queda inicializado
// siempre (liberar con decoded_free aun si falla).
Status decode_image(const DecodeContext *ctx, Decoded *out);

// Para decodificadores contenedores (ICO -> PNG, BMP -> JPEG): decodifica un sub-buffer.
Status decode_nested(const DecodeContext *parent, const u8 *data, size_t size, Decoded *out);

// Ayudas comunes -------------------------------------------------------------------------------

// ¿Hay que producir el cuadro/pagina `index` de `total`? (FRAMES_DEFAULT -> solo el 0)
bool frame_wanted(const DecodeOptions *opt, u32 index, u32 total);

// Nombre de formato para errores y reporte.
void source_set(SourceInfo *s, const char *format, const char *codec);
void source_detail(SourceInfo *s, const char *fmt, ...) PRINTF_LIKE(2, 3);

// Firma de "no es de ningun formato conocido" con un pista (JPEG 2000, EXR, SVG...).
const char *format_hint_unsupported(const u8 *data, size_t size);
