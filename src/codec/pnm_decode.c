// codec/pnm_decode.c - familia Netpbm: PBM/PGM/PPM (P1..P6, ASCII y binario, hasta 16 bits),
// PAM (P7, con los TUPLTYPE de alfa) y PFM (PF/Pf, flotante HDR).
//
// Detalle que se come mas de un decodificador: en PBM (P1/P4) 1 es NEGRO, pero en PAM
// BLACKANDWHITE 1 es BLANCO. Y en PFM las filas van de abajo hacia arriba, con el signo de la
// "escala" indicando el orden de bytes.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "codec.h"
#include "float_image.h"

enum { PNM_MAX_VALUE = 65535, PNM_MAX_DEPTH = 4 };

typedef enum Tuple { T_BW, T_GRAY, T_RGB, T_BW_ALPHA, T_GRAY_ALPHA, T_RGB_ALPHA } Tuple;

typedef struct PnmHeader {
    char  kind;              // '1'..'7', 'F' (PFM color), 'f' (PFM gris)
    u32   width, height, maxval, depth;
    Tuple tuple;
    float scale;             // PFM
} PnmHeader;

typedef struct Scanner {
    const u8 *p, *end;
    bool      bad;
} Scanner;

static void skip_space(Scanner *s) {
    while (s->p < s->end) {
        if (*s->p == '#') { while (s->p < s->end && *s->p != '\n' && *s->p != '\r') s->p++; }
        else if (*s->p == ' ' || *s->p == '\t' || *s->p == '\n' || *s->p == '\r' || *s->p == '\v' || *s->p == '\f') s->p++;
        else break;
    }
}

static u32 scan_uint(Scanner *s) {
    skip_space(s);
    if (s->p >= s->end || *s->p < '0' || *s->p > '9') { s->bad = true; return 0; }
    u64 v = 0;
    while (s->p < s->end && *s->p >= '0' && *s->p <= '9') { v = v * 10 + (u64)(*s->p++ - '0'); if (v > 0xFFFFFFFFu) { s->bad = true; return 0; } }
    return (u32)v;
}

static bool scan_word(Scanner *s, char *out, size_t cap) {
    skip_space(s);
    size_t n = 0;
    while (s->p < s->end && *s->p > ' ' && n + 1 < cap) out[n++] = (char)*s->p++;
    out[n] = 0;
    while (s->p < s->end && *s->p > ' ') s->p++;
    return n > 0;
}

static Status parse_header(const DecodeContext *ctx, Scanner *s, PnmHeader *h) {
    Diag *diag = ctx->diag;
    h->kind = (char)s->p[1];
    s->p += 2;
    if (h->kind == '7') {
        char word[32];
        h->tuple = T_RGB;
        bool tuple_given = false;
        for (;;) {
            if (!scan_word(s, word, sizeof word)) return diag_fail(diag, ST_TRUNCATED, "encabezado PAM incompleto");
            if (strcmp(word, "ENDHDR") == 0) break;
            if (strcmp(word, "WIDTH") == 0) h->width = scan_uint(s);
            else if (strcmp(word, "HEIGHT") == 0) h->height = scan_uint(s);
            else if (strcmp(word, "DEPTH") == 0) h->depth = scan_uint(s);
            else if (strcmp(word, "MAXVAL") == 0) h->maxval = scan_uint(s);
            else if (strcmp(word, "TUPLTYPE") == 0) {
                scan_word(s, word, sizeof word);
                tuple_given = true;
                if (strcmp(word, "BLACKANDWHITE") == 0) h->tuple = T_BW;
                else if (strcmp(word, "GRAYSCALE") == 0) h->tuple = T_GRAY;
                else if (strcmp(word, "RGB") == 0) h->tuple = T_RGB;
                else if (strcmp(word, "BLACKANDWHITE_ALPHA") == 0) h->tuple = T_BW_ALPHA;
                else if (strcmp(word, "GRAYSCALE_ALPHA") == 0) h->tuple = T_GRAY_ALPHA;
                else if (strcmp(word, "RGB_ALPHA") == 0) h->tuple = T_RGB_ALPHA;
                else tuple_given = false;
            }
            if (s->bad) return diag_fail(diag, ST_CORRUPT, "encabezado PAM invalido");
        }
        if (s->p < s->end && (*s->p == '\n' || *s->p == '\r')) s->p++;
        if (!tuple_given) {       // sin TUPLTYPE: se deduce de la profundidad
            static const Tuple BY_DEPTH[5] = { T_GRAY, T_GRAY, T_GRAY_ALPHA, T_RGB, T_RGB_ALPHA };
            h->tuple = BY_DEPTH[MIN(h->depth, 4u)];
        }
        static const u32 DEPTH_OF[6] = { 1, 1, 3, 2, 2, 4 };
        if (h->depth < DEPTH_OF[h->tuple] || h->depth > PNM_MAX_DEPTH)
            return diag_fail(diag, ST_UNSUPPORTED, "PAM con DEPTH %u no soportado", h->depth);
        return ST_OK;
    }
    h->width = scan_uint(s);
    h->height = scan_uint(s);
    if (h->kind == 'F' || h->kind == 'f') {
        skip_space(s);
        char num[64];
        size_t n = 0;
        while (s->p < s->end && *s->p > ' ' && n + 1 < sizeof num) num[n++] = (char)*s->p++;
        num[n] = 0;
        h->scale = strtof(num, NULL);
        if (!n || h->scale == 0.0f || !isfinite(h->scale)) return diag_fail(diag, ST_CORRUPT, "escala PFM invalida");
        h->depth = h->kind == 'F' ? 3 : 1;
        h->tuple = h->kind == 'F' ? T_RGB : T_GRAY;
    } else if (h->kind == '1' || h->kind == '4') {
        h->maxval = 1;
        h->depth = 1;
        h->tuple = T_BW;
    } else {
        h->maxval = scan_uint(s);
        h->depth = (h->kind == '3' || h->kind == '6') ? 3 : 1;
        h->tuple = h->depth == 3 ? T_RGB : T_GRAY;
    }
    if (s->bad) return diag_fail(diag, ST_CORRUPT, "encabezado PNM invalido");
    // Exactamente un caracter de espacio separa el encabezado de los datos binarios.
    if (h->kind != '1' && h->kind != '2' && h->kind != '3' && s->p < s->end) s->p++;
    return ST_OK;
}

