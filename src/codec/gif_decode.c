// codec/gif_decode.c - GIF 87a/89a: LZW, entrelazado, paletas globales y locales, transparencia y
// animacion con los cuatro modos de descarte, compuesta igual que un navegador.
//
// Recorrido: (1) se recorren los bloques y se anota cada cuadro (rectangulo, paleta, extension de
// control grafico, donde empiezan sus datos LZW); (2) el LZW de todos los cuadros se decodifica
// en paralelo (cada uno es independiente); (3) los cuadros se componen en orden sobre un lienzo
// RGBA que arranca transparente.
//
// Decisiones "de navegador" (las que ve la gente):
//   - el fondo es transparente (no el color de fondo del encabezado, que nadie respeta);
//   - descarte 2 limpia a transparente, descarte 3 restaura lo anterior;
//   - retardos de 0 o 1 centesima se estiran a 10 (100 ms), salvo --raw-delays;
//   - sin extension NETSCAPE la animacion se reproduce una vez; con repeticion N, N+1 veces.
#include <stdio.h>
#include "codec.h"
#include "reader.h"
#include "compose.h"
#include "../core/pool.h"

enum {
    LZW_MAX_CODES = 4096, LZW_NONE = 0xFFFF, GIF_MIN_DELAY_CS = 2, GIF_BROWSER_DELAY_CS = 10,
    DISPOSE_UNSPECIFIED = 0, DISPOSE_KEEP = 1, DISPOSE_TO_BACKGROUND = 2, DISPOSE_TO_PREVIOUS = 3,
};

typedef struct GifFrame {
    u16       left, top, width, height;
    bool      interlaced;
    const u8 *palette;            // RGB x palette_size (local o global)
    u32       palette_size;
    u8        min_code_size;
    const u8 *data;               // primer sub-bloque de LZW
    const u8 *data_end;
    u8        disposal;
    bool      has_transparency;
    u8        transparent_index;
    u16       delay_cs;
    // resultado del LZW
    u8       *indices;
    u32       decoded;            // indices validos (< width*height si estaba cortado)
    bool      corrupt;
} GifFrame;

typedef struct GifFile {
    u16        width, height;
    const u8  *global_palette;
    u32        global_size;
    Vec        frames;            // GifFrame
    bool       has_loop;
    u32        loop;
    bool       trailer;
} GifFile;

// ---- LZW -------------------------------------------------------------------------------------

typedef struct LzwTables {
    u16 prefix[LZW_MAX_CODES];
    u8  suffix[LZW_MAX_CODES];
    u8  first[LZW_MAX_CODES];
    u16 length[LZW_MAX_CODES];
} LzwTables;

// Junta los sub-bloques (largo + datos) en un buffer continuo.
static u8 *gather_blocks(const u8 *p, const u8 *end, size_t *size) {
    size_t total = 0;
    for (const u8 *q = p; q < end && *q; q += 1 + *q) total += *q;
    u8 *buf = mem_alloc(total + 8);
    if (!buf) return NULL;
    size_t at = 0;
    for (const u8 *q = p; q < end && *q; q += 1 + *q) {
        size_t n = MIN((size_t)*q, (size_t)(end - q - 1));
        memcpy(buf + at, q + 1, n);
        at += n;
        if (n < *q) break;
    }
    memset(buf + at, 0, 8);
    *size = at;
    return buf;
}

// Decodifica hasta `count` indices. Devuelve cuantos salieron; *corrupt si el flujo estaba roto.
static u32 lzw_decode(const u8 *data, size_t size, u32 min_code_size, u8 *out, u32 count, LzwTables *t, bool *corrupt) {
    *corrupt = false;
    if (min_code_size < 2 || min_code_size > 11) { *corrupt = true; return 0; }
    const u32 clear = 1u << min_code_size, eoi = clear + 1;
    u32 code_size = min_code_size + 1, next = clear + 2, prev = LZW_NONE;
    for (u32 i = 0; i < clear; i++) { t->prefix[i] = LZW_NONE; t->suffix[i] = (u8)i; t->first[i] = (u8)i; t->length[i] = 1; }
    u64 bits = 0;
    u32 nbits = 0;
    size_t pos = 0;
    u32 produced = 0;
    while (produced < count) {
        while (nbits < code_size && pos < size) { bits |= (u64)data[pos++] << nbits; nbits += 8; }
        if (nbits < code_size) break;                              // datos agotados (sin EOI)
        u32 code = (u32)(bits & ((1u << code_size) - 1));
        bits >>= code_size;
        nbits -= code_size;
        if (code == clear) { code_size = min_code_size + 1; next = clear + 2; prev = LZW_NONE; continue; }
        if (code == eoi) break;
        u32 emit;
        if (prev == LZW_NONE) {
            if (code >= clear) { *corrupt = true; break; }
            emit = code;
        } else if (code < next) {
            emit = code;
            if (next < LZW_MAX_CODES) {
                t->prefix[next] = (u16)prev; t->suffix[next] = t->first[code];
                t->first[next] = t->first[prev]; t->length[next] = (u16)(t->length[prev] + 1);
                next++;
            }
        } else if (code == next && next < LZW_MAX_CODES) {        // caso KwKwK
            t->prefix[next] = (u16)prev; t->suffix[next] = t->first[prev];
            t->first[next] = t->first[prev]; t->length[next] = (u16)(t->length[prev] + 1);
            emit = next++;
        } else {
            *corrupt = true;
            break;
        }
        if (next == (1u << code_size) && code_size < 12) code_size++;
        // La cadena se escribe de atras para adelante siguiendo los prefijos: su largo ya se
        // conoce, asi que no hace falta una pila.
        u32 len = t->length[emit];
        u32 c = emit;
        for (u32 k = len; k-- > 0;) {
            if (produced + k < count) out[produced + k] = t->suffix[c];
            c = t->prefix[c];
        }
        produced = MIN(produced + len, count);
        prev = code;
    }
    return produced;
}

