// codec/bcn.h - decodificacion de bloques de compresion de texturas (4x4 pixeles por bloque).
//
//   BC1 (DXT1)  color 5:6:5 + 2 bits/pixel; con c0 <= c1, el 4.o color es TRANSPARENTE (alfa 1 bit)
//   BC2 (DXT3)  BC1 de 4 colores + alfa explicito de 4 bits
//   BC3 (DXT5)  BC1 de 4 colores + alfa interpolado (8 u 6 niveles)
//   BC4 / BC5   uno / dos canales interpolados (normal maps), con variantes con signo
//   BC6H        HDR, 14 modos, flotante de 16 bits sin signo o con signo
//   BC7         8 modos, particiones de 1-3 subconjuntos, rotacion de canales, alfa de calidad
// Las tablas de particiones, anclas y empaquetado de bits son las de la especificacion D3D/BPTC.
#pragma once
#include "../core/base.h"

// Salida: 16 pixeles RGBA8 en orden de filas (out[4*(y*4+x) + c]).
void bc1_block(const u8 *src, u8 out[64], bool four_colors);
void bc2_block(const u8 *src, u8 out[64]);
void bc3_block(const u8 *src, u8 out[64]);
void bc7_block(const u8 *src, u8 out[64]);

// Un canal interpolado (alfa de BC3, BC4, cada mitad de BC5): 16 valores en [0,255] o, con
// signo, en [-127,127].
void bc_channel_block(const u8 *src, int out[16], bool is_signed);

// BC6H: 16 pixeles RGB flotantes lineales.
void bc6h_block(const u8 *src, float out[48], bool is_signed);
