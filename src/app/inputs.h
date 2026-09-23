// app/inputs.h - de lo que escribio el usuario a la lista de archivos a procesar.
//
//   archivo       tal cual (cualquier extension: el formato se detecta por el contenido)
//   carpeta       sus imagenes (con -r, tambien las de las subcarpetas; no sigue junctions)
//   comodines     "fotos\*.jpg", "c:\capturas\2026-??-*.png" (Windows no los expande solo)
//   -             la entrada estandar
// Las rutas repetidas (mismo archivo por dos caminos) se procesan una vez: conjunto hash de
// rutas absolutas normalizadas sin distinguir mayusculas (como NTFS).
#pragma once
#include "../core/base.h"
#include "../core/mem.h"
#include "../core/status.h"
#include <wchar.h>

typedef struct InputFile {
    wchar_t *path;            // absoluta; NULL = stdin
    wchar_t *root;            // carpeta dada por el usuario (para --keep-tree) o NULL
    char    *display;         // como mostrarla (relativa a lo que escribio el usuario), UTF-8
    u64      size;
    bool     missing;         // no existe: se reporta como error de ese archivo
} InputFile;

typedef struct InputOptions {
    bool              recursive;
    const char *const *extensions;     // extensiones de imagen conocidas (minusculas, sin punto)
    size_t            extension_count;
} InputOptions;

// `positionals` = char* UTF-8. Agrega a `out` (InputFile). Los avisos (carpeta vacia, comodin sin
// coincidencias) van a `diag` como avisos; nada de esto es fatal.
Status inputs_expand(const Vec *positionals, const InputOptions *opt, Vec *out, Diag *diag);
void   inputs_free(Vec *files);
