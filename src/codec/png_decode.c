// codec/png_decode.c - PNG y APNG: todos los tipos de color y profundidades, Adam7, tRNS,
// animacion con composicion completa, rescate de archivos cortados y metadatos.
//
// Recorrido: (1) parsear chunks verificando CRC; (2) inflar cada flujo zlib con libdeflate;
// (3) deshacer filtros fila por fila (SSE); (4) llevar cada fila a RGBA8/RGBA16.
// En APNG cada cuadro es un flujo independiente: se decodifican todos EN PARALELO y despues se
// componen en orden sobre el lienzo (la composicion es secuencial por definicion).
#include <stdio.h>
#include "codec.h"
#include "reader.h"
#include "zlib.h"
#include "exif.h"
#include "compose.h"
#include "png_unfilter.h"
#include "../core/pool.h"
#include "../core/text.h"

#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

enum {
    CK_IHDR = FOURCC('I', 'H', 'D', 'R'), CK_PLTE = FOURCC('P', 'L', 'T', 'E'),
    CK_IDAT = FOURCC('I', 'D', 'A', 'T'), CK_IEND = FOURCC('I', 'E', 'N', 'D'),
    CK_tRNS = FOURCC('t', 'R', 'N', 'S'), CK_gAMA = FOURCC('g', 'A', 'M', 'A'),
    CK_cHRM = FOURCC('c', 'H', 'R', 'M'), CK_sRGB = FOURCC('s', 'R', 'G', 'B'),
    CK_iCCP = FOURCC('i', 'C', 'C', 'P'), CK_sBIT = FOURCC('s', 'B', 'I', 'T'),
    CK_pHYs = FOURCC('p', 'H', 'Y', 's'), CK_bKGD = FOURCC('b', 'K', 'G', 'D'),
    CK_tEXt = FOURCC('t', 'E', 'X', 't'), CK_zTXt = FOURCC('z', 'T', 'X', 't'),
    CK_iTXt = FOURCC('i', 'T', 'X', 't'), CK_eXIf = FOURCC('e', 'X', 'I', 'f'),
    CK_acTL = FOURCC('a', 'c', 'T', 'L'), CK_fcTL = FOURCC('f', 'c', 'T', 'L'),
    CK_fdAT = FOURCC('f', 'd', 'A', 'T'), CK_cICP = FOURCC('c', 'I', 'C', 'P'),
};

enum { PNG_GRAY = 0, PNG_RGB = 2, PNG_PALETTE = 3, PNG_GRAY_ALPHA = 4, PNG_RGBA = 6 };
enum { APNG_DISPOSE_NONE = 0, APNG_DISPOSE_BACKGROUND = 1, APNG_DISPOSE_PREVIOUS = 2 };
enum { APNG_BLEND_SOURCE = 0, APNG_BLEND_OVER = 1 };
enum { MAX_TEXT_BYTES = 16 << 20, MAX_ICC_BYTES = 64 << 20, PNG_MAX_DIMENSION = 0x7FFFFFFF };

static const u8 PNG_SIGNATURE[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };

// Pasadas de Adam7: origen (x, y) y paso (dx, dy).
static const u8 ADAM7[7][4] = {
    { 0, 0, 8, 8 }, { 4, 0, 8, 8 }, { 0, 4, 4, 8 }, { 2, 0, 4, 4 },
    { 0, 2, 2, 4 }, { 1, 0, 2, 2 }, { 0, 1, 1, 2 },
};

typedef struct PngInfo {
    u32  width, height;
    u8   bit_depth, color_type, interlace, channels;
    u32  filter_bpp;              // bytes por pixel completo para los filtros (minimo 1)
    u8   palette[256][4];         // RGBA (alfa de tRNS)
    u32  palette_size;
    bool has_trns;
    u16  trns_gray, trns_rgb[3];  // valores crudos de la muestra
    u32  trns_count;
} PngInfo;

typedef struct ApngFrame {
    u32    sequence, width, height, x, y;
    u16    delay_num, delay_den;
    u8     dispose, blend;
    Vec    spans;                 // ZSpan (payloads de fdAT)
    Image  image;                 // el rectangulo decodificado
    Status status;
    bool   truncated;
    u32    palette_oob;
    char   error[DIAG_MESSAGE_CAP];
} ApngFrame;

typedef struct PngParse {
    PngInfo info;
    Vec     idat;                 // ZSpan
    bool    has_actl;
    u32     actl_frames, actl_plays;
    Vec     frames;               // ApngFrame
    bool    idat_is_frame;        // hubo fcTL antes del primer IDAT: la imagen por defecto es el cuadro 0
    bool    seen_idat, idat_closed;
    u32     next_sequence;
    bool    apng_broken;
    char    apng_problem[160];
} PngParse;

// =============================================================================================
// Conversion de filas a RGBA
// =============================================================================================

