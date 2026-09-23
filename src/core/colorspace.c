// core/colorspace.c - ver colorspace.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include "colorspace.h"

enum { ENCODE_LUT_SIZE = 4096 };

static float g_decode8[256];
static float g_encode[ENCODE_LUT_SIZE + 1];
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

float srgb_to_linear(float v) {
    return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float v) {
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static BOOL CALLBACK build_tables(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    UNUSED(once); UNUSED(param); UNUSED(ctx);
    for (int i = 0; i < 256; i++) g_decode8[i] = srgb_to_linear(i / 255.0f);
    for (int i = 0; i <= ENCODE_LUT_SIZE; i++) g_encode[i] = linear_to_srgb((float)i / ENCODE_LUT_SIZE);
    return TRUE;
}

void colorspace_init(void) { InitOnceExecuteOnce(&g_once, build_tables, NULL, NULL); }

float srgb8_to_linear(u8 v) { return g_decode8[v]; }

float linear_to_srgb_fast(float v) {
    if (!(v > 0.0f)) return 0.0f;             // tambien atrapa NaN
    if (v >= 1.0f) return 1.0f;
    // La curva es muy empinada cerca de 0 (pendiente 12,92): ahi la tabla perderia precision,
    // asi que el tramo lineal se calcula directo.
    if (v <= 0.0031308f) return v * 12.92f;
    float pos = v * ENCODE_LUT_SIZE;
    int i = (int)pos;
    float t = pos - (float)i;
    return g_encode[i] + (g_encode[i + 1] - g_encode[i]) * t;
}

// ACES fitted (aproximacion de Narkowicz): curva filmica con hombro suave, satura menos que clip
// y conserva mejor el contraste que Reinhard.
static float aces(float x) {
    x *= 0.6f;
    return saturatef((x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f));
}

void tonemap_rgb(float rgb[3], ToneOperator op, float exposure_scale) {
    float r = fmaxf(rgb[0] * exposure_scale, 0.0f);
    float g = fmaxf(rgb[1] * exposure_scale, 0.0f);
    float b = fmaxf(rgb[2] * exposure_scale, 0.0f);
    switch (op) {
    case TONE_ACES:
        r = aces(r); g = aces(g); b = aces(b);
        break;
    case TONE_REINHARD: {
        // Sobre la luminancia, para no desaturar los brillos (Reinhard por canal los lava).
        float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        float scale = lum > 0.0f ? (1.0f / (1.0f + lum)) : 1.0f;
        r = saturatef(r * scale); g = saturatef(g * scale); b = saturatef(b * scale);
        break;
    }
    case TONE_AUTO:
    case TONE_CLIP:
        r = saturatef(r); g = saturatef(g); b = saturatef(b);
        break;
    }
    rgb[0] = r; rgb[1] = g; rgb[2] = b;
}

ToneOperator tonemap_resolve(ToneOperator op, const float *rgba, size_t pixels, float exposure_scale) {
    if (op != TONE_AUTO) return op;
    // Tolerancia: codecs que guardan 1,0 como 1,0000001 no son HDR.
    const float threshold = 1.001f / exposure_scale;
    for (size_t i = 0; i < pixels; i++, rgba += 4)
        if (rgba[0] > threshold || rgba[1] > threshold || rgba[2] > threshold) return TONE_ACES;
    return TONE_CLIP;
}

void tonemap_row_rgba16(const float *rgba, u32 width, ToneOperator op, float exposure_scale, u16 *out) {
    for (u32 x = 0; x < width; x++, rgba += 4, out += 4) {
        float c[3] = { rgba[0], rgba[1], rgba[2] };
        if (!isfinite(c[0])) c[0] = 0.0f;
        if (!isfinite(c[1])) c[1] = 0.0f;
        if (!isfinite(c[2])) c[2] = 0.0f;
        tonemap_rgb(c, op, exposure_scale);
        for (int i = 0; i < 3; i++) out[i] = (u16)(linear_to_srgb_fast(c[i]) * 65535.0f + 0.5f);
        float a = isfinite(rgba[3]) ? saturatef(rgba[3]) : 1.0f;
        out[3] = (u16)(a * 65535.0f + 0.5f);
    }
}

float half_to_float(u16 h) {
    u32 sign = (u32)(h & 0x8000) << 16;
    u32 exp = (h >> 10) & 0x1F, mant = h & 0x3FF;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {                                          // subnormal: normalizar
            exp = 1;
            while (!(mant & 0x400)) { mant <<= 1; exp--; }
            mant &= 0x3FF;
            bits = sign | ((exp + 112) << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (mant << 13);       // inf / NaN
    } else {
        bits = sign | ((exp + 112) << 23) | (mant << 13);
    }
    return f32_from_bits(bits);
}
