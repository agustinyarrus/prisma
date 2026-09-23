// core/fileio.c - ver fileio.h
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "fileio.h"
#include "mem.h"
#include "text.h"

enum { IO_CHUNK = 1 << 30 };           // ReadFile/WriteFile aceptan DWORD: de a 1 GB
enum { LONG_PATH_LIMIT = MAX_PATH - 12 };

// ---- rutas -----------------------------------------------------------------------------

wchar_t *path_full(const wchar_t *path) {
    DWORD n = GetFullPathNameW(path, 0, NULL, NULL);
    if (!n) return wstr_dup(path);
    wchar_t *full = mem_alloc((size_t)n * sizeof(wchar_t));
    if (!full) return NULL;
    if (!GetFullPathNameW(path, n, full, NULL)) { mem_free(full); return wstr_dup(path); }
    return full;
}

wchar_t *path_for_io(const wchar_t *path) {
    if (wcslen(path) < LONG_PATH_LIMIT || wcsncmp(path, L"\\\\?\\", 4) == 0) return wstr_dup(path);
    wchar_t *full = path_full(path);
    if (!full) return NULL;
    wchar_t *out;
    if (wcsncmp(full, L"\\\\", 2) == 0) out = wstr_concat(L"\\\\?\\UNC\\", full + 2);   // \\server\share
    else out = wstr_concat(L"\\\\?\\", full);
    mem_free(full);
    return out;
}

const wchar_t *path_basename(const wchar_t *path) {
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; p++)
        if (*p == L'\\' || *p == L'/' || *p == L':') base = p + 1;
    return base;
}

const wchar_t *path_extension(const wchar_t *path) {
    const wchar_t *base = path_basename(path);
    const wchar_t *dot = wcsrchr(base, L'.');
    return (dot && dot != base) ? dot : base + wcslen(base);
}

bool path_exists(const wchar_t *path) {
    wchar_t *io = path_for_io(path);
    DWORD attrs = io ? GetFileAttributesW(io) : INVALID_FILE_ATTRIBUTES;
    mem_free(io);
    return attrs != INVALID_FILE_ATTRIBUTES;
}

bool path_is_directory(const wchar_t *path) {
    wchar_t *io = path_for_io(path);
    DWORD attrs = io ? GetFileAttributesW(io) : INVALID_FILE_ATTRIBUTES;
    mem_free(io);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

bool ensure_directory(const wchar_t *path) {
    if (!path[0] || path_is_directory(path)) return true;
    wchar_t *full = path_full(path);
    if (!full) return false;
    // Recorre la ruta creando cada nivel que falte ("C:\a\b\c" -> a, a\b, a\b\c).
    bool ok = true;
    size_t len = wcslen(full);
    size_t start = (len > 2 && full[1] == L':') ? 3 : 2;
    for (size_t i = start; i <= len && ok; i++) {
        if (full[i] != L'\\' && full[i] != L'/' && full[i] != 0) continue;
        wchar_t saved = full[i];
        full[i] = 0;
        wchar_t *io = path_for_io(full);
        if (io && !CreateDirectoryW(io, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) ok = false;
        mem_free(io);
        full[i] = saved;
    }
    mem_free(full);
    return ok && path_is_directory(path);
}

static HANDLE open_for_attributes(const wchar_t *path, DWORD access) {
    wchar_t *io = path_for_io(path);
    HANDLE h = io ? CreateFileW(io, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL)
                  : INVALID_HANDLE_VALUE;
    mem_free(io);
    return h;
}

u64 file_mtime(const wchar_t *path) {
    HANDLE h = open_for_attributes(path, FILE_READ_ATTRIBUTES);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILETIME ft;
    u64 t = GetFileTime(h, NULL, NULL, &ft) ? ((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) : 0;
    CloseHandle(h);
    return t;
}

void file_set_mtime(const wchar_t *path, u64 filetime) {
    HANDLE h = open_for_attributes(path, FILE_WRITE_ATTRIBUTES);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft = { (DWORD)filetime, (DWORD)(filetime >> 32) };
    SetFileTime(h, NULL, NULL, &ft);
    CloseHandle(h);
}

u64 file_size(const wchar_t *path) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    wchar_t *io = path_for_io(path);
    bool ok = io && GetFileAttributesExW(io, GetFileExInfoStandard, &info);
    mem_free(io);
    return ok ? ((u64)info.nFileSizeHigh << 32 | info.nFileSizeLow) : 0;
}

// ---- mensajes de error del SO ------------------------------------------------------------

static Status fail_win32(Diag *diag, Status status, const char *what, const wchar_t *path) {
    DWORD code = GetLastError();
    wchar_t wmsg[256] = L"";
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, code,
                   MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), wmsg, ARRAY_LEN(wmsg), NULL);
    for (size_t i = wcslen(wmsg); i > 0 && (wmsg[i - 1] == L'\r' || wmsg[i - 1] == L'\n' || wmsg[i - 1] == L'.'); i--)
        wmsg[i - 1] = 0;
    char *msg = utf16_to_utf8(wmsg);
    char *name = path ? utf16_to_utf8(path_basename(path)) : NULL;
    diag_fail(diag, status, "%s%s%s: %s (Win32 %lu)", what, name ? " " : "", name ? name : "",
              msg ? msg : "?", (unsigned long)code);
    mem_free(msg);
    mem_free(name);
    return status;
}

