// codec/psd_decode.c - Photoshop PSD/PSB: la imagen compuesta ("merged") que Photoshop guarda al
// final del archivo, en 1/8/16/32 bits, modos bitmap, gris, paleta, RGB, CMYK, Lab, duotono y
// multicanal, sin comprimir, PackBits, ZIP y ZIP con prediccion.
//
// Lo delicado es la TRANSPARENCIA:
//   - el compuesto tiene alfa solo si el conteo de capas es NEGATIVO ("el primer canal alfa es la
//     transparencia del compuesto"); un canal extra sin eso es una seleccion guardada o una tinta
//     plana, NO alfa. Para documentos de 16/32 bits el conteo esta en el bloque Lr16/Lr32.
//   - Photoshop guarda el compuesto MATEADO SOBRE BLANCO: donde el alfa es parcial, el color esta
//     mezclado con blanco. Se deshace (color = (c - (1 - a)) / a) salvo --psd-keep-matte.
#include <math.h>
#include <stdio.h>
#include "codec.h"
#include "reader.h"
#include "zlib.h"
#include "float_image.h"
#include "../core/colorspace.h"

enum {
    MODE_BITMAP = 0, MODE_GRAY = 1, MODE_INDEXED = 2, MODE_RGB = 3, MODE_CMYK = 4,
    MODE_MULTICHANNEL = 7, MODE_DUOTONE = 8, MODE_LAB = 9,
    COMP_RAW = 0, COMP_RLE = 1, COMP_ZIP = 2, COMP_ZIP_PREDICT = 3,
    RES_ICC = 1039, RES_TRANSPARENT_INDEX = 1047,
    PSD_MAX_CHANNELS = 56, PSD_MAX_DIM = 30000, PSB_MAX_DIM = 300000,
};

typedef struct PsdInfo {
    bool psb;
    u32  channels, width, height, depth, mode;
    const u8 *color_data;
    size_t    color_size;
    bool has_merged_alpha;
    int  transparent_index;           // modo paleta: indice transparente (recurso 1047) o -1
} PsdInfo;

// ---- secciones ---------------------------------------------------------------------------------

static u64 rd_len(Reader *r, bool wide) { return wide ? rd_be64(r) : rd_be32(r); }

// Busca en los bloques "8BIM"/"8B64" de informacion adicional una clave Lr16/Lr32 y devuelve su
// conteo de capas (con signo) si esta.
static bool find_layer_count_in_tagged(const u8 *p, const u8 *end, bool psb, i16 *count) {
    while (end - p >= 12) {
        if (memcmp(p, "8BIM", 4) != 0 && memcmp(p, "8B64", 4) != 0) return false;
        const u8 *key = p + 4;
        bool long_len = psb && (memcmp(key, "LMsk", 4) == 0 || memcmp(key, "Lr16", 4) == 0 || memcmp(key, "Lr32", 4) == 0 ||
                                memcmp(key, "Layr", 4) == 0 || memcmp(key, "Mt16", 4) == 0 || memcmp(key, "Mt32", 4) == 0 ||
                                memcmp(key, "Mtrn", 4) == 0 || memcmp(key, "Alph", 4) == 0 || memcmp(key, "FMsk", 4) == 0 ||
                                memcmp(key, "lnk2", 4) == 0 || memcmp(key, "FEid", 4) == 0 || memcmp(key, "FXid", 4) == 0 ||
                                memcmp(key, "PxSD", 4) == 0);
        u64 len = long_len ? load_be64(p + 8) : load_be32(p + 8);
        const u8 *data = p + 8 + (long_len ? 8 : 4);
        if (data > end || len > (u64)(end - data)) return false;
        if ((memcmp(key, "Lr16", 4) == 0 || memcmp(key, "Lr32", 4) == 0 || memcmp(key, "Layr", 4) == 0) && len >= 2) {
            *count = (i16)load_be16(data);
            return true;
        }
        p = data + ((len + 3) & ~(u64)3);
    }
    return false;
}