static Status decode_pfm(const DecodeContext *ctx, Scanner *s, const PnmHeader *h, Decoded *out) {
    size_t pixels = (size_t)h->width * h->height, bytes;
    if (!mul_ok(pixels, 16, &bytes)) return diag_fail(ctx->diag, ST_TOO_LARGE, "PFM demasiado grande");
    float *rgba = mem_alloc(bytes);
    if (!rgba) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    bool little = h->scale < 0.0f;
    size_t needed = pixels * h->depth * 4, avail = (size_t)(s->end - s->p);
    for (u32 row = 0; row < h->height; row++) {
        u32 y = h->height - 1 - row;                             // PFM va de abajo hacia arriba
        for (u32 x = 0; x < h->width; x++) {
            float *px = rgba + ((size_t)y * h->width + x) * 4;
            size_t at = (((size_t)row * h->width + x) * h->depth) * 4;
            for (u32 c = 0; c < 3; c++) {
                size_t off = at + (h->depth == 3 ? c : 0) * 4;
                u32 bits = off + 4 <= avail ? (little ? load_le32(s->p + off) : load_be32(s->p + off)) : 0;
                px[c] = f32_from_bits(bits);
            }
            px[3] = 1.0f;
        }
    }
    if (avail < needed) diag_warn(ctx->diag, "PFM cortado: lo que falta queda negro");
    Frame *f = decoded_add_frame(out);
    ToneOperator used = TONE_CLIP;
    Status st = f ? float_image_to_rgba16(rgba, h->width, h->height, ctx->opt, &f->image, &used, ctx->diag)
                  : diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    mem_free(rgba);
    if (st != ST_OK) return st;
    SourceInfo *src = &out->source;
    source_set(src, "PFM", "propio");
    src->bits = 32;
    src->channels = h->depth;
    src->is_float = true;
    source_detail(src, "%s · flotante 32 bits · tonemap %s", h->depth == 3 ? "RGB" : "gris", tone_name(used));
    return ST_OK;
}

