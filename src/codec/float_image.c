// codec/float_image.c - ver float_image.h
#include <math.h>
#include "float_image.h"
#include "../core/pool.h"

typedef struct ToneJob {
    const float *rgba;
    Image       *out;
    ToneOperator op;
    float        scale;
} ToneJob;

static void tone_rows(void *arg, size_t y0, size_t y1) {
    const ToneJob *j = arg;
    for (size_t y = y0; y < y1; y++)
        tonemap_row_rgba16(j->rgba + y * j->out->width * 4, j->out->width, j->op, j->scale, (u16 *)image_row(j->out, (u32)y));
}

Status float_image_to_rgba16(const float *rgba, u32 width, u32 height, const DecodeOptions *opt,
                             Image *out, ToneOperator *used, Diag *diag) {
    colorspace_init();
    Status s = image_alloc(out, width, height, 16, false, opt->max_pixels, diag);
    if (s != ST_OK) return s;
    float scale = exp2f(opt->exposure);
    *used = tonemap_resolve(opt->tonemap, rgba, (size_t)width * height, scale);
    ToneJob job = { rgba, out, *used, scale };
    parallel_range(height, 64, tone_rows, &job);
    return ST_OK;
}

const char *tone_name(ToneOperator op) {
    switch (op) {
    case TONE_ACES:     return "ACES";
    case TONE_REINHARD: return "Reinhard";
    case TONE_CLIP:     return "recorte";
    default:            return "auto";
    }
}

void float_image_unpremultiply(float *rgba, size_t pixels) {
    for (size_t i = 0; i < pixels; i++, rgba += 4) {
        float a = rgba[3];
        if (a >= 1.0f) continue;
        float k = a > 0.0f ? 1.0f / a : 0.0f;               // alfa 0: color 0 (no hay nada que ver)
        rgba[0] *= k; rgba[1] *= k; rgba[2] *= k;
    }
}
