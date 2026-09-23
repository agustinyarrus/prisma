// codec/hdr_decode.c - Radiance RGBE (.hdr/.pic): el HDR clasico de los mapas de entorno.
//
// Cada pixel es R, G, B con un exponente comun (E): valor = (mantisa + 0,5) * 2^(E - 136). Las
// filas vienen en RLE "nuevo" (cada canal por separado) o planas. Se soportan las 8 orientaciones
// del renglon de resolucion y el formato XYZE (se pasa a sRGB lineal con la matriz de Bradford
// ya adaptada a D65). La salida pasa por el tonemap comun (ver float_image.h).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "codec.h"
#include "float_image.h"

enum { HDR_MIN_RLE = 8, HDR_MAX_RLE = 0x7FFF, HDR_LINE_MAX = 4096 };

// XYZ (D65) -> sRGB lineal.
static const float XYZ_TO_SRGB[3][3] = {
    { 3.2404542f, -1.5371385f, -0.4985314f },
    { -0.9692660f, 1.8760108f, 0.0415560f },
    { 0.0556434f, -0.2040259f, 1.0572252f },
};

// Lee una linea de texto del encabezado; devuelve su largo o -1 si se termino el archivo.
static long read_line(const u8 **p, const u8 *end, char *out, size_t cap) {
    if (*p >= end) return -1;
    size_t n = 0;
    while (*p < end && **p != '\n') { if (n + 1 < cap) out[n++] = (char)**p; (*p)++; }
    if (*p < end) (*p)++;
    while (n && out[n - 1] == '\r') n--;
    out[n] = 0;
    return (long)n;
}

static bool decode_scanline(const u8 **pp, const u8 *end, u32 w, u8 *rgbe) {
    const u8 *p = *pp;
    if (w >= HDR_MIN_RLE && w <= HDR_MAX_RLE && end - p >= 4 && p[0] == 2 && p[1] == 2 && ((u32)p[2] << 8 | p[3]) == w && !(p[2] & 0x80)) {
        p += 4;
        for (u32 c = 0; c < 4; c++) {
            for (u32 x = 0; x < w;) {
                if (p >= end) return false;
                u32 count = *p++;
                if (count > 128) {
                    count -= 128;
                    if (p >= end || x + count > w) return false;
                    for (u32 k = 0; k < count; k++) rgbe[4 * (size_t)(x + k) + c] = *p;
                    p++;
                } else {
                    if (!count || x + count > w || (size_t)(end - p) < count) return false;
                    for (u32 k = 0; k < count; k++) rgbe[4 * (size_t)(x + k) + c] = p[k];
                    p += count;
                }
                x += count;
            }
        }
        *pp = p;
        return true;
    }
    // Plano o RLE "viejo" (1,1,1,n repite el pixel anterior n << shift veces).
    u32 shift = 0;
    for (u32 x = 0; x < w;) {
        if (end - p < 4) return false;
        if (p[0] == 1 && p[1] == 1 && p[2] == 1) {
            u32 count = (u32)p[3] << shift;
            if (!x) return false;
            for (u32 k = 0; k < count && x < w; k++, x++) memcpy(rgbe + 4 * (size_t)x, rgbe + 4 * (size_t)(x - 1), 4);
            shift += 8;
        } else {
            memcpy(rgbe + 4 * (size_t)x, p, 4);
            x++;
            shift = 0;
        }
        p += 4;
    }
    *pp = p;
    return true;
}

