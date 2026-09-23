// core/cpu.c - ver cpu.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <immintrin.h>
#include "cpu.h"
#include "mem.h"

static CpuInfo g_cpu;
static INIT_ONCE g_cpu_once = INIT_ONCE_STATIC_INIT;

static u32 count_physical_cores(void) {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, NULL, &len);
    if (!len) return 0;
    u8 *buf = mem_alloc(len);
    if (!buf) return 0;
    u32 cores = 0;
    if (GetLogicalProcessorInformationEx(RelationProcessorCore, (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)buf, &len)) {
        for (DWORD off = 0; off < len;) {
            PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX info = (PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX)(buf + off);
            if (info->Relationship == RelationProcessorCore) cores++;
            off += info->Size;
        }
    }
    mem_free(buf);
    return cores;
}

static BOOL CALLBACK detect(PINIT_ONCE once, PVOID param, PVOID *ctx) {
    UNUSED(once); UNUSED(param); UNUSED(ctx);
    int r[4];
    __cpuid(r, 0);
    int max_leaf = r[0];
    __cpuid(r, 1);
    bool osxsave = (r[2] >> 27) & 1, avx = (r[2] >> 28) & 1, fma = (r[2] >> 12) & 1;
    g_cpu.sse41 = (r[2] >> 19) & 1;
    bool ymm_saved = false, zmm_saved = false;
    if (osxsave) {
        u64 xcr0 = _xgetbv(0);
        ymm_saved = (xcr0 & 0x6) == 0x6;
        zmm_saved = (xcr0 & 0xE6) == 0xE6;
    }
    if (max_leaf >= 7) {
        __cpuidex(r, 7, 0);
        bool avx2 = (r[1] >> 5) & 1, bmi2 = (r[1] >> 8) & 1;
        bool f = (r[1] >> 16) & 1, bw = (r[1] >> 30) & 1, vl = (r[1] >> 31) & 1;
        g_cpu.avx2 = avx && avx2 && fma && bmi2 && ymm_saved;
        g_cpu.avx512 = f && bw && vl && zmm_saved;
    }
    __cpuid(r, (int)0x80000000);
    if ((u32)r[0] >= 0x80000004u) {
        for (int i = 0; i < 3; i++) {
            __cpuid(r, (int)(0x80000002u + (u32)i));
            memcpy(g_cpu.brand + 16 * i, r, 16);
        }
        g_cpu.brand[48] = 0;
        // "  Intel(R) Core..." viene con espacios a la izquierda en algunas CPUs.
        char *b = g_cpu.brand;
        while (*b == ' ') b++;
        memmove(g_cpu.brand, b, strlen(b) + 1);
    }
    g_cpu.logical_cores = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (!g_cpu.logical_cores) g_cpu.logical_cores = 1;
    g_cpu.physical_cores = count_physical_cores();
    if (!g_cpu.physical_cores) g_cpu.physical_cores = g_cpu.logical_cores;
    // PRISMA_NO_AVX2=1 fuerza la ruta base: sirve para probar que las dos dan lo mismo.
    char env[8];
    if (GetEnvironmentVariableA("PRISMA_NO_AVX2", env, sizeof env) && env[0] == '1') g_cpu.avx2 = false;
    return TRUE;
}

const CpuInfo *cpu_info(void) {
    InitOnceExecuteOnce(&g_cpu_once, detect, NULL, NULL);
    return &g_cpu;
}