static Status parse_psd(const DecodeContext *ctx, PsdInfo *info, Reader *r, Decoded *out) {
    Diag *diag = ctx->diag;
    if (!rd_match(r, "8BPS", 4)) return diag_fail(diag, ST_CORRUPT, "no es PSD");
    u16 version = rd_be16(r);
    info->psb = version == 2;
    if (version != 1 && version != 2) return diag_fail(diag, ST_UNSUPPORTED, "PSD version %u", version);
    rd_skip(r, 6);
    info->channels = rd_be16(r);
    info->height = rd_be32(r);
    info->width = rd_be32(r);
    info->depth = rd_be16(r);
    info->mode = rd_be16(r);
    if (r->bad) return diag_fail(diag, ST_TRUNCATED, "encabezado PSD incompleto");
    u32 max_dim = info->psb ? PSB_MAX_DIM : PSD_MAX_DIM;
    if (!info->channels || info->channels > PSD_MAX_CHANNELS || !info->width || !info->height || info->width > max_dim || info->height > max_dim)
        return diag_fail(diag, ST_CORRUPT, "encabezado PSD invalido (%u canales, %u x %u)", info->channels, info->width, info->height);
    if (info->depth != 1 && info->depth != 8 && info->depth != 16 && info->depth != 32)
        return diag_fail(diag, ST_UNSUPPORTED, "PSD de %u bits", info->depth);

    u32 color_len = rd_be32(r);
    info->color_data = rd_bytes(r, color_len);
    info->color_size = color_len;

    // Recursos de imagen: perfil ICC e indice transparente.
    info->transparent_index = -1;
    u32 res_len = rd_be32(r);
    const u8 *res = rd_bytes(r, res_len);
    if (res) {
        Reader rr = reader_make(res, res_len);
        while (rd_left(&rr) >= 12 && rd_match(&rr, "8BIM", 4)) {
            u16 id = rd_be16(&rr);
            u8 name_len = rd_u8(&rr);
            rd_skip(&rr, (size_t)(name_len + 1 + (name_len + 1) % 2) - 1);   // Pascal string par
            u32 size = rd_be32(&rr);
            const u8 *data = rd_bytes(&rr, size);
            if (!data) break;
            if (size & 1) rd_skip(&rr, 1);
            if (id == RES_ICC) blob_set(&out->meta.icc, data, size);
            else if (id == RES_TRANSPARENT_INDEX && size >= 2) info->transparent_index = load_be16(data);
        }
    }

    // Capas y mascaras: solo interesa el signo del conteo de capas.
    u64 lm_len = rd_len(r, info->psb);
    const u8 *lm = rd_bytes(r, (size_t)lm_len);
    if (lm && lm_len) {
        Reader lr = reader_make(lm, (size_t)lm_len);
        u64 layer_len = rd_len(&lr, info->psb);
        i16 count = 0;
        bool found = false;
        if (layer_len >= 2) { count = (i16)rd_be16(&lr); found = !lr.bad; rd_skip(&lr, (size_t)layer_len - 2); }
        else rd_skip(&lr, (size_t)layer_len);
        if (!found || count == 0) {
            u32 global_mask = rd_be32(&lr);
            rd_skip(&lr, global_mask);
            if (!lr.bad) found = find_layer_count_in_tagged(lr.p, lr.end, info->psb, &count) || found;
        }
        info->has_merged_alpha = found && count < 0;
    }
    if (r->bad) return diag_fail(diag, ST_TRUNCATED, "PSD cortado antes de la imagen compuesta");
    return ST_OK;
}

// ---- datos de canal --------------------------------------------------------------------------

static bool packbits(const u8 *src, size_t n, u8 *dst, size_t want) {
    size_t i = 0, o = 0;
    while (i < n && o < want) {
        i8 c = (i8)src[i++];
        if (c >= 0) {
            size_t count = (size_t)c + 1;
            if (i + count > n || o + count > want) return false;
            memcpy(dst + o, src + i, count);
            i += count;
            o += count;
        } else if (c != -128) {
            size_t count = (size_t)(1 - c);
            if (i >= n || o + count > want) return false;
            memset(dst + o, src[i++], count);
            o += count;
        }
    }
    return o == want;
}

// Deshace la prediccion de ZIP: diferencias por fila (16 bits big endian) o, en 32 bits, bytes
// "desentrelazados" (todos los bytes altos, luego los siguientes...) con diferencia por byte.
static void unpredict(u8 *plane, u32 w, u32 h, u32 depth) {
    size_t row = (size_t)w * depth / 8;
    for (u32 y = 0; y < h; y++) {
        u8 *p = plane + (size_t)y * row;
        if (depth == 8) {
            for (u32 x = 1; x < w; x++) p[x] = (u8)(p[x] + p[x - 1]);
        } else if (depth == 16) {
            for (u32 x = 1; x < w; x++) store_be16(p + 2 * x, (u16)(load_be16(p + 2 * x) + load_be16(p + 2 * x - 2)));
        } else if (depth == 32) {
            for (size_t i = 1; i < row; i++) p[i] = (u8)(p[i] + p[i - 1]);
            u8 *tmp = mem_alloc(row);
            if (!tmp) return;
            for (u32 x = 0; x < w; x++)
                for (u32 b = 0; b < 4; b++) tmp[4 * (size_t)x + b] = p[(size_t)b * w + x];
            memcpy(p, tmp, row);
            mem_free(tmp);
        }
    }
}