INLINE u32 sample_at(const u8 *row, u32 x, u32 depth) {
    u32 bit = x * depth;
    return (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
}

static void row_gray_low(const PngInfo *in, const u8 *src, u32 w, u8 *dst) {
    u32 d = in->bit_depth, scale = 255 / ((1u << d) - 1);
    for (u32 x = 0; x < w; x++, dst += 4) {
        u32 v = sample_at(src, x, d);
        dst[0] = dst[1] = dst[2] = (u8)(v * scale);
        dst[3] = in->has_trns && v == in->trns_gray ? 0 : 255;
    }
}

static void row_gray8(const PngInfo *in, const u8 *src, u32 w, u8 *dst) {
    if (!in->has_trns) {
        for (u32 x = 0; x < w; x++, dst += 4) { dst[0] = dst[1] = dst[2] = src[x]; dst[3] = 255; }
        return;
    }
    for (u32 x = 0; x < w; x++, dst += 4) {
        dst[0] = dst[1] = dst[2] = src[x];
        dst[3] = src[x] == in->trns_gray ? 0 : 255;
    }
}

static void row_gray16(const PngInfo *in, const u8 *src, u32 w, u16 *dst) {
    for (u32 x = 0; x < w; x++, dst += 4) {
        u16 v = load_be16(src + 2 * (size_t)x);
        dst[0] = dst[1] = dst[2] = v;
        dst[3] = in->has_trns && v == in->trns_gray ? 0 : 65535;
    }
}

static void row_rgb8(const PngInfo *in, const u8 *src, u32 w, u8 *dst) {
    if (!in->has_trns) {
        for (u32 x = 0; x < w; x++, src += 3, dst += 4) { dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255; }
        return;
    }
    for (u32 x = 0; x < w; x++, src += 3, dst += 4) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2];
        dst[3] = src[0] == in->trns_rgb[0] && src[1] == in->trns_rgb[1] && src[2] == in->trns_rgb[2] ? 0 : 255;
    }
}

static void row_rgb16(const PngInfo *in, const u8 *src, u32 w, u16 *dst) {
    for (u32 x = 0; x < w; x++, src += 6, dst += 4) {
        dst[0] = load_be16(src); dst[1] = load_be16(src + 2); dst[2] = load_be16(src + 4);
        dst[3] = in->has_trns && dst[0] == in->trns_rgb[0] && dst[1] == in->trns_rgb[1] && dst[2] == in->trns_rgb[2] ? 0 : 65535;
    }
}

static void row_palette(const PngInfo *in, const u8 *src, u32 w, u8 *dst, u32 *oob) {
    u32 d = in->bit_depth;
    for (u32 x = 0; x < w; x++, dst += 4) {
        u32 idx = d == 8 ? src[x] : sample_at(src, x, d);
        if (LIKELY(idx < in->palette_size)) memcpy(dst, in->palette[idx], 4);
        else { dst[0] = dst[1] = dst[2] = 0; dst[3] = 255; (*oob)++; }
    }
}

static void row_gray_alpha8(const u8 *src, u32 w, u8 *dst) {
    for (u32 x = 0; x < w; x++, src += 2, dst += 4) { dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1]; }
}

static void row_gray_alpha16(const u8 *src, u32 w, u16 *dst) {
    for (u32 x = 0; x < w; x++, src += 4, dst += 4) { dst[0] = dst[1] = dst[2] = load_be16(src); dst[3] = load_be16(src + 2); }
}

static void row_rgba16(const u8 *src, u32 w, u16 *dst) {
    for (size_t i = 0, n = (size_t)w * 4; i < n; i++) dst[i] = load_be16(src + 2 * i);
}

static void row_to_rgba(const PngInfo *in, const u8 *src, u32 w, u8 *dst, u32 *oob) {
    switch (in->color_type) {
    case PNG_GRAY:
        if (in->bit_depth == 16) row_gray16(in, src, w, (u16 *)dst);
        else if (in->bit_depth == 8) row_gray8(in, src, w, dst);
        else row_gray_low(in, src, w, dst);
        break;
    case PNG_RGB:
        if (in->bit_depth == 16) row_rgb16(in, src, w, (u16 *)dst); else row_rgb8(in, src, w, dst);
        break;
    case PNG_PALETTE:
        row_palette(in, src, w, dst, oob);
        break;
    case PNG_GRAY_ALPHA:
        if (in->bit_depth == 16) row_gray_alpha16(src, w, (u16 *)dst); else row_gray_alpha8(src, w, dst);
        break;
    default:   // PNG_RGBA
        if (in->bit_depth == 16) row_rgba16(src, w, (u16 *)dst); else memcpy(dst, src, (size_t)w * 4);
        break;
    }
}

// =============================================================================================
// Un flujo zlib -> una imagen RGBA de w x h (la imagen principal o un cuadro de APNG)
// =============================================================================================

static bool row_bytes_for(const PngInfo *in, u32 w, size_t *out) {
    u64 bits = (u64)w * in->channels * in->bit_depth;
    u64 bytes = (bits + 7) / 8;
    if (bytes > (u64)SIZE_MAX / 2) return false;
    *out = (size_t)bytes;
    return true;
}

typedef struct StreamResult {
    bool truncated;
    u32  rows_ok, rows_total;
    u32  palette_oob;
} StreamResult;

