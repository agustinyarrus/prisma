// tools/image2png/options.c - la tabla de opciones de image2png, sus acciones a medida y la ayuda.
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "config.h"
#include "../../app/ui.h"
#include "../../core/text.h"

#define OFF(field)  offsetof(Config, field)
#define FLAG(field) (offsetof(Config, field) + 1)

static const char G_IO[] = "Entradas y salidas";
static const char G_COMP[] = "Compresión";
static const char G_ALPHA[] = "Transparencia";
static const char G_BG[] = "Quitar fondos";
static const char G_GEOM[] = "Geometría";
static const char G_PAL[] = "Paleta con pérdida";
static const char G_META[] = "Color y metadatos";
static const char G_ANIM[] = "Animaciones, páginas e íconos";
static const char G_HDR[] = "HDR y decodificación";
static const char G_INFO[] = "Información y pantalla";

// ---- acciones a medida ---------------------------------------------------------------------------

#define CFG ((Config *)cfg)
#define ERR(...) do { if (err) snprintf(err, cap, __VA_ARGS__); return false; } while (0)

// Lista de numeros separados por coma: devuelve cuantos leyo (hasta max).
static int number_list(const char *v, double *out, int max) {
    if (!v) return 0;
    char tmp[128];
    size_t n = strlen(v);
    if (n >= sizeof tmp) return 0;
    memcpy(tmp, v, n + 1);
    int count = 0;
    for (char *tok = strtok(tmp, ",;"); tok && count < max; tok = strtok(NULL, ",;")) {
        if (tok[0] && tok[strlen(tok) - 1] == '%') tok[strlen(tok) - 1] = 0;
        if (!cli_parse_number(tok, &out[count])) return -1;
        count++;
    }
    return count;
}

static bool act_fast(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s); UNUSED(v); UNUSED(err); UNUSED(cap);
    if (cfg) CFG->encode.optimize = 0;
    return true;
}

static bool act_max(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s); UNUSED(v); UNUSED(err); UNUSED(cap);
    if (cfg) CFG->encode.optimize = OPTIMIZE_MAX;
    return true;
}

static bool act_depth(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    u32 d = v && strcmp(v, "auto") == 0 ? 0 : v && strcmp(v, "8") == 0 ? 8 : v && strcmp(v, "16") == 0 ? 16 : 1;
    if (d == 1) ERR("usa auto, 8 o 16");
    if (cfg) CFG->encode.depth = d;
    return true;
}

static bool act_color_type(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    OptionSpec local = *s;
    local.offset = OFF(encode.color);
    local.choices = "auto|rgba|rgb|gray|graya|palette";
    if (!cli_set_enum(&local, cfg, v, err, cap)) return false;
    if (cfg) {
        ColorChoice c = CFG->encode.color;
        // Pedir un tipo sin alfa compone sobre el fondo solo si hace falta; pedir gris convierte.
        CFG->pipeline.flatten_if_alpha = c == COLOR_RGB || c == COLOR_GRAY;
        CFG->pipeline.grayscale = c == COLOR_GRAY || c == COLOR_GRAY_ALPHA;
    }
    return true;
}

static bool act_alpha(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    OptionSpec local = *s;
    local.offset = OFF(pipeline.alpha);
    local.choices = "keep|remove|force";
    return cli_set_enum(&local, cfg, v, err, cap);
}

static bool act_flatten(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    Rgba c;
    if (v && !color_parse(v, &c, cfg ? err : NULL, cap)) return false;
    if (!cfg) return v != NULL;
    CFG->pipeline.alpha = ALPHA_REMOVE;
    if (v) { CFG->pipeline.background = c; CFG->pipeline.background_set = true; }
    return true;
}

static bool act_alpha_levels(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    double n[3];
    int k = number_list(v, n, 3);
    if (k < 2 || n[0] < 0 || n[1] > 255 || n[0] >= n[1]) ERR("usa LO,HI (0-255, LO < HI) y opcional ,GAMMA");
    if (k == 3 && (n[2] <= 0.0 || n[2] > 10.0)) ERR("la gamma tiene que estar entre 0 y 10");
    if (cfg) {
        CFG->pipeline.levels = true;
        CFG->pipeline.levels_lo = (float)(n[0] / 255.0);
        CFG->pipeline.levels_hi = (float)(n[1] / 255.0);
        CFG->pipeline.levels_gamma = k == 3 ? (float)n[2] : 1.0f;
    }
    return true;
}

static bool act_bleed(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    double px = 0;
    if (v && (!cli_parse_number(v, &px) || px < 1 || px > 100000 || px != (double)(u32)px)) {
        if (cfg) ERR("distancia en pixeles (1-100000) o nada para sangrar todo");
        return false;
    }
    if (!cfg) return v != NULL;
    CFG->pipeline.clean = CLEAN_BLEED;
    CFG->pipeline.bleed_px = (u32)px;
    return true;
}

