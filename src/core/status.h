// core/status.h - codigos de resultado y diagnostico por trabajo.
//
// Cada archivo que se convierte tiene su propio Diag: el primer error gana (es el que explica la
// causa; los que vienen despues suelen ser consecuencia) y los avisos no fatales se juntan
// aparte para mostrarlos en el reporte ("el BMP decia tener alfa pero era todo 0: se trato
// como opaco"). Nada de estado global: los hilos no se pisan.
#pragma once
#include "base.h"

typedef enum Status {
    ST_OK = 0,
    ST_UNSUPPORTED,      // formato reconocido, variante no soportada
    ST_UNKNOWN_FORMAT,   // ningun decodificador reconocio los datos
    ST_CORRUPT,          // datos mal formados
    ST_TRUNCATED,        // los datos terminan antes de tiempo
    ST_TOO_LARGE,        // supera los limites (pixeles, memoria)
    ST_NO_MEMORY,
    ST_IO,               // fallo de lectura/escritura
    ST_INVALID,          // argumento u opcion invalida
    ST_CANCELLED,        // Ctrl+C
    ST_SKIPPED,          // omitido a proposito (ya existe, misma ruta que la entrada...)
    ST__COUNT
} Status;

enum { DIAG_MESSAGE_CAP = 320, DIAG_WARNING_CAP = 200, DIAG_MAX_WARNINGS = 8 };

typedef struct Diag {
    Status status;
    char   message[DIAG_MESSAGE_CAP];
    u32    warning_count;                       // total (puede superar los guardados)
    char   warnings[DIAG_MAX_WARNINGS][DIAG_WARNING_CAP];
} Diag;

// Registra el error (solo si no habia uno) y lo devuelve, para escribir `return diag_fail(...)`.
Status diag_fail(Diag *d, Status status, const char *fmt, ...) PRINTF_LIKE(3, 4);
void   diag_warn(Diag *d, const char *fmt, ...) PRINTF_LIKE(2, 3);
void   diag_reset(Diag *d);
const char *status_label(Status status);        // texto corto en castellano