// Llevar filas ya desfiltradas a RGBA: no dependen entre si, asi que va en paralelo. Desfiltrar
// no: cada fila necesita la anterior reconstruida (medido en una foto 4K RGBA: inflar ~75 ms,
// desfiltrar ~45 ms en serie, convertir ~25 ms que en paralelo bajan a unos pocos).
enum { CONVERT_GRAIN = 32 };                 // filas por tramo

typedef struct ConvertJob {
    const PngInfo *in;
    const u8      *rows;                     // la primera fila, precedida por su byte de filtro
    size_t         row_stride;               // bytes de fila + 1 (el byte de filtro)
    u32            width;
    Image         *out;
    u32            palette_oob;              // suma atomica: una por tramo, no por pixel
} ConvertJob;

static void convert_rows(void *arg, size_t y0, size_t y1) {
    ConvertJob *j = arg;
    u32 oob = 0;
    for (size_t y = y0; y < y1; y++)
        row_to_rgba(j->in, j->rows + y * j->row_stride + 1, j->width, image_row(j->out, (u32)y), &oob);
    if (oob) __atomic_fetch_add(&j->palette_oob, oob, __ATOMIC_RELAXED);
}

static Status decode_stream(const PngInfo *in, const ZSpan *spans, size_t span_count, u32 w, u32 h,
                            u64 max_pixels, Image *out, StreamResult *res, Diag *diag) {
    memset(res, 0, sizeof *res);
    bool interlaced = in->interlace != 0;
    // Tamano exacto del flujo inflado: filas (1 byte de filtro + datos) de cada pasada.
    size_t raw_size = 0, max_row = 0;
    u32 pass_w[7], pass_h[7];
    for (int p = 0; p < (interlaced ? 7 : 1); p++) {
        pass_w[p] = interlaced ? (w > ADAM7[p][0] ? (w - ADAM7[p][0] + ADAM7[p][2] - 1) / ADAM7[p][2] : 0) : w;
        pass_h[p] = interlaced ? (h > ADAM7[p][1] ? (h - ADAM7[p][1] + ADAM7[p][3] - 1) / ADAM7[p][3] : 0) : h;
        if (!pass_w[p] || !pass_h[p]) continue;
        size_t rb, pass_bytes;
        if (!row_bytes_for(in, pass_w[p], &rb) || !mul_ok(rb + 1, pass_h[p], &pass_bytes) ||
            !add_ok(raw_size, pass_bytes, &raw_size))
            return diag_fail(diag, ST_TOO_LARGE, "PNG de %u x %u no entra en memoria", w, h);
        max_row = MAX(max_row, rb);
        res->rows_total += pass_h[p];
    }

    Status s = image_alloc(out, w, h, in->bit_depth == 16 ? 16 : 8, interlaced, max_pixels, diag);
    if (s != ST_OK) return s;
    u8 *raw = mem_alloc(raw_size);
    u8 *zero_row = mem_calloc(max_row + 1);
    u8 *scatter = interlaced ? mem_alloc((size_t)w * 8) : NULL;
    if (!raw || !zero_row || (interlaced && !scatter)) {
        mem_free(raw); mem_free(zero_row); mem_free(scatter); image_free(out);
        return diag_fail(diag, ST_NO_MEMORY, "sin memoria para descomprimir el PNG");
    }

    size_t produced = 0;
    s = zlib_inflate_exact(spans, span_count, raw, raw_size, &produced, diag);
    if (s == ST_TRUNCATED) {
        // Lo que falta queda en cero: filtro "None" y pixeles (0,0,0,0) -> transparentes.
        memset(raw + produced, 0, raw_size - produced);
        res->truncated = true;
        s = ST_OK;
    }
    if (s != ST_OK) { mem_free(raw); mem_free(zero_row); mem_free(scatter); image_free(out); return s; }

    const u8 *raw_end_valid = raw + produced;
    u8 *cur = raw;
    u32 bpp = image_bytes_per_pixel(out);
    bool broken = false;
    u32 bad_row = h;
    for (int p = 0; p < (interlaced ? 7 : 1) && !broken; p++) {
        if (!pass_w[p] || !pass_h[p]) continue;
        size_t rb;
        row_bytes_for(in, pass_w[p], &rb);
        const u8 *prev = zero_row + 1;
        u8 *pass_rows = cur;
        u32 unfiltered = 0;
        for (u32 j = 0; j < pass_h[p]; j++, cur += rb + 1) {
            u8 filter = cur[0];
            u8 *row = cur + 1;
            if (!png_unfilter_row(row, prev, rb, in->filter_bpp, filter)) {
                // Filtro invalido: datos basura desde aca. Se corta y el resto queda transparente.
                broken = true;
                res->truncated = true;
                bad_row = j;
                break;
            }
            if (cur + 1 + rb <= raw_end_valid) res->rows_ok++;
            if (interlaced) {
                row_to_rgba(in, row, pass_w[p], scatter, &res->palette_oob);
                u8 *dst_row = image_row(out, ADAM7[p][1] + j * ADAM7[p][3]);
                for (u32 i = 0; i < pass_w[p]; i++)
                    memcpy(dst_row + (size_t)(ADAM7[p][0] + i * ADAM7[p][2]) * bpp, scatter + (size_t)i * bpp, bpp);
            }
            prev = row;
            unfiltered++;
        }
        if (!interlaced) {
            ConvertJob job = { in, pass_rows, rb + 1, w, out, 0 };
            parallel_range(unfiltered, CONVERT_GRAIN, convert_rows, &job);
            res->palette_oob += job.palette_oob;
        }
    }
    if (broken && !interlaced) {
        for (u32 y = bad_row; y < h; y++) memset(image_row(out, y), 0, out->stride);
        res->rows_ok = MIN(res->rows_ok, bad_row);
    }
    mem_free(raw);
    mem_free(zero_row);
    mem_free(scatter);
    return ST_OK;
}

