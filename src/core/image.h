// core/image.h - la imagen en memoria y todo lo que viaja con ella.
//
// Una sola representacion de pixeles para todo el programa: RGBA con alfa DIRECTO (no
// premultiplicado), 8 o 16 bits por canal. Los 16 bits van en orden nativo (little endian): el
// big endian de PNG se resuelve solo en el borde (al leer y al escribir el archivo).
//
// Por que siempre 4 canales: cada operacion de transparencia se escribe UNA vez y no por tipo
// de color; el costo (un byte de mas en JPEG sin alfa) se recupera en el codificador, que
// reduce a RGB, gris o paleta segun lo que la imagen realmente use.
#pragma once
#include "base.h"
#include "status.h"
#include "mem.h"

typedef struct Image {
    u32    width, height;
    u32    depth;          // bits por canal: 8 o 16
    size_t stride;         // bytes por fila = width * bytes por pixel (sin relleno)
    u8    *pixels;         // RGBA; 64 bytes de holgura al final (ver mem.h)
} Image;

#define DEFAULT_MAX_PIXELS ((u64)1 << 31)    // 2 gigapixeles: 8 GB en RGBA8

INLINE u32  image_bytes_per_pixel(const Image *im) { return im->depth / 2; }   // 4 u 8
INLINE u8  *image_row(const Image *im, u32 y) { return im->pixels + (size_t)y * im->stride; }
INLINE u64  image_pixel_count(const Image *im) { return (u64)im->width * im->height; }

Status image_alloc(Image *im, u32 width, u32 height, u32 depth, bool zero, u64 max_pixels, Diag *diag);
void   image_free(Image *im);
Status image_clone(const Image *src, Image *dst, Diag *diag);
Status image_set_depth(Image *im, u32 depth, Diag *diag);        // 8 <-> 16 (reemplaza el buffer)
void   image_fill(Image *im, const u16 rgba16[4]);               // pinta toda la imagen
// Color premultiplicado -> directo, en el lugar: c' = round(c * max / a), y 0 donde a = 0.
// Enteros con redondeo exacto (sin empates raros de flotante), 8 y 16 bits, en paralelo por filas.
// Es la que usan todos los decodificadores que traen el color multiplicado (TGA, DDS, AVIF).
void   image_unpremultiply(Image *im);

// ---- metadatos ------------------------------------------------------------------------------
typedef struct Blob {
    u8    *data;
    size_t size;
} Blob;

bool blob_set(Blob *b, const void *data, size_t size);          // copia
void blob_free(Blob *b);

typedef struct TextEntry {
    char  keyword[80];
    char *text;                  // UTF-8, mem_alloc
} TextEntry;

typedef struct Metadata {
    Blob   icc;
    char   icc_name[64];         // descripcion del perfil si se pudo leer ("sRGB IEC61966-2.1")
    Blob   exif;                 // bloque TIFF crudo ("II*\0..."), sin el prefijo "Exif\0\0"
    Blob   xmp;
    Vec    texts;                // TextEntry
    u16    orientation;          // EXIF 1..8; 0 = no declarada
    bool   has_gamma;       double gamma;          // como en gAMA: 1/2.2 = 0.45455
    bool   has_srgb;        u8     srgb_intent;
    bool   has_chrm;        double chrm[8];        // blanco, rojo, verde, azul (x, y)
    bool   has_phys;        u32    phys_x, phys_y; u8 phys_unit;   // unidad 1 = metro
    bool   has_background;  u16    background[3];  // bKGD o equivalente, escala de 16 bits
    bool   has_sbit;        u8     sbit[4];
    bool   has_cicp;        u8     cicp[4];        // cICP (PNG 3.a ed.): primarios, transferencia, matriz 0, rango 1
} Metadata;

void metadata_free(Metadata *m);
bool metadata_add_text(Metadata *m, const char *keyword, const char *text, size_t text_len);

// ---- de donde vino la transparencia --------------------------------------------------------
typedef enum AlphaOrigin {
    ALPHA_ORIGIN_NONE = 0,       // el formato no trae transparencia
    ALPHA_ORIGIN_CHANNEL,        // canal alfa completo
    ALPHA_ORIGIN_KEY,            // color clave (tRNS gris/RGB, indice transparente de GIF)
    ALPHA_ORIGIN_PALETTE,        // alfa por entrada de paleta (PNG8 con tRNS, TGA mapeado)
    ALPHA_ORIGIN_MASK,           // mascara de 1 bit (AND de ICO/CUR, saltos de BMP RLE)
    ALPHA_ORIGIN_PREMULTIPLIED,  // venia premultiplicado: se des-premultiplico al leer
    ALPHA_ORIGIN_MATTED,         // venia mateado sobre un color (PSD): se quito el mate
} AlphaOrigin;

const char *alpha_origin_label(AlphaOrigin origin);

typedef struct SourceInfo {
    char        format[16];      // "PNG", "JPEG", "GIF", ...
    char        detail[160];     // "RGBA 16 bits · entrelazado"
    char        codec[64];       // "propio" o "WIC · Microsoft HEIF Decoder"
    u32         bits;            // bits por canal del original
    u32         channels;
    bool        has_alpha;       // el original declara transparencia
    AlphaOrigin alpha_origin;
    bool        is_float;        // HDR / flotante: paso por tonemap
    u32         total_frames;    // cuadros o paginas que tiene el archivo
} SourceInfo;

typedef struct Frame {
    Image image;
    u32   delay_num, delay_den;  // duracion num/den segundos (APNG)
    u32   index;                 // posicion en el archivo (pagina, tamano de icono)
    i32   hotspot_x, hotspot_y;  // cursores .cur; -1 si no aplica
} Frame;

typedef struct Decoded {
    Vec        frames;           // Frame
    bool       animated;         // animacion (GIF/APNG/WebP) vs paginas o tamanos
    u32        loop_count;       // 0 = infinito
    SourceInfo source;
    Metadata   meta;
} Decoded;

void   decoded_init(Decoded *d);
Frame *decoded_add_frame(Decoded *d);
void   decoded_free(Decoded *d);
INLINE Frame *decoded_frame(const Decoded *d, size_t i) { return &VEC_AT(&d->frames, Frame, i); }
INLINE u32 decoded_frame_count(const Decoded *d) { return (u32)d->frames.count; }
