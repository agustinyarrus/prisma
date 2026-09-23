// codec/zlib.c - ver zlib.h
#include "zlib.h"
#include "../core/mem.h"
#include "../core/pool.h"
#include "../../third_party/libdeflate/libdeflate.h"

enum { MAX_LEVEL = 12, ADLER_BASE = 65521, SYNC_FLUSH_SLACK = 16 };

// Un (des)compresor por hilo y por nivel: reservarlos cuesta (el de nivel 12 pesa megas) y el
// pool reusa los mismos hilos para todo el lote. Se liberan al salir el proceso.
static _Thread_local struct libdeflate_decompressor *tls_inflater;
static _Thread_local struct libdeflate_compressor   *tls_deflaters[MAX_LEVEL + 1];

static struct libdeflate_decompressor *inflater(void) {
    if (!tls_inflater) tls_inflater = libdeflate_alloc_decompressor();
    return tls_inflater;
}

static struct libdeflate_compressor *deflater(int level) {
    level = CLAMP(level, 0, MAX_LEVEL);
    if (!tls_deflaters[level]) tls_deflaters[level] = libdeflate_alloc_compressor(level);
    return tls_deflaters[level];
}

u32 crc32_update(u32 crc, const void *data, size_t size) { return libdeflate_crc32(crc, data, size); }
u32 adler32_update(u32 adler, const void *data, size_t size) { return libdeflate_adler32(adler, data, size); }

// Adler-32 de A||B a partir de adler(A), adler(B) y |B| (la misma aritmetica que adler32_combine
// de zlib): s1 suma los bytes, s2 suma los s1 parciales; B desplaza a s2 en |B| * s1(A).
u32 adler32_combine(u32 adler_a, u32 adler_b, size_t size_b) {
    u32 rem = (u32)(size_b % ADLER_BASE);
    u32 sum1 = adler_a & 0xFFFF;
    u32 sum2 = (u32)(((u64)rem * sum1) % ADLER_BASE);
    sum1 += (adler_b & 0xFFFF) + ADLER_BASE - 1;
    sum2 += (adler_a >> 16) + (adler_b >> 16) + ADLER_BASE - rem;
    if (sum1 >= ADLER_BASE) sum1 -= ADLER_BASE;
    if (sum1 >= ADLER_BASE) sum1 -= ADLER_BASE;
    if (sum2 >= 2u * ADLER_BASE) sum2 -= 2u * ADLER_BASE;
    if (sum2 >= ADLER_BASE) sum2 -= ADLER_BASE;
    return sum1 | (sum2 << 16);
}

// =============================================================================================
// Inflador de rescate: decodifica un flujo DEFLATE hasta donde se pueda (la estructura de "puff"
// de Mark Adler, reescrita). Lento a proposito (un bit por vez en Huffman): solo corre cuando
// libdeflate ya rechazo el flujo, para salvar las filas buenas de un PNG cortado.
// =============================================================================================

typedef struct Salvage {
    const u8 *in;
    size_t    in_size, in_pos;
    u64       bitbuf;
    u32       bitcnt;
    u8       *out;
    size_t    out_cap, out_pos;
    bool      failed;
} Salvage;

typedef struct Huffman {
    i16 count[16];
    i16 symbol[320];
} Huffman;

