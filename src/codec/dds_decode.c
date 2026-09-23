// codec/dds_decode.c - DirectDraw Surface: texturas de juegos y motores 3D.
//
// Formatos: BC1..BC7 y BC6H (HDR), DXT2/DXT4 (premultiplicados), mas de 25 formatos DXGI sin
// comprimir (8/10/16 bits, half y float, 5:6:5, 5:5:5:1, 4:4:4:4, R11G11B10F, RGB9E5) y los
// heredados por mascaras (RGB, luminancia, A8L8, A8). Cubos, arreglos y volumenes: por defecto la
// cara/capa 0 del nivel 0; --frames all saca todas (volumenes: todas las rebanadas).
//
// Transparencia: el encabezado DX10 declara el modo de alfa. 2 = PREMULTIPLICADO: se
// des-premultiplica; 3 = OPACO: el canal alfa se ignora (texturas que lo usan para otra cosa).
// BC1 con c0 <= c1 trae alfa de 1 bit. BC5 (normal maps): --normal-z reconstruye el azul.
#include <math.h>
#include <stdio.h>
#include "codec.h"
#include "bcn.h"
#include "masks.h"
#include "float_image.h"
#include "../core/pool.h"

enum {
    DDS_HEADER = 124, DX10_HEADER = 20,        // encabezado sin contar la firma "DDS "
    DDSD_MIPMAPCOUNT = 0x20000, DDSD_DEPTH = 0x800000,
    DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40, DDPF_LUMINANCE = 0x20000,
    CAPS2_CUBEMAP = 0x200, CAPS2_FACES = 0xFC00, CAPS2_VOLUME = 0x200000,
    DX10_MISC_CUBE = 0x4, DIM_TEXTURE3D = 4,
    ALPHA_MODE_UNKNOWN = 0, ALPHA_MODE_STRAIGHT = 1, ALPHA_MODE_PREMULTIPLIED = 2, ALPHA_MODE_OPAQUE = 3,
    DDS_MAX_DIM = 65536, DDS_MAX_SURFACES = 4096,
};

typedef enum DdsKind {
    K_BC1, K_BC2, K_BC3, K_BC4, K_BC5, K_BC6H, K_BC7,
    K_RGBA8, K_BGRA8, K_BGRX8, K_RGB10A2, K_RGBA16, K_RGBA16F, K_RGBA32F, K_RGB32F,
    K_R8, K_RG8, K_A8, K_R16, K_RG16, K_R16F, K_R32F, K_B5G6R5, K_B5G5R5A1, K_B4G4R4A4,
    K_R11G11B10F, K_R9G9B9E5, K_MASKED,
} DdsKind;

typedef struct DdsFormat {
    DdsKind kind;
    u32     size;              // bytes por bloque de 4x4 o por pixel
    bool    block, is_signed, premultiplied, srgb, is_float, has_alpha, wide;
    char    name[32];
    // K_MASKED
    u32     masks[4];
    bool    luminance, alpha_only;
} DdsFormat;

typedef struct DxgiEntry {
    u32     dxgi;
    DdsKind kind;
    u8      size;
    u8      flags;             // 1 = signo, 2 = sRGB
    const char *name;
} DxgiEntry;

