// core/base.h - tipos, macros y utilidades de bajo nivel que usa todo prisma.
//
// prisma se compila SOLO con clang-cl (viene con Visual Studio, componente "C++ Clang tools").
// Eso nos deja usar sus builtins (__builtin_mul_overflow, __builtin_expect, __builtin_ctz) y el
// atributo target("avx2") para multiversionar los nucleos calientes: la misma funcion se escribe
// una sola vez como plantilla inline y se instancia dos veces (base SSE4.1 y AVX2); el despacho
// se decide una vez al arrancar segun la CPU (ver core/cpu.h).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__clang__)
#  error "prisma se compila con clang-cl (Visual Studio > Desarrollo de escritorio con C++ > C++ Clang tools)"
#endif

// Build con AddressSanitizer (build.ps1 -Mode asan): la memoria se reserva de forma que ASan vea
// cada byte de mas (ver core/mem.c).
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define PRISMA_ASAN 1
#  endif
#endif
#ifndef PRISMA_ASAN
#  define PRISMA_ASAN 0
#endif

#define PRISMA_NAME         "prisma"
#define PRISMA_VERSION      "1.0.0"

#define LIKELY(x)     __builtin_expect(!!(x), 1)
#define UNLIKELY(x)   __builtin_expect(!!(x), 0)
#define INLINE        static inline __attribute__((always_inline))
#define NOINLINE      __attribute__((noinline))
#define TARGET_AVX2   __attribute__((target("avx2,fma,bmi,bmi2,lzcnt,popcnt")))
#define ALIGN(n)      __attribute__((aligned(n)))
#define UNUSED(x)     ((void)(x))
#define ARRAY_LEN(a)  (sizeof(a) / sizeof((a)[0]))
#define PRINTF_LIKE(fmt_index, first_arg) __attribute__((format(printf, fmt_index, first_arg)))

#define MIN(a, b)         ((a) < (b) ? (a) : (b))
#define MAX(a, b)         ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi)  ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;

// Toda cuenta de tamanos de buffer que venga de un archivo pasa por aca: un encabezado malicioso
// con 65535 x 65535 x 16 bytes no puede dar la vuelta y pedir un buffer chico.
INLINE bool mul_ok(size_t a, size_t b, size_t *out) { return !__builtin_mul_overflow(a, b, out); }
INLINE bool add_ok(size_t a, size_t b, size_t *out) { return !__builtin_add_overflow(a, b, out); }

// ---- enteros sin alineacion, con endianness explicita --------------------------------------
INLINE u16 load_le16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }
INLINE u32 load_le32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
INLINE u64 load_le64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }
INLINE u16 load_be16(const u8 *p) { return (u16)((u16)p[0] << 8 | p[1]); }
INLINE u32 load_be32(const u8 *p) { u32 v; memcpy(&v, p, 4); return __builtin_bswap32(v); }
INLINE u64 load_be64(const u8 *p) { u64 v; memcpy(&v, p, 8); return __builtin_bswap64(v); }
INLINE void store_le16(u8 *p, u16 v) { memcpy(p, &v, 2); }
INLINE void store_le32(u8 *p, u32 v) { memcpy(p, &v, 4); }
INLINE void store_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
INLINE void store_be32(u8 *p, u32 v) { v = __builtin_bswap32(v); memcpy(p, &v, 4); }

INLINE float f32_from_bits(u32 b) { float f; memcpy(&f, &b, 4); return f; }
INLINE u32   f32_bits(float f)    { u32 b; memcpy(&b, &f, 4); return b; }

// ---- conversiones exactas entre 8 y 16 bits por canal --------------------------------------
// 8 -> 16 replica el byte (v * 257): 0 -> 0, 255 -> 65535, y la vuelta es exacta.
INLINE u16 u8_to_u16(u8 v) { return (u16)(v * 257u); }
// 16 -> 8 redondeando al mas cercano: round(v / 257) == (v * 255 + 32895) >> 16 para todo v de
// 16 bits (lo verifica tests/unit a fuerza bruta). Sin division.
INLINE u8 u16_to_u8(u16 v) { return (u8)((v * 255u + 32895u) >> 16); }

// Division por 255 con redondeo, exacta para todo x en [0, 255*255]: el truco de Blinn.
INLINE u32 div255_round(u32 x) { x += 128; return (x + (x >> 8)) >> 8; }

INLINE u32 u32_div_round(u32 num, u32 den) { return (num + den / 2) / den; }
INLINE bool is_pow2(size_t x) { return x && !(x & (x - 1)); }
INLINE size_t align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }
INLINE u32 bit_width(u32 x) { return x ? 32u - (u32)__builtin_clz(x) : 0; }

INLINE float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
INLINE float saturatef(float x) { return x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x; }
