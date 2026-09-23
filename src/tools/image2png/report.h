// tools/image2png/report.h - todo lo que image2png muestra.
#pragma once
#include "job.h"

void report_header(const Config *cfg, size_t jobs, u64 total_bytes, u32 threads);
void report_job(const Config *cfg, const Job *job);                    // seguro entre hilos
void report_progress(u64 done, u64 total, u64 bytes_done, u64 bytes_total, double elapsed);
void report_summary(const Config *cfg, const Job *jobs, size_t count, double elapsed, u32 threads);
void report_error(const char *title, const char *message);             // errores de uso/arranque

// --info: analiza una entrada (sin convertir) y muestra su tarjeta. Devuelve el estado.
Status report_info(const Config *cfg, Job *job);
void   report_formats(void);
void   report_version(void);
