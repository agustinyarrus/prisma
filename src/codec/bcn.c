// codec/bcn.c - ver bcn.h. Las tablas de BC6H/BC7 son datos de la especificacion (transcriptas del
// decodificador de dominio publico CC0 de Pillow y verificadas contra la especificacion BPTC).
#include "bcn.h"
#include "../core/colorspace.h"

// ---- BC1..BC5 ------------------------------------------------------------------------------------

static void rgb565(u32 v, u8 out[3]) {
    u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    out[0] = (u8)(r << 3 | r >> 2);
    out[1] = (u8)(g << 2 | g >> 4);
    out[2] = (u8)(b << 3 | b >> 2);
}

void bc1_block(const u8 *src, u8 out[64], bool four_colors) {
    u32 c0 = load_le16(src), c1 = load_le16(src + 2), bits = load_le32(src + 4);
    u8 pal[4][4];
    rgb565(c0, pal[0]);
    rgb565(c1, pal[1]);
    pal[0][3] = pal[1][3] = pal[2][3] = pal[3][3] = 255;
    if (c0 > c1 || four_colors) {
        for (int c = 0; c < 3; c++) {
            pal[2][c] = (u8)((2 * pal[0][c] + pal[1][c] + 1) / 3);
            pal[3][c] = (u8)((pal[0][c] + 2 * pal[1][c] + 1) / 3);
        }
    } else {
        for (int c = 0; c < 3; c++) { pal[2][c] = (u8)((pal[0][c] + pal[1][c] + 1) / 2); pal[3][c] = 0; }
        pal[3][3] = 0;                                  // el color 3 es transparente: alfa de 1 bit
    }
    for (int i = 0; i < 16; i++) memcpy(out + 4 * i, pal[(bits >> (2 * i)) & 3], 4);
}

// Division con redondeo al mas cercano tambien para negativos (BC4/BC5 con signo).
static int div_nearest(int num, int den) { return num >= 0 ? (num + den / 2) / den : -((-num + den / 2) / den); }