// Lee todos los canales del compuesto a planos contiguos: planes[c] = w*h*(depth/8) bytes.
static Status read_planes(const DecodeContext *ctx, const PsdInfo *info, Reader *r, u32 wanted, u8 **planes) {
    Diag *diag = ctx->diag;
    u16 comp = rd_be16(r);
    if (r->bad) return diag_fail(diag, ST_TRUNCATED, "PSD sin datos de imagen compuesta");
    size_t row = ((size_t)info->width * info->depth + 7) / 8, plane = row * info->height;
    for (u32 c = 0; c < wanted; c++) {
        planes[c] = mem_calloc(plane);
        if (!planes[c]) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
    }
    if (comp == COMP_RAW) {
        for (u32 c = 0; c < info->channels; c++) {
            const u8 *d = rd_bytes(r, plane);
            if (!d) { diag_warn(diag, "PSD cortado: faltan canales"); break; }
            if (c < wanted) memcpy(planes[c], d, plane);
        }
        return ST_OK;
    }
    if (comp == COMP_RLE) {
        size_t rows = (size_t)info->channels * info->height, entry = info->psb ? 4 : 2;
        const u8 *counts = rd_bytes(r, rows * entry);
        if (!counts) return diag_fail(diag, ST_TRUNCATED, "tabla RLE del PSD cortada");
        u32 bad_rows = 0;
        for (size_t i = 0; i < rows; i++) {
            size_t n = entry == 4 ? load_be32(counts + 4 * i) : load_be16(counts + 2 * i);
            const u8 *d = rd_bytes(r, n);
            u32 c = (u32)(i / info->height), y = (u32)(i % info->height);
            if (!d) { bad_rows += (u32)(rows - i); break; }
            if (c < wanted && !packbits(d, n, planes[c] + (size_t)y * row, row)) bad_rows++;
        }
        if (bad_rows) diag_warn(diag, "PSD: %u filas RLE dañadas o faltantes", bad_rows);
        return ST_OK;
    }
    if (comp == COMP_ZIP || comp == COMP_ZIP_PREDICT) {
        // El compuesto no guarda largos por canal: es UN flujo zlib con todos los planos seguidos.
        size_t total;
        if (!mul_ok(plane, info->channels, &total)) return diag_fail(diag, ST_TOO_LARGE, "PSD demasiado grande");
        u8 *all = mem_calloc(total);
        if (!all) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
        ZSpan span = { r->p, rd_left(r) };
        size_t got = 0;
        if (zlib_inflate_exact(&span, 1, all, total, &got, &(Diag){ 0 }) != ST_OK)
            diag_warn(diag, "PSD: datos ZIP dañados o cortados (%zu de %zu bytes)", got, total);
        for (u32 c = 0; c < wanted; c++) {
            memcpy(planes[c], all + (size_t)c * plane, plane);
            if (comp == COMP_ZIP_PREDICT) unpredict(planes[c], info->width, info->height, info->depth);
        }
        mem_free(all);
        return ST_OK;
    }
    return diag_fail(diag, ST_UNSUPPORTED, "compresion PSD %u", comp);
}

// ---- conversion a RGBA -------------------------------------------------------------------------

INLINE float sample(const u8 *plane, size_t i, u32 depth) {
    switch (depth) {
    case 8:  return plane[i] / 255.0f;
    case 16: return load_be16(plane + 2 * i) / 65535.0f;
    default: return f32_from_bits(load_be32(plane + 4 * i));
    }
}