static const DxgiEntry DXGI_TABLE[] = {
    { 70, K_BC1, 8, 0, "BC1" }, { 71, K_BC1, 8, 0, "BC1" }, { 72, K_BC1, 8, 2, "BC1 sRGB" },
    { 73, K_BC2, 16, 0, "BC2" }, { 74, K_BC2, 16, 0, "BC2" }, { 75, K_BC2, 16, 2, "BC2 sRGB" },
    { 76, K_BC3, 16, 0, "BC3" }, { 77, K_BC3, 16, 0, "BC3" }, { 78, K_BC3, 16, 2, "BC3 sRGB" },
    { 79, K_BC4, 8, 0, "BC4" }, { 80, K_BC4, 8, 0, "BC4" }, { 81, K_BC4, 8, 1, "BC4 con signo" },
    { 82, K_BC5, 16, 0, "BC5" }, { 83, K_BC5, 16, 0, "BC5" }, { 84, K_BC5, 16, 1, "BC5 con signo" },
    { 94, K_BC6H, 16, 0, "BC6H" }, { 95, K_BC6H, 16, 0, "BC6H UF16" }, { 96, K_BC6H, 16, 1, "BC6H SF16" },
    { 97, K_BC7, 16, 0, "BC7" }, { 98, K_BC7, 16, 0, "BC7" }, { 99, K_BC7, 16, 2, "BC7 sRGB" },
    { 27, K_RGBA8, 4, 0, "R8G8B8A8" }, { 28, K_RGBA8, 4, 0, "R8G8B8A8" }, { 29, K_RGBA8, 4, 2, "R8G8B8A8 sRGB" },
    { 90, K_BGRA8, 4, 0, "B8G8R8A8" }, { 87, K_BGRA8, 4, 0, "B8G8R8A8" }, { 91, K_BGRA8, 4, 2, "B8G8R8A8 sRGB" },
    { 92, K_BGRX8, 4, 0, "B8G8R8X8" }, { 88, K_BGRX8, 4, 0, "B8G8R8X8" }, { 93, K_BGRX8, 4, 2, "B8G8R8X8 sRGB" },
    { 24, K_RGB10A2, 4, 0, "R10G10B10A2" }, { 11, K_RGBA16, 8, 0, "R16G16B16A16" }, { 10, K_RGBA16F, 8, 0, "R16G16B16A16 half" },
    { 2, K_RGBA32F, 16, 0, "R32G32B32A32 float" }, { 6, K_RGB32F, 12, 0, "R32G32B32 float" },
    { 61, K_R8, 1, 0, "R8" }, { 49, K_RG8, 2, 0, "R8G8" }, { 65, K_A8, 1, 0, "A8" }, { 56, K_R16, 2, 0, "R16" },
    { 35, K_RG16, 4, 0, "R16G16" }, { 54, K_R16F, 2, 0, "R16 half" }, { 41, K_R32F, 4, 0, "R32 float" },
    { 85, K_B5G6R5, 2, 0, "B5G6R5" }, { 86, K_B5G5R5A1, 2, 0, "B5G5R5A1" }, { 115, K_B4G4R4A4, 2, 0, "B4G4R4A4" },
    { 26, K_R11G11B10F, 4, 0, "R11G11B10 float" }, { 67, K_R9G9B9E5, 4, 0, "R9G9B9E5" },
};

static void set_format(DdsFormat *f, DdsKind kind, u32 size, const char *name) {
    memset(f, 0, sizeof *f);
    f->kind = kind;
    f->size = size;
    f->block = kind <= K_BC7;
    f->is_float = kind == K_BC6H || kind == K_RGBA16F || kind == K_RGBA32F || kind == K_RGB32F || kind == K_R16F ||
                  kind == K_R32F || kind == K_R11G11B10F || kind == K_R9G9B9E5;
    f->has_alpha = kind == K_BC1 || kind == K_BC2 || kind == K_BC3 || kind == K_BC7 || kind == K_RGBA8 || kind == K_BGRA8 ||
                   kind == K_RGB10A2 || kind == K_RGBA16 || kind == K_RGBA16F || kind == K_RGBA32F || kind == K_A8 ||
                   kind == K_B5G5R5A1 || kind == K_B4G4R4A4;
    f->wide = kind == K_RGB10A2 || kind == K_RGBA16 || kind == K_R16 || kind == K_RG16;
    snprintf(f->name, sizeof f->name, "%s", name);
}

#define FOURCC(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)

