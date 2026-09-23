// codec/codec.c - registro de formatos, elegir decodificador por firma y caer a WIC.
#include <stdarg.h>
#include <stdio.h>
#include "codec.h"
#include "wic.h"

enum { PROBE_ACCEPT = 30, MAX_NESTING = 4 };

static const Codec *const BUILTIN[] = {
    &CODEC_PNG, &CODEC_GIF, &CODEC_BMP, &CODEC_ICO, &CODEC_QOI, &CODEC_PNM, &CODEC_FARBFELD,
    &CODEC_PSD, &CODEC_HDR, &CODEC_PCX, &CODEC_DDS, &CODEC_SGI, &CODEC_TGA,
};

const Codec *const *codec_builtin_list(size_t *count) {
    *count = ARRAY_LEN(BUILTIN);
    return BUILTIN;
}

bool frame_wanted(const DecodeOptions *opt, u32 index, u32 total) {
    UNUSED(total);
    switch (opt->frame_mode) {
    case FRAMES_ALL: return true;
    case FRAMES_ONE: return index == opt->frame_index;
    default:         return index == 0;
    }
}

void source_set(SourceInfo *s, const char *format, const char *codec) {
    snprintf(s->format, sizeof s->format, "%s", format);
    snprintf(s->codec, sizeof s->codec, "%s", codec);
}

void source_detail(SourceInfo *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s->detail, sizeof s->detail, fmt, ap);
    va_end(ap);
}

// Firmas de formatos que prisma reconoce pero no puede leer: mejor un mensaje claro que un
// "formato desconocido" pelado.
const char *format_hint_unsupported(const u8 *d, size_t n) {
    if (n >= 12 && memcmp(d, "\0\0\0\x0CjP  \r\n\x87\n", 12) == 0) return "JPEG 2000 (Windows no trae codec)";
    if (n >= 4 && memcmp(d, "\xFF\x4F\xFF\x51", 4) == 0) return "JPEG 2000 (codestream, Windows no trae codec)";
    if (n >= 4 && memcmp(d, "\x76\x2F\x31\x01", 4) == 0) return "OpenEXR";
    if (n >= 4 && memcmp(d, "%PDF", 4) == 0) return "PDF (es un documento, no una imagen)";
    if (n >= 9 && memcmp(d, "gimp xcf ", 9) == 0) return "GIMP XCF";
    if (n >= 4 && memcmp(d, "icns", 4) == 0) return "ICNS de macOS";
    if (n >= 4 && memcmp(d, "\xD7\xCD\xC6\x9A", 4) == 0) return "WMF (metarchivo vectorial)";
    if (n >= 44 && memcmp(d + 40, " EMF", 4) == 0) return "EMF (metarchivo vectorial)";
    if (n >= 12 && memcmp(d + 4, "ftyp", 4) == 0) {
        if (memcmp(d + 8, "avif", 4) == 0 || memcmp(d + 8, "avis", 4) == 0)
            return "AVIF: falta la \"Extensión de video AV1\" de Microsoft Store";
        if (memcmp(d + 8, "hei", 3) == 0 || memcmp(d + 8, "mif1", 4) == 0 || memcmp(d + 8, "hev", 3) == 0)
            return "HEIF/HEIC: faltan \"Extensiones de imagen HEIF\" y \"Extensiones de video HEVC\" de Microsoft Store";
    }
    if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0)
        return "WebP: falta \"Extensiones de imagen Webp\" de Microsoft Store";
    if ((n >= 2 && d[0] == 0xFF && d[1] == 0x0A) || (n >= 12 && memcmp(d, "\0\0\0\x0CJXL \r\n\x87\n", 12) == 0))
        return "JPEG XL: falta \"Extensión de imagen JPEG XL\" de Microsoft Store";
    // SVG: texto con "<svg" en el primer KB.
    size_t scan = MIN(n, (size_t)1024);
    for (size_t i = 0; i + 4 <= scan; i++)
        if (d[i] == '<' && (d[i + 1] | 32) == 's' && (d[i + 2] | 32) == 'v' && (d[i + 3] | 32) == 'g') return "SVG (vectorial)";
    return NULL;
}

static const Codec *best_builtin(const DecodeContext *ctx, u8 *score_out) {
    const Codec *best = NULL;
    u8 best_score = 0;
    for (size_t i = 0; i < ARRAY_LEN(BUILTIN); i++) {
        u8 score = BUILTIN[i]->probe(ctx->data, ctx->size, ctx->ext ? ctx->ext : "");
        if (score > best_score) { best_score = score; best = BUILTIN[i]; }
    }
    *score_out = best_score;
    return best;
}

Status decode_image(const DecodeContext *ctx, Decoded *out) {
    decoded_init(out);
    Diag *diag = ctx->diag;
    if (ctx->size == 0) return diag_fail(diag, ST_CORRUPT, "el archivo esta vacio (0 bytes)");
    if (ctx->nesting > MAX_NESTING) return diag_fail(diag, ST_CORRUPT, "demasiados niveles de imagenes anidadas");

    u8 score = 0;
    const Codec *codec = ctx->opt->force_wic ? NULL : best_builtin(ctx, &score);
    if (codec && score >= PROBE_ACCEPT) {
        Status s = codec->decode(ctx, out);
        if (s == ST_OK || s != ST_UNSUPPORTED || ctx->opt->no_wic) return s;
        // Variante que el decodificador propio no cubre: WIC como segunda opinion.
        Diag first = *diag;
        diag_reset(diag);
        decoded_free(out);
        decoded_init(out);
        if (wic_decode(ctx, out) == ST_OK) {
            diag_warn(diag, "%s: %s; se leyo con WIC", codec->name, first.message);
            return ST_OK;
        }
        decoded_free(out);
        decoded_init(out);
        *diag = first;
        return s;
    }
    if (ctx->opt->no_wic) {
        const char *hint = format_hint_unsupported(ctx->data, ctx->size);
        return diag_fail(diag, ST_UNKNOWN_FORMAT, hint ? "formato no soportado: %s" : "formato desconocido%s", hint ? hint : "");
    }
    Status s = wic_decode(ctx, out);
    if (s == ST_UNKNOWN_FORMAT) {
        const char *hint = format_hint_unsupported(ctx->data, ctx->size);
        if (hint) {
            diag_reset(diag);
            diag_fail(diag, ST_UNKNOWN_FORMAT, "formato no soportado: %s", hint);
        }
    }
    return s;
}

Status decode_nested(const DecodeContext *parent, const u8 *data, size_t size, Decoded *out) {
    DecodeContext child = *parent;
    child.data = data;
    child.size = size;
    child.ext = "";
    child.nesting = parent->nesting + 1;
    return decode_image(&child, out);
}
