// ops/pipeline.c - ver pipeline.h
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "pipeline.h"
#include "pixelops.h"
#include "propagate.h"
#include "../codec/wic.h"
#include "../core/icc.h"
#include "../core/cicp.h"
#include "../core/text.h"

static const float DEFAULT_BG_FUZZ = 0.08f;
static const Rgba WHITE = { 1, 1, 1, 1 };

void pipeline_defaults(PipelineOptions *o) {
    memset(o, 0, sizeof *o);
    o->auto_orient = true;
    o->threshold = -1;
    o->opacity = 1.0f;
    o->fuzz = -1.0f;                 // cada operacion elige su default
    o->c2a_transparency = 0.0f;
    o->c2a_opacity = 1.0f;
    o->remove_bg_edge = 2;
    o->levels_gamma = 1.0f;
    o->filter = RF_LANCZOS3;
    o->dither = DITHER_FS;
    o->dither_level = 1.0f;
}

// ---- resumen legible ------------------------------------------------------------------------

static void note(PipelineReport *rep, const char *fmt, ...) {
    size_t len = strlen(rep->summary);
    if (len + 4 >= sizeof rep->summary) return;
    if (len) { memcpy(rep->summary + len, " · ", 5); len += 4; }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(rep->summary + len, sizeof rep->summary - len, fmt, ap);
    va_end(ap);
    rep->steps++;
}

#define EACH_FRAME(doc, f) for (Frame *f = decoded_frame(doc, 0), *f##_end = f + decoded_frame_count(doc); f < f##_end; f++)

static Status run_kernels(Decoded *doc, const Kernel *k, u32 n) {
    if (!n) return ST_OK;
    EACH_FRAME(doc, f) pixelops_run(&f->image, k, n);
    return ST_OK;
}

// ---- mascara externa ---------------------------------------------------------------------------

static bool image_has_alpha_variation(const Image *im) {
    for (u32 y = 0; y < im->height; y++) {
        const u8 *row = image_row(im, y);
        for (u32 x = 0; x < im->width; x++) {
            u32 a = im->depth == 8 ? row[4 * (size_t)x + 3] : ((const u16 *)row)[4 * (size_t)x + 3] >> 8;
            if (a != 255) return true;
        }
    }
    return false;
}

// Valores [0,1] de la mascara al tamano pedido (se redimensiona una copia si hace falta).
static float *mask_values(const PipelineOptions *o, u32 w, u32 h, Diag *diag) {
    Image copy;
    if (image_clone(o->mask, &copy, diag) != ST_OK) return NULL;
    if ((copy.width != w || copy.height != h) && resize_image(&copy, w, h, RF_BILINEAR, false, diag) != ST_OK) {
        image_free(&copy);
        return NULL;
    }
    MaskChannelChoice ch = o->mask_channel;
    if (ch == MASKCH_AUTO) ch = image_has_alpha_variation(&copy) ? MASKCH_ALPHA : MASKCH_LUMA;
    float *m = mem_alloc((size_t)w * h * sizeof(float));
    if (!m) { image_free(&copy); diag_fail(diag, ST_NO_MEMORY, "sin memoria para la mascara"); return NULL; }
    for (u32 y = 0; y < h; y++) {
        const u8 *row = image_row(&copy, y);
        for (u32 x = 0; x < w; x++) {
            float px[4];
            if (copy.depth == 8) for (int c = 0; c < 4; c++) px[c] = row[4 * (size_t)x + c] / 255.0f;
            else for (int c = 0; c < 4; c++) px[c] = ((const u16 *)row)[4 * (size_t)x + c] / 65535.0f;
            float v = ch == MASKCH_ALPHA ? px[3] : ch == MASKCH_R ? px[0] : ch == MASKCH_G ? px[1] : ch == MASKCH_B ? px[2]
                    : 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
            m[(size_t)y * w + x] = o->mask_invert ? 1.0f - v : v;
        }
    }
    image_free(&copy);
    return m;
}

// ---- utilidades ----------------------------------------------------------------------------------

