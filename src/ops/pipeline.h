// ops/pipeline.h - todas las transformaciones entre decodificar y codificar, en un orden fijo.
//
//   0  --icc srgb            perfil ICC embebido (formatos propios; WIC ya lo hace) o codigos
//                            CICP (cICP de PNG, AVIF/HEIC): primarios en luz lineal a sRGB
//   1  orientacion EXIF      (salvo --no-auto-orient)
//   2  --crop
//   3  fuente de alfa        --alpha-from / --mask
//   4  reparaciones          --unpremultiply, --unmatte
//   5  sacar fondo           --key, --color-to-alpha, --remove-bg
//   6  ajustes de alfa       --threshold, --alpha-levels, --opacity, --invert-alpha
//   7  --defringe
//   8  --trim                (caja UNION de todos los cuadros: la animacion no se desalinea)
//   9  --rotate / --flip     (compuestos en UNA transformacion)
//  10  --resize              (premultiplicado; --linear en luz lineal)
//  11  --pad
//  12  salida del alfa       --extract-alpha | --flatten / --alpha remove
//  13  --bleed / --clean     (color bajo el alfa 0)
//  14  --grayscale / --premultiply
//  15  --colors N            (paleta con perdida, comun a todos los cuadros)
// Los pasos pixel a pixel contiguos se fusionan en una sola pasada (ver pixelops.h).
#pragma once
#include "../codec/codec.h"
#include "color.h"
#include "resize.h"
#include "geometry.h"
#include "background.h"
#include "quantize.h"

typedef enum AlphaChoice { ALPHA_KEEP = 0, ALPHA_REMOVE, ALPHA_FORCE } AlphaChoice;
typedef enum CleanMode { CLEAN_KEEP = 0, CLEAN_BLACK, CLEAN_BLEED } CleanMode;
typedef enum AlphaFrom { AFROM_NONE = 0, AFROM_LUMA, AFROM_INV_LUMA, AFROM_R, AFROM_G, AFROM_B } AlphaFrom;
typedef enum MaskChannelChoice { MASKCH_AUTO = 0, MASKCH_ALPHA, MASKCH_LUMA, MASKCH_R, MASKCH_G, MASKCH_B } MaskChannelChoice;

typedef struct PipelineOptions {
    bool              to_srgb;
    bool              auto_orient;
    bool              crop;             u32 crop_x, crop_y, crop_w, crop_h;
    const Image      *mask;             // ya decodificada (se carga una vez en el borde)
    MaskChannelChoice mask_channel;
    bool              mask_invert;
    AlphaFrom         alpha_from;
    bool              unpremultiply;
    bool              unmatte;          Rgba unmatte_color;
    bool              key;              Rgba key_color;
    float             fuzz, feather;
    bool              color_to_alpha;   Rgba c2a_color; float c2a_transparency, c2a_opacity;
    bool              remove_bg;        bool remove_bg_auto; Rgba remove_bg_color; u32 remove_bg_edge;
    int               threshold;        // -1 = no
    bool              levels;           float levels_lo, levels_hi, levels_gamma;
    float             opacity;          // 1 = sin cambio
    bool              invert_alpha;
    u32               defringe;
    bool              trim;             u32 trim_pad; u32 trim_threshold;
    int               rotate;           bool flip_h, flip_v;
    ResizeSpec        resize;           ResizeFilter filter; bool linear;
    bool              pad;              u32 pad_px[4]; Rgba pad_color;
    AlphaChoice       alpha;
    bool              background_set;   Rgba background;
    bool              flatten_if_alpha; // --color rgb|gray: aplanar solo si hace falta
    bool              extract_alpha;
    CleanMode         clean;
    u32               bleed_px;         // 0 = sin limite (con CLEAN_BLEED)
    bool              grayscale;
    bool              premultiply;
    u32               colors;           DitherMode dither; float dither_level;
} PipelineOptions;

typedef struct PipelineReport {
    Orientation    oriented;            // ORIENT_NONE si no se roto
    bool           converted_srgb;
    bool           removed_bg;          BgRemoveReport bg;
    u64            defringed, bled;
    bool           trimmed;             u32 trim_x, trim_y, trim_w, trim_h;
    bool           resized;             u32 from_w, from_h;
    bool           flattened;           Rgba flattened_on;
    bool           quantized;           QuantizeReport quant;
    u32            steps;               // operaciones aplicadas
    char           summary[256];        // "orientacion 6 · sin fondo (blanco) · trim · 800x600"
} PipelineReport;

void   pipeline_defaults(PipelineOptions *opt);
Status pipeline_run(Decoded *doc, const PipelineOptions *opt, PipelineReport *rep, Diag *diag);
