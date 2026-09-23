// png/plan.h - con que tipo de color y profundidad se escribe el PNG.
//
// Reglas (todas sin perdida, salvo lo que se pida explicito):
//   sin alfa real          -> sin canal alfa
//   alfa solo 0/maximo     -> color clave en tRNS (gris o RGB) si hay un color libre, o paleta
//   <= 256 colores RGBA    -> paleta 1/2/4/8 bits con el alfa en tRNS (entradas translucidas
//                             primero: el tRNS queda del largo justo)
//   gris puro              -> gris 1/2/4/8/16 bits (la minima que sea exacta)
//   16 bits "falsos"       -> 8 bits (cuando cada muestra es v*257)
// Entre varias opciones validas se estima el tamano crudo y se elige la menor; los niveles de
// optimizacion altos prueban de verdad las dos mejores (ver encode.c).
#pragma once
#include "analyze.h"

typedef enum ColorChoice {
    COLOR_AUTO = 0, COLOR_RGBA, COLOR_RGB, COLOR_GRAY, COLOR_GRAY_ALPHA, COLOR_PALETTE,
} ColorChoice;

enum { PNG_CT_GRAY = 0, PNG_CT_RGB = 2, PNG_CT_PALETTE = 3, PNG_CT_GRAY_ALPHA = 4, PNG_CT_RGBA = 6 };

typedef struct PngPlan {
    u8   color_type;
    u8   bit_depth;
    u32  palette_size;
    u32  palette[MAX_PALETTE];     // RGBA8 empaquetado (r | g<<8 | b<<16 | a<<24), orden final
    u32  trns_count;               // entradas de paleta con alfa < 255 (van primero)
    bool key;                      // tRNS con color clave (gris o RGB)
    u16  key_value[3];             // en la escala de la profundidad de salida
    bool clean_to_key;             // los pixeles con alfa 0 se escriben como el color clave
    bool source16;                 // la imagen es de 16 bits
    bool write8;                   // se escriben muestras de 8 bits (o menos)
} PngPlan;

typedef struct PlanRequest {
    ColorChoice color;
    u32         depth;             // 0 = auto, 8 o 16
    bool        reduce;            // reducciones sin perdida (default si)
    bool        may_clean;         // se puede reescribir el RGB invisible (alfa 0)
    bool        for_animation;     // APNG: hace falta una entrada transparente libre en la paleta
    bool        allow_gray;        // false si un perfil ICC RGB (no sRGB) obliga a quedarse en color
    u32         alternatives;      // cuantos planes devolver como maximo (>= 1)
    u32        *alternatives_found;
} PlanRequest;

// Elige el plan (o los `alternatives` mejores, de mejor a peor, en out[0..n)). `images` son
// todas las imagenes que se van a escribir: sirven para verificar que un color clave este libre.
Status plan_choose(const ColorStats *stats, const Image *const *images, u32 image_count,
                   const PlanRequest *req, PngPlan *out, Diag *diag);

// Bytes por fila empaquetada y bytes por pixel para los filtros.
size_t plan_row_bytes(const PngPlan *plan, u32 width);
u32    plan_filter_bpp(const PngPlan *plan);
u32    plan_channels(const PngPlan *plan);
const char *plan_describe(const PngPlan *plan, char *buf, size_t cap);   // "paleta 4 bits · tRNS"