// =============================================================================================
// Chunks
// =============================================================================================

static bool depth_valid(u8 color_type, u8 depth) {
    switch (color_type) {
    case PNG_GRAY:       return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case PNG_PALETTE:    return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case PNG_RGB: case PNG_GRAY_ALPHA: case PNG_RGBA: return depth == 8 || depth == 16;
    default:             return false;
    }
}

static Status parse_ihdr(PngInfo *in, const u8 *d, u32 len, Diag *diag) {
    if (len != 13) return diag_fail(diag, ST_CORRUPT, "IHDR de %u bytes (deben ser 13)", len);
    in->width = load_be32(d);
    in->height = load_be32(d + 4);
    in->bit_depth = d[8];
    in->color_type = d[9];
    in->interlace = d[12];
    if (!in->width || !in->height || in->width > PNG_MAX_DIMENSION || in->height > PNG_MAX_DIMENSION)
        return diag_fail(diag, ST_CORRUPT, "dimensiones invalidas: %u x %u", in->width, in->height);
    if (!depth_valid(in->color_type, in->bit_depth))
        return diag_fail(diag, ST_CORRUPT, "combinacion invalida: tipo de color %u con %u bits", in->color_type, in->bit_depth);
    if (d[10] != 0 || d[11] != 0)
        return diag_fail(diag, ST_UNSUPPORTED, "metodo de compresion/filtro desconocido (%u/%u)", d[10], d[11]);
    if (in->interlace > 1) return diag_fail(diag, ST_CORRUPT, "entrelazado desconocido (%u)", in->interlace);
    static const u8 CHANNELS[7] = { 1, 0, 3, 1, 2, 0, 4 };
    in->channels = CHANNELS[in->color_type];
    in->filter_bpp = MAX(1u, (u32)in->channels * in->bit_depth / 8);
    return ST_OK;
}

// Texto Latin-1 (tEXt/zTXt) a UTF-8.
static char *latin1_to_utf8(const u8 *s, size_t n, size_t *out_len) {
    char *out = mem_alloc(n * 2 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        u8 c = s[i];
        if (c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xC0 | c >> 6); out[o++] = (char)(0x80 | (c & 0x3F)); }
    }
    out[o] = 0;
    *out_len = o;
    return out;
}

// keyword\0 al principio del chunk: devuelve su largo o 0 si no es valida (1..79 bytes).
static size_t keyword_length(const u8 *d, u32 len) {
    const u8 *nul = memchr(d, 0, MIN(len, 80u));
    return nul && nul != d ? (size_t)(nul - d) : 0;
}

static void parse_text(Decoded *out, u32 type, const u8 *d, u32 len, Diag *diag) {
    size_t klen = keyword_length(d, len);
    if (!klen) { diag_warn(diag, "chunk de texto con clave invalida: ignorado"); return; }
    char keyword[80];
    memcpy(keyword, d, klen);
    keyword[klen] = 0;
    const u8 *p = d + klen + 1, *end = d + len;
    u8 *inflated = NULL;
    size_t inflated_len = 0;
    char *text = NULL;
    size_t text_len = 0;
    if (type == CK_tEXt) {
        text = latin1_to_utf8(p, (size_t)(end - p), &text_len);
    } else if (type == CK_zTXt) {
        if (end - p < 1 || p[0] != 0) return;
        if (zlib_inflate_alloc(p + 1, (size_t)(end - p - 1), MAX_TEXT_BYTES, &inflated, &inflated_len, &(Diag){ 0 }) != ST_OK) {
            diag_warn(diag, "zTXt \"%s\" ilegible: ignorado", keyword);
            return;
        }
        text = latin1_to_utf8(inflated, inflated_len, &text_len);
    } else {   // iTXt: flag, metodo, idioma\0, clave traducida\0, texto UTF-8
        if (end - p < 2) return;
        u8 compressed = p[0], method = p[1];
        p += 2;
        const u8 *lang_end = memchr(p, 0, (size_t)(end - p));
        if (!lang_end) return;
        const u8 *trans_end = memchr(lang_end + 1, 0, (size_t)(end - lang_end - 1));
        if (!trans_end) return;
        p = trans_end + 1;
        if (compressed) {
            if (method != 0 || zlib_inflate_alloc(p, (size_t)(end - p), MAX_TEXT_BYTES, &inflated, &inflated_len, &(Diag){ 0 }) != ST_OK) {
                diag_warn(diag, "iTXt \"%s\" ilegible: ignorado", keyword);
                return;
            }
            p = inflated;
            end = inflated + inflated_len;
        }
        text_len = (size_t)(end - p);
        text = mem_alloc(text_len + 1);
        if (text) { memcpy(text, p, text_len); text[text_len] = 0; }
    }
    if (text) {
        if (strcmp(keyword, "XML:com.adobe.xmp") == 0) blob_set(&out->meta.xmp, text, text_len);
        else metadata_add_text(&out->meta, keyword, text, text_len);
    }
    mem_free(text);
    mem_free(inflated);
}