static bool legacy_format(const u8 *pf, DdsFormat *f) {
    u32 flags = load_le32(pf + 4), fourcc = load_le32(pf + 8), bits = load_le32(pf + 12);
    if (flags & DDPF_FOURCC) {
        switch (fourcc) {
        case FOURCC('D', 'X', 'T', '1'): set_format(f, K_BC1, 8, "DXT1"); return true;
        case FOURCC('D', 'X', 'T', '2'): set_format(f, K_BC2, 16, "DXT2 premultiplicado"); f->premultiplied = true; return true;
        case FOURCC('D', 'X', 'T', '3'): set_format(f, K_BC2, 16, "DXT3"); return true;
        case FOURCC('D', 'X', 'T', '4'): set_format(f, K_BC3, 16, "DXT4 premultiplicado"); f->premultiplied = true; return true;
        case FOURCC('D', 'X', 'T', '5'): set_format(f, K_BC3, 16, "DXT5"); return true;
        case FOURCC('A', 'T', 'I', '1'): case FOURCC('B', 'C', '4', 'U'): set_format(f, K_BC4, 8, "BC4 (ATI1)"); return true;
        case FOURCC('B', 'C', '4', 'S'): set_format(f, K_BC4, 8, "BC4 con signo"); f->is_signed = true; return true;
        case FOURCC('A', 'T', 'I', '2'): case FOURCC('B', 'C', '5', 'U'): set_format(f, K_BC5, 16, "BC5 (ATI2)"); return true;
        case FOURCC('B', 'C', '5', 'S'): set_format(f, K_BC5, 16, "BC5 con signo"); f->is_signed = true; return true;
        case 36:  set_format(f, K_RGBA16, 8, "A16B16G16R16"); return true;
        case 111: set_format(f, K_R16F, 2, "R16F"); return true;
        case 113: set_format(f, K_RGBA16F, 8, "A16B16G16R16F"); return true;
        case 114: set_format(f, K_R32F, 4, "R32F"); return true;
        case 116: set_format(f, K_RGBA32F, 16, "A32B32G32R32F"); return true;
        default: return false;
        }
    }
    if (!(flags & (DDPF_RGB | DDPF_LUMINANCE | DDPF_ALPHA)) || !bits || bits > 32 || bits % 8) return false;
    set_format(f, K_MASKED, bits / 8, "");
    for (int i = 0; i < 4; i++) f->masks[i] = load_le32(pf + 16 + 4 * i);
    if (!(flags & (DDPF_ALPHAPIXELS | DDPF_ALPHA))) f->masks[3] = 0;
    f->luminance = (flags & DDPF_LUMINANCE) != 0;
    f->alpha_only = (flags & DDPF_ALPHA) && !(flags & (DDPF_RGB | DDPF_LUMINANCE));
    f->has_alpha = f->masks[3] != 0;
    for (int i = 0; i < 4; i++) if (bit_width(f->masks[i] >> (f->masks[i] ? __builtin_ctz(f->masks[i]) : 0)) > 8) f->wide = true;
    snprintf(f->name, sizeof f->name, "%s%u bits%s", f->luminance ? "luminancia " : f->alpha_only ? "alfa " : "RGB ", bits,
             f->has_alpha && !f->alpha_only ? " + alfa" : "");
    return true;
}

// ---- conversion de pixeles --------------------------------------------------------------------

// R11G11B10 / R9G9B9E5: flotantes sin signo empaquetados.
static float small_float(u32 v, u32 mant_bits) {
    u32 exp = v >> mant_bits, mant = v & ((1u << mant_bits) - 1);
    if (exp == 0) return ldexpf((float)mant, -14 - (int)mant_bits);
    if (exp == 31) return mant ? NAN : INFINITY;
    return ldexpf(1.0f + (float)mant / (float)(1u << mant_bits), (int)exp - 15);
}

typedef struct DdsJob {
    const DdsFormat  *fmt;
    const u8         *data;
    size_t            avail;
    u32               w, h;
    Image            *out8;          // formatos enteros (8 o 16 bits)
    float            *outf;          // formatos flotantes
    const MaskChannel *ch;
    bool              normal_z;
} DdsJob;

static void put8(const DdsJob *j, u32 x, u32 y, u32 r, u32 g, u32 b, u32 a) {
    u8 *p = image_row(j->out8, y) + 4 * (size_t)x;
    p[0] = (u8)r; p[1] = (u8)g; p[2] = (u8)b; p[3] = (u8)a;
}