static bool act_clean(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    CleanMode m = CLEAN_BLACK;
    if (v) {
        if (strcmp(v, "black") == 0) m = CLEAN_BLACK;
        else if (strcmp(v, "bleed") == 0) m = CLEAN_BLEED;
        else { if (cfg) ERR("usa black o bleed"); return false; }
    }
    if (!cfg) return v != NULL;
    CFG->pipeline.clean = m;
    return true;
}

static bool act_trim(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    double pad = 0;
    if (v && (!cli_parse_number(v, &pad) || pad < 0 || pad > 100000 || pad != (double)(u32)pad)) {
        if (cfg) ERR("margen en pixeles (0-100000)");
        return false;
    }
    if (!cfg) return v != NULL;
    CFG->pipeline.trim = true;
    CFG->pipeline.trim_pad = (u32)pad;
    return true;
}

static bool act_c2a_thresholds(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    double n[2];
    if (number_list(v, n, 2) != 2 || n[0] < 0 || n[1] > 100 || n[0] >= n[1]) ERR("usa T,O en %% con T < O (p.ej. 0,100 o 5,80)");
    if (cfg) { CFG->pipeline.c2a_transparency = (float)(n[0] / 100.0); CFG->pipeline.c2a_opacity = (float)(n[1] / 100.0); }
    return true;
}

static bool act_remove_bg(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    bool automatic = !v || strcmp(v, "auto") == 0;
    Rgba c = { 1, 1, 1, 1 };
    if (!automatic && !color_parse(v, &c, cfg ? err : NULL, cap)) return false;
    if (!cfg) return v != NULL;
    CFG->pipeline.remove_bg = true;
    CFG->pipeline.remove_bg_auto = automatic;
    CFG->pipeline.remove_bg_color = c;
    return true;
}

static bool act_resize(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    ResizeSpec spec;
    char local[160];
    if (!resize_spec_parse(v ? v : "", &spec, local, sizeof local)) ERR("%s", local);
    if (cfg) CFG->pipeline.resize = spec;
    return true;
}

static bool act_crop(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    double n[4];
    if (number_list(v, n, 4) != 4 || n[0] < 0 || n[1] < 0 || n[2] < 1 || n[3] < 1) ERR("usa X,Y,ANCHO,ALTO en pixeles");
    if (cfg) {
        PipelineOptions *p = &CFG->pipeline;
        p->crop = true;
        p->crop_x = (u32)n[0]; p->crop_y = (u32)n[1]; p->crop_w = (u32)n[2]; p->crop_h = (u32)n[3];
    }
    return true;
}

static bool act_rotate(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    double d;
    if (!cli_parse_number(v, &d) || fmod(d, 90.0) != 0.0) ERR("solo multiplos de 90 (90, 180, 270, -90)");
    if (cfg) CFG->pipeline.rotate = (((int)d % 360) + 360) % 360;
    return true;
}

static bool act_flip(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    bool h = v && strchr(v, 'h'), vv = v && strchr(v, 'v');
    if (!v || (!h && !vv) || strspn(v, "hv") != strlen(v)) ERR("usa h, v o hv");
    if (cfg) { CFG->pipeline.flip_h = h; CFG->pipeline.flip_v = vv; }
    return true;
}

static bool act_pad(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    double n[4];
    int k = number_list(v, n, 4);
    if (k != 1 && k != 4) ERR("usa N (igual en los 4 lados) o ARRIBA,DERECHA,ABAJO,IZQUIERDA");
    for (int i = 0; i < k; i++) if (n[i] < 0 || n[i] > 100000) ERR("margenes entre 0 y 100000 px");
    if (cfg) {
        PipelineOptions *p = &CFG->pipeline;
        p->pad = true;
        if (k == 1) p->pad_px[0] = p->pad_px[1] = p->pad_px[2] = p->pad_px[3] = (u32)n[0];
        else { p->pad_px[1] = (u32)n[0]; p->pad_px[2] = (u32)n[1]; p->pad_px[3] = (u32)n[2]; p->pad_px[0] = (u32)n[3]; }
    }
    return true;
}

static bool act_icc(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    OptionSpec local = *s;
    local.offset = OFF(icc);
    local.choices = "keep|strip|srgb";
    if (!cli_set_enum(&local, cfg, v, err, cap)) return false;
    if (cfg) CFG->decode.to_srgb = CFG->pipeline.to_srgb = CFG->icc == ICC_SRGB;
    return true;
}

static bool act_strip(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s); UNUSED(v); UNUSED(err); UNUSED(cap);
    if (cfg) { CFG->encode.meta = META_NONE; CFG->meta_set = true; }
    return true;
}

static bool act_keep_meta(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s); UNUSED(v); UNUSED(err); UNUSED(cap);
    if (cfg) { CFG->encode.meta = META_ALL; CFG->meta_set = true; }
    return true;
}