static void parse_iccp(Decoded *out, const u8 *d, u32 len, Diag *diag) {
    size_t klen = keyword_length(d, len);
    if (!klen || (size_t)len < klen + 2 || d[klen + 1] != 0) { diag_warn(diag, "iCCP invalido: ignorado"); return; }
    u8 *profile;
    size_t size;
    if (zlib_inflate_alloc(d + klen + 2, len - klen - 2, MAX_ICC_BYTES, &profile, &size, &(Diag){ 0 }) != ST_OK) {
        diag_warn(diag, "perfil ICC comprimido ilegible: ignorado");
        return;
    }
    blob_free(&out->meta.icc);
    out->meta.icc.data = profile;
    out->meta.icc.size = size;
    size_t n = MIN(klen, sizeof out->meta.icc_name - 1);
    memcpy(out->meta.icc_name, d, n);
    out->meta.icc_name[n] = 0;
}

static void parse_bkgd(const PngInfo *in, Metadata *m, const u8 *d, u32 len) {
    if (in->color_type == PNG_PALETTE) {
        if (len < 1 || d[0] >= in->palette_size) return;
        for (int c = 0; c < 3; c++) m->background[c] = u8_to_u16(in->palette[d[0]][c]);
    } else if (in->color_type == PNG_GRAY || in->color_type == PNG_GRAY_ALPHA) {
        if (len < 2) return;
        u32 max = (1u << in->bit_depth) - 1, v = MIN((u32)load_be16(d), max);
        m->background[0] = m->background[1] = m->background[2] = (u16)(v * 65535u / max);
    } else {
        if (len < 6) return;
        for (int c = 0; c < 3; c++) {
            u16 v = load_be16(d + 2 * c);
            m->background[c] = in->bit_depth == 16 ? v : u8_to_u16((u8)MIN(v, 255));
        }
    }
    m->has_background = true;
}

static void apng_fail(PngParse *p, const char *why) {
    if (p->apng_broken) return;
    p->apng_broken = true;
    snprintf(p->apng_problem, sizeof p->apng_problem, "%s", why);
}

static void parse_fctl(PngParse *p, const u8 *d, u32 len) {
    if (len != 26) { apng_fail(p, "fcTL de largo invalido"); return; }
    ApngFrame f = { 0 };
    f.sequence = load_be32(d);
    f.width = load_be32(d + 4);
    f.height = load_be32(d + 8);
    f.x = load_be32(d + 12);
    f.y = load_be32(d + 16);
    f.delay_num = load_be16(d + 20);
    f.delay_den = load_be16(d + 22);
    f.dispose = d[24];
    f.blend = d[25];
    if (!f.width || !f.height || (u64)f.x + f.width > p->info.width || (u64)f.y + f.height > p->info.height) {
        apng_fail(p, "un cuadro se sale del lienzo");
        return;
    }
    if (f.dispose > APNG_DISPOSE_PREVIOUS || f.blend > APNG_BLEND_OVER) { apng_fail(p, "dispose/blend invalidos"); return; }
    if (f.sequence != p->next_sequence) apng_fail(p, "numeros de secuencia desordenados");
    p->next_sequence = f.sequence + 1;
    if (!p->seen_idat) {
        if (p->frames.count > 0) { apng_fail(p, "dos fcTL antes de los datos"); return; }
        if (f.x || f.y || f.width != p->info.width || f.height != p->info.height) {
            apng_fail(p, "el cuadro 0 no cubre el lienzo");
            return;
        }
        p->idat_is_frame = true;
    }
    vec_init(&f.spans, sizeof(ZSpan));
    ApngFrame *slot = vec_push(&p->frames);
    if (slot) *slot = f;
}

static void parse_fdat(PngParse *p, const u8 *d, u32 len) {
    if (len < 4) { apng_fail(p, "fdAT vacio"); return; }
    if (load_be32(d) != p->next_sequence) apng_fail(p, "numeros de secuencia desordenados");
    p->next_sequence = load_be32(d) + 1;
    bool owner_is_idat = p->frames.count == 1 && p->idat_is_frame;
    if (p->frames.count == 0 || owner_is_idat) { apng_fail(p, "fdAT sin su fcTL"); return; }
    ApngFrame *f = &VEC_AT(&p->frames, ApngFrame, p->frames.count - 1);
    ZSpan *span = vec_push(&f->spans);
    if (span) { span->data = d + 4; span->size = len - 4; }
}

