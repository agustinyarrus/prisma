// codec/compose.c - ver compose.h
#include "compose.h"

// OVER con alfa directo: a = as + ad(1 - as); c = (cs*as + cd*ad*(1 - as)) / a.
// En enteros escalando los pesos por 255*255 para no perder precision.
void compose_over_rgba8(u8 *dst, const u8 *src, size_t pixels) {
    for (size_t i = 0; i < pixels; i++, dst += 4, src += 4) {
        u32 sa = src[3];
        if (sa == 255) { memcpy(dst, src, 4); continue; }
        if (sa == 0) continue;
        u32 da = dst[3];
        u32 sw = sa * 255, dw = da * (255 - sa), ow = sw + dw;   // ow = a * 255^2
        for (int c = 0; c < 3; c++) dst[c] = (u8)((src[c] * sw + dst[c] * dw + ow / 2) / ow);
        dst[3] = (u8)((ow + 127) / 255);
    }
}

static void compose_over_rgba16(u16 *dst, const u16 *src, size_t pixels) {
    for (size_t i = 0; i < pixels; i++, dst += 4, src += 4) {
        if (src[3] == 65535) { memcpy(dst, src, 8); continue; }
        if (src[3] == 0) continue;
        double sa = src[3] / 65535.0, da = dst[3] / 65535.0;
        double dw = da * (1.0 - sa), oa = sa + dw;
        for (int c = 0; c < 3; c++) dst[c] = (u16)((src[c] * sa + dst[c] * dw) / oa + 0.5);
        dst[3] = (u16)(oa * 65535.0 + 0.5);
    }
}

// Interseccion del rectangulo con el lienzo; false si queda vacio.
static bool clip(const Image *canvas, u32 x, u32 y, u32 *w, u32 *h) {
    if (x >= canvas->width || y >= canvas->height) return false;
    *w = MIN(*w, canvas->width - x);
    *h = MIN(*h, canvas->height - y);
    return *w && *h;
}

void compose_draw(Image *canvas, const Image *src, u32 x, u32 y, bool over) {
    u32 w = src->width, h = src->height;
    if (!clip(canvas, x, y, &w, &h)) return;
    u32 bpp = image_bytes_per_pixel(canvas);
    for (u32 r = 0; r < h; r++) {
        u8 *d = image_row(canvas, y + r) + (size_t)x * bpp;
        const u8 *s = image_row(src, r);
        if (!over) memcpy(d, s, (size_t)w * bpp);
        else if (canvas->depth == 8) compose_over_rgba8(d, s, w);
        else compose_over_rgba16((u16 *)d, (const u16 *)s, w);
    }
}

void compose_clear(Image *canvas, u32 x, u32 y, u32 w, u32 h) {
    if (!clip(canvas, x, y, &w, &h)) return;
    u32 bpp = image_bytes_per_pixel(canvas);
    for (u32 r = 0; r < h; r++) memset(image_row(canvas, y + r) + (size_t)x * bpp, 0, (size_t)w * bpp);
}

Status compose_save(const Image *canvas, u32 x, u32 y, u32 w, u32 h, Image *saved, Diag *diag) {
    if (!clip(canvas, x, y, &w, &h)) { memset(saved, 0, sizeof *saved); return ST_OK; }
    Status s = image_alloc(saved, w, h, canvas->depth, false, 0, diag);
    if (s != ST_OK) return s;
    u32 bpp = image_bytes_per_pixel(canvas);
    for (u32 r = 0; r < h; r++)
        memcpy(image_row(saved, r), image_row(canvas, y + r) + (size_t)x * bpp, (size_t)w * bpp);
    return ST_OK;
}

void compose_restore(Image *canvas, const Image *saved, u32 x, u32 y) {
    if (saved->pixels) compose_draw(canvas, saved, x, y, false);
}
