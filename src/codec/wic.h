// codec/wic.h - Windows Imaging Component: el decodificador universal de respaldo.
//
// Todo lo que no tiene decodificador propio pasa por WIC: JPEG (con CMYK/YCCK), TIFF (paginas,
// alfa asociado y no asociado), JPEG XR, HEIF/HEIC, AVIF, WebP, JPEG XL, RAW de camara y
// cualquier codec que se instale en el sistema en el futuro. COM se inicializa por hilo (MTA) la
// primera vez que un hilo lo necesita, y cada hilo tiene su propia fabrica.
#pragma once
#include "codec.h"

Status wic_decode(const DecodeContext *ctx, Decoded *out);

// Enumera los decodificadores instalados (para --formats).
typedef void (*WicCodecVisitor)(void *ctx, const char *friendly_name, const char *extensions);
void wic_enumerate_decoders(WicCodecVisitor visit, void *ctx);

// Convierte los pixeles de `image` del perfil ICC `icc` a sRGB con el motor de color de Windows.
Status wic_convert_to_srgb(Image *image, const u8 *icc, size_t icc_size, Diag *diag);