static void decode_frame_task(void *arg, size_t index) {
    GifFrame *f = &((GifFrame *)arg)[index];
    u32 count = (u32)f->width * f->height;
    f->indices = mem_alloc(count ? count : 1);
    LzwTables *tables = mem_alloc(sizeof *tables);
    size_t size = 0;
    u8 *data = f->indices && tables ? gather_blocks(f->data, f->data_end, &size) : NULL;
    if (!data) { f->corrupt = true; f->decoded = 0; mem_free(tables); return; }
    f->decoded = lzw_decode(data, size, f->min_code_size, f->indices, count, tables, &f->corrupt);
    mem_free(data);
    mem_free(tables);
}

// ---- estructura del archivo --------------------------------------------------------------------

static void skip_blocks(Reader *r) {
    for (;;) {
        u8 n = rd_u8(r);
        if (r->bad || n == 0) return;
        rd_skip(r, n);
    }
}

static Status parse_gif(const DecodeContext *ctx, GifFile *g, Decoded *out) {
    Reader r = reader_make(ctx->data, ctx->size);
    const u8 *sig = rd_bytes(&r, 6);
    if (!sig || memcmp(sig, "GIF", 3) != 0) return diag_fail(ctx->diag, ST_UNKNOWN_FORMAT, "no es un GIF");
    g->width = rd_le16(&r);
    g->height = rd_le16(&r);
    u8 packed = rd_u8(&r);
    rd_skip(&r, 2);                                             // indice de fondo, aspecto
    if (packed & 0x80) {
        g->global_size = 2u << (packed & 7);
        g->global_palette = rd_bytes(&r, 3 * (size_t)g->global_size);
    }
    if (r.bad) return diag_fail(ctx->diag, ST_TRUNCATED, "encabezado de GIF incompleto");

    // Extension de control grafico pendiente: se aplica al proximo cuadro.
    u8 disposal = 0, ti = 0;
    bool has_t = false;
    u16 delay = 0;
    while (!r.bad) {
        u8 block = rd_u8(&r);
        if (r.bad) break;
        if (block == 0x3B) { g->trailer = true; break; }
        if (block == 0x21) {
            u8 label = rd_u8(&r);
            if (label == 0xF9) {
                u8 n = rd_u8(&r);
                const u8 *gce = rd_bytes(&r, n);
                if (gce && n >= 4) {
                    disposal = (gce[0] >> 2) & 7;
                    has_t = gce[0] & 1;
                    delay = load_le16(gce + 1);
                    ti = gce[3];
                }
                skip_blocks(&r);
            } else if (label == 0xFF) {
                u8 n = rd_u8(&r);
                const u8 *app = rd_bytes(&r, n);
                if (app && n == 11 && (memcmp(app, "NETSCAPE2.0", 11) == 0 || memcmp(app, "ANIMEXTS1.0", 11) == 0)) {
                    u8 sub = rd_u8(&r);
                    const u8 *d = rd_bytes(&r, sub);
                    if (d && sub >= 3 && d[0] == 1) { g->has_loop = true; g->loop = load_le16(d + 1); }
                }
                skip_blocks(&r);
            } else if (label == 0xFE) {
                const u8 *start = r.p;
                skip_blocks(&r);
                size_t n = 0;
                u8 *text = gather_blocks(start, r.p, &n);
                if (text) {
                    bool printable = n > 0;
                    for (size_t i = 0; i < n && printable; i++) printable = text[i] >= 0x20 || text[i] == '\n' || text[i] == '\r' || text[i] == '\t';
                    if (printable) metadata_add_text(&out->meta, "Comment", (const char *)text, n);
                    mem_free(text);
                }
            } else {
                skip_blocks(&r);
            }
            continue;
        }
        if (block != 0x2C) { diag_warn(ctx->diag, "bloque GIF desconocido (0x%02X): se deja de leer ahi", block); break; }

        GifFrame f = { 0 };
        f.left = rd_le16(&r);
        f.top = rd_le16(&r);
        f.width = rd_le16(&r);
        f.height = rd_le16(&r);
        u8 fp = rd_u8(&r);
        f.interlaced = fp & 0x40;
        if (fp & 0x80) {
            f.palette_size = 2u << (fp & 7);
            f.palette = rd_bytes(&r, 3 * (size_t)f.palette_size);
        } else {
            f.palette = g->global_palette;
            f.palette_size = g->global_size;
        }
        f.min_code_size = rd_u8(&r);
        f.data = r.p;
        skip_blocks(&r);
        f.data_end = r.p;
        f.disposal = disposal;
        f.has_transparency = has_t;
        f.transparent_index = ti;
        f.delay_cs = delay;
        disposal = 0; has_t = false; delay = 0; ti = 0;
        if (f.width == 0 || f.height == 0) continue;           // cuadro vacio: no dibuja nada
        GifFrame *slot = vec_push(&g->frames);
        if (!slot) return diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria");
        *slot = f;
        if (r.bad) break;
    }
    if (!g->frames.count) return diag_fail(ctx->diag, r.bad ? ST_TRUNCATED : ST_CORRUPT, "el GIF no tiene imagenes");
    if (!g->trailer) diag_warn(ctx->diag, "GIF sin terminador: el archivo parece cortado");
    // Lienzo de 0x0 (encabezado roto): se agranda a lo que ocupan los cuadros, como hacen los navegadores.
    if (!g->width || !g->height) {
        for (size_t i = 0; i < g->frames.count; i++) {
            GifFrame *f = &VEC_AT(&g->frames, GifFrame, i);
            g->width = (u16)MAX((u32)g->width, MIN(65535u, (u32)f->left + f->width));
            g->height = (u16)MAX((u32)g->height, MIN(65535u, (u32)f->top + f->height));
        }
    }
    return ST_OK;
}