static Status hdr_decode(const DecodeContext *ctx, Decoded *out) {
    Diag *diag = ctx->diag;
    const u8 *p = ctx->data, *end = ctx->data + ctx->size;
    char line[HDR_LINE_MAX];
    bool xyz = false;
    float exposure = 1.0f;
    if (read_line(&p, end, line, sizeof line) < 0 || (strncmp(line, "#?RADIANCE", 10) != 0 && strncmp(line, "#?RGBE", 6) != 0 && strncmp(line, "#?", 2) != 0))
        return diag_fail(diag, ST_CORRUPT, "no es un Radiance HDR");
    for (;;) {
        long n = read_line(&p, end, line, sizeof line);
        if (n < 0) return diag_fail(diag, ST_TRUNCATED, "encabezado HDR sin terminar");
        if (n == 0) break;
        if (strncmp(line, "FORMAT=", 7) == 0) {
            if (strcmp(line + 7, "32-bit_rle_xyze") == 0) xyz = true;
            else if (strcmp(line + 7, "32-bit_rle_rgbe") != 0) return diag_fail(diag, ST_UNSUPPORTED, "HDR con %s", line);
        } else if (strncmp(line, "EXPOSURE=", 9) == 0) {
            float e = strtof(line + 9, NULL);
            if (e > 0.0f && isfinite(e)) exposure *= e;
        }
    }
    // Renglon de resolucion: dos ejes con signo, p.ej. "-Y 768 +X 1024" (el normal).
    char a1[3] = { 0 }, a2[3] = { 0 };
    unsigned n1 = 0, n2 = 0;
    if (read_line(&p, end, line, sizeof line) < 0 || sscanf(line, "%2s %u %2s %u", a1, &n1, a2, &n2) != 4 || !n1 || !n2)
        return diag_fail(diag, ST_CORRUPT, "renglon de resolucion HDR invalido");
    bool y_major = a1[1] == 'Y';
    u32 lines = n1, per_line = n2;
    u32 w = y_major ? n2 : n1, h = y_major ? n1 : n2;
    bool flip_y = y_major ? a1[0] == '+' : a2[0] == '+';          // +Y = de abajo hacia arriba
    bool flip_x = y_major ? a2[0] == '-' : a1[0] == '-';
    size_t pixels = (size_t)w * h, bytes;
    if (!mul_ok(pixels, 16, &bytes)) return diag_fail(diag, ST_TOO_LARGE, "HDR demasiado grande");
    if (ctx->opt->max_pixels && pixels > ctx->opt->max_pixels) return diag_fail(diag, ST_TOO_LARGE, "HDR supera --max-pixels");
    float *rgba = mem_alloc(bytes);
    u8 *scan = mem_alloc((size_t)per_line * 4 + 4);
    if (!rgba || !scan) { mem_free(rgba); mem_free(scan); return diag_fail(diag, ST_NO_MEMORY, "sin memoria"); }
    u32 ok_lines = 0;
    for (u32 li = 0; li < lines; li++) {
        if (!decode_scanline(&p, end, per_line, scan)) break;
        ok_lines++;
        for (u32 k = 0; k < per_line; k++) {
            const u8 *e = scan + 4 * (size_t)k;
            float c[3] = { 0, 0, 0 };
            if (e[3]) {
                float f = ldexpf(1.0f, (int)e[3] - (128 + 8));
                for (int i = 0; i < 3; i++) c[i] = (e[i] + 0.5f) * f / exposure;
            }
            if (xyz) {
                float x = c[0], y = c[1], z = c[2];
                for (int i = 0; i < 3; i++) c[i] = XYZ_TO_SRGB[i][0] * x + XYZ_TO_SRGB[i][1] * y + XYZ_TO_SRGB[i][2] * z;
            }
            u32 px = y_major ? k : li, py = y_major ? li : k;
            if (flip_x) px = w - 1 - px;
            if (flip_y) py = h - 1 - py;
            float *dst = rgba + ((size_t)py * w + px) * 4;
            dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2]; dst[3] = 1.0f;
        }
    }
    mem_free(scan);
    if (ok_lines < lines) {
        // Lo no leido: negro opaco (en HDR "transparente" no tiene sentido).
        for (u32 li = ok_lines; li < lines; li++)
            for (u32 k = 0; k < per_line; k++) {
                u32 px = y_major ? k : li, py = y_major ? li : k;
                if (flip_x) px = w - 1 - px;
                if (flip_y) py = h - 1 - py;
                float *dst = rgba + ((size_t)py * w + px) * 4;
                dst[0] = dst[1] = dst[2] = 0.0f; dst[3] = 1.0f;
            }
        diag_warn(diag, "HDR cortado o dañado: %u de %u renglones", ok_lines, lines);
    }
    Frame *f = decoded_add_frame(out);
    ToneOperator used = TONE_CLIP;
    Status st = f ? float_image_to_rgba16(rgba, w, h, ctx->opt, &f->image, &used, diag) : diag_fail(diag, ST_NO_MEMORY, "sin memoria");
    mem_free(rgba);
    if (st != ST_OK) return st;
    SourceInfo *src = &out->source;
    source_set(src, "HDR", "propio");
    src->bits = 32;
    src->channels = 3;
    src->is_float = true;
    source_detail(src, "Radiance %s · tonemap %s%s", xyz ? "XYZE" : "RGBE", tone_name(used), exposure != 1.0f ? " · con EXPOSURE" : "");
    return ST_OK;
}

static u8 hdr_probe(const u8 *d, size_t n, const char *ext) {
    UNUSED(ext);
    if (n >= 10 && memcmp(d, "#?RADIANCE", 10) == 0) return 100;
    if (n >= 6 && memcmp(d, "#?RGBE", 6) == 0) return 100;
    return 0;
}

const Codec CODEC_HDR = {
    .name = "HDR",
    .extensions = "hdr pic rgbe xyze",
    .description = "Radiance RGBE/XYZE con tonemap (ACES/Reinhard/recorte)",
    .probe = hdr_probe,
    .decode = hdr_decode,
};
