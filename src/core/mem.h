// core/mem.h - memoria alineada para buffers de pixeles.
//
// Dos caminos segun el tamano, invisibles para quien llama:
//  - chico  (< MEM_LARGE_THRESHOLD): heap del CRT alineado a 64 bytes;
//  - grande (>= umbral): VirtualAlloc directo. El SO entrega paginas ya en cero y recien las
//    materializa al tocarlas, asi que un calloc de 400 MB cuesta lo mismo que un malloc y la
//    memoria que un decodificador reserva "por las dudas" y no usa no se paga.
// Todo bloque lleva un encabezado de 64 bytes (tamano + tipo) y 64 bytes de cola: los nucleos
// SIMD pueden leer un vector de mas al final de una fila sin salirse de lo reservado.
#pragma once
#include "base.h"

enum { MEM_ALIGN = 64, MEM_TAIL_SLACK = 64 };
#define MEM_LARGE_THRESHOLD ((size_t)1 << 20)

void *mem_alloc(size_t size);               // alineado a 64, contenido indefinido
void *mem_calloc(size_t size);              // alineado a 64, en cero
void *mem_realloc(void *ptr, size_t size);  // conserva el contenido (min de ambos tamanos)
void  mem_free(void *ptr);                  // acepta NULL
size_t mem_size(const void *ptr);           // tamano pedido

char *mem_strdup(const char *s);

// Arreglo dinamico minimo: crece x1.5, usado por listas de trabajos, cuadros, chunks, etc.
typedef struct Vec {
    void  *items;
    size_t count;
    size_t capacity;
    size_t item_size;
} Vec;

void  vec_init(Vec *v, size_t item_size);
void *vec_push(Vec *v);                     // devuelve el slot nuevo (en cero) o NULL
bool  vec_reserve(Vec *v, size_t capacity);
void  vec_free(Vec *v);
#define VEC_AT(v, type, i) (((type *)(v)->items)[i])