// ---- lectura ------------------------------------------------------------------------------

Status file_read_all(const wchar_t *path, FileData *out, Diag *diag) {
    memset(out, 0, sizeof *out);
    wchar_t *io = path_for_io(path);
    if (!io) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para la ruta");
    HANDLE h = CreateFileW(io, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    mem_free(io);
    if (h == INVALID_HANDLE_VALUE) return fail_win32(diag, ST_IO, "no se pudo abrir", path);

    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size)) { Status s = fail_win32(diag, ST_IO, "no se pudo medir", path); CloseHandle(h); return s; }
    if ((u64)size.QuadPart > (u64)SIZE_MAX) { CloseHandle(h); return diag_fail(diag, ST_TOO_LARGE, "archivo demasiado grande"); }
    out->size = (size_t)size.QuadPart;
    if (out->size == 0) { CloseHandle(h); return ST_OK; }

    Status status = ST_OK;
    if ((u64)size.QuadPart >= FILE_MAP_THRESHOLD) {
        HANDLE mapping = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
        if (mapping) {
            out->view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
            CloseHandle(mapping);          // la vista mantiene vivo el mapeo
        }
        if (out->view) { out->data = out->view; CloseHandle(h); return ST_OK; }
        // Si no se pudo mapear (p.ej. un recurso de red raro) se cae a la lectura normal.
    }
    u8 *buf = mem_alloc(out->size);
    if (!buf) { CloseHandle(h); return diag_fail(diag, ST_NO_MEMORY, "sin memoria para leer %zu bytes", out->size); }
    for (size_t done = 0; done < out->size;) {
        DWORD want = (DWORD)MIN(out->size - done, (size_t)IO_CHUNK), got = 0;
        if (!ReadFile(h, buf + done, want, &got, NULL)) { status = fail_win32(diag, ST_IO, "error leyendo", path); break; }
        if (got == 0) { status = diag_fail(diag, ST_IO, "el archivo se achico mientras se leia"); break; }
        done += got;
    }
    CloseHandle(h);
    if (status != ST_OK) { mem_free(buf); out->size = 0; return status; }
    out->owned = buf;
    out->data = buf;
    return ST_OK;
}