void bc_channel_block(const u8 *src, int out[16], bool is_signed) {
    int a0 = is_signed ? (i8)src[0] : src[0], a1 = is_signed ? (i8)src[1] : src[1];
    if (is_signed) { if (a0 == -128) a0 = -127; if (a1 == -128) a1 = -127; }
    int v[8] = { a0, a1 };
    if (a0 > a1) {
        for (int k = 1; k <= 6; k++) v[k + 1] = div_nearest((7 - k) * a0 + k * a1, 7);
    } else {
        for (int k = 1; k <= 4; k++) v[k + 1] = div_nearest((5 - k) * a0 + k * a1, 5);
        v[6] = is_signed ? -127 : 0;
        v[7] = is_signed ? 127 : 255;
    }
    u64 bits = 0;
    for (int i = 0; i < 6; i++) bits |= (u64)src[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = v[(bits >> (3 * i)) & 7];
}

void bc2_block(const u8 *src, u8 out[64]) {
    bc1_block(src + 8, out, true);
    for (int i = 0; i < 16; i++) {
        u32 a = (src[i / 2] >> (4 * (i & 1))) & 15;
        out[4 * i + 3] = (u8)(a * 17);
    }
}

void bc3_block(const u8 *src, u8 out[64]) {
    bc1_block(src + 8, out, true);
    int a[16];
    bc_channel_block(src, a, false);
    for (int i = 0; i < 16; i++) out[4 * i + 3] = (u8)a[i];
}

// ---- lector de bits de 128 bits -----------------------------------------------------------------

typedef struct Bits128 {
    u64 lo, hi;
    u32 pos;
} Bits128;

static u32 take(Bits128 *b, u32 n) {
    if (!n) return 0;
    u64 v;
    if (b->pos >= 64) v = b->hi >> (b->pos - 64);
    else if (b->pos + n <= 64) v = b->lo >> b->pos;
    else v = (b->lo >> b->pos) | (b->hi << (64 - b->pos));
    b->pos += n;
    return (u32)(v & ((1ull << n) - 1));
}

static u32 bit_at(const u8 *src, u32 i) { return (src[i >> 3] >> (i & 7)) & 1; }

// ---- tablas BPTC ------------------------------------------------------------------------------

static const u16 PARTITION2[64] = {
    0xcccc, 0x8888, 0xeeee, 0xecc8, 0xc880, 0xfeec, 0xfec8, 0xec80, 0xc800, 0xffec,
    0xfe80, 0xe800, 0xffe8, 0xff00, 0xfff0, 0xf000, 0xf710, 0x008e, 0x7100, 0x08ce,
    0x008c, 0x7310, 0x3100, 0x8cce, 0x088c, 0x3110, 0x6666, 0x366c, 0x17e8, 0x0ff0,
    0x718e, 0x399c, 0xaaaa, 0xf0f0, 0x5a5a, 0x33cc, 0x3c3c, 0x55aa, 0x9696, 0xa55a,
    0x73ce, 0x13c8, 0x324c, 0x3bdc, 0x6996, 0xc33c, 0x9966, 0x0660, 0x0272, 0x04e4,
    0x4e40, 0x2720, 0xc936, 0x936c, 0x39c6, 0x639c, 0x9336, 0x9cc6, 0x817e, 0xe718,
    0xccf0, 0x0fcc, 0x7744, 0xee22,
};

static const u32 PARTITION3[64] = {
    0xaa685050, 0x6a5a5040, 0x5a5a4200, 0x5450a0a8, 0xa5a50000, 0xa0a05050, 0x5555a0a0,
    0x5a5a5050, 0xaa550000, 0xaa555500, 0xaaaa5500, 0x90909090, 0x94949494, 0xa4a4a4a4,
    0xa9a59450, 0x2a0a4250, 0xa5945040, 0x0a425054, 0xa5a5a500, 0x55a0a0a0, 0xa8a85454,
    0x6a6a4040, 0xa4a45000, 0x1a1a0500, 0x0050a4a4, 0xaaa59090, 0x14696914, 0x69691400,
    0xa08585a0, 0xaa821414, 0x50a4a450, 0x6a5a0200, 0xa9a58000, 0x5090a0a8, 0xa8a09050,
    0x24242424, 0x00aa5500, 0x24924924, 0x24499224, 0x50a50a50, 0x500aa550, 0xaaaa4444,
    0x66660000, 0xa5a0a5a0, 0x50a050a0, 0x69286928, 0x44aaaa44, 0x66666600, 0xaa444444,
    0x54a854a8, 0x95809580, 0x96969600, 0xa85454a8, 0x80959580, 0xaa141414, 0x96960000,
    0xaaaa1414, 0xa05050a0, 0xa0a5a5a0, 0x96000000, 0x40804080, 0xa9a8a9a8, 0xaaaaaa44,
    0x2a4a5254,
};

static const u8 ANCHOR2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15,
};

static const u8 ANCHOR3A[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3,
};

static const u8 ANCHOR3B[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8,
};

static const u8 WEIGHTS2[4] = { 0, 21, 43, 64 };
static const u8 WEIGHTS3[8] = { 0, 9, 18, 27, 37, 46, 55, 64 };
static const u8 WEIGHTS4[16] = { 0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64 };

static const u8 *weights_for(u32 bits) { return bits == 2 ? WEIGHTS2 : bits == 3 ? WEIGHTS3 : WEIGHTS4; }

static u32 subset_of(u32 subsets, u32 partition, u32 i) {
    if (subsets == 2) return (PARTITION2[partition] >> i) & 1;
    if (subsets == 3) return (PARTITION3[partition] >> (2 * i)) & 3;
    return 0;
}

static bool is_anchor(u32 subsets, u32 partition, u32 i) {
    if (i == 0) return true;
    if (subsets == 2) return i == ANCHOR2[partition];
    if (subsets == 3) return i == ANCHOR3A[partition] || i == ANCHOR3B[partition];
    return false;
}

// ---- BC7 -----------------------------------------------------------------------------------------

typedef struct Bc7Mode {
    u8 subsets, partition_bits, rotation_bits, selector_bits, color_bits, alpha_bits,
       endpoint_pbits, shared_pbits, index_bits, index2_bits;
} Bc7Mode;

static const Bc7Mode BC7_MODES[8] = {
    { 3, 4, 0, 0, 4, 0, 1, 0, 3, 0 },
    { 2, 6, 0, 0, 6, 0, 0, 1, 3, 0 },
    { 3, 6, 0, 0, 5, 0, 0, 0, 2, 0 },
    { 2, 6, 0, 0, 7, 0, 1, 0, 2, 0 },
    { 1, 0, 2, 1, 5, 6, 0, 0, 2, 3 },
    { 1, 0, 2, 0, 7, 8, 0, 0, 2, 2 },
    { 1, 0, 0, 0, 7, 7, 1, 0, 4, 0 },
    { 2, 6, 0, 0, 5, 5, 1, 0, 2, 0 },
};

