// tools/image2png/job.h - un archivo a convertir: de donde sale, a donde va y como le fue.
#pragma once
#include "config.h"
#include "../../app/inputs.h"

typedef struct JobTimes {
    double read, decode, process, encode, write;
} JobTimes;

typedef struct Job {
    const InputFile *in;
    wchar_t         *output;          // ruta de salida; NULL = stdout
    bool             to_stdout;
    bool             skip;            // decidido al planificar (misma ruta que la entrada, etc.)
    char             skip_reason[160];
    // resultado
    Status           status;
    Diag             diag;
    JobTimes         t;
    u64              in_bytes, out_bytes;
    u32              out_files;
    u32              width, height;   // de la salida
    u32              frames_in, frames_out;
    SourceInfo       source;
    EncodeReport     enc;
    PipelineReport   pipe;
    char            *output_display;  // nombre de salida para mostrar (UTF-8)
    bool             done;
} Job;

// Decide la ruta de salida de cada trabajo (y cuales se omiten). Valida combinaciones de -o.
Status plan_outputs(Job *jobs, size_t count, const Config *cfg, Diag *diag);

// Ruta de un cuadro/pagina: "base-007.png" (el ancho del numero sale de la cantidad total).
wchar_t *frame_output_path(const wchar_t *base_png, u32 index, u32 total);

// Convierte un trabajo (lee, decodifica, procesa, codifica y escribe). Nunca aborta el proceso: un
// archivo roto o una excepcion quedan registrados en job->status/diag.
void convert_job(Job *job, const Config *cfg);