static u32 salvage_bits(Salvage *s, u32 need) {
    while (s->bitcnt < need) {
        if (s->in_pos == s->in_size) { s->failed = true; return 0; }
        s->bitbuf |= (u64)s->in[s->in_pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    u32 val = (u32)(s->bitbuf & ((1ull << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return val;
}

// Codigo canonico a partir de las longitudes. < 0: sobresuscripto (invalido); > 0: incompleto.
static int huffman_build(Huffman *h, const i16 *lengths, int n) {
    memset(h->count, 0, sizeof h->count);
    for (int s = 0; s < n; s++) h->count[lengths[s]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    i16 offs[16] = { 0 };
    for (int len = 1; len < 15; len++) offs[len + 1] = (i16)(offs[len] + h->count[len]);
    for (int s = 0; s < n; s++)
        if (lengths[s]) h->symbol[offs[lengths[s]]++] = (i16)s;
    return left;
}

static int huffman_decode(Salvage *s, const Huffman *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)salvage_bits(s, 1);
        if (s->failed) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    s->failed = true;
    return -1;
}

static const u16 LEN_BASE[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
                                  51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8  LEN_EXTRA[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
                                   4, 4, 5, 5, 5, 5, 0 };
static const u16 DIST_BASE[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257,
                                   385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289,
                                   16385, 24577 };
static const u8  DIST_EXTRA[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9,
                                    9, 10, 10, 11, 11, 12, 12, 13, 13 };
static const u8  CODE_LENGTH_ORDER[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };

// true = bloque terminado bien; false = salida llena o flujo roto (se corta el rescate).
static bool salvage_codes(Salvage *s, const Huffman *lencode, const Huffman *distcode) {
    for (;;) {
        int sym = huffman_decode(s, lencode);
        if (s->failed) return false;
        if (sym < 256) {
            if (s->out_pos == s->out_cap) return false;
            s->out[s->out_pos++] = (u8)sym;
            continue;
        }
        if (sym == 256) return true;
        sym -= 257;
        if (sym >= 29) { s->failed = true; return false; }
        size_t len = LEN_BASE[sym] + salvage_bits(s, LEN_EXTRA[sym]);
        int dsym = huffman_decode(s, distcode);
        if (s->failed || dsym >= 30) { s->failed = true; return false; }
        size_t dist = DIST_BASE[dsym] + salvage_bits(s, DIST_EXTRA[dsym]);
        if (s->failed || dist > s->out_pos) { s->failed = true; return false; }
        for (; len; len--) {
            if (s->out_pos == s->out_cap) return false;
            s->out[s->out_pos] = s->out[s->out_pos - dist];
            s->out_pos++;
        }
    }
}

static bool salvage_stored(Salvage *s) {
    s->bitbuf = 0;                       // descarta los bits hasta el limite de byte
    s->bitcnt = 0;
    if (s->in_size - s->in_pos < 4) { s->failed = true; return false; }
    u32 len = load_le16(s->in + s->in_pos), nlen = load_le16(s->in + s->in_pos + 2);
    s->in_pos += 4;
    if ((len ^ 0xFFFFu) != nlen) { s->failed = true; return false; }
    size_t avail = MIN(MIN((size_t)len, s->in_size - s->in_pos), s->out_cap - s->out_pos);
    memcpy(s->out + s->out_pos, s->in + s->in_pos, avail);
    s->out_pos += avail;
    s->in_pos += avail;
    return avail == len;
}

static bool salvage_fixed(Salvage *s) {
    Huffman lencode, distcode;
    i16 lengths[288];
    for (int i = 0; i < 144; i++) lengths[i] = 8;
    for (int i = 144; i < 256; i++) lengths[i] = 9;
    for (int i = 256; i < 280; i++) lengths[i] = 7;
    for (int i = 280; i < 288; i++) lengths[i] = 8;
    huffman_build(&lencode, lengths, 288);
    for (int i = 0; i < 30; i++) lengths[i] = 5;
    huffman_build(&distcode, lengths, 30);
    return salvage_codes(s, &lencode, &distcode);
}

static bool salvage_dynamic(Salvage *s) {
    u32 nlen = salvage_bits(s, 5) + 257, ndist = salvage_bits(s, 5) + 1, ncode = salvage_bits(s, 4) + 4;
    if (s->failed || nlen > 286 || ndist > 30) { s->failed = true; return false; }
    i16 lengths[320] = { 0 };
    for (u32 i = 0; i < ncode; i++) lengths[CODE_LENGTH_ORDER[i]] = (i16)salvage_bits(s, 3);
    Huffman lencode, distcode;
    if (s->failed || huffman_build(&lencode, lengths, 19) != 0) { s->failed = true; return false; }
    for (u32 i = 0; i < nlen + ndist;) {
        int sym = huffman_decode(s, &lencode);
        if (s->failed) return false;
        if (sym < 16) { lengths[i++] = (i16)sym; continue; }
        i16 repeat_value = 0;
        u32 repeat;
        if (sym == 16) {
            if (i == 0) { s->failed = true; return false; }
            repeat_value = lengths[i - 1];
            repeat = 3 + salvage_bits(s, 2);
        } else if (sym == 17) repeat = 3 + salvage_bits(s, 3);
        else repeat = 11 + salvage_bits(s, 7);
        if (s->failed || i + repeat > nlen + ndist) { s->failed = true; return false; }
        while (repeat--) lengths[i++] = repeat_value;
    }
    if (lengths[256] == 0) { s->failed = true; return false; }
    int err = huffman_build(&lencode, lengths, (int)nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) { s->failed = true; return false; }
    err = huffman_build(&distcode, lengths + nlen, (int)ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) { s->failed = true; return false; }
    return salvage_codes(s, &lencode, &distcode);
}

static size_t inflate_salvage(const u8 *in, size_t in_size, u8 *out, size_t out_cap) {
    Salvage s = { .in = in, .in_size = in_size, .out = out, .out_cap = out_cap };
    for (bool last = false; !last && !s.failed && s.out_pos < s.out_cap;) {
        last = salvage_bits(&s, 1) != 0;
        u32 type = salvage_bits(&s, 2);
        if (s.failed) break;
        bool ok = type == 0 ? salvage_stored(&s) : type == 1 ? salvage_fixed(&s)
                : type == 2 ? salvage_dynamic(&s) : (s.failed = true, false);
        if (!ok) break;
    }
    return s.out_pos;
}

// =============================================================================================
// Inflado
// =============================================================================================

static bool zlib_header_ok(const u8 *p) {
    u32 cmf = p[0], flg = p[1];
    return (cmf & 0x0F) == 8 && (cmf >> 4) <= 7 && ((cmf << 8) | flg) % 31 == 0 && !(flg & 0x20);
}

Status zlib_inflate_exact(const ZSpan *spans, size_t span_count, u8 *out, size_t out_size,
                          size_t *produced, Diag *diag) {
    *produced = 0;
    size_t in_size = 0;
    for (size_t i = 0; i < span_count; i++)
        if (!add_ok(in_size, spans[i].size, &in_size)) return diag_fail(diag, ST_TOO_LARGE, "flujo comprimido demasiado grande");
    // libdeflate necesita la entrada contigua: con un solo tramo se usa tal cual (lo comun en
    // PNG bien escritos); con varios, se juntan en un buffer.
    const u8 *in = span_count == 1 ? spans[0].data : NULL;
    u8 *joined = NULL;
    if (span_count != 1) {
        joined = mem_alloc(in_size ? in_size : 1);
        if (!joined) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para juntar el flujo comprimido");
        size_t at = 0;
        for (size_t i = 0; i < span_count; i++) { memcpy(joined + at, spans[i].data, spans[i].size); at += spans[i].size; }
        in = joined;
    }
    Status status = ST_OK;
    if (in_size < 2) {
        status = diag_fail(diag, ST_TRUNCATED, "no hay datos de imagen comprimidos");
    } else if (!zlib_header_ok(in)) {
        status = diag_fail(diag, ST_CORRUPT, "cabecera zlib invalida");
    } else {
        size_t used_in = 0, got = 0;
        enum libdeflate_result r = libdeflate_deflate_decompress_ex(inflater(), in + 2, in_size - 2,
                                                                    out, out_size, &used_in, &got);
        if (r == LIBDEFLATE_SUCCESS && got == out_size) {
            *produced = got;
        } else {
            // Flujo roto, corto o con datos de mas: rescatar lo que se pueda.
            got = inflate_salvage(in + 2, in_size - 2, out, out_size);
            *produced = got;
            if (got < out_size) status = ST_TRUNCATED;
        }
    }
    mem_free(joined);
    return status;
}

Status zlib_inflate_alloc(const u8 *in, size_t in_size, size_t max_out, u8 **out, size_t *out_size, Diag *diag) {
    *out = NULL;
    *out_size = 0;
    size_t cap = MIN(MAX(in_size * 4, (size_t)4096), max_out);
    for (;;) {
        u8 *buf = mem_alloc(cap);
        if (!buf) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para descomprimir");
        size_t got = 0;
        enum libdeflate_result r = libdeflate_zlib_decompress_ex(inflater(), in, in_size, buf, cap, NULL, &got);
        if (r == LIBDEFLATE_SUCCESS) { *out = buf; *out_size = got; return ST_OK; }
        mem_free(buf);
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE) return diag_fail(diag, ST_CORRUPT, "bloque comprimido invalido");
        if (cap >= max_out) return diag_fail(diag, ST_TOO_LARGE, "bloque comprimido de mas de %zu MB", max_out >> 20);
        cap = MIN(cap * 4, max_out);
    }
}

// =============================================================================================
// Compresion
// =============================================================================================

static void zlib_header(u8 out[2], int level) {
    // FLEVEL es informativo (0 = mas rapido .. 3 = maxima); FCHECK hace el par multiplo de 31.
    u32 flevel = level < 2 ? 0 : level < 6 ? 1 : level == 6 ? 2 : 3;
    u32 cmf = 0x78, flg = flevel << 6;
    flg += 31 - ((cmf << 8 | flg) % 31);
    out[0] = (u8)cmf;
    out[1] = (u8)flg;
}

typedef struct ChunkJob {
    const u8 *in;
    size_t    in_size;
    int       level;
    u8       *out;
    size_t    out_size;
    u32       adler;
    bool      final;
    bool      failed;
} ChunkJob;

static void compress_chunk(void *ctx, size_t index) {
    ChunkJob *job = (ChunkJob *)ctx + index;
    struct libdeflate_compressor *c = deflater(job->level);
    if (!c) { job->failed = true; return; }
    size_t bound = libdeflate_deflate_compress_bound(c, job->in_size) + SYNC_FLUSH_SLACK;
    job->out = mem_alloc(bound);
    if (!job->out) { job->failed = true; return; }
    job->out_size = libdeflate_deflate_compress_chunk(c, job->in, job->in_size, job->out, bound, job->final);
    job->adler = libdeflate_adler32(1, job->in, job->in_size);
    if (!job->out_size) job->failed = true;
}

static size_t choose_chunk_size(size_t in_size) {
    // Dos tramos por hilo balancean bien sin achicar tanto los tramos que se note la perdida.
    size_t per_thread = in_size / ((size_t)pool_threads() * 2);
    return MAX(per_thread, ZLIB_PARALLEL_MIN_CHUNK);
}

Status zlib_compress(const u8 *in, size_t in_size, int level, bool parallel,
                     u8 **out, size_t *out_size, Diag *diag) {
    *out = NULL;
    *out_size = 0;
    size_t chunk = choose_chunk_size(in_size);
    size_t pieces = (in_size + chunk - 1) / chunk;
    if (!parallel || pool_threads() == 1 || pieces < 2) {
        struct libdeflate_compressor *c = deflater(level);
        if (!c) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para el compresor");
        size_t bound = libdeflate_zlib_compress_bound(c, in_size);
        u8 *buf = mem_alloc(bound);
        if (!buf) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para comprimir");
        size_t n = libdeflate_zlib_compress(c, in, in_size, buf, bound);
        if (!n) { mem_free(buf); return diag_fail(diag, ST_NO_MEMORY, "fallo interno del compresor"); }
        *out = buf;
        *out_size = n;
        return ST_OK;
    }

    ChunkJob *jobs = mem_calloc(pieces * sizeof *jobs);
    if (!jobs) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para comprimir en paralelo");
    for (size_t i = 0; i < pieces; i++) {
        jobs[i].in = in + i * chunk;
        jobs[i].in_size = MIN(chunk, in_size - i * chunk);
        jobs[i].level = level;
        jobs[i].final = i + 1 == pieces;
    }
    parallel_for(pieces, compress_chunk, jobs);

    Status status = ST_OK;
    size_t total = 2 + 4;
    for (size_t i = 0; i < pieces && status == ST_OK; i++) {
        if (jobs[i].failed) status = diag_fail(diag, ST_NO_MEMORY, "fallo un tramo de la compresion paralela");
        else total += jobs[i].out_size;
    }
    u8 *buf = status == ST_OK ? mem_alloc(total) : NULL;
    if (status == ST_OK && !buf) status = diag_fail(diag, ST_NO_MEMORY, "sin memoria para juntar la salida");
    if (status == ST_OK) {
        zlib_header(buf, level);
        size_t at = 2;
        u32 adler = jobs[0].adler;
        for (size_t i = 0; i < pieces; i++) {
            memcpy(buf + at, jobs[i].out, jobs[i].out_size);
            at += jobs[i].out_size;
            if (i) adler = adler32_combine(adler, jobs[i].adler, jobs[i].in_size);
        }
        store_be32(buf + at, adler);
        *out = buf;
        *out_size = at + 4;
    }
    for (size_t i = 0; i < pieces; i++) mem_free(jobs[i].out);
    mem_free(jobs);
    return status;
}

static _Thread_local u8    *tls_quick_buf;
static _Thread_local size_t tls_quick_cap;

size_t zlib_quick_size(const u8 *in, size_t in_size, int level) {
    struct libdeflate_compressor *c = deflater(level);
    if (!c) return SIZE_MAX;
    size_t bound = libdeflate_deflate_compress_bound(c, in_size);
    if (bound > tls_quick_cap) {
        mem_free(tls_quick_buf);
        tls_quick_cap = MAX(bound, (size_t)64 << 10);
        tls_quick_buf = mem_alloc(tls_quick_cap);
        if (!tls_quick_buf) { tls_quick_cap = 0; return SIZE_MAX; }
    }
    size_t n = libdeflate_deflate_compress(c, in, in_size, tls_quick_buf, tls_quick_cap);
    return n ? n : SIZE_MAX;
}

size_t zlib_compressed_size(const u8 *in, size_t in_size, int level, bool parallel) {
    u8 *out;
    size_t size;
    Diag scratch = { 0 };
    if (zlib_compress(in, in_size, level, parallel, &out, &size, &scratch) != ST_OK) return SIZE_MAX;
    mem_free(out);
    return size;
}
