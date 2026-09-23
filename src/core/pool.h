// core/pool.h - pool de hilos con paralelismo anidado sin deadlocks.
//
// Un solo pool para todo el proceso. El lote reparte archivos entre hilos y, adentro de cada
// archivo, el analisis, los filtros PNG y el deflate por tramos vuelven a repartir filas en el
// MISMO pool. Eso funciona porque quien espera no se duerme mientras haya trabajo: ayuda
// ("work helping"). Un hilo que espera su trabajo toma tareas de su propio trabajo o de los mas
// nuevos que el (anidados), nunca de los mas viejos (asi un archivo no queda frenado detras de
// otro archivo entero). Como la anidacion es un arbol, nunca hay ciclos de espera.
//
// Toda la contabilidad va bajo un SRWLOCK con una variable de condicion; las tareas corren
// afuera del lock. Las tareas deben ser gruesas (bandas de filas, archivos): el costo del lock
// por tarea es de decenas de nanosegundos.
#pragma once
#include "base.h"

typedef void (*PoolTask)(void *ctx, size_t index);
typedef void (*PoolRange)(void *ctx, size_t begin, size_t end);

void pool_start(u32 threads);      // hilos TOTALES contando al que llama (crea threads - 1)
void pool_stop(void);
u32  pool_threads(void);           // 1 si no se inicio

// Ejecuta fn(ctx, i) para i en [0, count) y vuelve cuando todas terminaron.
void parallel_for(size_t count, PoolTask fn, void *ctx);

// Parte [0, total) en tramos de al menos `grain` elementos (a lo sumo ~4 por hilo) y ejecuta
// fn(ctx, begin, end) en paralelo. Con total chico corre todo en el hilo actual.
void parallel_range(size_t total, size_t grain, PoolRange fn, void *ctx);

// Cancelacion cooperativa (Ctrl+C): las tareas largas la consultan entre bandas.
void pool_request_cancel(void);
bool pool_cancelled(void);
