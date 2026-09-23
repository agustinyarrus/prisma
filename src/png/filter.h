// png/filter.h - filtros de fila de PNG y las heuristicas para elegir uno por fila.
//
// Cada fila de un PNG puede guardarse "tal cual" o como diferencia con su vecino (izquierda,
// arriba, promedio o Paeth); el deflate comprime mucho mejor diferencias chicas. Elegir bien el
// filtro fila por fila es la mitad del tamano final. Estrategias:
//   none/sub/up/avg/paeth  un filtro fijo para todas las filas
//   minsum                 el de menor suma de |diferencia| (la heuristica clasica de libpng)
//   entropy                el de menor entropia de Shannon de los bytes de la fila
//   bigrams                el de menos pares de bytes distintos (patrones repetidos)
//   brute                  el que de verdad comprime menos (deflate de prueba con contexto)
// Las decisiones por fila son independientes: todo corre en paralelo por bandas. El nucleo se
// compila dos veces (base y AVX2) desde la misma fuente y se elige al arrancar.
#pragma once
#include "pack.h"

typedef enum FilterStrategy {
    FS_NONE = 0, FS_SUB, FS_UP, FS_AVG, FS_PAETH,    // = tipo de filtro PNG
    FS_MINSUM, FS_ENTROPY, FS_BIGRAMS, FS_BRUTE,
    FS__COUNT
} FilterStrategy;

typedef struct Filtered {
    u8    *data;                // height * (1 + row_bytes)
    size_t size;
    u64    usage[5];            // filas por tipo de filtro (para el reporte)
} Filtered;

const char *filter_strategy_name(FilterStrategy s);
bool        filter_strategy_parse(const char *name, FilterStrategy *out);

Status filter_packed(const Packed *packed, u32 bpp, FilterStrategy strategy, Filtered *out, Diag *diag);
void   filtered_free(Filtered *f);