static bool act_text(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    const char *eq = v ? strchr(v, '=') : NULL;
    size_t klen = eq ? (size_t)(eq - v) : 0;
    if (!eq || klen < 1 || klen > 79) ERR("usa CLAVE=VALOR (clave de 1 a 79 caracteres)");
    if (cfg) {
        TextPair *t = vec_push(&CFG->texts);
        if (!t) ERR("sin memoria");
        t->keyword = mem_alloc(klen + 1);
        t->value = mem_strdup(eq + 1);
        if (!t->keyword || !t->value) ERR("sin memoria");
        memcpy(t->keyword, v, klen);
        t->keyword[klen] = 0;
    }
    return true;
}

static bool act_frame(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    double n;
    if (!cli_parse_number(v, &n) || n < 0 || n > 1e6 || n != (double)(u32)n) ERR("numero de cuadro/pagina desde 0");
    if (cfg) { CFG->decode.frame_mode = FRAMES_ONE; CFG->decode.frame_index = (u32)n; }
    return true;
}

static bool act_frames(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap) {
    UNUSED(s);
    if (!v || strcmp(v, "all") != 0) ERR("el unico valor es all");
    if (cfg) CFG->decode.frame_mode = FRAMES_ALL;
    return true;
}

// ---- la tabla ------------------------------------------------------------------------------------

#define ROW(name, letter, arg, meta, group, help, action, off, flag, min, max, choices) \
    { name, letter, arg, meta, group, help, action, off, flag, min, max, choices }

