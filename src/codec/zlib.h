// codec/zlib.h - DEFLATE/zlib sobre libdeflate: inflado exacto, rescate y compresion paralela.
//
//  - Inflado: libdeflate descomprime en una sola pasada al buffer final (sin streaming, que es
//    lo que la hace ~2x mas rapida que zlib). El adler32 NO se verifica: cada chunk de PNG ya
//    trae su CRC32 y verificarlo dos veces es trabajo perdido.
//  - Rescate: si el flujo esta cortado (descarga incompleta), un inflador propio simple decodifica
//    todo lo que se pueda; el decodificador PNG muestra las filas recuperadas (como un navegador)
//    en lugar de fallar entero.
//  - Compresion paralela (la tecnica de pigz): la entrada se parte en tramos que se comprimen en
//    paralelo; todos menos el ultimo terminan con un "sync flush" (bloque guardado vacio que
//    alinea a byte) y se concatenan en UN flujo valido. El adler32 total se arma combinando los
//    parciales con aritmetica modular, sin volver a recorrer los datos.
#pragma once
#include "../core/base.h"
#include "../core/status.h"

typedef struct ZSpan {
    const u8 *data;
    size_t    size;
} ZSpan;

u32 crc32_update(u32 crc, const void *data, size_t size);
u32 adler32_update(u32 adler, const void *data, size_t size);
u32 adler32_combine(u32 adler_a, u32 adler_b, size_t size_b);

// Infla un flujo zlib (repartido en varios tramos, p.ej. muchos IDAT) a exactamente out_size
// bytes. ST_OK si salio completo; ST_TRUNCATED si solo se recupero una parte (`*produced`).
Status zlib_inflate_exact(const ZSpan *spans, size_t span_count, u8 *out, size_t out_size,
                          size_t *produced, Diag *diag);

// Infla un flujo zlib de tamano desconocido (perfiles ICC, texto comprimido). mem_alloc.
Status zlib_inflate_alloc(const u8 *in, size_t in_size, size_t max_out, u8 **out, size_t *out_size, Diag *diag);

// Tramo minimo para paralelizar: por debajo, un solo flujo (la perdida de razon de cortar el
// diccionario de 32 KB entre tramos es ~0,1 % con tramos de 1 MB; ver tests/bench).
#define ZLIB_PARALLEL_MIN_CHUNK ((size_t)1 << 20)

// Comprime a zlib. `parallel` habilita los tramos (se ignora con entradas chicas o un hilo).
Status zlib_compress(const u8 *in, size_t in_size, int level, bool parallel,
                     u8 **out, size_t *out_size, Diag *diag);

// Solo mide: tamano del flujo comprimido (para comparar estrategias sin guardar la salida).
size_t zlib_compressed_size(const u8 *in, size_t in_size, int level, bool parallel);

// Tamano DEFLATE crudo de un bloque chico, con buffer de salida reutilizado por hilo (filtro
// "brute": miles de mediciones por imagen, sin reservar memoria en cada una).
size_t zlib_quick_size(const u8 *in, size_t in_size, int level);