static bool any_alpha(const Decoded *doc) {
    EACH_FRAME((Decoded *)doc, f) {
        const Image *im = &f->image;
        for (u32 y = 0; y < im->height; y++) {
            const u8 *row = image_row(im, y);
            if (im->depth == 8) { for (u32 x = 0; x < im->width; x++) if (row[4 * (size_t)x + 3] != 255) return true; }
            else { const u16 *p = (const u16 *)row; for (u32 x = 0; x < im->width; x++) if (p[4 * (size_t)x + 3] != 65535) return true; }
        }
    }
    return false;
}

static Status alpha_to_gray(Image *im) {
    for (u32 y = 0; y < im->height; y++) {
        u8 *row = image_row(im, y);
        if (im->depth == 8) for (u32 x = 0; x < im->width; x++) { u8 *p = row + 4 * (size_t)x; p[0] = p[1] = p[2] = p[3]; p[3] = 255; }
        else for (u32 x = 0; x < im->width; x++) { u16 *p = (u16 *)row + 4 * (size_t)x; p[0] = p[1] = p[2] = p[3]; p[3] = 65535; }
    }
    return ST_OK;
}

static void rgba16_of(const Rgba *c, u16 out[4]) {
    out[0] = (u16)(saturatef(c->r) * 65535.0f + 0.5f);
    out[1] = (u16)(saturatef(c->g) * 65535.0f + 0.5f);
    out[2] = (u16)(saturatef(c->b) * 65535.0f + 0.5f);
    out[3] = (u16)(saturatef(c->a) * 65535.0f + 0.5f);
}

// ---- CICP -> sRGB ------------------------------------------------------------------------------------

static Status cicp_frames_to_srgb(Decoded *doc, PipelineReport *rep, Diag *diag) {
    const u8 *code = doc->meta.cicp;
    Cicp c = { code[0], code[1], code[2], code[3] != 0 };
    const char *name = cicp_primaries_name(c.primaries);
    float gamut[9];
    if (cicp_is_hdr(&c) || !cicp_transfer_invertible(c.transfer) || !cicp_gamut_to_srgb(c.primaries, gamut)) {
        diag_warn(diag, "--icc srgb: CICP %u/%u (%s) no se convierte (HDR o codigo desconocido): se conserva el cICP",
                  c.primaries, c.transfer, cicp_is_hdr(&c) ? "HDR" : name ? name : "?");
        return ST_OK;
    }
    float *table = mem_alloc((TRANSFER_LUT_SIZE + 1) * sizeof(float));
    if (!table) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
    for (u32 i = 0; i <= TRANSFER_LUT_SIZE; i++) table[i] = cicp_to_linear(c.transfer, (float)i / TRANSFER_LUT_SIZE);
    Kernel k = kernel_gamut_to_srgb(gamut, table);
    EACH_FRAME(doc, f) pixelops_run(&f->image, &k, 1);
    mem_free(table);
    doc->meta.has_cicp = false;
    doc->meta.has_srgb = true;
    doc->meta.srgb_intent = 0;
    rep->converted_srgb = true;
    note(rep, "CICP %s → sRGB", name ? name : "?");
    return ST_OK;
}

// ---- pipeline ----------------------------------------------------------------------------------------