const OptionSpec IMAGE2PNG_OPTIONS[] = {
    ROW("output", 'o', ARG_REQUIRED, "RUTA", G_IO, "archivo de salida (una entrada), carpeta (varias) o - para stdout", cli_set_string, OFF(output), 0, 0, 0, NULL),
    ROW("recursive", 'r', ARG_NONE, NULL, G_IO, "incluye las subcarpetas de las carpetas dadas", cli_set_true, OFF(recursive), 0, 0, 0, NULL),
    ROW("suffix", 0, ARG_REQUIRED, "TEXTO", G_IO, "agrega TEXTO al nombre: foto.jpg -> foto<TEXTO>.png", cli_set_string, OFF(suffix), 0, 0, 0, NULL),
    ROW("keep-tree", 0, ARG_NONE, NULL, G_IO, "con -r y -o, replica la estructura de carpetas en la salida", cli_set_true, OFF(keep_tree), 0, 0, 0, NULL),
    ROW("in-place", 0, ARG_NONE, NULL, G_IO, "permite reescribir un .png sobre si mismo (reoptimizar)", cli_set_true, OFF(in_place), 0, 0, 0, NULL),
    ROW("no-clobber", 'n', ARG_NONE, NULL, G_IO, "no pisa salidas que ya existen (las omite)", cli_set_true, OFF(no_clobber), 0, 0, 0, NULL),
    ROW("dry-run", 0, ARG_NONE, NULL, G_IO, "muestra que haria, sin escribir nada", cli_set_true, OFF(dry_run), 0, 0, 0, NULL),
    ROW("preserve-time", 0, ARG_NONE, NULL, G_IO, "copia la fecha de modificacion de la entrada a la salida", cli_set_true, OFF(preserve_time), 0, 0, 0, NULL),
    ROW("threads", 'j', ARG_REQUIRED, "N", G_IO, "hilos de trabajo (0 = todos los nucleos logicos)", cli_set_u32, OFF(threads), 0, 0, 256, NULL),
    ROW("max-pixels", 0, ARG_REQUIRED, "MP", G_IO, "limite de megapixeles por imagen (default 2147)", cli_set_u32, OFF(max_megapixels), 0, 1, 100000, NULL),

    ROW("optimize", 'O', ARG_REQUIRED, "0-4", G_COMP, "0 rapido · 1 · 2 equilibrado: elige el filtro con una muestra (default) · 3 prueba filtros · 4 prueba todo", cli_set_int, OFF(encode.optimize), 0, 0, 4, NULL),
    ROW("fast", 0, ARG_NONE, NULL, G_COMP, "atajo de -O0", act_fast, 0, 0, 0, 0, NULL),
    ROW("max", 0, ARG_NONE, NULL, G_COMP, "atajo de -O4 (deflate 12, todas las estrategias, un solo flujo)", act_max, 0, 0, 0, 0, NULL),
    ROW("level", 'l', ARG_REQUIRED, "0-12", G_COMP, "nivel de deflate (pisa el de -O)", cli_set_int, OFF(encode.level), 0, 0, 12, NULL),
    ROW("filter", 0, ARG_REQUIRED, "F", G_COMP, "filtro de fila: auto none sub up avg paeth minsum entropy bigrams brute", cli_set_enum, OFF(encode.filter), 0, 0, 0,
        "none|sub|up|avg|paeth|minsum|entropy|bigrams|brute|auto"),
    ROW("interlace", 0, ARG_NONE, NULL, G_COMP, "entrelazado Adam7 (carga progresiva; archivo algo mas grande)", cli_set_true, OFF(encode.interlace), 0, 0, 0, NULL),
    ROW("depth", 0, ARG_REQUIRED, "auto|8|16", G_COMP, "bits por canal (auto: 16 solo si la imagen los usa de verdad)", act_depth, 0, 0, 0, 0, NULL),
    ROW("color", 0, ARG_REQUIRED, "TIPO", G_COMP, "tipo de color: auto rgba rgb gray graya palette", act_color_type, 0, 0, 0, 0, NULL),
    ROW("no-reduce", 0, ARG_NONE, NULL, G_COMP, "no reduce tipo de color ni profundidad (RGBA/RGB tal cual)", cli_set_false, OFF(encode.reduce), 0, 0, 0, NULL),
    ROW("single-stream", 0, ARG_NONE, NULL, G_COMP, "deflate en un solo flujo, sin tramos paralelos (~0,1 % mas chico)", cli_set_false, OFF(encode.parallel), 0, 0, 0, NULL),

    ROW("alpha", 0, ARG_REQUIRED, "MODO", G_ALPHA, "keep: la que haya · remove: aplanar sobre el fondo · force: canal alfa siempre", act_alpha, 0, 0, 0, 0, NULL),
    ROW("background", 'b', ARG_REQUIRED, "COLOR", G_ALPHA, "fondo para aplanar (default: bKGD del archivo, si no blanco)", cli_set_color, OFF(pipeline.background), FLAG(pipeline.background_set), 0, 0, NULL),
    ROW("flatten", 0, ARG_OPTIONAL, "[COLOR]", G_ALPHA, "quita el alfa componiendo sobre un color (= --alpha remove -b COLOR)", act_flatten, 0, 0, 0, 0, NULL),
    ROW("unpremultiply", 0, ARG_NONE, NULL, G_ALPHA, "des-premultiplica fuentes con alfa premultiplicado no declarado", cli_set_true, OFF(pipeline.unpremultiply), 0, 0, 0, NULL),
    ROW("premultiply", 0, ARG_NONE, NULL, G_ALPHA, "guarda el color premultiplicado (motores que lo esperan asi)", cli_set_true, OFF(pipeline.premultiply), 0, 0, 0, NULL),
    ROW("unmatte", 0, ARG_REQUIRED, "COLOR", G_ALPHA, "deshace un mate: recupera el color real de bordes compuestos sobre COLOR", cli_set_color, OFF(pipeline.unmatte_color), FLAG(pipeline.unmatte), 0, 0, NULL),
    ROW("threshold", 0, ARG_REQUIRED, "0-255", G_ALPHA, "alfa binario: >= umbral queda opaco, el resto transparente", cli_set_int, OFF(pipeline.threshold), 0, 0, 255, NULL),
    ROW("alpha-levels", 0, ARG_REQUIRED, "LO,HI[,G]", G_ALPHA, "reescala el alfa: LO pasa a 0 y HI a 255, con gamma opcional", act_alpha_levels, 0, 0, 0, 0, NULL),
    ROW("opacity", 0, ARG_REQUIRED, "PCT", G_ALPHA, "multiplica el alfa (50 = mitad de opacidad)", cli_set_percent, OFF(pipeline.opacity), 0, 0, 100, NULL),
    ROW("invert-alpha", 0, ARG_NONE, NULL, G_ALPHA, "invierte el alfa (lo transparente pasa a opaco y viceversa)", cli_set_true, OFF(pipeline.invert_alpha), 0, 0, 0, NULL),
    ROW("mask", 0, ARG_REQUIRED, "ARCHIVO", G_ALPHA, "usa otra imagen como mascara de alfa (se ajusta al tamaño)", cli_set_string, OFF(mask_path), 0, 0, 0, NULL),
    ROW("mask-channel", 0, ARG_REQUIRED, "C", G_ALPHA, "canal de la mascara: auto alpha luma r g b", cli_set_enum, OFF(pipeline.mask_channel), 0, 0, 0, "auto|alpha|luma|r|g|b"),
    ROW("mask-invert", 0, ARG_NONE, NULL, G_ALPHA, "invierte la mascara", cli_set_true, OFF(pipeline.mask_invert), 0, 0, 0, NULL),
    ROW("alpha-from", 0, ARG_REQUIRED, "C", G_ALPHA, "crea el alfa desde un canal propio: luma inverse-luma r g b", cli_set_enum, OFF(pipeline.alpha_from), 0, 0, 0, "none|luma|inverse-luma|r|g|b"),
    ROW("extract-alpha", 0, ARG_NONE, NULL, G_ALPHA, "guarda solo el canal alfa, como imagen en escala de grises", cli_set_true, OFF(pipeline.extract_alpha), 0, 0, 0, NULL),
    ROW("defringe", 0, ARG_REQUIRED, "PX", G_ALPHA, "reemplaza el color de los bordes translucidos por el del interior (quita ribetes)", cli_set_u32, OFF(pipeline.defringe), 0, 1, 64, NULL),
    ROW("bleed", 0, ARG_OPTIONAL, "[PX]", G_ALPHA, "extiende colores bajo el alfa 0 (texturas: sin halos con mipmaps)", act_bleed, 0, 0, 0, 0, NULL),
    ROW("clean", 0, ARG_OPTIONAL, "[black|bleed]", G_ALPHA, "normaliza el RGB invisible bajo el alfa 0 (comprime mejor)", act_clean, 0, 0, 0, 0, NULL),
    ROW("trim", 0, ARG_OPTIONAL, "[MARGEN]", G_ALPHA, "recorta al contenido no transparente (mas un margen opcional)", act_trim, 0, 0, 0, 0, NULL),
    ROW("trim-threshold", 0, ARG_REQUIRED, "0-254", G_ALPHA, "alfa minimo que --trim considera contenido", cli_set_u32, OFF(pipeline.trim_threshold), 0, 0, 254, NULL),
    ROW("bmp-alpha", 0, ARG_REQUIRED, "auto|on|off", G_ALPHA, "BMP de 32 bits: usar el 4.o byte como alfa", cli_set_enum, OFF(decode.bmp_alpha), 0, 0, 0, "auto|on|off"),
    ROW("tga-alpha", 0, ARG_REQUIRED, "auto|on|off", G_ALPHA, "TGA: usar el canal alfa aunque el pie diga otra cosa", cli_set_enum, OFF(decode.tga_alpha), 0, 0, 0, "auto|on|off"),
    ROW("psd-keep-matte", 0, ARG_NONE, NULL, G_ALPHA, "PSD: conserva el mate blanco del compuesto (no lo quita)", cli_set_true, OFF(decode.psd_keep_matte), 0, 0, 0, NULL),

    ROW("key", 0, ARG_REQUIRED, "COLOR", G_BG, "vuelve transparente un color en toda la imagen", cli_set_color, OFF(pipeline.key_color), FLAG(pipeline.key), 0, 0, NULL),
    ROW("fuzz", 0, ARG_REQUIRED, "PCT", G_BG, "tolerancia de --key y --remove-bg (distancia RGB; default 0 y 8)", cli_set_percent, OFF(pipeline.fuzz), 0, 0, 100, NULL),
    ROW("feather", 0, ARG_REQUIRED, "PCT", G_BG, "--key: borde suave, rampa de alfa despues de la tolerancia", cli_set_percent, OFF(pipeline.feather), 0, 0, 100, NULL),
    ROW("color-to-alpha", 0, ARG_REQUIRED, "COLOR", G_BG, "quita un color conservando antialias y sombras (algoritmo de GIMP)", cli_set_color, OFF(pipeline.c2a_color), FLAG(pipeline.color_to_alpha), 0, 0, NULL),
    ROW("c2a-thresholds", 0, ARG_REQUIRED, "T,O", G_BG, "--color-to-alpha: umbral de transparencia y de opacidad en %", act_c2a_thresholds, 0, 0, 0, 0, NULL),
    ROW("remove-bg", 0, ARG_OPTIONAL, "[COLOR]", G_BG, "borra el fondo conectado a los bordes (auto detecta el color) y limpia el halo", act_remove_bg, 0, 0, 0, 0, NULL),
    ROW("bg-edge", 0, ARG_REQUIRED, "PX", G_BG, "ancho de la banda que --remove-bg des-mezcla a cada lado del borde (0-16, default 2)", cli_set_u32, OFF(pipeline.remove_bg_edge), 0, 0, 16, NULL),

    ROW("resize", 0, ARG_REQUIRED, "TAM", G_GEOM, "WxH entra · WxH! exacto · WxH^ cubre · Wx · xH · N%; sufijo > solo achica", act_resize, 0, 0, 0, 0, NULL),
    ROW("resample", 0, ARG_REQUIRED, "F", G_GEOM, "filtro: lanczos3 lanczos2 mitchell catmull bilinear box nearest", cli_set_enum, OFF(pipeline.filter), 0, 0, 0,
        "lanczos3|lanczos2|mitchell|catmull|bilinear|box|nearest"),
    ROW("linear", 0, ARG_NONE, NULL, G_GEOM, "redimensiona y compone en luz lineal (bordes y degrades mas fieles)", cli_set_true, OFF(pipeline.linear), 0, 0, 0, NULL),
    ROW("crop", 0, ARG_REQUIRED, "X,Y,W,H", G_GEOM, "recorta un rectangulo (antes de cualquier otra operacion)", act_crop, 0, 0, 0, 0, NULL),
    ROW("rotate", 0, ARG_REQUIRED, "GRADOS", G_GEOM, "rota 90, 180 o 270 en sentido horario (-90 = antihorario)", act_rotate, 0, 0, 0, 0, NULL),
    ROW("flip", 0, ARG_REQUIRED, "h|v|hv", G_GEOM, "espeja en horizontal, vertical o ambos", act_flip, 0, 0, 0, 0, NULL),
    ROW("pad", 0, ARG_REQUIRED, "N|T,R,B,L", G_GEOM, "agrega margen (transparente salvo --pad-color)", act_pad, 0, 0, 0, 0, NULL),
    ROW("pad-color", 0, ARG_REQUIRED, "COLOR", G_GEOM, "color del margen de --pad", cli_set_color, OFF(pipeline.pad_color), 0, 0, 0, NULL),
    ROW("no-auto-orient", 0, ARG_NONE, NULL, G_GEOM, "no rota segun la orientacion EXIF de la foto", cli_set_false, OFF(pipeline.auto_orient), 0, 0, 0, NULL),

    ROW("colors", 0, ARG_REQUIRED, "2-256", G_PAL, "cuantiza a una paleta de N colores CON alfa suave (PNG8 translucido)", cli_set_u32, OFF(pipeline.colors), 0, 2, 256, NULL),
    ROW("dither", 0, ARG_REQUIRED, "MODO", G_PAL, "fs (Floyd-Steinberg, default) · ordered (Bayer 8x8) · none", cli_set_enum, OFF(pipeline.dither), 0, 0, 0, "fs|none|ordered"),
    ROW("dither-level", 0, ARG_REQUIRED, "PCT", G_PAL, "intensidad del dithering (0-100, default 100)", cli_set_percent, OFF(pipeline.dither_level), 0, 0, 100, NULL),

    ROW("icc", 0, ARG_REQUIRED, "MODO", G_META, "keep: conserva el perfil o el CICP · strip: los descarta · srgb: convierte los pixeles a sRGB", act_icc, 0, 0, 0, 0, NULL),
    ROW("meta", 0, ARG_REQUIRED, "MODO", G_META, "none · color (default: perfil, gamma, DPI) · all (+ EXIF, XMP y textos)", cli_set_enum, OFF(encode.meta), FLAG(meta_set), 0, 0, "none|color|all"),
    ROW("strip", 0, ARG_NONE, NULL, G_META, "atajo de --meta none", act_strip, 0, 0, 0, 0, NULL),
    ROW("keep-meta", 0, ARG_NONE, NULL, G_META, "atajo de --meta all (la orientacion EXIF queda en 1: ya se aplico)", act_keep_meta, 0, 0, 0, 0, NULL),
    ROW("text", 0, ARG_REQUIRED, "CLAVE=VALOR", G_META, "agrega un texto (Author, Title, Copyright...); se puede repetir", act_text, 0, 0, 0, 0, NULL),
    ROW("dpi", 0, ARG_REQUIRED, "N", G_META, "densidad declarada (pHYs)", cli_set_u32, OFF(dpi), 0, 1, 100000, NULL),
    ROW("grayscale", 0, ARG_NONE, NULL, G_META, "convierte a escala de grises (luminancia en luz lineal)", cli_set_true, OFF(pipeline.grayscale), 0, 0, 0, NULL),

    ROW("frame", 0, ARG_REQUIRED, "N", G_ANIM, "un solo cuadro/pagina/tamaño (desde 0)", act_frame, 0, 0, 0, 0, NULL),
    ROW("frames", 0, ARG_REQUIRED, "all", G_ANIM, "todos: animaciones a APNG, paginas y tamaños a archivos -000, -001...", act_frames, 0, 0, 0, 0, NULL),
    ROW("split", 0, ARG_NONE, NULL, G_ANIM, "cuadros de animacion como PNG sueltos en vez de APNG", cli_set_true, OFF(split), 0, 0, 0, NULL),
    ROW("no-apng", 0, ARG_NONE, NULL, G_ANIM, "animaciones: solo la imagen estatica", cli_set_false, OFF(apng), 0, 0, 0, NULL),
    ROW("loops", 0, ARG_REQUIRED, "N", G_ANIM, "repeticiones del APNG (0 = infinito; default: las del original)", cli_set_u32, OFF(loops), FLAG(loops_set), 0, 65535, NULL),
    ROW("raw-delays", 0, ARG_NONE, NULL, G_ANIM, "GIF: no estira retardos de 0-10 ms a 100 ms (lo que hacen los navegadores)", cli_set_true, OFF(decode.raw_gif_delays), 0, 0, 0, NULL),
    ROW("ico-size", 0, ARG_REQUIRED, "PX", G_ANIM, "ICO/CUR: elige el tamaño (default: el mas grande)", cli_set_u32, OFF(decode.ico_size), 0, 1, 1024, NULL),
    ROW("normal-z", 0, ARG_NONE, NULL, G_ANIM, "DDS BC5 (normal maps): reconstruye Z en el canal azul", cli_set_true, OFF(decode.bc5_reconstruct_z), 0, 0, 0, NULL),

    ROW("tonemap", 0, ARG_REQUIRED, "OP", G_HDR, "HDR: auto (ACES solo si hay luces > 1) aces reinhard clip", cli_set_enum, OFF(decode.tonemap), 0, 0, 0, "auto|aces|reinhard|clip"),
    ROW("exposure", 0, ARG_REQUIRED, "EV", G_HDR, "HDR: exposicion en pasos (+1 = el doble de luz)", cli_set_float, OFF(decode.exposure), 0, -20, 20, NULL),
    ROW("wic", 0, ARG_NONE, NULL, G_HDR, "decodifica todo con WIC (para comparar con los decodificadores propios)", cli_set_true, OFF(decode.force_wic), 0, 0, 0, NULL),
    ROW("no-wic", 0, ARG_NONE, NULL, G_HDR, "no usa WIC: solo decodificadores propios", cli_set_true, OFF(decode.no_wic), 0, 0, 0, NULL),
    ROW("ignore-crc", 0, ARG_NONE, NULL, G_HDR, "lee PNG con CRC incorrecto", cli_set_true, OFF(decode.ignore_crc), 0, 0, 0, NULL),

    ROW("info", 'i', ARG_NONE, NULL, G_INFO, "analiza sin convertir: formato, alfa, colores, perfil y el mejor PNG posible", cli_set_true, OFF(info), 0, 0, 0, NULL),
    ROW("formats", 0, ARG_NONE, NULL, G_INFO, "lista los formatos que se pueden leer (propios + codecs de Windows)", cli_set_true, OFF(formats), 0, 0, 0, NULL),
    ROW("json", 0, ARG_NONE, NULL, G_INFO, "reporte en JSON, una linea por archivo (stdout)", cli_set_true, OFF(json), 0, 0, 0, NULL),
    ROW("quiet", 'q', ARG_NONE, NULL, G_INFO, "solo errores", cli_set_true, OFF(quiet), 0, 0, 0, NULL),
    ROW("verbose", 'v', ARG_NONE, NULL, G_INFO, "detalle: operaciones, avisos y decisiones del codificador", cli_set_true, OFF(verbose), 0, 0, 0, NULL),
    ROW("plain", 0, ARG_NONE, NULL, G_INFO, "sin colores ni barra de progreso", cli_set_true, OFF(plain), 0, 0, 0, NULL),
    ROW("bench", 0, ARG_NONE, NULL, G_INFO, "tiempo por etapa: leer, decodificar, procesar, codificar, escribir", cli_set_true, OFF(bench), 0, 0, 0, NULL),
    ROW("help", 'h', ARG_NONE, NULL, G_INFO, "esta ayuda", cli_set_true, OFF(help), 0, 0, 0, NULL),
    ROW("examples", 0, ARG_NONE, NULL, G_INFO, "recetas de uso", cli_set_true, OFF(examples), 0, 0, 0, NULL),
    ROW("version", 'V', ARG_NONE, NULL, G_INFO, "version y CPU", cli_set_true, OFF(version), 0, 0, 0, NULL),
};