static Status parse_chunks(const DecodeContext *ctx, PngParse *p, Decoded *out) {
    Diag *diag = ctx->diag;
    Reader r = reader_make(ctx->data, ctx->size);
    if (!rd_match(&r, PNG_SIGNATURE, 8)) return diag_fail(diag, ST_UNKNOWN_FORMAT, "no es un PNG");
    PngInfo *in = &p->info;
    u32 index = 0;
    for (bool ended = false; !ended; index++) {
        if (rd_left(&r) < 12) {
            if (!p->seen_idat) return diag_fail(diag, ST_TRUNCATED, "el archivo termina antes de los datos de imagen");
            diag_warn(diag, "PNG sin IEND: el archivo parece cortado");
            break;
        }
        u32 len = rd_be32(&r);
        const u8 *type_ptr = r.p;
        u32 type = rd_be32(&r);
        if (len > 0x7FFFFFFFu) return diag_fail(diag, ST_CORRUPT, "chunk %u con largo imposible", index);
        if (rd_left(&r) < (size_t)len + 4) {
            // Chunk cortado: si son datos de imagen se usa lo que haya (el rescate hace el resto).
            if (type == CK_IDAT && index > 0) {
                ZSpan *span = vec_push(&p->idat);
                if (span) { span->data = r.p; span->size = rd_left(&r); }
                p->seen_idat = true;
            }
            if (!p->seen_idat) return diag_fail(diag, ST_TRUNCATED, "el archivo termina a mitad del chunk %.4s", (const char *)type_ptr);
            diag_warn(diag, "PNG cortado a mitad de un chunk %.4s", (const char *)type_ptr);
            break;
        }
        const u8 *data = rd_bytes(&r, len);
        u32 crc = rd_be32(&r);
        bool critical = !(type_ptr[0] & 0x20);
        if (!ctx->opt->ignore_crc && crc32_update(0, type_ptr, (size_t)len + 4) != crc) {
            if (critical)
                return diag_fail(diag, ST_CORRUPT, "CRC incorrecto en %.4s (--ignore-crc lo lee igual)", (const char *)type_ptr);
            diag_warn(diag, "CRC incorrecto en %.4s: se ignora ese chunk", (const char *)type_ptr);
            continue;
        }
        if (index == 0 && type != CK_IHDR) return diag_fail(diag, ST_CORRUPT, "el primer chunk no es IHDR");
        if (type != CK_IDAT && p->seen_idat) p->idat_closed = true;

        switch (type) {
        case CK_IHDR: {
            if (index != 0) return diag_fail(diag, ST_CORRUPT, "IHDR repetido");
            Status s = parse_ihdr(in, data, len, diag);
            if (s != ST_OK) return s;
            break;
        }
        case CK_PLTE:
            if (len % 3 || len == 0 || len > 768) {
                if (in->color_type == PNG_PALETTE) return diag_fail(diag, ST_CORRUPT, "PLTE de %u bytes", len);
                break;
            }
            in->palette_size = len / 3;
            for (u32 i = 0; i < in->palette_size; i++) {
                memcpy(in->palette[i], data + 3 * i, 3);
                in->palette[i][3] = 255;
            }
            break;
        case CK_tRNS:
            if (in->color_type == PNG_PALETTE) {
                u32 n = MIN(len, in->palette_size);
                if (len > in->palette_size) diag_warn(diag, "tRNS con mas entradas que la paleta");
                for (u32 i = 0; i < n; i++) in->palette[i][3] = data[i];
                in->trns_count = n;
                in->has_trns = n > 0;
            } else if (in->color_type == PNG_GRAY && len >= 2) {
                in->trns_gray = load_be16(data);
                in->has_trns = true;
            } else if (in->color_type == PNG_RGB && len >= 6) {
                for (int c = 0; c < 3; c++) in->trns_rgb[c] = load_be16(data + 2 * c);
                in->has_trns = true;
            } else {
                diag_warn(diag, "tRNS no aplica a este tipo de color: ignorado");
            }
            break;
        case CK_IDAT: {
            if (p->idat_closed) diag_warn(diag, "IDAT no consecutivos (se leen igual)");
            ZSpan *span = vec_push(&p->idat);
            if (!span) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
            span->data = data;
            span->size = len;
            p->seen_idat = true;
            break;
        }
        case CK_IEND:  ended = true; break;
        case CK_gAMA:
            if (len == 4 && load_be32(data)) { out->meta.has_gamma = true; out->meta.gamma = load_be32(data) / 100000.0; }
            break;
        case CK_cHRM:
            if (len == 32) {
                out->meta.has_chrm = true;
                for (int i = 0; i < 8; i++) out->meta.chrm[i] = load_be32(data + 4 * i) / 100000.0;
            }
            break;
        case CK_sRGB:
            if (len == 1) { out->meta.has_srgb = true; out->meta.srgb_intent = data[0]; }
            break;
        case CK_iCCP: parse_iccp(out, data, len, diag); break;
        case CK_cICP:
            // Solo RGB (matriz 0): es lo unico que PNG admite; lo demas se ignora con aviso.
            if (len == 4 && data[2] == 0) { out->meta.has_cicp = true; memcpy(out->meta.cicp, data, 4); }
            else diag_warn(diag, "cICP invalido (matriz %u): ignorado", len == 4 ? data[2] : 0);
            break;
        case CK_sBIT:
            if (len >= 1 && len <= 4) { out->meta.has_sbit = true; memcpy(out->meta.sbit, data, len); }
            break;
        case CK_pHYs:
            if (len == 9) {
                out->meta.has_phys = true;
                out->meta.phys_x = load_be32(data);
                out->meta.phys_y = load_be32(data + 4);
                out->meta.phys_unit = data[8];
            }
            break;
        case CK_bKGD: parse_bkgd(in, &out->meta, data, len); break;
        case CK_tEXt: case CK_zTXt: case CK_iTXt: parse_text(out, type, data, len, diag); break;
        case CK_eXIf:
            blob_set(&out->meta.exif, data, len);
            out->meta.orientation = exif_orientation(data, len);
            break;
        case CK_acTL:
            if (len == 8 && !p->seen_idat) {
                p->has_actl = true;
                p->actl_frames = load_be32(data);
                p->actl_plays = load_be32(data + 4);
            }
            break;
        case CK_fcTL: if (p->has_actl) parse_fctl(p, data, len); break;
        case CK_fdAT: if (p->has_actl) parse_fdat(p, data, len); break;
        default:
            if (critical) return diag_fail(diag, ST_UNSUPPORTED, "chunk critico desconocido %.4s", (const char *)type_ptr);
            break;
        }
    }
    if (!p->idat.count) return diag_fail(diag, ST_CORRUPT, "el PNG no tiene datos de imagen (IDAT)");
    if (in->color_type == PNG_PALETTE && !in->palette_size) return diag_fail(diag, ST_CORRUPT, "PNG con paleta pero sin PLTE");
    return ST_OK;
}

