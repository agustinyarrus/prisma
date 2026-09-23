// ops/color.c - ver color.h
#include <stdio.h>
#include <stdlib.h>
#include "color.h"
#include "../core/text.h"

typedef struct NamedColor {
    const char *name;
    u32         rgb;
} NamedColor;

// Ordenada alfabeticamente: busqueda binaria O(log n).
static const NamedColor NAMED[] = {
    { "aliceblue", 0xf0f8ff }, { "antiquewhite", 0xfaebd7 }, { "aqua", 0x00ffff }, { "aquamarine", 0x7fffd4 },
    { "azure", 0xf0ffff }, { "beige", 0xf5f5dc }, { "bisque", 0xffe4c4 }, { "black", 0x000000 },
    { "blanchedalmond", 0xffebcd }, { "blue", 0x0000ff }, { "blueviolet", 0x8a2be2 }, { "brown", 0xa52a2a },
    { "burlywood", 0xdeb887 }, { "cadetblue", 0x5f9ea0 }, { "chartreuse", 0x7fff00 }, { "chocolate", 0xd2691e },
    { "coral", 0xff7f50 }, { "cornflowerblue", 0x6495ed }, { "cornsilk", 0xfff8dc }, { "crimson", 0xdc143c },
    { "cyan", 0x00ffff }, { "darkblue", 0x00008b }, { "darkcyan", 0x008b8b }, { "darkgoldenrod", 0xb8860b },
    { "darkgray", 0xa9a9a9 }, { "darkgreen", 0x006400 }, { "darkgrey", 0xa9a9a9 }, { "darkkhaki", 0xbdb76b },
    { "darkmagenta", 0x8b008b }, { "darkolivegreen", 0x556b2f }, { "darkorange", 0xff8c00 }, { "darkorchid", 0x9932cc },
    { "darkred", 0x8b0000 }, { "darksalmon", 0xe9967a }, { "darkseagreen", 0x8fbc8f }, { "darkslateblue", 0x483d8b },
    { "darkslategray", 0x2f4f4f }, { "darkslategrey", 0x2f4f4f }, { "darkturquoise", 0x00ced1 }, { "darkviolet", 0x9400d3 },
    { "deeppink", 0xff1493 }, { "deepskyblue", 0x00bfff }, { "dimgray", 0x696969 }, { "dimgrey", 0x696969 },
    { "dodgerblue", 0x1e90ff }, { "firebrick", 0xb22222 }, { "floralwhite", 0xfffaf0 }, { "forestgreen", 0x228b22 },
    { "fuchsia", 0xff00ff }, { "gainsboro", 0xdcdcdc }, { "ghostwhite", 0xf8f8ff }, { "gold", 0xffd700 },
    { "goldenrod", 0xdaa520 }, { "gray", 0x808080 }, { "green", 0x008000 }, { "greenyellow", 0xadff2f },
    { "grey", 0x808080 }, { "honeydew", 0xf0fff0 }, { "hotpink", 0xff69b4 }, { "indianred", 0xcd5c5c },
    { "indigo", 0x4b0082 }, { "ivory", 0xfffff0 }, { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa },
    { "lavenderblush", 0xfff0f5 }, { "lawngreen", 0x7cfc00 }, { "lemonchiffon", 0xfffacd }, { "lightblue", 0xadd8e6 },
    { "lightcoral", 0xf08080 }, { "lightcyan", 0xe0ffff }, { "lightgoldenrodyellow", 0xfafad2 }, { "lightgray", 0xd3d3d3 },
    { "lightgreen", 0x90ee90 }, { "lightgrey", 0xd3d3d3 }, { "lightpink", 0xffb6c1 }, { "lightsalmon", 0xffa07a },
    { "lightseagreen", 0x20b2aa }, { "lightskyblue", 0x87cefa }, { "lightslategray", 0x778899 }, { "lightslategrey", 0x778899 },
    { "lightsteelblue", 0xb0c4de }, { "lightyellow", 0xffffe0 }, { "lime", 0x00ff00 }, { "limegreen", 0x32cd32 },
    { "linen", 0xfaf0e6 }, { "magenta", 0xff00ff }, { "maroon", 0x800000 }, { "mediumaquamarine", 0x66cdaa },
    { "mediumblue", 0x0000cd }, { "mediumorchid", 0xba55d3 }, { "mediumpurple", 0x9370db }, { "mediumseagreen", 0x3cb371 },
    { "mediumslateblue", 0x7b68ee }, { "mediumspringgreen", 0x00fa9a }, { "mediumturquoise", 0x48d1cc }, { "mediumvioletred", 0xc71585 },
    { "midnightblue", 0x191970 }, { "mintcream", 0xf5fffa }, { "mistyrose", 0xffe4e1 }, { "moccasin", 0xffe4b5 },
    { "navajowhite", 0xffdead }, { "navy", 0x000080 }, { "oldlace", 0xfdf5e6 }, { "olive", 0x808000 },
    { "olivedrab", 0x6b8e23 }, { "orange", 0xffa500 }, { "orangered", 0xff4500 }, { "orchid", 0xda70d6 },
    { "palegoldenrod", 0xeee8aa }, { "palegreen", 0x98fb98 }, { "paleturquoise", 0xafeeee }, { "palevioletred", 0xdb7093 },
    { "papayawhip", 0xffefd5 }, { "peachpuff", 0xffdab9 }, { "peru", 0xcd853f }, { "pink", 0xffc0cb },
    { "plum", 0xdda0dd }, { "powderblue", 0xb0e0e6 }, { "purple", 0x800080 }, { "rebeccapurple", 0x663399 },
    { "red", 0xff0000 }, { "rosybrown", 0xbc8f8f }, { "royalblue", 0x4169e1 }, { "saddlebrown", 0x8b4513 },
    { "salmon", 0xfa8072 }, { "sandybrown", 0xf4a460 }, { "seagreen", 0x2e8b57 }, { "seashell", 0xfff5ee },
    { "sienna", 0xa0522d }, { "silver", 0xc0c0c0 }, { "skyblue", 0x87ceeb }, { "slateblue", 0x6a5acd },
    { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "snow", 0xfffafa }, { "springgreen", 0x00ff7f },
    { "steelblue", 0x4682b4 }, { "tan", 0xd2b48c }, { "teal", 0x008080 }, { "thistle", 0xd8bfd8 },
    { "tomato", 0xff6347 }, { "transparent", 0x000000 }, { "turquoise", 0x40e0d0 }, { "violet", 0xee82ee },
    { "wheat", 0xf5deb3 }, { "white", 0xffffff }, { "whitesmoke", 0xf5f5f5 }, { "yellow", 0xffff00 },
    { "yellowgreen", 0x9acd32 },
};