const size_t IMAGE2PNG_OPTION_COUNT = ARRAY_LEN(IMAGE2PNG_OPTIONS);

// ---- configuracion ---------------------------------------------------------------------------------

void config_defaults(Config *cfg) {
    memset(cfg, 0, sizeof *cfg);
    cfg->apng = true;
    cfg->max_megapixels = (u32)(DEFAULT_MAX_PIXELS / 1000000);
    vec_init(&cfg->texts, sizeof(TextPair));
    cfg->decode.max_pixels = DEFAULT_MAX_PIXELS;
    cfg->decode.tonemap = TONE_AUTO;
    pipeline_defaults(&cfg->pipeline);
    encode_options_default(&cfg->encode);
}

void config_free(Config *cfg) {
    mem_free((void *)cfg->output);
    mem_free((void *)cfg->suffix);
    mem_free((void *)cfg->mask_path);
    for (size_t i = 0; i < cfg->texts.count; i++) {
        TextPair *t = &VEC_AT(&cfg->texts, TextPair, i);
        mem_free(t->keyword);
        mem_free(t->value);
    }
    vec_free(&cfg->texts);
    image_free(&cfg->mask);
}

// ---- ayuda -----------------------------------------------------------------------------------------

enum { HELP_LEFT = 32 };

// Parte `text` en renglones de `width` columnas y los agrega con sangria `indent`.
static void wrap(Buf *b, const char *text, u32 indent, u32 width, u32 first_col) {
    u32 col = first_col;
    const char *p = text;
    while (*p) {
        const char *word = p;
        while (*p && *p != ' ') p++;
        size_t len = (size_t)(p - word);
        u32 w = (u32)utf8_display_width(word, len);
        if (col > indent && col + w > width) {
            buf_puts(b, "\n");
            buf_repeat(b, " ", indent);
            col = indent;
        }
        buf_append(b, word, len);
        col += w;
        while (*p == ' ') { if (col < width) { buf_puts(b, " "); col++; } p++; }
    }
}