static void put16(const DdsJob *j, u32 x, u32 y, u16 r, u16 g, u16 b, u16 a) {
    if (j->out8->depth == 8) { put8(j, x, y, u16_to_u8(r), u16_to_u8(g), u16_to_u8(b), u16_to_u8(a)); return; }
    u16 *p = (u16 *)image_row(j->out8, y) + 4 * (size_t)x;
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static void putf(const DdsJob *j, u32 x, u32 y, float r, float g, float b, float a) {
    float *p = j->outf + 4 * ((size_t)y * j->w + x);
    p[0] = r; p[1] = g; p[2] = b; p[3] = a;
}

static u8 snorm_to_u8(int v) { return (u8)((v + 127) * 255 / 254); }

static void decode_block_rows(void *arg, size_t by0, size_t by1) {
    const DdsJob *j = arg;
    const DdsFormat *f = j->fmt;
    u32 bw = (j->w + 3) / 4;
    for (size_t by = by0; by < by1; by++) {
        for (u32 bx = 0; bx < bw; bx++) {
            size_t at = (by * bw + bx) * f->size;
            if (at + f->size > j->avail) return;            // archivo cortado: el resto queda en 0
            const u8 *src = j->data + at;
            u8 px[64];
            float fx[48];
            int c0[16], c1[16];
            switch (f->kind) {
            case K_BC1: bc1_block(src, px, false); break;
            case K_BC2: bc2_block(src, px); break;
            case K_BC3: bc3_block(src, px); break;
            case K_BC7: bc7_block(src, px); break;
            case K_BC4:
                bc_channel_block(src, c0, f->is_signed);
                for (int i = 0; i < 16; i++) { u8 v = f->is_signed ? snorm_to_u8(c0[i]) : (u8)c0[i]; px[4 * i] = px[4 * i + 1] = px[4 * i + 2] = v; px[4 * i + 3] = 255; }
                break;
            case K_BC5:
                bc_channel_block(src, c0, f->is_signed);
                bc_channel_block(src + 8, c1, f->is_signed);
                for (int i = 0; i < 16; i++) {
                    u8 r = f->is_signed ? snorm_to_u8(c0[i]) : (u8)c0[i], g = f->is_signed ? snorm_to_u8(c1[i]) : (u8)c1[i], b = 0;
                    if (j->normal_z) {
                        float nx = r / 127.5f - 1.0f, ny = g / 127.5f - 1.0f;
                        float nz = sqrtf(fmaxf(0.0f, 1.0f - nx * nx - ny * ny));
                        b = (u8)((nz * 0.5f + 0.5f) * 255.0f + 0.5f);
                    }
                    px[4 * i] = r; px[4 * i + 1] = g; px[4 * i + 2] = b; px[4 * i + 3] = 255;
                }
                break;
            default:   // K_BC6H
                bc6h_block(src, fx, f->is_signed);
                break;
            }
            for (u32 i = 0; i < 16; i++) {
                u32 x = bx * 4 + (i & 3), y = (u32)by * 4 + (i >> 2);
                if (x >= j->w || y >= j->h) continue;
                if (f->kind == K_BC6H) putf(j, x, y, fx[3 * i], fx[3 * i + 1], fx[3 * i + 2], 1.0f);
                else put8(j, x, y, px[4 * i], px[4 * i + 1], px[4 * i + 2], px[4 * i + 3]);
            }
        }
    }
}

static void decode_pixel_rows(void *arg, size_t y0, size_t y1) {
    const DdsJob *j = arg;
    const DdsFormat *f = j->fmt;
    for (size_t y = y0; y < y1; y++) {
        for (u32 x = 0; x < j->w; x++) {
            size_t at = (y * j->w + x) * f->size;
            if (at + f->size > j->avail) return;
            const u8 *s = j->data + at;
            u32 v32 = f->size >= 4 ? load_le32(s) : f->size == 2 ? load_le16(s) : s[0];
            u32 yy = (u32)y;
            switch (f->kind) {
            case K_RGBA8:  put8(j, x, yy, s[0], s[1], s[2], s[3]); break;
            case K_BGRA8:  put8(j, x, yy, s[2], s[1], s[0], s[3]); break;
            case K_BGRX8:  put8(j, x, yy, s[2], s[1], s[0], 255); break;
            case K_R8:     put8(j, x, yy, s[0], s[0], s[0], 255); break;
            case K_RG8:    put8(j, x, yy, s[0], s[1], 0, 255); break;
            case K_A8:     put8(j, x, yy, 0, 0, 0, s[0]); break;
            case K_RGB10A2:
                put16(j, x, yy, (u16)(((v32 & 1023) * 65535 + 511) / 1023), (u16)((((v32 >> 10) & 1023) * 65535 + 511) / 1023),
                      (u16)((((v32 >> 20) & 1023) * 65535 + 511) / 1023), (u16)((v32 >> 30) * 21845));
                break;
            case K_RGBA16: put16(j, x, yy, load_le16(s), load_le16(s + 2), load_le16(s + 4), load_le16(s + 6)); break;
            case K_R16:    { u16 v = load_le16(s); put16(j, x, yy, v, v, v, 65535); break; }
            case K_RG16:   put16(j, x, yy, load_le16(s), load_le16(s + 2), 0, 65535); break;
            case K_B5G6R5: {
                u32 r = (v32 >> 11) & 31, g = (v32 >> 5) & 63, b = v32 & 31;
                put8(j, x, yy, r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2, 255);
                break;
            }
            case K_B5G5R5A1: {
                u32 r = (v32 >> 10) & 31, g = (v32 >> 5) & 31, b = v32 & 31;
                put8(j, x, yy, r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, (v32 >> 15) ? 255 : 0);
                break;
            }
            case K_B4G4R4A4:
                put8(j, x, yy, ((v32 >> 8) & 15) * 17, ((v32 >> 4) & 15) * 17, (v32 & 15) * 17, ((v32 >> 12) & 15) * 17);
                break;
            case K_RGBA16F:
                putf(j, x, yy, half_to_float(load_le16(s)), half_to_float(load_le16(s + 2)), half_to_float(load_le16(s + 4)),
                     half_to_float(load_le16(s + 6)));
                break;
            case K_RGBA32F:
                putf(j, x, yy, f32_from_bits(load_le32(s)), f32_from_bits(load_le32(s + 4)), f32_from_bits(load_le32(s + 8)),
                     f32_from_bits(load_le32(s + 12)));
                break;
            case K_RGB32F:
                putf(j, x, yy, f32_from_bits(load_le32(s)), f32_from_bits(load_le32(s + 4)), f32_from_bits(load_le32(s + 8)), 1.0f);
                break;
            case K_R16F:   { float v = half_to_float(load_le16(s)); putf(j, x, yy, v, v, v, 1.0f); break; }
            case K_R32F:   { float v = f32_from_bits(v32); putf(j, x, yy, v, v, v, 1.0f); break; }
            case K_R11G11B10F:
                putf(j, x, yy, small_float(v32 & 0x7FF, 6), small_float((v32 >> 11) & 0x7FF, 6), small_float(v32 >> 22, 5), 1.0f);
                break;
            case K_R9G9B9E5: {
                float scale = ldexpf(1.0f, (int)(v32 >> 27) - 15 - 9);
                putf(j, x, yy, (float)(v32 & 511) * scale, (float)((v32 >> 9) & 511) * scale, (float)((v32 >> 18) & 511) * scale, 1.0f);
                break;
            }
            default: {  // K_MASKED
                u32 px = 0;
                for (u32 k = 0; k < f->size; k++) px |= (u32)s[k] << (8 * k);
                u16 r = mask_channel_value(&j->ch[0], px), g = mask_channel_value(&j->ch[1], px), b = mask_channel_value(&j->ch[2], px);
                u16 a = f->masks[3] ? mask_channel_value(&j->ch[3], px) : 65535;
                if (f->luminance) g = b = r;
                if (f->alpha_only) r = g = b = 0;
                put16(j, x, yy, r, g, b, a);
                break;
            }
            }
        }
    }
}

// ---- tamanos -----------------------------------------------------------------------------------

static size_t level_bytes(const DdsFormat *f, u32 w, u32 h) {
    if (f->block) return (size_t)MAX(1u, (w + 3) / 4) * MAX(1u, (h + 3) / 4) * f->size;
    return (size_t)w * h * f->size;
}

static size_t chain_bytes(const DdsFormat *f, u32 w, u32 h, u32 depth, u32 mips) {
    size_t total = 0;
    for (u32 l = 0; l < mips; l++) {
        total += level_bytes(f, MAX(1u, w >> l), MAX(1u, h >> l)) * MAX(1u, depth >> l);
        if ((w >> l) <= 1 && (h >> l) <= 1 && (depth >> l) <= 1) break;
    }
    return total;
}

static void force_opaque(Image *im) {
    for (u32 y = 0; y < im->height; y++) {
        u8 *p = image_row(im, y);
        if (im->depth == 8) for (u32 x = 0; x < im->width; x++) p[4 * (size_t)x + 3] = 255;
        else for (u32 x = 0; x < im->width; x++) ((u16 *)p)[4 * (size_t)x + 3] = 65535;
    }
}

static Status decode_surface(const DecodeContext *ctx, const DdsFormat *fmt, const u8 *data, size_t avail, u32 w, u32 h,
                             u32 alpha_mode, Frame *frame, bool *truncated, ToneOperator *tone) {
    DdsJob job = { fmt, data, avail, w, h, NULL, NULL, NULL, ctx->opt->bc5_reconstruct_z };
    size_t need = level_bytes(fmt, w, h);
    *truncated = avail < need;
    MaskChannel ch[4];
    memset(ch, 0, sizeof ch);
    if (fmt->kind == K_MASKED) {
        for (int i = 0; i < 4; i++)
            if (!mask_channel_init(&ch[i], fmt->masks[i])) { for (int k = 0; k < i; k++) mask_channel_free(&ch[k]); return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria"); }
        job.ch = ch;
    }
    Status s = ST_OK;
    if (fmt->is_float) {
        size_t bytes;
        if (!mul_ok((size_t)w * h, 16, &bytes)) return diag_fail(ctx->diag, ST_TOO_LARGE, "DDS demasiado grande");
        job.outf = mem_calloc(bytes);
        if (!job.outf) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    } else {
        job.out8 = &frame->image;
        s = image_alloc(&frame->image, w, h, fmt->wide ? 16 : 8, true, ctx->opt->max_pixels, ctx->diag);
    }
    if (s == ST_OK) {
        if (fmt->block) parallel_range((h + 3) / 4, 16, decode_block_rows, &job);
        else parallel_range(h, 64, decode_pixel_rows, &job);
    }
    for (int i = 0; i < 4; i++) mask_channel_free(&ch[i]);
    // Flotante premultiplicado: se divide en luz lineal, ANTES del tonemap (que no es lineal).
    bool unpremultiply = fmt->premultiplied && alpha_mode != ALPHA_MODE_OPAQUE;
    if (s == ST_OK && fmt->is_float) {
        if (unpremultiply) float_image_unpremultiply(job.outf, (size_t)w * h);
        s = float_image_to_rgba16(job.outf, w, h, ctx->opt, &frame->image, tone, ctx->diag);
    }
    mem_free(job.outf);
    if (s != ST_OK) return s;
    if (alpha_mode == ALPHA_MODE_OPAQUE) force_opaque(&frame->image);
    else if (unpremultiply && !fmt->is_float) image_unpremultiply(&frame->image);
    return ST_OK;
}

static Status dds_decode(const DecodeContext *ctx, Decoded *out) {
    Diag *diag = ctx->diag;
    const u8 *d = ctx->data;
    if (ctx->size < 4 + DDS_HEADER || memcmp(d, "DDS ", 4) != 0 || load_le32(d + 4) != 124)
        return diag_fail(diag, ST_CORRUPT, "encabezado DDS invalido");
    const u8 *hdr = d + 4;
    u32 flags = load_le32(hdr + 4), h = load_le32(hdr + 8), w = load_le32(hdr + 12);
    u32 depth = (flags & DDSD_DEPTH) ? MAX(1u, load_le32(hdr + 20)) : 1;
    u32 mips = (flags & DDSD_MIPMAPCOUNT) ? MAX(1u, load_le32(hdr + 24)) : 1;
    const u8 *pf = hdr + 72;
    u32 caps2 = load_le32(hdr + 108);
    if (!w || !h || w > DDS_MAX_DIM || h > DDS_MAX_DIM || mips > 32) return diag_fail(diag, ST_CORRUPT, "dimensiones DDS invalidas");

    DdsFormat fmt;
    u32 alpha_mode = ALPHA_MODE_UNKNOWN, surfaces = 1;
    bool volume = (caps2 & CAPS2_VOLUME) && depth > 1;
    size_t data_start = 4 + DDS_HEADER;
    if ((load_le32(pf + 4) & DDPF_FOURCC) && load_le32(pf + 8) == FOURCC('D', 'X', '1', '0')) {
        if (ctx->size < data_start + DX10_HEADER) return diag_fail(diag, ST_TRUNCATED, "encabezado DX10 cortado");
        const u8 *x = d + data_start;
        u32 dxgi = load_le32(x), dim = load_le32(x + 4), misc = load_le32(x + 8), array = MAX(1u, load_le32(x + 12));
        alpha_mode = load_le32(x + 16) & 7;
        data_start += DX10_HEADER;
        const DxgiEntry *e = NULL;
        for (size_t i = 0; i < ARRAY_LEN(DXGI_TABLE) && !e; i++) if (DXGI_TABLE[i].dxgi == dxgi) e = &DXGI_TABLE[i];
        if (!e) return diag_fail(diag, ST_UNSUPPORTED, "DDS con formato DXGI %u", dxgi);
        set_format(&fmt, e->kind, e->size, e->name);
        fmt.is_signed = e->flags & 1;
        fmt.srgb = (e->flags & 2) != 0;
        fmt.premultiplied = alpha_mode == ALPHA_MODE_PREMULTIPLIED;
        volume = dim == DIM_TEXTURE3D && depth > 1;
        surfaces = volume ? 1 : array * ((misc & DX10_MISC_CUBE) ? 6 : 1);
    } else {
        if (!legacy_format(pf, &fmt)) return diag_fail(diag, ST_UNSUPPORTED, "DDS con formato de pixel desconocido");
        if (caps2 & CAPS2_CUBEMAP) surfaces = MAX(1u, (u32)__builtin_popcount(caps2 & CAPS2_FACES));
    }
    if (surfaces > DDS_MAX_SURFACES) return diag_fail(diag, ST_CORRUPT, "DDS con %u superficies", surfaces);
    u32 frames_total = volume ? depth : surfaces;
    const DecodeOptions *opt = ctx->opt;
    if (opt->frame_mode == FRAMES_ONE && opt->frame_index >= frames_total)
        return diag_fail(diag, ST_INVALID, "--frame %u: el DDS tiene %u (0..%u)", opt->frame_index, frames_total, frames_total - 1);

    size_t chain = chain_bytes(&fmt, w, h, volume ? depth : 1, mips), level0 = level_bytes(&fmt, w, h);
    ToneOperator tone = TONE_CLIP;
    bool any_truncated = false;
    for (u32 i = 0; i < frames_total; i++) {
        if (!frame_wanted(opt, i, frames_total)) continue;
        size_t offset = data_start + (volume ? (size_t)i * level0 : (size_t)i * chain);
        if (offset >= ctx->size) { diag_warn(diag, "DDS cortado: falta la superficie %u", i); break; }
        Frame *f = decoded_add_frame(out);
        if (!f) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
        f->index = i;
        bool truncated;
        Status s = decode_surface(ctx, &fmt, d + offset, ctx->size - offset, w, h, alpha_mode, f, &truncated, &tone);
        if (s != ST_OK) return s;
        any_truncated |= truncated;
    }
    if (!decoded_frame_count(out)) return diag_fail(diag, ST_TRUNCATED, "DDS sin datos de imagen");
    if (any_truncated) diag_warn(diag, "DDS cortado: los bloques que faltan quedan transparentes");
    if (frames_total > 1 && opt->frame_mode == FRAMES_DEFAULT)
        diag_warn(diag, "el DDS tiene %u %s: se convirtio la primera (--frames all para todas)", frames_total, volume ? "rebanadas" : "superficies");

    SourceInfo *src = &out->source;
    source_set(src, "DDS", "propio");
    src->bits = fmt.is_float ? 16 : fmt.wide ? 16 : 8;
    src->channels = 4;
    src->is_float = fmt.is_float;
    src->total_frames = frames_total;
    bool alpha = fmt.has_alpha && alpha_mode != ALPHA_MODE_OPAQUE;
    src->has_alpha = alpha;
    src->alpha_origin = !alpha ? ALPHA_ORIGIN_NONE : fmt.premultiplied ? ALPHA_ORIGIN_PREMULTIPLIED : ALPHA_ORIGIN_CHANNEL;
    static const char *const MODES[] = { "alfa sin declarar", "alfa directo", "alfa premultiplicado", "alfa ignorado (opaco)", "alfa de uso propio" };
    source_detail(src, "%s · %ux%u%s%s%s%s", fmt.name, w, h, mips > 1 ? " · con mipmaps" : "",
                  surfaces == 6 ? " · cubo" : surfaces > 1 ? " · arreglo" : volume ? " · volumen" : "",
                  alpha_mode && alpha_mode <= 4 ? " · " : "", alpha_mode && alpha_mode <= 4 ? MODES[alpha_mode] : "");
    if (fmt.is_float) diag_warn(diag, "DDS flotante/HDR: tonemap %s", tone_name(tone));
    return ST_OK;
}

static u8 dds_probe(const u8 *d, size_t n, const char *ext) {
    UNUSED(ext);
    return n >= 4 + DDS_HEADER && memcmp(d, "DDS ", 4) == 0 && load_le32(d + 4) == 124 ? 100 : 0;
}

const Codec CODEC_DDS = {
    .name = "DDS",
    .extensions = "dds",
    .description = "DirectDraw Surface: BC1-BC7, BC6H HDR, DXT2/4 premultiplicado, 25+ formatos DXGI, cubos y volumenes",
    .probe = dds_probe,
    .decode = dds_decode,
};
