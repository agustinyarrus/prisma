// core/status.c - ver status.h
#include "status.h"
#include <stdarg.h>
#include <stdio.h>

static const char *const STATUS_LABELS[ST__COUNT] = {
    [ST_OK]             = "ok",
    [ST_UNSUPPORTED]    = "variante no soportada",
    [ST_UNKNOWN_FORMAT] = "formato desconocido",
    [ST_CORRUPT]        = "datos corruptos",
    [ST_TRUNCATED]      = "archivo truncado",
    [ST_TOO_LARGE]      = "demasiado grande",
    [ST_NO_MEMORY]      = "sin memoria",
    [ST_IO]             = "error de E/S",
    [ST_INVALID]        = "argumento inválido",
    [ST_CANCELLED]      = "cancelado",
    [ST_SKIPPED]        = "omitido",
};

const char *status_label(Status status) {
    return (unsigned)status < ST__COUNT ? STATUS_LABELS[status] : "?";
}

Status diag_fail(Diag *d, Status status, const char *fmt, ...) {
    if (d && d->status == ST_OK) {
        d->status = status;
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(d->message, sizeof d->message, fmt, ap);
        va_end(ap);
    }
    return status;
}

void diag_warn(Diag *d, const char *fmt, ...) {
    if (!d) return;
    // Avisos repetidos (el mismo en cada cuadro de un GIF) se cuentan una sola vez.
    char text[DIAG_WARNING_CAP];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    u32 stored = MIN(d->warning_count, (u32)DIAG_MAX_WARNINGS);
    for (u32 i = 0; i < stored; i++)
        if (strcmp(d->warnings[i], text) == 0) return;
    if (d->warning_count < DIAG_MAX_WARNINGS)
        memcpy(d->warnings[d->warning_count], text, sizeof text);
    d->warning_count++;
}

void diag_reset(Diag *d) {
    d->status = ST_OK;
    d->message[0] = 0;
    d->warning_count = 0;
}