static const char *g_names[ARRAY_LEN(NAMED)];

const char *const *color_names(size_t *count) {
    for (size_t i = 0; i < ARRAY_LEN(NAMED); i++) g_names[i] = NAMED[i].name;
    *count = ARRAY_LEN(NAMED);
    return g_names;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static bool set_error(char *error, size_t cap, const char *msg) {
    if (error && cap) snprintf(error, cap, "%s", msg);
    return false;
}

// Componente de rgb(): entero 0-255, o porcentaje; en rgba() el 4.o es 0-1 o porcentaje.
static bool parse_component(const char **p, bool alpha, float *out) {
    while (**p == ' ') (*p)++;
    char *end;
    double v = strtod(*p, &end);
    if (end == *p) return false;
    bool percent = *end == '%';
    if (percent) end++;
    *out = (float)(percent ? v / 100.0 : alpha ? v : v / 255.0);
    *out = saturatef(*out);
    *p = end;
    while (**p == ' ') (*p)++;
    return true;
}

bool color_parse(const char *text, Rgba *out, char *error, size_t cap) {
    char s[64];
    size_t n = strlen(text);
    if (!n || n >= sizeof s) return set_error(error, cap, "color vacio o demasiado largo");
    memcpy(s, text, n + 1);
    str_lower_ascii(s);
    const char *p = s;
    while (*p == ' ') p++;

    if (*p == '#' || (n == 6 || n == 8 || n == 3 || n == 4)) {
        const char *hex = *p == '#' ? p + 1 : p;
        size_t len = strlen(hex);
        int v[8];
        bool ok = len == 3 || len == 4 || len == 6 || len == 8;
        for (size_t i = 0; ok && i < len; i++) ok = (v[i] = hex_digit(hex[i])) >= 0;
        if (ok) {
            if (len <= 4) {
                out->r = v[0] * 17 / 255.0f; out->g = v[1] * 17 / 255.0f; out->b = v[2] * 17 / 255.0f;
                out->a = len == 4 ? v[3] * 17 / 255.0f : 1.0f;
            } else {
                out->r = (v[0] * 16 + v[1]) / 255.0f; out->g = (v[2] * 16 + v[3]) / 255.0f; out->b = (v[4] * 16 + v[5]) / 255.0f;
                out->a = len == 8 ? (v[6] * 16 + v[7]) / 255.0f : 1.0f;
            }
            return true;
        }
        if (*p == '#') return set_error(error, cap, "hexadecimal invalido: usa #rgb, #rgba, #rrggbb o #rrggbbaa");
    }
    if (str_starts_with(p, "rgb(") || str_starts_with(p, "rgba(")) {
        bool has_alpha = p[3] == 'a';
        p += has_alpha ? 5 : 4;
        float c[4] = { 0, 0, 0, 1 };
        for (int i = 0; i < (has_alpha ? 4 : 3); i++) {
            if (!parse_component(&p, i == 3, &c[i])) return set_error(error, cap, "rgb()/rgba() mal formado");
            if (i < (has_alpha ? 3 : 2)) { if (*p != ',') return set_error(error, cap, "faltan comas en rgb()"); p++; }
        }
        if (*p != ')') return set_error(error, cap, "falta el ')' de rgb()");
        *out = (Rgba){ c[0], c[1], c[2], c[3] };
        return true;
    }
    size_t lo = 0, hi = ARRAY_LEN(NAMED);
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int cmp = strcmp(p, NAMED[mid].name);
        if (cmp == 0) {
            u32 v = NAMED[mid].rgb;
            *out = (Rgba){ (v >> 16) / 255.0f, ((v >> 8) & 255) / 255.0f, (v & 255) / 255.0f,
                           strcmp(p, "transparent") == 0 ? 0.0f : 1.0f };
            return true;
        }
        if (cmp < 0) hi = mid; else lo = mid + 1;
    }
    // Sugerencia: el nombre conocido mas parecido.
    u32 best = UINT32_MAX;
    const char *guess = NULL;
    for (size_t i = 0; i < ARRAY_LEN(NAMED); i++) {
        u32 d = edit_distance(p, NAMED[i].name);
        if (d < best) { best = d; guess = NAMED[i].name; }
    }
    if (error && cap) {
        if (guess && best <= 2) snprintf(error, cap, "color desconocido \"%s\" (¿quisiste decir %s?)", text, guess);
        else snprintf(error, cap, "color desconocido \"%s\": usa #rrggbb, rgb(r,g,b) o un nombre CSS", text);
    }
    return false;
}

void color_format(const Rgba *c, char *out, size_t cap) {
    int r = (int)(c->r * 255 + 0.5f), g = (int)(c->g * 255 + 0.5f), b = (int)(c->b * 255 + 0.5f), a = (int)(c->a * 255 + 0.5f);
    if (a == 255) snprintf(out, cap, "#%02x%02x%02x", r, g, b);
    else snprintf(out, cap, "#%02x%02x%02x%02x", r, g, b, a);
}
