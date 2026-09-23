// codec/masks.c - ver masks.h
#include "masks.h"
#include "../core/mem.h"

bool mask_channel_init(MaskChannel *c, u32 mask) {
    memset(c, 0, sizeof *c);
    c->mask = mask;
    if (!mask) return true;
    c->shift = (u32)__builtin_ctz(mask);
    c->bits = bit_width(mask >> c->shift);
    if (c->bits > 16) return true;
    u32 max = (1u << c->bits) - 1;
    c->lut = mem_alloc(((size_t)max + 1) * sizeof(u16));
    if (!c->lut) return false;
    for (u32 v = 0; v <= max; v++) c->lut[v] = (u16)(((u64)v * 65535 + max / 2) / max);
    return true;
}

void mask_channel_free(MaskChannel *c) {
    mem_free(c->lut);
    c->lut = NULL;
}
