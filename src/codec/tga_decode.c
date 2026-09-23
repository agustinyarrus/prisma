// codec/tga_decode.c - Truevision TGA: tipos 1/2/3 y sus RLE 9/10/11; 8/15/16/24/32 bits;
// paletas de 15/16/24/32 bits; los 4 origenes posibles; pie TGA 2.0 con el TIPO DE ALFA.
//
// El pie 2.0 dice que significa el canal extra: 0 no hay alfa, 1-2 datos basura a ignorar,
// 3 alfa util, 4 alfa PREMULTIPLICADO (se des-premultiplica al leer). Sin pie, el alfa se usa
// si hay algun valor != 0 (miles de TGA de 32 bits se guardaron con el alfa en 0 por error);
// --tga-alpha on|off manda. Los paquetes RLE pueden cruzar filas (muchos escritores lo hacen).
#include <stdio.h>
#include "codec.h"
#include "reader.h"

enum {
    TGA_MAPPED = 1, TGA_TRUE = 2, TGA_GRAY = 3, TGA_RLE_MAPPED = 9, TGA_RLE_TRUE = 10, TGA_RLE_GRAY = 11,
    TGA_HEADER = 18, TGA_FOOTER = 26, TGA_EXT_SIZE = 495, TGA_MAX_PALETTE = 4096,
    ALPHA_TYPE_NONE = 0, ALPHA_TYPE_IGNORE = 1, ALPHA_TYPE_RETAIN = 2, ALPHA_TYPE_USEFUL = 3, ALPHA_TYPE_PREMUL = 4,
};

typedef struct TgaHeader {
    u8  id_length, cmap_type, type;
    u16 cmap_first, cmap_length;
    u8  cmap_bits;
    u16 width, height;
    u8  bits, descriptor;
} TgaHeader;

static bool read_header(const u8 *d, size_t n, TgaHeader *h) {
    if (n < TGA_HEADER) return false;
    h->id_length = d[0];
    h->cmap_type = d[1];
    h->type = d[2];
    h->cmap_first = load_le16(d + 3);
    h->cmap_length = load_le16(d + 5);
    h->cmap_bits = d[7];
    h->width = load_le16(d + 12);
    h->height = load_le16(d + 14);
    h->bits = d[16];
    h->descriptor = d[17];
    return true;
}

static bool header_plausible(const TgaHeader *h) {
    bool mapped = h->type == TGA_MAPPED || h->type == TGA_RLE_MAPPED;
    bool gray = h->type == TGA_GRAY || h->type == TGA_RLE_GRAY;
    bool truec = h->type == TGA_TRUE || h->type == TGA_RLE_TRUE;
    if (!mapped && !gray && !truec) return false;
    if (h->cmap_type > 1 || !h->width || !h->height) return false;
    if (mapped && (h->cmap_type != 1 || (h->bits != 8 && h->bits != 16) || !h->cmap_length)) return false;
    if (h->cmap_type == 1 && h->cmap_bits != 15 && h->cmap_bits != 16 && h->cmap_bits != 24 && h->cmap_bits != 32) return false;
    if (gray && h->bits != 8 && h->bits != 16) return false;
    if (truec && h->bits != 15 && h->bits != 16 && h->bits != 24 && h->bits != 32) return false;
    return (h->descriptor & 0xC0) == 0;
}

// Un pixel crudo (1..4 bytes) a RGBA8.
static void unpack(const u8 *p, u32 bits, bool gray, u8 out[4]) {
    if (gray) {
        out[0] = out[1] = out[2] = p[0];
        out[3] = bits == 16 ? p[1] : 255;
        return;
    }
    switch (bits) {
    case 15: case 16: {
        u32 v = load_le16(p);
        u32 r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
        out[0] = (u8)(r << 3 | r >> 2); out[1] = (u8)(g << 3 | g >> 2); out[2] = (u8)(b << 3 | b >> 2);
        out[3] = (bits == 16 && !(v & 0x8000)) ? 0 : 255;
        break;
    }
    case 24: out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255; break;
    default: out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = p[3]; break;
    }
}

