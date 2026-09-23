// codec/reader.h - lector de bytes con limites, para parsear encabezados de archivos ajenos.
//
// Patron de "error pegajoso": cualquier lectura fuera de rango devuelve 0 y marca `bad`; el
// parser sigue sin ifs en cada campo y chequea `bad` una vez al final de cada bloque. Asi un
// archivo truncado o malicioso nunca lee fuera del buffer, y el codigo del parser se lee como la
// especificacion del formato.
#pragma once
#include "../core/base.h"

typedef struct Reader {
    const u8 *base;
    const u8 *p;
    const u8 *end;
    bool      bad;
} Reader;

INLINE Reader reader_make(const u8 *data, size_t size) {
    Reader r = { data, data, data + size, false };
    return r;
}

INLINE size_t rd_left(const Reader *r)  { return (size_t)(r->end - r->p); }
INLINE size_t rd_pos(const Reader *r)   { return (size_t)(r->p - r->base); }
INLINE size_t rd_size(const Reader *r)  { return (size_t)(r->end - r->base); }

INLINE bool rd_need(Reader *r, size_t n) {
    if (UNLIKELY(r->bad || rd_left(r) < n)) { r->bad = true; return false; }
    return true;
}

INLINE u8  rd_u8(Reader *r)   { if (!rd_need(r, 1)) return 0; return *r->p++; }
INLINE u16 rd_le16(Reader *r) { if (!rd_need(r, 2)) return 0; u16 v = load_le16(r->p); r->p += 2; return v; }
INLINE u32 rd_le32(Reader *r) { if (!rd_need(r, 4)) return 0; u32 v = load_le32(r->p); r->p += 4; return v; }
INLINE u64 rd_le64(Reader *r) { if (!rd_need(r, 8)) return 0; u64 v = load_le64(r->p); r->p += 8; return v; }
INLINE u16 rd_be16(Reader *r) { if (!rd_need(r, 2)) return 0; u16 v = load_be16(r->p); r->p += 2; return v; }
INLINE u32 rd_be32(Reader *r) { if (!rd_need(r, 4)) return 0; u32 v = load_be32(r->p); r->p += 4; return v; }
INLINE u64 rd_be64(Reader *r) { if (!rd_need(r, 8)) return 0; u64 v = load_be64(r->p); r->p += 8; return v; }
INLINE i16 rd_le16s(Reader *r) { return (i16)rd_le16(r); }
INLINE i32 rd_le32s(Reader *r) { return (i32)rd_le32(r); }
INLINE i16 rd_be16s(Reader *r) { return (i16)rd_be16(r); }

INLINE const u8 *rd_bytes(Reader *r, size_t n) {
    if (!rd_need(r, n)) return NULL;
    const u8 *q = r->p;
    r->p += n;
    return q;
}

INLINE void rd_skip(Reader *r, size_t n) { if (rd_need(r, n)) r->p += n; }

INLINE void rd_seek(Reader *r, size_t pos) {
    if (UNLIKELY(pos > rd_size(r))) { r->bad = true; r->p = r->end; return; }
    r->p = r->base + pos;
}

INLINE bool rd_match(Reader *r, const void *magic, size_t n) {
    if (!rd_need(r, n) || memcmp(r->p, magic, n) != 0) return false;
    r->p += n;
    return true;
}