// =============================================================================================
// Decodificacion
// =============================================================================================

static void describe(const PngParse *p, Decoded *out, bool apng) {
    const PngInfo *in = &p->info;
    SourceInfo *s = &out->source;
    source_set(s, apng ? "APNG" : "PNG", "propio");
    s->bits = in->bit_depth;
    s->channels = in->channels;
    s->has_alpha = in->color_type == PNG_GRAY_ALPHA || in->color_type == PNG_RGBA || in->has_trns;
    s->alpha_origin = in->color_type == PNG_GRAY_ALPHA || in->color_type == PNG_RGBA ? ALPHA_ORIGIN_CHANNEL
                    : !in->has_trns ? ALPHA_ORIGIN_NONE
                    : in->color_type == PNG_PALETTE ? ALPHA_ORIGIN_PALETTE : ALPHA_ORIGIN_KEY;
    static const char *const KIND[7] = { "gris", "?", "RGB", "paleta", "gris+alfa", "?", "RGBA" };
    char extra[64] = "";
    if (in->color_type == PNG_PALETTE) snprintf(extra, sizeof extra, " (%u colores)", in->palette_size);
    source_detail(s, "%s%s · %u bits%s%s", KIND[in->color_type], extra, in->bit_depth,
                  in->has_trns ? " · tRNS" : "", in->interlace ? " · Adam7" : "");
}

typedef struct FramesJob {
    const PngParse *parse;
    ApngFrame      *frames;
    u64             max_pixels;
} FramesJob;

static void decode_frame_task(void *arg, size_t i) {
    FramesJob *job = arg;
    ApngFrame *f = &job->frames[i];
    const PngParse *p = job->parse;
    const Vec *spans = (i == 0 && p->idat_is_frame) ? &p->idat : &f->spans;
    if (!spans->count) {
        f->status = ST_CORRUPT;
        snprintf(f->error, sizeof f->error, "el cuadro %zu no tiene datos", i);
        return;
    }
    Diag local = { 0 };
    StreamResult res;
    f->status = decode_stream(&p->info, spans->items, spans->count, f->width, f->height, job->max_pixels, &f->image, &res, &local);
    f->truncated = res.truncated;
    f->palette_oob = res.palette_oob;
    if (f->status != ST_OK) memcpy(f->error, local.message, sizeof f->error);
}

static Status decode_static(const DecodeContext *ctx, const PngParse *p, Decoded *out) {
    Frame *frame = decoded_add_frame(out);
    if (!frame) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    StreamResult res;
    Status s = decode_stream(&p->info, p->idat.items, p->idat.count, p->info.width, p->info.height,
                             ctx->opt->max_pixels, &frame->image, &res, ctx->diag);
    if (s != ST_OK) return s;
    if (res.truncated)
        diag_warn(ctx->diag, "PNG cortado o dañado: se recuperaron %u de %u filas (el resto queda transparente)",
                  res.rows_ok, res.rows_total);
    if (res.palette_oob) diag_warn(ctx->diag, "%u pixeles apuntan fuera de la paleta (se pintan negros)", res.palette_oob);
    return ST_OK;
}

