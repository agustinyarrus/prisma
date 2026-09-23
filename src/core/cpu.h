// core/cpu.h - que sabe hacer esta CPU, detectado una sola vez.
//
// Los nucleos calientes (filtros PNG, analisis de color, conversiones de fila) tienen dos
// versiones compiladas de la MISMA fuente: base (x86-64-v2, SSE4.2) y AVX2. El despacho se
// resuelve al arrancar con estas banderas; no hay un if por pixel.
#pragma once
#include "base.h"

typedef struct CpuInfo {
    bool sse41;
    bool avx2;             // AVX2 + FMA + BMI2 y el SO guarda los registros YMM
    bool avx512;           // F+BW+VL, solo informativo
    u32  logical_cores;
    u32  physical_cores;
    char brand[64];
} CpuInfo;

const CpuInfo *cpu_info(void);