Status pipeline_run(Decoded *doc, const PipelineOptions *o, PipelineReport *rep, Diag *diag) {
    memset(rep, 0, sizeof *rep);
    rep->oriented = ORIENT_NONE;
    if (!decoded_frame_count(doc)) return diag_fail(diag, ST_CORRUPT, "no hay imagen para procesar");
    Status s = ST_OK;
    char color_text[16];

    // 0) Perfil de color -> sRGB (los formatos propios; WIC ya convirtio y descarto el perfil).
    if (o->to_srgb && doc->meta.icc.size && !icc_is_srgb(doc->meta.icc.data, doc->meta.icc.size)) {
        EACH_FRAME(doc, f) {
            s = wic_convert_to_srgb(&f->image, doc->meta.icc.data, doc->meta.icc.size, diag);
            if (s != ST_OK) return s;
        }
        blob_free(&doc->meta.icc);
        doc->meta.has_srgb = true;
        doc->meta.srgb_intent = 0;
        rep->converted_srgb = true;
        note(rep, "perfil ICC → sRGB");
    }

    // 0b) CICP (AVIF, HEIC, PNG con cICP) -> sRGB: decodificar la curva, matriz de primarios en luz
    //     lineal (Bradford si el blanco no es D65) y codificar sRGB. HDR (PQ/HLG) necesitaria mapeo
    //     de tonos con la luminancia de referencia: queda afuera, con aviso.
    if (o->to_srgb && doc->meta.has_cicp) {
        s = cicp_frames_to_srgb(doc, rep, diag);
        if (s != ST_OK) return s;
    }

    // 1) Orientacion EXIF.
    if (o->auto_orient && doc->meta.orientation > 1 && doc->meta.orientation <= 8) {
        EACH_FRAME(doc, f) { s = geometry_orient(&f->image, (Orientation)doc->meta.orientation, diag); if (s != ST_OK) return s; }
        rep->oriented = (Orientation)doc->meta.orientation;
        note(rep, "orientación EXIF %u aplicada", doc->meta.orientation);
    }

    // 2) Recorte.
    if (o->crop) {
        EACH_FRAME(doc, f) { s = geometry_crop(&f->image, o->crop_x, o->crop_y, o->crop_w, o->crop_h, diag); if (s != ST_OK) return s; }
        note(rep, "recorte %ux%u", decoded_frame(doc, 0)->image.width, decoded_frame(doc, 0)->image.height);
    }

    // 3-5) Fuente de alfa, reparaciones y fondo por color: una sola pasada fusionada.
    Kernel k[8];
    u32 nk = 0;
    float *mask = NULL;
    switch (o->alpha_from) {
    case AFROM_LUMA:     k[nk++] = kernel_simple(K_ALPHA_FROM_LUMA, 0, 0, 0); note(rep, "alfa desde luminancia"); break;
    case AFROM_INV_LUMA: k[nk++] = kernel_simple(K_ALPHA_FROM_LUMA, 0, 0, 0); k[nk++] = kernel_simple(K_ALPHA_INVERT_SOURCE, 0, 0, 0);
                         note(rep, "alfa desde luminancia invertida"); break;
    case AFROM_R: case AFROM_G: case AFROM_B:
        k[nk++] = kernel_simple(K_ALPHA_FROM_CHANNEL, (float)(o->alpha_from - AFROM_R), 0, 0);
        note(rep, "alfa desde el canal %c", "RGB"[o->alpha_from - AFROM_R]);
        break;
    default: break;
    }
    if (o->mask) {
        const Image *first = &decoded_frame(doc, 0)->image;
        mask = mask_values(o, first->width, first->height, diag);
        if (!mask) return diag->status ? diag->status : diag_fail(diag, ST_NO_MEMORY, "sin memoria para la mascara");
        Kernel km = { K_MASK, { 0 }, mask };
        k[nk++] = km;
        note(rep, "máscara externa%s", o->mask_invert ? " (invertida)" : "");
    }
    if (o->unpremultiply) { k[nk++] = kernel_simple(K_UNPREMULTIPLY, 0, 0, 0); note(rep, "des-premultiplicada"); }
    if (o->unmatte) {
        k[nk++] = kernel_unmatte(o->unmatte_color);
        color_format(&o->unmatte_color, color_text, sizeof color_text);
        note(rep, "mate %s removido", color_text);
    }
    if (o->key) {
        float fuzz = o->fuzz >= 0.0f ? o->fuzz : 0.0f;
        k[nk++] = kernel_key(o->key_color, fuzz, o->feather);
        color_format(&o->key_color, color_text, sizeof color_text);
        note(rep, "clave %s ±%.0f %%", color_text, fuzz * 100.0f);
    }
    if (o->color_to_alpha) {
        k[nk++] = kernel_color_to_alpha(o->c2a_color, o->c2a_transparency, o->c2a_opacity);
        color_format(&o->c2a_color, color_text, sizeof color_text);
        note(rep, "color→alfa %s", color_text);
    }
    run_kernels(doc, k, nk);
    mem_free(mask);

    // 5b) Fondo conectado a los bordes (el color auto se detecta en el cuadro 0 y se reusa).
    if (o->remove_bg) {
        BgRemoveOptions bo = { o->remove_bg_auto, o->remove_bg_color, o->fuzz >= 0.0f ? o->fuzz : DEFAULT_BG_FUZZ, o->remove_bg_edge };
        u64 removed = 0, softened = 0;
        bool first = true;
        EACH_FRAME(doc, f) {
            BgRemoveReport br;
            s = background_remove(&f->image, &bo, &br, diag);
            if (s != ST_OK) return s;
            if (first) { rep->bg = br; bo.auto_color = false; bo.color = br.color; first = false; }
            removed += br.removed;
            softened += br.softened;
        }
        rep->removed_bg = true;
        rep->bg.removed = removed;
        rep->bg.softened = softened;
        char n1[32], n2[32];
        color_format(&rep->bg.color, color_text, sizeof color_text);
        fmt_count(n1, sizeof n1, removed);
        fmt_count(n2, sizeof n2, softened);
        note(rep, "fondo %s quitado (%s px, halo %s px)", color_text, n1, n2);
    }

    // 6) Ajustes del alfa.
    nk = 0;
    if (o->threshold >= 0) { k[nk++] = kernel_simple(K_THRESHOLD, o->threshold / 255.0f, 0, 0); note(rep, "umbral de alfa %d", o->threshold); }
    if (o->levels) { k[nk++] = kernel_simple(K_LEVELS, o->levels_lo, o->levels_hi, o->levels_gamma); note(rep, "niveles de alfa"); }
    if (o->opacity != 1.0f) { k[nk++] = kernel_simple(K_OPACITY, o->opacity, 0, 0); note(rep, "opacidad %.0f %%", o->opacity * 100.0f); }
    if (o->invert_alpha) { k[nk++] = kernel_simple(K_INVERT_ALPHA, 0, 0, 0); note(rep, "alfa invertido"); }
    run_kernels(doc, k, nk);

    // 7) Defringe.
    if (o->defringe) {
        u64 total = 0;
        EACH_FRAME(doc, f) {
            u64 touched;
            s = propagate_colors(&f->image, PROPAGATE_DEFRINGE, o->defringe, &touched, diag);
            if (s != ST_OK) return s;
            total += touched;
        }
        rep->defringed = total;
        char n[32];
        fmt_count(n, sizeof n, total);
        note(rep, "defringe %u px (%s px)", o->defringe, n);
    }

    // 8) Trim con la caja union de todos los cuadros.
    if (o->trim) {
        u32 bx0 = UINT32_MAX, by0 = UINT32_MAX, bx1 = 0, by1 = 0;
        bool found = false;
        EACH_FRAME(doc, f) {
            u32 x0, y0, x1, y1;
            if (!geometry_content_box(&f->image, o->trim_threshold, &x0, &y0, &x1, &y1)) continue;
            found = true;
            bx0 = MIN(bx0, x0); by0 = MIN(by0, y0); bx1 = MAX(bx1, x1); by1 = MAX(by1, y1);
        }
        if (!found) {
            diag_warn(diag, "--trim: la imagen es completamente transparente, no se recorta");
        } else {
            u16 clear[4] = { 0, 0, 0, 0 };
            EACH_FRAME(doc, f) {
                s = geometry_crop(&f->image, bx0, by0, bx1 - bx0, by1 - by0, diag);
                if (s == ST_OK && o->trim_pad) s = geometry_pad(&f->image, o->trim_pad, o->trim_pad, o->trim_pad, o->trim_pad, clear, diag);
                if (s != ST_OK) return s;
            }
            rep->trimmed = true;
            rep->trim_x = bx0; rep->trim_y = by0; rep->trim_w = bx1 - bx0; rep->trim_h = by1 - by0;
            note(rep, "trim %ux%u+%u+%u%s", rep->trim_w, rep->trim_h, bx0, by0, o->trim_pad ? " + margen" : "");
        }
    }

    // 9) Rotacion y espejos compuestos en una transformacion.
    Orientation turn = geometry_compose(o->rotate, o->flip_h, o->flip_v);
    if (turn != ORIENT_NONE) {
        EACH_FRAME(doc, f) { s = geometry_orient(&f->image, turn, diag); if (s != ST_OK) return s; }
        note(rep, "%s%s%s", o->rotate ? "rotada" : "", o->rotate && (o->flip_h || o->flip_v) ? " + " : "", (o->flip_h || o->flip_v) ? "espejada" : "");
    }

    // 10) Redimensionar.
    {
        const Image *first = &decoded_frame(doc, 0)->image;
        u32 tw, th, fw = first->width, fh = first->height;
        if (resize_target(&o->resize, fw, fh, &tw, &th)) {
            EACH_FRAME(doc, f) { s = resize_image(&f->image, tw, th, o->filter, o->linear, diag); if (s != ST_OK) return s; }
            rep->resized = true;
            rep->from_w = fw;
            rep->from_h = fh;
            note(rep, "%ux%u → %ux%u (%s%s)", fw, fh, tw, th, resize_filter_name(o->filter), o->linear ? ", lineal" : "");
        }
    }

    // 11) Margen.
    if (o->pad) {
        u16 color[4];
        rgba16_of(&o->pad_color, color);
        EACH_FRAME(doc, f) {
            s = geometry_pad(&f->image, o->pad_px[0], o->pad_px[1], o->pad_px[2], o->pad_px[3], color, diag);
            if (s != ST_OK) return s;
        }
        note(rep, "margen");
    }

    // 12) Salida del alfa.
    nk = 0;
    if (o->extract_alpha) {
        EACH_FRAME(doc, f) alpha_to_gray(&f->image);
        note(rep, "canal alfa extraído como gris");
    } else if (o->alpha == ALPHA_REMOVE || (o->flatten_if_alpha && any_alpha(doc))) {
        Rgba bg = o->background_set ? o->background : WHITE;
        if (!o->background_set && doc->meta.has_background)
            bg = (Rgba){ doc->meta.background[0] / 65535.0f, doc->meta.background[1] / 65535.0f, doc->meta.background[2] / 65535.0f, 1.0f };
        k[nk++] = kernel_flatten(bg, o->linear);
        rep->flattened = true;
        rep->flattened_on = bg;
        color_format(&bg, color_text, sizeof color_text);
        note(rep, "aplanada sobre %s%s", color_text, !o->background_set && doc->meta.has_background ? " (bKGD)" : "");
        if (o->flatten_if_alpha && o->alpha != ALPHA_REMOVE)
            diag_warn(diag, "el formato pedido no tiene alfa: se compuso sobre %s (-b para otro fondo)", color_text);
    }

    // 13) Color bajo el alfa 0.
    if (!o->extract_alpha && !rep->flattened) {
        if (o->clean == CLEAN_BLEED) {
            u64 total = 0;
            EACH_FRAME(doc, f) {
                u64 touched;
                s = propagate_colors(&f->image, PROPAGATE_BLEED, o->bleed_px ? o->bleed_px : PROPAGATE_UNLIMITED, &touched, diag);
                if (s != ST_OK) return s;
                total += touched;
            }
            rep->bled = total;
            char n[32];
            fmt_count(n, sizeof n, total);
            note(rep, "sangrado de color (%s px)", n);
        } else if (o->clean == CLEAN_BLACK) {
            k[nk++] = kernel_simple(K_CLEAN_BLACK, 0, 0, 0);
            note(rep, "RGB bajo alfa 0 limpio");
        }
    }

    // 14) Gris y premultiplicado.
    if (o->grayscale) { k[nk++] = kernel_simple(K_GRAYSCALE, 0, 0, 0); note(rep, "escala de grises"); }
    if (o->premultiply) { k[nk++] = kernel_simple(K_PREMULTIPLY, 0, 0, 0); note(rep, "premultiplicada"); }
    run_kernels(doc, k, nk);

    // 15) Paleta con perdida.
    if (o->colors) {
        u32 n = decoded_frame_count(doc);
        Image **frames = mem_alloc(n * sizeof *frames);
        if (!frames) return diag_fail(diag, ST_NO_MEMORY, "sin memoria");
        for (u32 i = 0; i < n; i++) frames[i] = &decoded_frame(doc, i)->image;
        QuantizeOptions qo = { o->colors, o->dither, o->dither_level };
        s = quantize_frames(frames, n, &qo, &rep->quant, diag);
        mem_free(frames);
        if (s != ST_OK) return s;
        rep->quantized = true;
        char psnr[24];
        fmt_decimal(psnr, sizeof psnr, rep->quant.psnr, 1);
        if (rep->quant.lossless) note(rep, "ya tenía %u colores (sin pérdida)", rep->quant.palette);
        else note(rep, "paleta de %u colores (PSNR %s dB)", rep->quant.palette, psnr);
    }
    return ST_OK;
}