static Status pnm_decode(const DecodeContext *ctx, Decoded *out) {
    Scanner s = { ctx->data, ctx->data + ctx->size, false };
    PnmHeader h = { 0 };
    Status st = parse_header(ctx, &s, &h);
    if (st != ST_OK) return st;
    if (!h.width || !h.height) return diag_fail(ctx->diag, ST_CORRUPT, "dimensiones invalidas: %u x %u", h.width, h.height);
    if (h.kind == 'F' || h.kind == 'f') return decode_pfm(ctx, &s, &h, out);
    if (!h.maxval || h.maxval > PNM_MAX_VALUE) return diag_fail(ctx->diag, ST_CORRUPT, "MAXVAL invalido (%u)", h.maxval);

    u32 depth_out = h.maxval > 255 ? 16 : 8;
    Frame *f = decoded_add_frame(out);
    if (!f) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    st = image_alloc(&f->image, h.width, h.height, depth_out, false, ctx->opt->max_pixels, ctx->diag);
    if (st != ST_OK) return st;
    // Tabla muestra -> 16 bits (maxval arbitrario, p.ej. 1023 de camaras o 7 de un dither).
    u16 *lut = mem_alloc(((size_t)h.maxval + 1) * sizeof(u16));
    if (!lut) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
    for (u32 v = 0; v <= h.maxval; v++) lut[v] = (u16)(((u64)v * 65535 + h.maxval / 2) / h.maxval);

    bool ascii = h.kind == '1' || h.kind == '2' || h.kind == '3';
    bool pbm = h.kind == '1' || h.kind == '4';
    u32 channels = h.depth, wide = h.maxval > 255 ? 2 : 1;
    size_t row_bytes = h.kind == '4' ? (h.width + 7) / 8 : (size_t)h.width * channels * wide;
    u64 missing = 0;
    for (u32 y = 0; y < h.height; y++) {
        const u8 *row = s.p + (size_t)y * row_bytes;
        for (u32 x = 0; x < h.width; x++) {
            u32 v[4] = { 0, 0, 0, h.maxval };
            if (h.kind == '1') {
                skip_space(&s);
                if (s.p < s.end && (*s.p == '0' || *s.p == '1')) v[0] = *s.p++ == '1'; else missing++;
            } else if (ascii) {
                for (u32 c = 0; c < channels; c++) { v[c] = scan_uint(&s); if (s.bad) { missing++; s.bad = false; s.p = s.end; } }
            } else if (h.kind == '4') {
                if (row + x / 8 < s.end) v[0] = (row[x / 8] >> (7 - (x & 7))) & 1; else missing++;
            } else {
                for (u32 c = 0; c < channels; c++) {
                    const u8 *q = row + ((size_t)x * channels + c) * wide;
                    if (q + wide <= s.end) v[c] = wide == 2 ? load_be16(q) : q[0]; else missing++;
                }
            }
            for (u32 c = 0; c < 4; c++) if (v[c] > h.maxval) v[c] = h.maxval;
            u16 r, g, b, a = 65535;
            if (pbm) { r = g = b = v[0] ? 0 : 65535; }                              // PBM: 1 = negro
            else switch (h.tuple) {
                case T_BW:         r = g = b = v[0] ? 65535 : 0; break;             // PAM: 1 = blanco
                case T_BW_ALPHA:   r = g = b = v[0] ? 65535 : 0; a = v[1] ? 65535 : 0; break;
                case T_GRAY:       r = g = b = lut[v[0]]; break;
                case T_GRAY_ALPHA: r = g = b = lut[v[0]]; a = lut[v[1]]; break;
                case T_RGB:        r = lut[v[0]]; g = lut[v[1]]; b = lut[v[2]]; break;
                default:           r = lut[v[0]]; g = lut[v[1]]; b = lut[v[2]]; a = lut[v[3]]; break;
            }
            if (depth_out == 8) {
                u8 *px = image_row(&f->image, y) + 4 * (size_t)x;
                px[0] = u16_to_u8(r); px[1] = u16_to_u8(g); px[2] = u16_to_u8(b); px[3] = u16_to_u8(a);
            } else {
                u16 *px = (u16 *)image_row(&f->image, y) + 4 * (size_t)x;
                px[0] = r; px[1] = g; px[2] = b; px[3] = a;
            }
        }
    }
    mem_free(lut);
    if (missing) diag_warn(ctx->diag, "archivo Netpbm cortado: faltan %llu muestras", (unsigned long long)missing);

    bool alpha = h.tuple == T_BW_ALPHA || h.tuple == T_GRAY_ALPHA || h.tuple == T_RGB_ALPHA;
    static const char *const NAMES[] = { "PBM", "PGM", "PPM", "PBM", "PGM", "PPM", "PAM" };
    int k = h.kind - '1';
    SourceInfo *src = &out->source;
    source_set(src, (k >= 0 && k < 7) ? NAMES[k] : "PNM", "propio");
    src->bits = bit_width(h.maxval);
    src->channels = channels;
    src->has_alpha = alpha;
    src->alpha_origin = alpha ? ALPHA_ORIGIN_CHANNEL : ALPHA_ORIGIN_NONE;
    static const char *const TUPLE[] = { "blanco y negro", "gris", "RGB", "b/n + alfa", "gris + alfa", "RGBA" };
    source_detail(src, "P%c %s · %s · maxval %u", h.kind, ascii ? "ASCII" : "binario", TUPLE[h.tuple], h.maxval);
    return ST_OK;
}

static u8 pnm_probe(const u8 *d, size_t n, const char *ext) {
    UNUSED(ext);
    if (n < 3 || d[0] != 'P') return 0;
    bool space = d[2] == ' ' || d[2] == '\n' || d[2] == '\r' || d[2] == '\t';
    if (d[1] >= '1' && d[1] <= '7' && space) return 90;
    if ((d[1] == 'F' || d[1] == 'f') && space) return 90;
    return 0;
}

const Codec CODEC_PNM = {
    .name = "PNM",
    .extensions = "pbm pgm ppm pnm pam pfm",
    .description = "Netpbm: PBM/PGM/PPM ASCII y binario hasta 16 bits, PAM con alfa, PFM flotante",
    .probe = pnm_probe,
    .decode = pnm_decode,
};
