// tools/image2png/config.h - todo lo que el usuario pidio, ya validado (una sola vez, al parsear).
#pragma once
#include "../../app/cli.h"
#include "../../codec/codec.h"
#include "../../ops/pipeline.h"
#include "../../png/encode.h"
#include "../../core/term.h"

typedef enum IccMode { ICC_KEEP = 0, ICC_STRIP, ICC_SRGB } IccMode;

typedef struct TextPair {
    char *keyword;
    char *value;
} TextPair;

typedef struct Config {
    // entradas / salidas
    const char     *output;          // -o (archivo, carpeta o "-")
    const char     *suffix;
    bool            recursive, keep_tree, in_place, no_clobber, dry_run, preserve_time;
    u32             threads;
    u32             max_megapixels;
    // modos
    bool            info, formats, json, quiet, verbose, help, examples, version, bench, plain;
    // animacion / paginas
    bool            apng;            // animacion -> APNG (default)
    bool            split;           // cuadros sueltos
    bool            loops_set;
    u32             loops;
    // color / metadatos
    IccMode         icc;
    bool            meta_set;
    Vec             texts;           // TextPair
    u32             dpi;
    const char     *mask_path;
    Image           mask;            // cargada una vez
    // las tres etapas
    DecodeOptions   decode;
    PipelineOptions pipeline;
    EncodeOptions   encode;
} Config;

extern const OptionSpec IMAGE2PNG_OPTIONS[];
extern const size_t IMAGE2PNG_OPTION_COUNT;

void config_defaults(Config *cfg);
void config_free(Config *cfg);

void print_help(void);
void print_examples(void);