static Status decode_animation(const DecodeContext *ctx, PngParse *p, Decoded *out) {
    const DecodeOptions *opt = ctx->opt;
    u32 count = (u32)p->frames.count;
    ApngFrame *frames = p->frames.items;
    // Solo se decodifica hasta el ultimo cuadro que se va a entregar (la composicion necesita
    // todos los anteriores).
    u32 last = count - 1;
    if (opt->frame_mode == FRAMES_ONE) {
        if (opt->frame_index >= count)
            return diag_fail(ctx->diag, ST_INVALID, "--frame %u: la animacion tiene %u cuadros (0..%u)", opt->frame_index, count, count - 1);
        last = opt->frame_index;
    }
    FramesJob job = { p, frames, opt->max_pixels };
    parallel_for((size_t)last + 1, decode_frame_task, &job);
    for (u32 i = 0; i <= last; i++) {
        if (frames[i].status != ST_OK) return diag_fail(ctx->diag, frames[i].status, "APNG cuadro %u: %s", i, frames[i].error);
        if (frames[i].truncated) diag_warn(ctx->diag, "APNG: el cuadro %u esta cortado o dañado", i);
    }

    Image canvas;
    Status s = image_alloc(&canvas, p->info.width, p->info.height, frames[0].image.depth, true, opt->max_pixels, ctx->diag);
    if (s != ST_OK) return s;
    for (u32 i = 0; i <= last && s == ST_OK; i++) {
        ApngFrame *f = &frames[i];
        u8 dispose = (i == 0 && f->dispose == APNG_DISPOSE_PREVIOUS) ? APNG_DISPOSE_BACKGROUND : f->dispose;
        Image saved = { 0 };
        if (dispose == APNG_DISPOSE_PREVIOUS) s = compose_save(&canvas, f->x, f->y, f->width, f->height, &saved, ctx->diag);
        if (s != ST_OK) break;
        compose_draw(&canvas, &f->image, f->x, f->y, f->blend == APNG_BLEND_OVER);
        if (opt->frame_mode != FRAMES_ONE || i == last) {
            Frame *out_frame = decoded_add_frame(out);
            if (!out_frame) { s = diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria"); break; }
            s = image_clone(&canvas, &out_frame->image, ctx->diag);
            out_frame->index = i;
            out_frame->delay_num = f->delay_num;
            out_frame->delay_den = f->delay_den ? f->delay_den : 100;
        }
        if (dispose == APNG_DISPOSE_BACKGROUND) compose_clear(&canvas, f->x, f->y, f->width, f->height);
        else if (dispose == APNG_DISPOSE_PREVIOUS) compose_restore(&canvas, &saved, f->x, f->y);
        image_free(&saved);
    }
    image_free(&canvas);
    out->animated = opt->frame_mode != FRAMES_ONE;
    out->loop_count = p->actl_plays;
    return s;
}

static Status png_decode(const DecodeContext *ctx, Decoded *out) {
    PngParse p;
    memset(&p, 0, sizeof p);
    vec_init(&p.idat, sizeof(ZSpan));
    vec_init(&p.frames, sizeof(ApngFrame));
    Status s = parse_chunks(ctx, &p, out);
    bool apng = s == ST_OK && p.has_actl && p.frames.count > 0;
    if (apng && p.apng_broken) {
        diag_warn(ctx->diag, "APNG invalido (%s): se usa la imagen estatica", p.apng_problem);
        apng = false;
    }
    if (apng && p.actl_frames != p.frames.count)
        diag_warn(ctx->diag, "acTL anuncia %u cuadros pero hay %zu", p.actl_frames, p.frames.count);
    if (s == ST_OK) {
        describe(&p, out, apng);
        out->source.total_frames = apng ? (u32)p.frames.count : 1;
        // Salida estatica sin cuadro pedido: la imagen por defecto (lo que muestra un visor sin APNG).
        bool static_default = !apng || (ctx->opt->single_frame && ctx->opt->frame_mode != FRAMES_ONE);
        s = static_default ? decode_static(ctx, &p, out) : decode_animation(ctx, &p, out);
    }
    for (size_t i = 0; i < p.frames.count; i++) {
        ApngFrame *f = &VEC_AT(&p.frames, ApngFrame, i);
        vec_free(&f->spans);
        image_free(&f->image);
    }
    vec_free(&p.frames);
    vec_free(&p.idat);
    return s;
}

static u8 png_probe(const u8 *data, size_t size, const char *ext) {
    UNUSED(ext);
    return size >= 8 && memcmp(data, PNG_SIGNATURE, 8) == 0 ? 100 : 0;
}

const Codec CODEC_PNG = {
    .name = "PNG",
    .extensions = "png apng",
    .description = "Portable Network Graphics + APNG animado (todos los tipos, Adam7, tRNS, rescate de truncados)",
    .probe = png_probe,
    .decode = png_decode,
};