static void lab_to_srgb_linear(float L, float a, float b, float out[3]) {
    // Lab (D50) -> XYZ (D50) -> XYZ (D65, Bradford) -> sRGB lineal.
    float fy = (L + 16.0f) / 116.0f, fx = fy + a / 500.0f, fz = fy - b / 200.0f;
    float e = 216.0f / 24389.0f, k = 24389.0f / 27.0f;
    float xr = fx * fx * fx > e ? fx * fx * fx : (116.0f * fx - 16.0f) / k;
    float yr = L > k * e ? fy * fy * fy : L / k;
    float zr = fz * fz * fz > e ? fz * fz * fz : (116.0f * fz - 16.0f) / k;
    float X = xr * 0.96422f, Y = yr, Z = zr * 0.82521f;
    float X65 = 0.9555766f * X - 0.0230393f * Y + 0.0631636f * Z;
    float Y65 = -0.0282895f * X + 1.0099416f * Y + 0.0210077f * Z;
    float Z65 = 0.0122982f * X - 0.0204830f * Y + 1.3299098f * Z;
    out[0] = 3.2404542f * X65 - 1.5371385f * Y65 - 0.4985314f * Z65;
    out[1] = -0.9692660f * X65 + 1.8760108f * Y65 + 0.0415560f * Z65;
    out[2] = 0.0556434f * X65 - 0.2040259f * Y65 + 1.0572252f * Z65;
}

