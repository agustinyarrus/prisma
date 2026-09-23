// codec/png_unfilter.h - deshacer los filtros de fila de PNG (Sub, Up, Average, Paeth).
//
// Es el cuello de botella del decodificador despues del inflado: Paeth tiene una dependencia
// serial (cada pixel necesita el de su izquierda YA reconstruido), asi que no se vectoriza a lo
// ancho. Se vectoriza a lo PROFUNDO: los 3, 4, 6 u 8 bytes de un pixel van juntos en un
// registro SSE de 16 bits por carril y se resuelven de una (la tecnica de libpng).
#pragma once
#include "../core/base.h"

// Deshace el filtro `filter` de una fila en el lugar. `prev` es la fila anterior ya
// reconstruida (ceros para la primera). `bpp` = bytes por pixel completo (minimo 1).
// Devuelve false si el tipo de filtro es invalido.
bool png_unfilter_row(u8 *row, const u8 *prev, size_t rowbytes, u32 bpp, u8 filter);
