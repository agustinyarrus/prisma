// png/writer.c - ver writer.h
#include <stdio.h>
#include "writer.h"
#include "../codec/zlib.h"
#include "../codec/exif.h"
#include "../core/icc.h"

enum { IDAT_MAX = 0x40000000, TEXT_COMPRESS_THRESHOLD = 1024, ICC_NAME_MAX = 79 };

void bv_put(ByteVec *v, const void *data, size_t size) {
    if (v->failed || !size) return;
    size_t need;
    if (!add_ok(v->len, size, &need)) { v->failed = true; return; }
    if (need > v->cap) {
        size_t cap = MAX(need, v->cap ? v->cap + v->cap / 2 : (size_t)4096);
        u8 *grown = mem_realloc(v->data, cap);
        if (!grown) { v->failed = true; return; }
        v->data = grown;
        v->cap = cap;
    }
    memcpy(v->data + v->len, data, size);
    v->len += size;
}

void bv_free(ByteVec *v) {
    mem_free(v->data);
    memset(v, 0, sizeof *v);
}

static void bv_u32(ByteVec *v, u32 x) { u8 b[4]; store_be32(b, x); bv_put(v, b, 4); }

void png_write_signature(ByteVec *v) {
    static const u8 SIG[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    bv_put(v, SIG, 8);
}

void png_write_chunk(ByteVec *v, const char type[4], const void *data, size_t size) {
    bv_u32(v, (u32)size);
    u32 crc = crc32_update(0, type, 4);
    bv_put(v, type, 4);
    if (size) { bv_put(v, data, size); crc = crc32_update(crc, data, size); }
    bv_u32(v, crc);
}

void png_write_ihdr(ByteVec *v, u32 width, u32 height, const PngPlan *plan, bool interlace) {
    u8 ihdr[13];
    store_be32(ihdr, width);
    store_be32(ihdr + 4, height);
    ihdr[8] = plan->bit_depth;
    ihdr[9] = plan->color_type;
    ihdr[10] = 0;
    ihdr[11] = 0;
    ihdr[12] = interlace ? 1 : 0;
    png_write_chunk(v, "IHDR", ihdr, sizeof ihdr);
}

void png_write_palette(ByteVec *v, const PngPlan *plan) {
    if (plan->color_type == PNG_CT_PALETTE) {
        u8 plte[3 * MAX_PALETTE], trns[MAX_PALETTE];
        for (u32 i = 0; i < plan->palette_size; i++) {
            u32 c = plan->palette[i];
            plte[3 * i] = (u8)c;
            plte[3 * i + 1] = (u8)(c >> 8);
            plte[3 * i + 2] = (u8)(c >> 16);
            trns[i] = (u8)(c >> 24);
        }
        png_write_chunk(v, "PLTE", plte, 3 * (size_t)plan->palette_size);
        if (plan->trns_count) png_write_chunk(v, "tRNS", trns, plan->trns_count);
        return;
    }
    if (!plan->key) return;
    u8 trns[6];
    if (plan->color_type == PNG_CT_GRAY) {
        store_be16(trns, plan->key_value[0]);
        png_write_chunk(v, "tRNS", trns, 2);
    } else {
        for (int c = 0; c < 3; c++) store_be16(trns + 2 * c, plan->key_value[c]);
        png_write_chunk(v, "tRNS", trns, 6);
    }
}

// ---- metadatos ------------------------------------------------------------------------------

static bool is_latin1_safe(const char *s) {
    // tEXt es Latin-1: solo se usa si el texto es ASCII puro (evita reinterpretar UTF-8).
    for (; *s; s++) if ((u8)*s >= 0x80) return false;
    return true;
}

static void write_text(ByteVec *v, const char *keyword, const char *text) {
    size_t klen = MIN(strlen(keyword), (size_t)ICC_NAME_MAX), tlen = strlen(text);
    ByteVec body = { 0 };
    bool compress = tlen >= TEXT_COMPRESS_THRESHOLD;
    u8 *z = NULL;
    size_t zlen = 0;
    if (compress && zlib_compress((const u8 *)text, tlen, 9, false, &z, &zlen, &(Diag){ 0 }) != ST_OK) compress = false;
    bv_put(&body, keyword, klen);
    static const u8 NUL = 0;
    bv_put(&body, &NUL, 1);
    if (is_latin1_safe(text)) {
        if (compress) { bv_put(&body, &NUL, 1); bv_put(&body, z, zlen); png_write_chunk(v, "zTXt", body.data, body.len); }
        else { bv_put(&body, text, tlen); png_write_chunk(v, "tEXt", body.data, body.len); }
    } else {
        u8 flags[2] = { compress ? 1 : 0, 0 };
        bv_put(&body, flags, 2);
        bv_put(&body, &NUL, 1);            // idioma vacio
        bv_put(&body, &NUL, 1);            // clave traducida vacia
        if (compress) bv_put(&body, z, zlen); else bv_put(&body, text, tlen);
        png_write_chunk(v, "iTXt", body.data, body.len);
    }
    mem_free(z);
    bv_free(&body);
}

static void write_icc(ByteVec *v, const Metadata *m) {
    u8 *z = NULL;
    size_t zlen = 0;
    if (zlib_compress(m->icc.data, m->icc.size, 12, false, &z, &zlen, &(Diag){ 0 }) != ST_OK) return;
    // El nombre del perfil es Latin-1 de 1..79 caracteres imprimibles.
    char name[ICC_NAME_MAX + 1] = "ICC profile";
    if (m->icc_name[0]) {
        size_t o = 0;
        for (const char *p = m->icc_name; *p && o < ICC_NAME_MAX; p++)
            if ((u8)*p >= 0x20 && (u8)*p < 0x7F) name[o++] = *p;
        if (o) name[o] = 0;
    }
    ByteVec body = { 0 };
    static const u8 NUL_METHOD[2] = { 0, 0 };
    bv_put(&body, name, strlen(name));
    bv_put(&body, NUL_METHOD, 2);
    bv_put(&body, z, zlen);
    png_write_chunk(v, "iCCP", body.data, body.len);
    bv_free(&body);
    mem_free(z);
}

void png_write_metadata(ByteVec *v, const Metadata *m, bool orientation_applied) {
    // cICP manda sobre iCCP/sRGB/gAMA en los lectores que lo entienden; los demas siguen con el resto.
    if (m->has_cicp) png_write_chunk(v, "cICP", m->cicp, 4);
    if (m->icc.size) {
        if (icc_is_srgb(m->icc.data, m->icc.size)) {
            u8 intent = 0;
            png_write_chunk(v, "sRGB", &intent, 1);
        } else {
            write_icc(v, m);
        }
    } else if (m->has_srgb) {
        png_write_chunk(v, "sRGB", &m->srgb_intent, 1);
    } else {
        if (m->has_gamma) { u8 g[4]; store_be32(g, (u32)(m->gamma * 100000.0 + 0.5)); png_write_chunk(v, "gAMA", g, 4); }
        if (m->has_chrm) {
            u8 c[32];
            for (int i = 0; i < 8; i++) store_be32(c + 4 * i, (u32)(m->chrm[i] * 100000.0 + 0.5));
            png_write_chunk(v, "cHRM", c, 32);
        }
    }
    if (m->has_phys) {
        u8 p[9];
        store_be32(p, m->phys_x);
        store_be32(p + 4, m->phys_y);
        p[8] = m->phys_unit;
        png_write_chunk(v, "pHYs", p, 9);
    }
    if (m->exif.size) {
        u8 *exif = mem_alloc(m->exif.size);
        if (exif) {
            memcpy(exif, m->exif.data, m->exif.size);
            // Los pixeles ya se rotaron: la orientacion guardada tiene que decir "normal".
            if (orientation_applied) exif_set_orientation(exif, m->exif.size, 1);
            png_write_chunk(v, "eXIf", exif, m->exif.size);
            mem_free(exif);
        }
    }
    if (m->xmp.size) {
        char *xmp = mem_alloc(m->xmp.size + 1);
        if (xmp) {
            memcpy(xmp, m->xmp.data, m->xmp.size);
            xmp[m->xmp.size] = 0;
            write_text(v, "XML:com.adobe.xmp", xmp);
            mem_free(xmp);
        }
    }
    for (size_t i = 0; i < m->texts.count; i++) {
        const TextEntry *t = &VEC_AT(&m->texts, TextEntry, i);
        write_text(v, t->keyword, t->text);
    }
}

void png_write_data_chunks(ByteVec *v, const char type[4], const u8 *zdata, size_t size) {
    // Un solo IDAT mientras entre (los lectores lo usan sin copiar); si no, tramos de 1 GB.
    do {
        size_t n = MIN(size, (size_t)IDAT_MAX);
        png_write_chunk(v, type, zdata, n);
        zdata += n;
        size -= n;
    } while (size);
}

void png_write_end(ByteVec *v) { png_write_chunk(v, "IEND", NULL, 0); }
