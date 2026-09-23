// core/mem.c - ver mem.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <malloc.h>
#include "mem.h"

enum { KIND_HEAP = 0x48454150u /* 'HEAP' */, KIND_VIRTUAL = 0x5649524Du /* 'VIRM' */ };

typedef struct BlockHeader {
    size_t size;
    u32    kind;
    u32    guard;
    u8     pad[MEM_ALIGN - sizeof(size_t) - 2 * sizeof(u32)];
} BlockHeader;
_Static_assert(sizeof(BlockHeader) == MEM_ALIGN, "el encabezado debe preservar la alineacion");

#if PRISMA_ASAN
// Con ASan: la holgura del final se envenena, asi una lectura o escritura de un byte pasado lo
// pedido explota en el acto (sin esto, los 64 bytes de cola esconderian justo el bug mas comun de
// un decodificador). Y todo va al heap del CRT: ASan no ve lo que se reserva con VirtualAlloc.
void __asan_poison_memory_region(void const volatile *addr, size_t size);
void __asan_unpoison_memory_region(void const volatile *addr, size_t size);
#  define POISON_TAIL(user, size)   __asan_poison_memory_region((u8 *)(user) + (size), MEM_TAIL_SLACK)
#  define UNPOISON_TAIL(user, size) __asan_unpoison_memory_region((u8 *)(user) + (size), MEM_TAIL_SLACK)
#  define USE_VIRTUAL_ALLOC 0
#else
#  define POISON_TAIL(user, size)   ((void)0)
#  define UNPOISON_TAIL(user, size) ((void)0)
#  define USE_VIRTUAL_ALLOC 1
#endif

static void *allocate(size_t size, bool zero) {
    size_t total;
    if (!add_ok(size, sizeof(BlockHeader) + MEM_TAIL_SLACK, &total)) return NULL;
    BlockHeader *h;
    if (USE_VIRTUAL_ALLOC && total >= MEM_LARGE_THRESHOLD) {
        // VirtualAlloc entrega paginas en cero: el pedido "zero" sale gratis.
        h = VirtualAlloc(NULL, total, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!h) return NULL;
        h->kind = KIND_VIRTUAL;
    } else {
        h = _aligned_malloc(total, MEM_ALIGN);
        if (!h) return NULL;
        if (zero) memset(h, 0, total);
        h->kind = KIND_HEAP;
    }
    h->size = size;
    h->guard = ~h->kind;
    POISON_TAIL(h + 1, size);
    return h + 1;
}

static BlockHeader *header_of(const void *ptr) {
    BlockHeader *h = (BlockHeader *)ptr - 1;
    // Un puntero que no salio de aca es un bug propio: fallar fuerte, no corromper el heap.
    if (h->guard != ~h->kind || (h->kind != KIND_HEAP && h->kind != KIND_VIRTUAL)) __builtin_trap();
    return h;
}

void *mem_alloc(size_t size)  { return allocate(size, false); }
void *mem_calloc(size_t size) { return allocate(size, true); }

void mem_free(void *ptr) {
    if (!ptr) return;
    BlockHeader *h = header_of(ptr);
    u32 kind = h->kind;
    UNPOISON_TAIL(ptr, h->size);
    h->guard = 0;
    if (kind == KIND_VIRTUAL) VirtualFree(h, 0, MEM_RELEASE);
    else _aligned_free(h);
}

size_t mem_size(const void *ptr) { return ptr ? header_of(ptr)->size : 0; }

void *mem_realloc(void *ptr, size_t size) {
    if (!ptr) return mem_alloc(size);
    size_t old = header_of(ptr)->size;
    // Achicar poco: no mover. Con ASan si, para que el bloque mida exactamente lo pedido.
    if (!PRISMA_ASAN && size <= old && old - size < MEM_LARGE_THRESHOLD) return ptr;
    void *fresh = mem_alloc(size);
    if (!fresh) return NULL;
    memcpy(fresh, ptr, MIN(old, size));
    mem_free(ptr);
    return fresh;
}

char *mem_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *copy = mem_alloc(n);
    if (copy) memcpy(copy, s, n);
    return copy;
}

void vec_init(Vec *v, size_t item_size) {
    v->items = NULL;
    v->count = v->capacity = 0;
    v->item_size = item_size;
}

bool vec_reserve(Vec *v, size_t capacity) {
    if (capacity <= v->capacity) return true;
    size_t bytes;
    if (!mul_ok(capacity, v->item_size, &bytes)) return false;
    void *items = mem_realloc(v->items, bytes);
    if (!items) return false;
    v->items = items;
    v->capacity = capacity;
    return true;
}

void *vec_push(Vec *v) {
    if (v->count == v->capacity) {
        size_t grown = v->capacity < 8 ? 8 : v->capacity + v->capacity / 2;
        if (!vec_reserve(v, grown)) return NULL;
    }
    u8 *slot = (u8 *)v->items + v->count * v->item_size;
    memset(slot, 0, v->item_size);
    v->count++;
    return slot;
}

void vec_free(Vec *v) {
    mem_free(v->items);
    v->items = NULL;
    v->count = v->capacity = 0;
}