static Status tga_decode(const DecodeContext *ctx, Decoded *out) {
    Diag *diag = ctx->diag;
    TgaHeader h;
    if (!read_header(ctx->data, ctx->size, &h) || !header_plausible(&h)) return diag_fail(diag, ST_CORRUPT, "encabezado TGA invalido");
    bool mapped = h.type == TGA_MAPPED || h.type == TGA_RLE_MAPPED;
    bool gray = h.type == TGA_GRAY || h.type == TGA_RLE_GRAY;
    bool rle = h.type >= TGA_RLE_MAPPED;
    u32 alpha_bits = h.descriptor & 15;

    // Pie TGA 2.0: tipo de alfa en el area de extension.
    int alpha_type = -1;
    if (ctx->size >= TGA_HEADER + TGA_FOOTER && memcmp(ctx->data + ctx->size - 18, "TRUEVISION-XFILE.", 17) == 0) {
        u32 ext = load_le32(ctx->data + ctx->size - TGA_FOOTER);
        if (ext && (size_t)ext + TGA_EXT_SIZE <= ctx->size - TGA_FOOTER && load_le16(ctx->data + ext) == TGA_EXT_SIZE)
            alpha_type = ctx->data[ext + 494];
    }

    Reader r = reader_make(ctx->data, ctx->size);
    rd_skip(&r, TGA_HEADER + h.id_length);
    u8 palette[TGA_MAX_PALETTE][4];           // 16 KB en pila; paletas mas grandes no existen en la practica
    u32 pal_count = 0;
    if (h.cmap_type == 1) {
        u32 entry = (h.cmap_bits + 7) / 8;
        const u8 *cm = rd_bytes(&r, (size_t)h.cmap_length * entry);
        if (!cm) return diag_fail(diag, ST_TRUNCATED, "paleta TGA cortada");
        pal_count = MIN((u32)h.cmap_length, (u32)ARRAY_LEN(palette));
        for (u32 i = 0; i < pal_count; i++) unpack(cm + (size_t)i * entry, h.cmap_bits, false, palette[i]);
    }
    u32 w = h.width, ht = h.height, bpp_in = (h.bits + 7) / 8;
    Image *im;
    Frame *f = decoded_add_frame(out);
    if (!f) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
    im = &f->image;
    Status s = image_alloc(im, w, ht, 8, true, ctx->opt->max_pixels, diag);
    if (s != ST_OK) return s;

    // Se decodifica en el orden del archivo a un buffer RGBA y despues se ubica segun el origen.
    size_t total = (size_t)w * ht, done = 0;
    u8 *seq = mem_alloc(total * 4);
    if (!seq) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
    const u8 *p = r.p, *end = r.end;
    u32 bad_index = 0;
#define EMIT(raw)                                                                          \
    do {                                                                                   \
        u8 *dst = seq + done * 4;                                                          \
        if (mapped) {                                                                      \
            u32 k = (bpp_in == 1 ? (raw)[0] : load_le16(raw)) - h.cmap_first;              \
            if (k < pal_count) memcpy(dst, palette[k], 4);                                 \
            else { dst[0] = dst[1] = dst[2] = 0; dst[3] = 255; bad_index++; }              \
        } else unpack(raw, h.bits, gray, dst);                                             \
        done++;                                                                            \
    } while (0)
    if (!rle) {
        size_t avail = (size_t)(end - p) / bpp_in;
        for (size_t i = 0; i < total && i < avail; i++) EMIT(p + i * bpp_in);
    } else {
        while (done < total && p < end) {
            u8 packet = *p++;
            u32 n = (packet & 127) + 1;
            if (packet & 128) {
                if ((size_t)(end - p) < bpp_in) break;
                for (u32 k = 0; k < n && done < total; k++) EMIT(p);
                p += bpp_in;
            } else {
                for (u32 k = 0; k < n && done < total; k++) {
                    if ((size_t)(end - p) < bpp_in) { p = end; break; }
                    EMIT(p);
                    p += bpp_in;
                }
            }
        }
    }
#undef EMIT
    if (done < total) {
        memset(seq + done * 4, 0, (total - done) * 4);
        diag_warn(diag, "TGA cortado: %zu de %zu pixeles (el resto queda transparente)", done, total);
    }
    if (bad_index) diag_warn(diag, "%u pixeles con indice fuera de la paleta", bad_index);

    bool right_to_left = h.descriptor & 0x10, top_to_bottom = h.descriptor & 0x20;
    for (u32 y = 0; y < ht; y++) {
        u32 dy = top_to_bottom ? y : ht - 1 - y;
        const u8 *src = seq + (size_t)y * w * 4;
        u8 *dst = image_row(im, dy);
        if (!right_to_left) memcpy(dst, src, (size_t)w * 4);
        else for (u32 x = 0; x < w; x++) memcpy(dst + 4 * (size_t)(w - 1 - x), src + 4 * (size_t)x, 4);
    }
    mem_free(seq);

    // ---- que hacer con el canal alfa --------------------------------------------------------
    bool has_channel = (!mapped && (h.bits == 32 || (h.bits == 16 && !gray))) || (gray && h.bits == 16) ||
                       (mapped && (h.cmap_bits == 32 || h.cmap_bits == 16));
    bool any_nonzero = false, any_partial = false;
    if (has_channel) {
        for (u32 y = 0; y < ht && !(any_nonzero && any_partial); y++) {
            const u8 *row = image_row(im, y);
            for (u32 x = 0; x < w; x++) { u8 a = row[4 * (size_t)x + 3]; any_nonzero |= a != 0; any_partial |= a != 0 && a != 255; }
        }
    }
    Tristate mode = ctx->opt->tga_alpha;
    bool use_alpha;
    if (mode == TRI_ON) use_alpha = has_channel;
    else if (mode == TRI_OFF) use_alpha = false;
    else if (alpha_type >= 0) use_alpha = has_channel && (alpha_type == ALPHA_TYPE_USEFUL || alpha_type == ALPHA_TYPE_PREMUL);
    else use_alpha = has_channel && any_nonzero;
    // 16 bits con el bit de atributo en 0 en todo el archivo y el descriptor diciendo "0 bits de
    // atributo": no es alfa, es un bit que el escritor dejo sin usar.
    if (mode == TRI_AUTO && alpha_type < 0 && h.bits == 16 && !gray && alpha_bits == 0) use_alpha = false;

    bool premultiplied = use_alpha && alpha_type == ALPHA_TYPE_PREMUL;
    if (!use_alpha)
        for (u32 y = 0; y < ht; y++) {
            u8 *row = image_row(im, y);
            for (u32 x = 0; x < w; x++) row[4 * (size_t)x + 3] = 255;
        }
    else if (premultiplied)
        image_unpremultiply(im);

    SourceInfo *src = &out->source;
    source_set(src, "TGA", "propio");
    src->bits = h.bits == 15 || h.bits == 16 ? 5 : 8;
    src->channels = gray ? (h.bits == 16 ? 2 : 1) : (h.bits == 32 ? 4 : 3);
    src->has_alpha = use_alpha;
    src->alpha_origin = !use_alpha ? ALPHA_ORIGIN_NONE : premultiplied ? ALPHA_ORIGIN_PREMULTIPLIED : ALPHA_ORIGIN_CHANNEL;
    static const char *const ATYPE[] = { "sin alfa", "alfa ignorable", "alfa a conservar", "alfa util", "alfa premultiplicado" };
    source_detail(src, "%s%s · %u bits%s%s", mapped ? "paleta" : gray ? "gris" : "color", rle ? " RLE" : "", h.bits,
                  alpha_type >= 0 && alpha_type <= 4 ? " · TGA 2.0: " : "", alpha_type >= 0 && alpha_type <= 4 ? ATYPE[alpha_type] : "");
    if (has_channel && !use_alpha && mode == TRI_AUTO && alpha_type < 0 && !any_nonzero && h.bits == 32)
        diag_warn(diag, "TGA de 32 bits con alfa todo en 0: se trata como opaco (--tga-alpha on lo respeta)");
    UNUSED(any_partial);
    return ST_OK;
}

// TGA no tiene firma: se reconoce por un encabezado coherente, con mas confianza si el archivo
// se llama .tga o trae el pie de TGA 2.0.
static u8 tga_probe(const u8 *d, size_t n, const char *ext) {
    TgaHeader h;
    if (!read_header(d, n, &h) || !header_plausible(&h)) return 0;
    bool footer = n >= TGA_HEADER + TGA_FOOTER && memcmp(d + n - 18, "TRUEVISION-XFILE.", 17) == 0;
    bool named = strcmp(ext, "tga") == 0 || strcmp(ext, "tpic") == 0 || strcmp(ext, "icb") == 0 ||
                 strcmp(ext, "vda") == 0 || strcmp(ext, "vst") == 0;
    return footer ? 90 : named ? 80 : 20;
}

const Codec CODEC_TGA = {
    .name = "TGA",
    .extensions = "tga tpic icb vda vst",
    .description = "Truevision TGA: RLE, paletas, 4 origenes, alfa segun pie 2.0 (incluido premultiplicado)",
    .probe = tga_probe,
    .decode = tga_decode,
};
