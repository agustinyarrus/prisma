// core/icc.c - ver icc.h
#include <stdio.h>
#include "icc.h"
#include "text.h"

enum { ICC_HEADER = 128, TAG_ENTRY = 12, MAX_TAGS = 256 };

#define SIG(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

static bool find_tag(const u8 *icc, size_t size, u32 signature, size_t *offset, size_t *length) {
    if (size < ICC_HEADER + 4 || load_be32(icc + 36) != SIG('a', 'c', 's', 'p')) return false;
    u32 count = load_be32(icc + ICC_HEADER);
    if (count > MAX_TAGS || ICC_HEADER + 4 + (size_t)count * TAG_ENTRY > size) return false;
    for (u32 i = 0; i < count; i++) {
        const u8 *e = icc + ICC_HEADER + 4 + (size_t)i * TAG_ENTRY;
        if (load_be32(e) != signature) continue;
        size_t off = load_be32(e + 4), len = load_be32(e + 8);
        if (off > size || len > size - off) return false;
        *offset = off;
        *length = len;
        return true;
    }
    return false;
}

bool icc_description(const u8 *icc, size_t size, char *out, size_t cap) {
    size_t off, len;
    if (!cap || !find_tag(icc, size, SIG('d', 'e', 's', 'c'), &off, &len) || len < 12) return false;
    const u8 *tag = icc + off;
    u32 type = load_be32(tag);
    if (type == SIG('d', 'e', 's', 'c')) {                      // v2: textDescriptionType
        size_t n = load_be32(tag + 8);
        if (n > len - 12) n = len - 12;
        size_t o = 0;
        for (size_t i = 0; i < n && tag[12 + i] && o + 1 < cap; i++) out[o++] = (char)(tag[12 + i] < 0x80 ? tag[12 + i] : '?');
        out[o] = 0;
        return o > 0;
    }
    if (type == SIG('m', 'l', 'u', 'c') && len >= 28) {         // v4: multiLocalizedUnicode
        size_t str_len = load_be32(tag + 20), str_off = load_be32(tag + 24);
        if (str_off > len || str_len > len - str_off) return false;
        size_t o = 0;
        for (size_t i = 0; i + 1 < str_len && o + 4 < cap; i += 2) {
            u32 cp = load_be16(tag + str_off + i);
            if (!cp) break;
            if (cp < 0x80) out[o++] = (char)cp;
            else if (cp < 0x800) { out[o++] = (char)(0xC0 | cp >> 6); out[o++] = (char)(0x80 | (cp & 0x3F)); }
            else { out[o++] = (char)(0xE0 | cp >> 12); out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[o++] = (char)(0x80 | (cp & 0x3F)); }
        }
        out[o] = 0;
        return o > 0;
    }
    return false;
}

bool icc_color_space(const u8 *icc, size_t size, char out[5]) {
    if (size < ICC_HEADER) return false;
    memcpy(out, icc + 16, 4);
    out[4] = 0;
    return true;
}

bool icc_is_srgb(const u8 *icc, size_t size) {
    char space[5], desc[128];
    if (!icc_color_space(icc, size, space) || strcmp(space, "RGB ") != 0) return false;
    if (load_be32(icc + 12) != SIG('m', 'n', 't', 'r')) return false;         // clase: monitor
    if (!icc_description(icc, size, desc, sizeof desc)) return false;
    // Variantes reales vistas en camaras, celulares, Photoshop, GIMP, Windows y navegadores.
    static const char *const KNOWN[] = {
        "sRGB IEC61966-2.1", "sRGB IEC61966-2-1 black scaled", "sRGB IEC61966-2-1 no black scaling",
        "sRGB built-in", "sRGB", "sRGB2014", "sRGB v4 ICC preference perceptual intent beta",
        "sRGB IEC61966-2.1 (Equivalent to www.srgb.com 1998 HP profile)", "Compact sRGB", "c2",
        "GIMP built-in sRGB", "sRGB v1.31 (Canon)",
    };
    for (size_t i = 0; i < ARRAY_LEN(KNOWN); i++)
        if (strcmp(desc, KNOWN[i]) == 0) return true;
    return false;
}