void print_help(void) {
    Buf b;
    buf_init(&b);
    u32 width = MIN(ui_width(), 120u);
    buf_puts(&b, "\n  ");
    buf_bold(&b);
    buf_inked(&b, UI_ACCENT, "image2png");
    buf_inked(&b, UI_DIM, " %s · %s\n\n", PRISMA_VERSION, "cualquier imagen a PNG, con la transparencia bien tratada");
    buf_inked(&b, UI_SUBTLE, "  uso   ");
    buf_puts(&b, "image2png [opciones] <entrada...>\n");
    buf_inked(&b, UI_SUBTLE, "        ");
    buf_inked(&b, UI_DIM, "entradas: archivos, carpetas, comodines (*.jpg), @lista.txt o - (stdin)\n");
    const char *group = NULL;
    for (size_t i = 0; i < IMAGE2PNG_OPTION_COUNT; i++) {
        const OptionSpec *o = &IMAGE2PNG_OPTIONS[i];
        if (o->group != group) {
            group = o->group;
            buf_puts(&b, "\n  ");
            buf_inked(&b, UI_TEAL, "%s", group);
            buf_puts(&b, "\n");
        }
        size_t start = b.len;
        buf_puts(&b, "    ");
        if (o->letter) buf_inked(&b, UI_TEAL, "-%c, ", o->letter);
        else buf_puts(&b, "    ");
        buf_inked(&b, UI_PATH, "--%s", o->name);
        if (o->metavar) buf_inked(&b, UI_FORMAT, " %s", o->metavar);
        size_t used = visible_width(b.data + start, b.len - start);
        if (used + 2 > HELP_LEFT) { buf_puts(&b, "\n"); buf_repeat(&b, " ", HELP_LEFT); }
        else buf_pad_to(&b, start, HELP_LEFT);
        buf_ink(&b, UI_SUBTLE);
        wrap(&b, o->help, HELP_LEFT, width, HELP_LEFT);
        buf_reset(&b);
        buf_puts(&b, "\n");
    }
    buf_puts(&b, "\n  ");
    buf_inked(&b, UI_DIM, "colores: #rgb #rrggbbaa rgb(r,g,b) rgba(r,g,b,a) o nombres CSS (white, transparent...)\n");
    buf_puts(&b, "  ");
    buf_inked(&b, UI_DIM, "image2png --examples muestra recetas · image2png --formats lista lo que puede leer\n\n");
    term_emit(&b);
    buf_free(&b);
}