// ---- composicion ------------------------------------------------------------------------------

// Fila de destino de la fila i guardada de un cuadro entrelazado (4 pasadas: 0,8,..; 4,12,..; 2,6,..; 1,3,..).
static u32 interlaced_row(u32 i, u32 h) {
    u32 p1 = (h + 7) / 8, p2 = (h + 3) / 8, p3 = (h + 1) / 4;
    if (i < p1) return i * 8;
    i -= p1;
    if (i < p2) return 4 + i * 8;
    i -= p2;
    if (i < p3) return 2 + i * 4;
    i -= p3;
    return 1 + i * 2;
}

static u32 draw_frame(Image *canvas, const GifFrame *f) {
    static const u8 FALLBACK_BW[6] = { 0, 0, 0, 255, 255, 255 };
    const u8 *pal = f->palette ? f->palette : FALLBACK_BW;
    u32 pal_size = f->palette ? f->palette_size : 2, oob = 0;
    for (u32 i = 0; i < f->height; i++) {
        u32 fy = f->interlaced ? interlaced_row(i, f->height) : i;
        u32 cy = (u32)f->top + fy;
        if (cy >= canvas->height) continue;
        u8 *row = image_row(canvas, cy);
        const u8 *src = f->indices + (size_t)i * f->width;
        for (u32 x = 0; x < f->width; x++) {
            u32 cx = (u32)f->left + x;
            if (cx >= canvas->width) break;
            if ((size_t)i * f->width + x >= f->decoded) return oob;     // lo que falta queda como estaba
            u8 idx = src[x];
            if (f->has_transparency && idx == f->transparent_index) continue;
            u8 *px = row + 4 * (size_t)cx;
            if (idx < pal_size) { px[0] = pal[3 * idx]; px[1] = pal[3 * idx + 1]; px[2] = pal[3 * idx + 2]; }
            else { px[0] = px[1] = px[2] = 0; oob++; }
            px[3] = 255;
        }
    }
    return oob;
}

