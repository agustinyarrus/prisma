// app/cli.h - analizador de linea de comandos por tabla, compartido por todas las herramientas.
//
// Cada opcion es una FILA DE DATOS: nombre, letra, tipo de argumento, campo destino (offsetof),
// rango, valores posibles, grupo y ayuda. La mayoria usa acciones genericas (bandera, entero con
// rango, porcentaje, color, enumeracion "a|b|c"); solo las que tienen sintaxis propia (--crop
// X,Y,W,H) traen una accion a medida. Cada valor se valida UNA vez, aca, en el borde: el resto del
// programa confia en la configuracion.
//
// Sintaxis: --nombre, --nombre=valor, --nombre valor, -x valor, -xvalor, cortas combinadas (-qr),
// "--" para terminar opciones y @archivo para leer argumentos de un archivo (uno por linea).
#pragma once
#include "../core/base.h"
#include "../core/mem.h"

typedef enum ArgKind { ARG_NONE, ARG_REQUIRED, ARG_OPTIONAL } ArgKind;

typedef struct OptionSpec OptionSpec;

// Valida `value` (NULL si la opcion no lo trajo) y lo guarda en `config`. Con config == NULL es
// "modo sondeo" (para ARG_OPTIONAL): solo responder si `value` seria valido, sin escribir nada.
typedef bool (*OptionAction)(const OptionSpec *spec, void *config, const char *value, char *error, size_t error_cap);

struct OptionSpec {
    const char  *name;          // sin guiones: "resize"
    char         letter;        // 0 si no tiene forma corta
    ArgKind      arg;
    const char  *metavar;       // "ANCHOxALTO"
    const char  *group;         // titulo de seccion para la ayuda
    const char  *help;          // en castellano
    OptionAction action;
    size_t       offset;        // campo destino dentro de la configuracion
    size_t       flag;          // campo bool "fue indicado" + 1 (0 = ninguno)
    double       min, max;      // rango de los numericos
    const char  *choices;       // enumeraciones: "keep|remove|force" (indice = valor)
};

// Acciones genericas.
bool cli_set_true(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);
bool cli_set_false(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);
bool cli_set_u32(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);
bool cli_set_int(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);
bool cli_set_float(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);
bool cli_set_percent(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);   // "10" o "10%" -> 0.10
bool cli_set_enum(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);
bool cli_set_string(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);    // copia (mem_strdup)
bool cli_set_color(const OptionSpec *s, void *cfg, const char *v, char *err, size_t cap);     // Rgba

// Ayudas para acciones a medida.
bool cli_parse_number(const char *text, double *out);
void cli_mark(const OptionSpec *s, void *cfg);           // pone en true el campo `flag`

typedef struct CliResult {
    Vec   positionals;          // char* (UTF-8)
    char  error[320];
} CliResult;

bool cli_parse(int argc, wchar_t **argv, const OptionSpec *table, size_t count, void *config, CliResult *out);
void cli_result_free(CliResult *r);
const char *cli_suggest(const OptionSpec *table, size_t count, const char *unknown);
