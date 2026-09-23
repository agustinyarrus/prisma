// codec/bmp.h - decodificador de DIB (el corazon de BMP) compartido con ICO/CUR.
//
// Un .bmp es "BM" + un DIB; un icono guarda DIBs sin el "BM", con la altura al doble (imagen
// XOR arriba, mascara AND de 1 bit abajo). Las dos cosas pasan por dib_decode.
#pragma once
#include "codec.h"

typedef struct DibRequest {
    const u8 *dib;              // empieza en el encabezado de info (BITMAPINFOHEADER o variante)
    size_t    dib_size;         // bytes disponibles desde `dib`
    const u8 *pixels;           // datos de pixeles si el contenedor los ubica (BMP: bfOffBits); NULL = tras la paleta
    bool      icon;             // altura doble + mascara AND a continuacion
} DibRequest;

typedef struct DibResult {
    Image      image;
    u32        bits;             // bits por pixel del DIB
    bool       alpha_used;       // trae alfa con informacion (algun valor != 0)
    bool       rle_holes;        // RLE con pixeles salteados (quedan transparentes)
    const u8  *and_mask;         // iconos: donde empieza la mascara AND (o NULL)
    size_t     and_mask_size;
    u32        and_stride;
} DibResult;

Status dib_decode(const DecodeContext *ctx, const DibRequest *req, DibResult *res, Decoded *meta_out);
