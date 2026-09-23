// core/fileio.h - lectura y escritura de archivos, con rutas UTF-16 y escritura atomica.
//
//  - Lectura: los archivos grandes (>= FILE_MAP_THRESHOLD) se mapean en memoria: el decodificador
//    lee directo de la cache del SO, sin copia. Los chicos se leen de una: mapear cuesta un fallo
//    de pagina por cada 4 KB y para un icono de 3 KB es mas caro que un ReadFile.
//  - Escritura atomica: se escribe a un temporal en la MISMA carpeta y se renombra al final. Si el
//    proceso muere a mitad de camino (Ctrl+C, corte de luz) nunca queda un PNG a medias con el
//    nombre final, y el archivo anterior sigue intacto hasta el ultimo instante.
//  - Rutas largas: todo lo que toca el disco pasa por path_for_io(), que antepone \\?\ cuando la
//    ruta supera MAX_PATH (Windows sigue teniendo ese limite si no se lo esquiva).
#pragma once
#include "base.h"
#include "status.h"
#include <wchar.h>

#define FILE_MAP_THRESHOLD ((u64)64 << 20)

typedef struct FileData {
    const u8 *data;
    size_t    size;
    void     *owned;       // buffer propio (lectura normal) o NULL
    void     *view;        // vista mapeada o NULL
} FileData;

Status file_read_all(const wchar_t *path, FileData *out, Diag *diag);
Status stdin_read_all(FileData *out, Diag *diag);
void   file_data_free(FileData *f);

typedef enum WriteMode {
    WRITE_REPLACE,         // pisa si existe
    WRITE_NO_CLOBBER,      // si existe -> ST_SKIPPED
} WriteMode;

Status file_write_atomic(const wchar_t *path, const void *data, size_t size, WriteMode mode, Diag *diag);
Status stdout_write_all(const void *data, size_t size, Diag *diag);

bool   path_exists(const wchar_t *path);
bool   path_is_directory(const wchar_t *path);
bool   ensure_directory(const wchar_t *path);               // crea toda la cadena de carpetas
wchar_t *path_full(const wchar_t *path);                     // absoluta y normalizada (mem_alloc)
wchar_t *path_for_io(const wchar_t *path);                   // \\?\ si hace falta (mem_alloc)
u64    file_mtime(const wchar_t *path);                      // FILETIME como u64, 0 si falla
void   file_set_mtime(const wchar_t *path, u64 filetime);
u64    file_size(const wchar_t *path);

// Partes de una ruta (punteros dentro de la misma cadena).
const wchar_t *path_basename(const wchar_t *path);          // "c:\a\foto.jpg" -> "foto.jpg"
const wchar_t *path_extension(const wchar_t *path);         // -> ".jpg" (o "" si no tiene)
