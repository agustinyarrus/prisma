// codec/compose.h - composicion de cuadros de animacion (APNG y GIF) sobre un lienzo RGBA.
//
// Las dos animaciones se describen igual: cada cuadro es un rectangulo que se dibuja sobre el
// lienzo (reemplazando o componiendo con alfa) y despues se "desecha" (nada, borrar a
// transparente, o volver a lo que habia antes). Un solo modulo para las dos, asi el manejo de
// transparencia entre cuadros es identico en ambas.
#pragma once
#include "../core/image.h"

typedef enum Dispose { DISPOSE_NONE = 0, DISPOSE_BACKGROUND = 1, DISPOSE_PREVIOUS = 2 } Dispose;

// Dibuja `src` en (x, y). over=false reemplaza (APNG SOURCE); over=true compone con alfa
// directo (APNG OVER). Recorta a los limites del lienzo.
void compose_draw(Image *canvas, const Image *src, u32 x, u32 y, bool over);

// Pone en transparente (0,0,0,0) un rectangulo (recortado al lienzo).
void compose_clear(Image *canvas, u32 x, u32 y, u32 w, u32 h);

// Copia un rectangulo del lienzo a `saved` / lo devuelve.
Status compose_save(const Image *canvas, u32 x, u32 y, u32 w, u32 h, Image *saved, Diag *diag);
void   compose_restore(Image *canvas, const Image *saved, u32 x, u32 y);

// Pixel a pixel, OVER de alfa directo en 8 bits, redondeando como la cuenta en flotante.
void compose_over_rgba8(u8 *dst, const u8 *src, size_t pixels);