static Status psd_decode(const DecodeContext *ctx, Decoded *out) {
    Diag *diag = ctx->diag;
    Reader r = reader_make(ctx->data, ctx->size);
    PsdInfo info = { 0 };
    Status st = parse_psd(ctx, &info, &r, out);
    if (st != ST_OK) return st;

    u32 color_ch;
    switch (info.mode) {
    case MODE_BITMAP: case MODE_GRAY: case MODE_INDEXED: case MODE_DUOTONE: case MODE_MULTICHANNEL: color_ch = 1; break;
    case MODE_RGB: case MODE_LAB: color_ch = 3; break;
    case MODE_CMYK: color_ch = 4; break;
    default: return diag_fail(diag, ST_UNSUPPORTED, "PSD en modo de color %u", info.mode);
    }
    if (info.channels < color_ch) return diag_fail(diag, ST_CORRUPT, "PSD con %u canales para un modo que usa %u", info.channels, color_ch);
    bool alpha = info.has_merged_alpha && info.channels > color_ch && info.mode != MODE_BITMAP;
    u32 wanted = color_ch + (alpha ? 1 : 0);
    u8 *planes[5] = { 0 };
    st = read_planes(ctx, &info, &r, wanted, planes);
    if (st != ST_OK) { for (int i = 0; i < 5; i++) mem_free(planes[i]); return st; }

    u32 w = info.width, h = info.height;
    size_t px_count = (size_t)w * h;
    bool is_float = info.depth == 32;
    bool unmatte = alpha && !ctx->opt->psd_keep_matte;
    colorspace_init();
    Frame *f = decoded_add_frame(out);
    if (!f) { for (int i = 0; i < 5; i++) mem_free(planes[i]); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }

    // Todo pasa por flotante: un solo camino para 8/16/32 bits y para Lab/CMYK.
    size_t bytes;
    float *rgba = mul_ok(px_count, 16, &bytes) ? mem_alloc(bytes) : NULL;
    if (!rgba) { for (int i = 0; i < 5; i++) mem_free(planes[i]); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }
    const u8 *pal = info.color_data && info.color_size >= 768 ? info.color_data : NULL;
    for (size_t i = 0; i < px_count; i++) {
        float c[3], a = 1.0f;
        switch (info.mode) {
        case MODE_BITMAP: {
            u32 x = (u32)(i % w), y = (u32)(i / w);
            u8 byte = planes[0][(size_t)y * ((w + 7) / 8) + x / 8];
            c[0] = c[1] = c[2] = (byte >> (7 - (x & 7))) & 1 ? 0.0f : 1.0f;     // 1 = negro
            break;
        }
        case MODE_INDEXED: {
            u32 k = planes[0][i];
            if (pal) { c[0] = pal[k] / 255.0f; c[1] = pal[256 + k] / 255.0f; c[2] = pal[512 + k] / 255.0f; }
            else c[0] = c[1] = c[2] = k / 255.0f;
            if ((int)k == info.transparent_index) a = 0.0f;
            break;
        }
        case MODE_RGB:
            for (int k = 0; k < 3; k++) c[k] = sample(planes[k], i, info.depth);
            break;
        case MODE_CMYK: {
            // PSD guarda CMYK invertido (0 = tinta plena): ingenuo pero correcto en proporcion.
            float cy = sample(planes[0], i, info.depth), mg = sample(planes[1], i, info.depth);
            float ye = sample(planes[2], i, info.depth), k = sample(planes[3], i, info.depth);
            c[0] = cy * k; c[1] = mg * k; c[2] = ye * k;
            break;
        }
        case MODE_LAB: {
            float L = sample(planes[0], i, info.depth) * 100.0f;
            float A = sample(planes[1], i, info.depth) * 255.0f - 128.0f, B = sample(planes[2], i, info.depth) * 255.0f - 128.0f;
            float lin[3];
            lab_to_srgb_linear(L, A, B, lin);
            for (int k = 0; k < 3; k++) c[k] = is_float ? lin[k] : linear_to_srgb(saturatef(lin[k]));
            break;
        }
        default:
            c[0] = c[1] = c[2] = sample(planes[0], i, info.depth);
            break;
        }
        if (alpha) a = saturatef(sample(planes[color_ch], i, info.depth));
        // Des-matear el blanco: se hace sobre los valores tal cual estan guardados (codificados
        // en 8/16 bits, lineales en 32), que es donde Photoshop hizo la mezcla.
        if (unmatte && a > 0.0f && a < 1.0f)
            for (int k = 0; k < 3; k++) c[k] = (c[k] - (1.0f - a)) / a;
        if (unmatte && a == 0.0f) c[0] = c[1] = c[2] = 0.0f;
        // 32 bits: lineal (va al tonemap). 8/16 bits: valores codificados tal cual.
        float *dst = rgba + 4 * i;
        dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2];
        dst[3] = a;
    }
    for (int i = 0; i < 5; i++) mem_free(planes[i]);

    ToneOperator used = TONE_CLIP;
    if (is_float) {
        st = float_image_to_rgba16(rgba, w, h, ctx->opt, &f->image, &used, diag);
    } else {
        // 8/16 bits: a la profundidad original, sin tonemap.
        st = image_alloc(&f->image, w, h, info.depth == 16 ? 16 : 8, false, ctx->opt->max_pixels, diag);
        for (u32 y = 0; y < h && st == ST_OK; y++) {
            for (u32 x = 0; x < w; x++) {
                const float *s = rgba + 4 * ((size_t)y * w + x);
                float e[4] = { saturatef(s[0]), saturatef(s[1]), saturatef(s[2]), saturatef(s[3]) };
                if (f->image.depth == 8) {
                    u8 *px = image_row(&f->image, y) + 4 * (size_t)x;
                    for (int k = 0; k < 4; k++) px[k] = (u8)(e[k] * 255.0f + 0.5f);
                } else {
                    u16 *px = (u16 *)image_row(&f->image, y) + 4 * (size_t)x;
                    for (int k = 0; k < 4; k++) px[k] = (u16)(e[k] * 65535.0f + 0.5f);
                }
            }
        }
    }
    mem_free(rgba);
    if (st != ST_OK) return st;

    static const char *const MODES[] = { "bitmap", "gris", "paleta", "RGB", "CMYK", "?", "?", "multicanal", "duotono", "Lab" };
    SourceInfo *src = &out->source;
    source_set(src, info.psb ? "PSB" : "PSD", "propio");
    src->bits = info.depth;
    src->channels = info.channels;
    src->is_float = is_float;
    src->has_alpha = alpha || info.transparent_index >= 0;
    src->alpha_origin = !src->has_alpha ? ALPHA_ORIGIN_NONE : unmatte ? ALPHA_ORIGIN_MATTED : ALPHA_ORIGIN_CHANNEL;
    source_detail(src, "compuesto %s · %u bits · %u canales%s%s", MODES[info.mode < 10 ? info.mode : 5], info.depth, info.channels,
                  alpha ? (unmatte ? " · alfa (mate blanco removido)" : " · alfa (mate conservado)") : "",
                  !alpha && info.channels > color_ch ? " · canales extra que no son transparencia" : "");
    if (info.mode == MODE_CMYK && !ctx->opt->to_srgb)
        diag_warn(diag, "PSD en CMYK: conversion simple a RGB (--icc srgb usa el perfil si lo hay)");
    return ST_OK;
}

static u8 psd_probe(const u8 *d, size_t n, const char *ext) {
    UNUSED(ext);
    return n >= 26 && memcmp(d, "8BPS", 4) == 0 && (load_be16(d + 4) == 1 || load_be16(d + 4) == 2) ? 100 : 0;
}

const Codec CODEC_PSD = {
    .name = "PSD",
    .extensions = "psd psb",
    .description = "Photoshop PSD/PSB (compuesto): 1/8/16/32 bits, RGB/CMYK/Lab/gris/paleta, alfa con mate blanco removido",
    .probe = psd_probe,
    .decode = psd_decode,
};