static Status gif_decode(const DecodeContext *ctx, Decoded *out) {
    GifFile g = { 0 };
    vec_init(&g.frames, sizeof(GifFrame));
    Status s = parse_gif(ctx, &g, out);
    if (s != ST_OK) { vec_free(&g.frames); return s; }
    const DecodeOptions *opt = ctx->opt;
    u32 count = (u32)g.frames.count;
    GifFrame *frames = g.frames.items;

    bool animated = count > 1;
    u32 last = count - 1;
    if (opt->frame_mode == FRAMES_ONE) {
        if (opt->frame_index >= count) {
            vec_free(&g.frames);
            return diag_fail(ctx->diag, ST_INVALID, "--frame %u: el GIF tiene %u cuadros (0..%u)", opt->frame_index, count, count - 1);
        }
        last = opt->frame_index;
    } else if (opt->single_frame) {
        last = 0;
    }
    parallel_for((size_t)last + 1, decode_frame_task, frames);

    SourceInfo *src = &out->source;
    source_set(src, "GIF", "propio");
    src->bits = 8;
    src->channels = 1;
    src->total_frames = count;
    bool any_t = false;
    for (u32 i = 0; i < count; i++) any_t |= frames[i].has_transparency;
    src->has_alpha = any_t;
    src->alpha_origin = any_t ? ALPHA_ORIGIN_KEY : ALPHA_ORIGIN_NONE;
    source_detail(src, "%.6s · paleta %s%u colores%s%s", (const char *)ctx->data, g.global_palette ? "global " : "",
                  g.global_palette ? g.global_size : frames[0].palette_size, any_t ? " · transparencia" : "",
                  count > 1 ? " · animado" : "");

    Image canvas;
    s = image_alloc(&canvas, g.width, g.height, 8, true, opt->max_pixels, ctx->diag);
    u32 corrupt = 0, oob = 0, fixed_delays = 0;
    Image saved = { 0 };
    u8 prev_disposal = DISPOSE_UNSPECIFIED;
    const GifFrame *prev = NULL;
    for (u32 i = 0; i <= last && s == ST_OK; i++) {
        GifFrame *f = &frames[i];
        if (prev) {
            if (prev_disposal == DISPOSE_TO_BACKGROUND) compose_clear(&canvas, prev->left, prev->top, prev->width, prev->height);
            else if (prev_disposal == DISPOSE_TO_PREVIOUS) compose_restore(&canvas, &saved, prev->left, prev->top);
        }
        image_free(&saved);
        if (f->disposal == DISPOSE_TO_PREVIOUS) s = compose_save(&canvas, f->left, f->top, f->width, f->height, &saved, ctx->diag);
        if (s != ST_OK) break;
        corrupt += f->corrupt || f->decoded < (u32)f->width * f->height;
        oob += draw_frame(&canvas, f);
        if (opt->frame_mode == FRAMES_ONE ? i == last : (i == last || !opt->single_frame)) {
            Frame *of = decoded_add_frame(out);
            if (!of) { s = diag_fail(ctx->diag, ST_NO_MEMORY, "sin memoria"); break; }
            s = image_clone(&canvas, &of->image, ctx->diag);
            of->index = i;
            u32 delay = f->delay_cs;
            if (!opt->raw_gif_delays && delay < GIF_MIN_DELAY_CS) { delay = GIF_BROWSER_DELAY_CS; fixed_delays++; }
            of->delay_num = delay;
            of->delay_den = 100;
        }
        prev_disposal = f->disposal;
        prev = f;
    }
    image_free(&saved);
    image_free(&canvas);
    for (u32 i = 0; i < count; i++) mem_free(frames[i].indices);
    vec_free(&g.frames);
    if (s != ST_OK) return s;

    out->animated = animated && opt->frame_mode != FRAMES_ONE && !opt->single_frame;
    // NETSCAPE: 0 = infinito; N = N repeticiones DESPUES de la primera. Sin la extension: una vez.
    out->loop_count = !g.has_loop ? 1 : g.loop == 0 ? 0 : (u32)g.loop + 1;
    if (corrupt) diag_warn(ctx->diag, "%u cuadro(s) con datos LZW cortados o dañados: se dibujo lo recuperable", corrupt);
    if (oob) diag_warn(ctx->diag, "%u pixeles con indice fuera de la paleta (se pintan negros)", oob);
    if (fixed_delays && out->animated)
        diag_warn(ctx->diag, "%u cuadros con retardo de 0-10 ms: se usan 100 ms como los navegadores (--raw-delays)", fixed_delays);
    return ST_OK;
}

static u8 gif_probe(const u8 *data, size_t size, const char *ext) {
    UNUSED(ext);
    return size >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0) ? 100 : 0;
}

const Codec CODEC_GIF = {
    .name = "GIF",
    .extensions = "gif",
    .description = "GIF 87a/89a: animacion con los 4 modos de descarte, transparencia, entrelazado",
    .probe = gif_probe,
    .decode = gif_decode,
};
