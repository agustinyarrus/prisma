// codec/masks.h - canales definidos por mascara de bits (BMP BITFIELDS, DDS heredado, TGA 16).
//
// Un canal es una mascara contigua dentro del pixel (p.ej. 0x03FF00000 = 10 bits en la posicion
// 20). El valor se escala a 16 bits con redondeo exacto; si el canal tiene <= 16 bits se usa una
// tabla (2^bits entradas) para no multiplicar y dividir por pixel.
#pragma once
#include "../core/base.h"

typedef struct MaskChannel {
    u32  mask, shift, bits;
    u16 *lut;
} MaskChannel;

bool mask_channel_init(MaskChannel *c, u32 mask);     // false: sin memoria para la tabla
void mask_channel_free(MaskChannel *c);

INLINE u16 mask_channel_value(const MaskChannel *c, u32 px) {
    if (!c->bits) return 0;
    u32 v = (px & c->mask) >> c->shift;
    if (c->lut) return c->lut[v];
    u64 max = (1ull << c->bits) - 1;
    return (u16)(((u64)v * 65535 + max / 2) / max);
}