INLINE u8 expand_bits(u32 v, u32 bits) {
    v <<= 8 - bits;
    return (u8)(v | (v >> bits));
}

INLINE u8 lerp64(u32 a, u32 b, u32 w) { return (u8)(((64 - w) * a + w * b + 32) >> 6); }

void bc7_block(const u8 *src, u8 out[64]) {
    if (!src[0]) { memset(out, 0, 64); return; }           // modo reservado: negro transparente
    u32 mode = (u32)__builtin_ctz(src[0]);
    const Bc7Mode *m = &BC7_MODES[mode];
    Bits128 b = { load_le64(src), load_le64(src + 8), mode + 1 };
    u32 partition = take(&b, m->partition_bits);
    u32 rotation = take(&b, m->rotation_bits);
    u32 selector = take(&b, m->selector_bits);
    u32 ends = 2u * m->subsets;
    u32 ep[6][4];
    for (u32 c = 0; c < 3; c++)
        for (u32 e = 0; e < ends; e++) ep[e][c] = take(&b, m->color_bits);
    for (u32 e = 0; e < ends; e++) ep[e][3] = m->alpha_bits ? take(&b, m->alpha_bits) : 255;
    u32 cb = m->color_bits, ab = m->alpha_bits;
    if (m->endpoint_pbits) {
        for (u32 e = 0; e < ends; e++) {
            u32 p = take(&b, 1);
            for (u32 c = 0; c < 3; c++) ep[e][c] = ep[e][c] << 1 | p;
            if (ab) ep[e][3] = ep[e][3] << 1 | p;
        }
        cb++;
        if (ab) ab++;
    }
    if (m->shared_pbits) {
        for (u32 s = 0; s < m->subsets; s++) {
            u32 p = take(&b, 1);
            for (u32 k = 0; k < 2; k++) {
                for (u32 c = 0; c < 3; c++) ep[2 * s + k][c] = ep[2 * s + k][c] << 1 | p;
                if (ab) ep[2 * s + k][3] = ep[2 * s + k][3] << 1 | p;
            }
        }
        cb++;
        if (ab) ab++;
    }
    for (u32 e = 0; e < ends; e++) {
        for (u32 c = 0; c < 3; c++) ep[e][c] = expand_bits(ep[e][c], cb);
        if (ab) ep[e][3] = expand_bits(ep[e][3], ab);
    }
    u32 idx[16], idx2[16] = { 0 };
    for (u32 i = 0; i < 16; i++) idx[i] = take(&b, m->index_bits - (is_anchor(m->subsets, partition, i) ? 1 : 0));
    if (m->index2_bits)
        for (u32 i = 0; i < 16; i++) idx2[i] = take(&b, m->index2_bits - (i == 0 ? 1 : 0));
    const u8 *w1 = weights_for(m->index_bits), *w2 = weights_for(m->index2_bits ? m->index2_bits : m->index_bits);
    for (u32 i = 0; i < 16; i++) {
        u32 s = subset_of(m->subsets, partition, i);
        const u32 *e0 = ep[2 * s], *e1 = ep[2 * s + 1];
        // Modo 4 con selector: el color usa el segundo juego de indices (3 bits) y el alfa el primero.
        u32 wc = w1[idx[i]], wa = m->index2_bits ? w2[idx2[i]] : wc;
        if (selector) { wc = w2[idx2[i]]; wa = w1[idx[i]]; }
        u8 *px = out + 4 * i;
        for (u32 c = 0; c < 3; c++) px[c] = lerp64(e0[c], e1[c], wc);
        px[3] = lerp64(e0[3], e1[3], wa);
        if (rotation) { u8 t = px[3]; px[3] = px[rotation - 1]; px[rotation - 1] = t; }
    }
}

// ---- BC6H ----------------------------------------------------------------------------------------

typedef struct Bc6Mode {
    u8 subsets, transformed, partition_bits, endpoint_bits, delta_r, delta_g, delta_b;
} Bc6Mode;