Status stdin_read_all(FileData *out, Diag *diag) {
    memset(out, 0, sizeof *out);
    HANDLE h = GetStdHandle(STD_INPUT_HANDLE);
    size_t cap = 1 << 20, len = 0;
    u8 *buf = mem_alloc(cap);
    if (!buf) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para stdin");
    for (;;) {
        if (len == cap) {
            u8 *grown = mem_realloc(buf, cap * 2);
            if (!grown) { mem_free(buf); return diag_fail(diag, ST_NO_MEMORY, "sin memoria para stdin"); }
            buf = grown;
            cap *= 2;
        }
        DWORD got = 0;
        DWORD want = (DWORD)MIN(cap - len, (size_t)IO_CHUNK);
        if (!ReadFile(h, buf + len, want, &got, NULL)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;       // fin normal de un pipe
            mem_free(buf);
            return fail_win32(diag, ST_IO, "error leyendo stdin", NULL);
        }
        if (got == 0) break;
        len += got;
    }
    out->owned = buf;
    out->data = buf;
    out->size = len;
    return ST_OK;
}

void file_data_free(FileData *f) {
    if (f->view) UnmapViewOfFile(f->view);
    mem_free(f->owned);
    memset(f, 0, sizeof *f);
}

// ---- escritura ----------------------------------------------------------------------------

static bool write_all(HANDLE h, const u8 *data, size_t size) {
    for (size_t done = 0; done < size;) {
        DWORD want = (DWORD)MIN(size - done, (size_t)IO_CHUNK), put = 0;
        if (!WriteFile(h, data + done, want, &put, NULL) || put == 0) return false;
        done += put;
    }
    return true;
}

Status file_write_atomic(const wchar_t *path, const void *data, size_t size, WriteMode mode, Diag *diag) {
    if (mode == WRITE_NO_CLOBBER && path_exists(path))
        return diag_fail(diag, ST_SKIPPED, "la salida ya existe (--no-clobber)");

    // Temporal unico por proceso+hilo en la misma carpeta: el rename final es atomico en NTFS
    // solo dentro del mismo volumen.
    wchar_t suffix[64];
    swprintf(suffix, ARRAY_LEN(suffix), L".%lu-%lu.prisma-tmp", GetCurrentProcessId(), GetCurrentThreadId());
    wchar_t *tmp = wstr_concat(path, suffix);
    wchar_t *tmp_io = tmp ? path_for_io(tmp) : NULL;
    wchar_t *dst_io = path_for_io(path);
    Status status = ST_OK;
    if (!tmp || !tmp_io || !dst_io) { status = diag_fail(diag, ST_NO_MEMORY, "sin memoria para la ruta"); goto done; }

    HANDLE h = CreateFileW(tmp_io, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (h == INVALID_HANDLE_VALUE) { status = fail_win32(diag, ST_IO, "no se pudo crear", path); goto done; }
    // Reservar el tamano de una evita fragmentar archivos grandes.
    LARGE_INTEGER end = { .QuadPart = (LONGLONG)size };
    if (size > (1u << 20) && SetFilePointerEx(h, end, NULL, FILE_BEGIN)) {
        SetEndOfFile(h);
        LARGE_INTEGER zero = { 0 };
        SetFilePointerEx(h, zero, NULL, FILE_BEGIN);
    }
    bool ok = write_all(h, data, size);
    if (!ok) status = fail_win32(diag, ST_IO, "error escribiendo", path);
    CloseHandle(h);
    if (ok) {
        // Sin MOVEFILE_WRITE_THROUGH: el rename ya es atomico, y forzar el flush de metadatos
        // por archivo frena un lote de miles de iconos sin ganar nada visible.
        DWORD flags = mode == WRITE_REPLACE ? MOVEFILE_REPLACE_EXISTING : 0;
        if (!MoveFileExW(tmp_io, dst_io, flags)) {
            DWORD err = GetLastError();
            if (mode == WRITE_NO_CLOBBER && (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS))
                status = diag_fail(diag, ST_SKIPPED, "la salida ya existe (--no-clobber)");
            else
                status = fail_win32(diag, ST_IO, "no se pudo reemplazar", path);
        }
    }
    if (status != ST_OK) DeleteFileW(tmp_io);
done:
    mem_free(tmp);
    mem_free(tmp_io);
    mem_free(dst_io);
    return status;
}

Status stdout_write_all(const void *data, size_t size, Diag *diag) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!write_all(h, data, size)) return fail_win32(diag, ST_IO, "error escribiendo stdout", NULL);
    return ST_OK;
}