typedef struct Example {
    const char *what;
    const char *command;
} Example;

void print_examples(void) {
    static const Example EXAMPLES[] = {
        { "convertir una foto (el formato se detecta por el contenido)", "image2png foto.heic" },
        { "toda una carpeta, con subcarpetas, a otra carpeta replicando la estructura", "image2png -r fotos -o png --keep-tree" },
        { "logo sobre fondo blanco -> transparente, bordes suaves sin halo", "image2png logo.jpg --remove-bg" },
        { "quitar un fondo verde conservando el antialias (como GIMP)", "image2png sprite.bmp --color-to-alpha #00ff00" },
        { "firma escaneada: el papel desaparece y la tinta queda translucida", "image2png firma.jpg --color-to-alpha white --trim 8" },
        { "GIF animado a APNG (transparencia de 8 bits, cuadros optimizados)", "image2png animacion.gif" },
        { "cuadros de un GIF como PNG sueltos", "image2png animacion.gif --split" },
        { "textura para un motor: sangrado de color bajo el alfa (sin halos en mipmaps)", "image2png arbol.tga --bleed" },
        { "PNG8 con alfa suave de 64 colores (pesa una fraccion)", "image2png icono.png --colors 64 -o icono-8.png" },
        { "la mascara de otra imagen como transparencia", "image2png retrato.jpg --mask mascara.png" },
        { "icono: todos los tamaños como archivos", "image2png app.ico --frames all" },
        { "HDR a PNG de 16 bits con tonemap ACES y un paso mas de luz", "image2png cielo.hdr --tonemap aces --exposure 1" },
        { "reoptimizar PNGs existentes al maximo, en el lugar", "image2png *.png --in-place --max" },
        { "miniaturas de 256 px en luz lineal, sin metadatos", "image2png -r fotos -o thumbs --resize 256x256 --linear --strip" },
        { "ver que tiene una imagen (alfa, colores, perfil) sin convertir", "image2png -i captura.png" },
        { "de stdin a stdout", "type foto.webp | image2png - -o - > foto.png" },
    };
    Buf b;
    buf_init(&b);
    buf_puts(&b, "\n  ");
    buf_bold(&b);
    buf_inked(&b, UI_ACCENT, "image2png");
    buf_inked(&b, UI_DIM, " · recetas\n\n");
    for (size_t i = 0; i < ARRAY_LEN(EXAMPLES); i++) {
        buf_puts(&b, "  ");
        buf_inked(&b, UI_SUBTLE, "%s\n", EXAMPLES[i].what);
        buf_puts(&b, "    ");
        buf_inked(&b, UI_TEAL, "›");
        buf_inked(&b, UI_PATH, " %s\n\n", EXAMPLES[i].command);
    }
    term_emit(&b);
    buf_free(&b);
}