static const Bc6Mode BC6_MODES[14] = {
    { 2, 1, 5, 10, 5, 5, 5 }, { 2, 1, 5, 7, 6, 6, 6 }, { 2, 1, 5, 11, 5, 4, 4 }, { 2, 1, 5, 11, 4, 5, 4 },
    { 2, 1, 5, 11, 4, 4, 5 }, { 2, 1, 5, 9, 5, 5, 5 }, { 2, 1, 5, 8, 6, 5, 5 }, { 2, 1, 5, 8, 5, 6, 5 },
    { 2, 1, 5, 8, 5, 5, 6 }, { 2, 0, 5, 6, 6, 6, 6 }, { 1, 0, 0, 10, 10, 10, 10 }, { 1, 1, 0, 11, 9, 9, 9 },
    { 1, 1, 0, 12, 8, 8, 8 }, { 1, 1, 0, 16, 4, 4, 4 },
};

// Donde va cada bit de extremos: (componente << 4) | bit, componentes r0 g0 b0 r1 g1 b1 r2 ... b3.
static const u8 BC6_PACKING[14][75] = {
    { 116, 132, 180, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38,
      39, 40, 41, 48, 49, 50, 51, 52, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 176, 160, 161, 162, 163, 80, 81, 82,
      83, 84, 177, 128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179 },
    { 117, 164, 165, 0, 1, 2, 3, 4, 5, 6, 176, 177, 132, 16, 17, 18, 19, 20, 21, 22, 133, 178, 116, 32, 33, 34, 35, 36,
      37, 38, 179, 181, 180, 48, 49, 50, 51, 52, 53, 112, 113, 114, 115, 64, 65, 66, 67, 68, 69, 160, 161, 162, 163, 80,
      81, 82, 83, 84, 85, 128, 129, 130, 131, 96, 97, 98, 99, 100, 101, 144, 145, 146, 147, 148, 149 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
      48, 49, 50, 51, 52, 10, 112, 113, 114, 115, 64, 65, 66, 67, 26, 176, 160, 161, 162, 163, 80, 81, 82, 83, 42, 177,
      128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
      48, 49, 50, 51, 10, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 26, 160, 161, 162, 163, 80, 81, 82, 83, 42, 177,
      128, 129, 130, 131, 96, 97, 98, 99, 176, 178, 144, 145, 146, 147, 116, 179 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
      48, 49, 50, 51, 10, 132, 112, 113, 114, 115, 64, 65, 66, 67, 26, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 42,
      128, 129, 130, 131, 96, 97, 98, 99, 177, 178, 144, 145, 146, 147, 180, 179 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 132, 16, 17, 18, 19, 20, 21, 22, 23, 24, 116, 32, 33, 34, 35, 36, 37, 38, 39, 40, 180,
      48, 49, 50, 51, 52, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 177,
      128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 164, 132, 16, 17, 18, 19, 20, 21, 22, 23, 178, 116, 32, 33, 34, 35, 36, 37, 38, 39, 179, 180,
      48, 49, 50, 51, 52, 53, 112, 113, 114, 115, 64, 65, 66, 67, 68, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 177,
      128, 129, 130, 131, 96, 97, 98, 99, 100, 101, 144, 145, 146, 147, 148, 149 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 176, 132, 16, 17, 18, 19, 20, 21, 22, 23, 117, 116, 32, 33, 34, 35, 36, 37, 38, 39, 165, 180,
      48, 49, 50, 51, 52, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 69, 160, 161, 162, 163, 80, 81, 82, 83, 84, 177,
      128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 177, 132, 16, 17, 18, 19, 20, 21, 22, 23, 133, 116, 32, 33, 34, 35, 36, 37, 38, 39, 181, 180,
      48, 49, 50, 51, 52, 164, 112, 113, 114, 115, 64, 65, 66, 67, 68, 176, 160, 161, 162, 163, 80, 81, 82, 83, 84, 85,
      128, 129, 130, 131, 96, 97, 98, 99, 100, 178, 144, 145, 146, 147, 148, 179 },
    { 0, 1, 2, 3, 4, 5, 164, 176, 177, 132, 16, 17, 18, 19, 20, 21, 117, 133, 178, 116, 32, 33, 34, 35, 36, 37, 165, 179,
      181, 180, 48, 49, 50, 51, 52, 53, 112, 113, 114, 115, 64, 65, 66, 67, 68, 69, 160, 161, 162, 163, 80, 81, 82, 83,
      84, 85, 128, 129, 130, 131, 96, 97, 98, 99, 100, 101, 144, 145, 146, 147, 148, 149 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
      48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
      48, 49, 50, 51, 52, 53, 54, 55, 56, 10, 64, 65, 66, 67, 68, 69, 70, 71, 72, 26, 80, 81, 82, 83, 84, 85, 86, 87, 88, 42 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
      48, 49, 50, 51, 52, 53, 54, 55, 11, 10, 64, 65, 66, 67, 68, 69, 70, 71, 27, 26, 80, 81, 82, 83, 84, 85, 86, 87, 43, 42 },
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41,
      48, 49, 50, 51, 15, 14, 13, 12, 11, 10, 64, 65, 66, 67, 31, 30, 29, 28, 27, 26, 80, 81, 82, 83, 47, 46, 45, 44, 43, 42 },
};

static int sign_extend(u32 v, u32 bits) {
    return (v & (1u << (bits - 1))) ? (int)(v | ~((1u << bits) - 1)) : (int)v;
}

static int bc6_unquantize(int v, u32 bits, bool is_signed) {
    if (!is_signed) {
        if (bits >= 15 || v == 0) return v;
        if (v == (1 << bits) - 1) return 0xFFFF;
        return ((v << 15) + 0x4000) >> (bits - 1);
    }
    if (bits >= 16) return v;
    bool negative = v < 0;
    int x = negative ? -v : v;
    if (x) x = x >= (1 << (bits - 1)) - 1 ? 0x7FFF : ((x << 15) + 0x4000) >> (bits - 1);
    return negative ? -x : x;
}

static float bc6_finish(int v, bool is_signed) {
    if (is_signed) return v < 0 ? half_to_float((u16)(0x8000 | ((-v * 31) / 32))) : half_to_float((u16)((v * 31) / 32));
    return half_to_float((u16)((v * 31) / 64));
}

void bc6h_block(const u8 *src, float out[48], bool is_signed) {
    u32 mode = src[0] & 0x1F, start = 5, endpoint_bits_total = 75, index_bits = 3;
    if ((mode & 3) < 2) { mode &= 3; start = 2; }
    else if ((mode & 3) == 2) { mode = 2 + (mode >> 2); endpoint_bits_total = 72; }
    else { mode = 10 + (mode >> 2); endpoint_bits_total = 60; index_bits = 4; }
    if (mode >= 14) { memset(out, 0, 48 * sizeof(float)); return; }
    const Bc6Mode *m = &BC6_MODES[mode];
    u32 raw[12] = { 0 };
    for (u32 i = 0; i < endpoint_bits_total; i++) {
        u32 where = BC6_PACKING[mode][i];
        raw[where >> 4] |= bit_at(src, start + i) << (where & 15);
    }
    u32 pos = start + endpoint_bits_total;
    u32 partition = 0;
    for (u32 i = 0; i < m->partition_bits; i++) partition |= bit_at(src, pos + i) << i;
    pos += m->partition_bits;
    u32 ends = m->subsets == 2 ? 12 : 6;
    int e[12];
    u32 mask = (1u << m->endpoint_bits) - 1;
    for (u32 i = 0; i < 3; i++) e[i] = is_signed ? sign_extend(raw[i], m->endpoint_bits) : (int)raw[i];
    const u32 delta[3] = { m->delta_r, m->delta_g, m->delta_b };
    for (u32 i = 3; i < ends; i++) {
        int v = (is_signed || m->transformed) ? sign_extend(raw[i], delta[i % 3]) : (int)raw[i];
        if (m->transformed) {
            v = (int)(((u32)(v + e[i % 3])) & mask);
            if (is_signed) v = sign_extend((u32)v, m->endpoint_bits);
        }
        e[i] = v;
    }
    for (u32 i = 0; i < ends; i++) e[i] = bc6_unquantize(e[i], m->endpoint_bits, is_signed);
    const u8 *w = weights_for(index_bits);
    for (u32 i = 0; i < 16; i++) {
        bool anchor = i == 0 || (m->subsets == 2 && i == ANCHOR2[partition]);
        u32 nb = index_bits - (anchor ? 1 : 0), idx = 0;
        for (u32 k = 0; k < nb; k++) idx |= bit_at(src, pos + k) << k;
        pos += nb;
        u32 s = subset_of(m->subsets, partition, i) * 6;
        for (u32 c = 0; c < 3; c++) {
            int v = (e[s + c] * (64 - w[idx]) + e[s + 3 + c] * w[idx]) >> 6;
            out[3 * i + c] = bc6_finish(v, is_signed);
        }
    }
}
