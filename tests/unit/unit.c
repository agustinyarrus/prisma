// tests/unit/unit.c - pruebas unitarias de los nucleos de prisma.
//
//   .\build.ps1 -Tools unit            ->  build\release\unit.exe
//   build\release\unit.exe [texto]     solo las pruebas cuyo grupo o nombre contengan el texto
//
// Lo que la suite de punta a punta (tests/suite.py) no puede aislar: la exactitud EXHAUSTIVA de
// las conversiones enteras, el algebra del grupo D4, las matrices CICP contra formulas escritas
// de nuevo, la ida y vuelta de deflate en tramos paralelos (adler32 combinado incluido), los
// filtros PNG vectorizados contra una referencia escalar, el texto UTF-8 y el lector ISOBMFF
// frente a TODOS sus truncamientos (con el build ASan, cualquier lectura fuera de rango explota).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include "../../src/core/base.h"
#include "../../src/core/cicp.h"
#include "../../src/core/colorspace.h"
#include "../../src/core/cpu.h"
#include "../../src/core/image.h"
#include "../../src/core/pool.h"
#include "../../src/core/term.h"
#include "../../src/core/text.h"
#include "../../src/codec/heif.h"
#include "../../src/codec/png_unfilter.h"
#include "../../src/codec/zlib.h"
#include "../../src/ops/background.h"
#include "../../src/ops/geometry.h"
#include "../../src/ops/quantize.h"
#include "../../src/png/filter.h"

// ---- mini marco de pruebas ---------------------------------------------------------------------

enum { MAX_TESTS = 64, FAILURE_CAP = 240, NAME_COLS = 44, CARD_COLS = 72 };

typedef void (*TestFn)(void);
typedef struct TestCase { const char *group, *name; TestFn fn; } TestCase;

static TestCase g_tests[MAX_TESTS];
static u32 g_test_count;
static u64 g_checks, g_check_failures;
static u32 g_current_failures;
static char g_first_failure[FAILURE_CAP];

static void register_test(const char *group, const char *name, TestFn fn) {
    if (g_test_count < MAX_TESTS) g_tests[g_test_count++] = (TestCase){ group, name, fn };
}

// Cuenta todos los chequeos; de cada prueba guarda solo el PRIMER fallo (el que explica).
static bool check_impl(bool ok, int line, const char *fmt, ...) PRINTF_LIKE(3, 4);
static bool check_impl(bool ok, int line, const char *fmt, ...) {
    g_checks++;
    if (ok) return true;
    g_check_failures++;
    if (!g_current_failures++) {
        int n = snprintf(g_first_failure, sizeof g_first_failure, "linea %d: ", line);
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(g_first_failure + n, sizeof g_first_failure - (size_t)n, fmt, ap);
        va_end(ap);
    }
    return false;
}
#define CHECK(cond, ...) check_impl((cond), __LINE__, __VA_ARGS__)

// Generador determinista (splitmix64): las pruebas "al azar" son siempre las mismas.
static u64 g_rng = 0x5EED20260923ull;
static u64 rnd(void) {
    u64 z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}
static u32 rnd_below(u32 n) { return (u32)(rnd() % n); }
static double rnd_unit(void) { return (double)(rnd() >> 11) / (double)(1ull << 53); }

static double now_seconds(void) {
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}

// =================================================================================================
// conversiones enteras: exhaustivas
// =================================================================================================

static void test_u16_to_u8(void) {
    for (u32 v = 0; v <= 0xFFFF; v++) {
        u32 expected = (2 * v + 257) / 514;                    // round(v / 257); 257 impar: nunca hay empate
        if (!CHECK(u16_to_u8((u16)v) == expected, "u16_to_u8(%u) = %u, esperado %u", v, u16_to_u8((u16)v), expected)) return;
    }
    for (u32 b = 0; b < 256; b++)
        CHECK(u16_to_u8(u8_to_u16((u8)b)) == b && u8_to_u16((u8)b) == b * 257, "ida y vuelta de %u", b);
}

static void test_div255(void) {
    for (u32 x = 0; x <= 255u * 255u; x++) {
        u32 expected = (2 * x + 255) / 510;                    // round(x / 255); 255 impar: sin empates
        if (!CHECK(div255_round(x) == expected, "div255_round(%u) = %u, esperado %u", x, div255_round(x), expected)) return;
    }
}

static void test_overflow_guards(void) {
    size_t out;
    CHECK(!mul_ok(SIZE_MAX / 2 + 1, 2, &out), "mul_ok no detecto el desborde");
    CHECK(mul_ok(65535u * 65535u, 8, &out) && out == (size_t)65535u * 65535u * 8, "mul_ok de un tamaño grande valido");
    CHECK(!add_ok(SIZE_MAX, 1, &out), "add_ok no detecto el desborde");
    CHECK(add_ok(SIZE_MAX - 1, 1, &out) && out == SIZE_MAX, "add_ok en el limite");
}

// =================================================================================================
// geometria: el grupo D4
// =================================================================================================

static bool make_labeled(Image *im, u32 w, u32 h) {
    if (image_alloc(im, w, h, 8, false, 0, &(Diag){ 0 }) != ST_OK) return false;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++) {
            u8 *p = image_row(im, y) + 4 * x;
            p[0] = (u8)x; p[1] = (u8)y; p[2] = (u8)(x * 7 + y * 13); p[3] = 255;
        }
    return true;
}

static bool same_image(const Image *a, const Image *b) {
    if (a->width != b->width || a->height != b->height || a->depth != b->depth) return false;
    for (u32 y = 0; y < a->height; y++)
        if (memcmp(image_row(a, y), image_row(b, y), (size_t)a->width * image_bytes_per_pixel(a))) return false;
    return true;
}

static void test_d4_composition(void) {
    Diag d = { 0 };
    for (int a = ORIENT_NONE; a <= ORIENT_ROT270; a++)
        for (int b = ORIENT_NONE; b <= ORIENT_ROT270; b++) {
            Image two_steps, one_step;
            if (!make_labeled(&two_steps, 5, 3) || !make_labeled(&one_step, 5, 3)) { CHECK(false, "sin memoria"); return; }
            geometry_orient(&two_steps, (Orientation)a, &d);
            geometry_orient(&two_steps, (Orientation)b, &d);
            geometry_orient(&one_step, geometry_then((Orientation)a, (Orientation)b), &d);
            CHECK(same_image(&two_steps, &one_step), "then(%d, %d) = %d no equivale a aplicar las dos", a, b, geometry_then((Orientation)a, (Orientation)b));
            image_free(&two_steps);
            image_free(&one_step);
        }
}

static void test_d4_inverse(void) {
    Diag d = { 0 };
    for (int o = ORIENT_NONE; o <= ORIENT_ROT270; o++) {
        Image im, ref;
        if (!make_labeled(&im, 7, 4) || !make_labeled(&ref, 7, 4)) { CHECK(false, "sin memoria"); return; }
        geometry_orient(&im, (Orientation)o, &d);
        geometry_orient(&im, geometry_inverse((Orientation)o), &d);
        CHECK(same_image(&im, &ref), "inverse(%d) no deshace %d", o, o);
        CHECK(geometry_then((Orientation)o, geometry_inverse((Orientation)o)) == ORIENT_NONE, "o · o^-1 != identidad (%d)", o);
        image_free(&im);
        image_free(&ref);
    }
    CHECK(geometry_compose(90, false, false) == ORIENT_ROT90, "compose(90)");
    CHECK(geometry_compose(-90, false, false) == ORIENT_ROT270, "compose(-90)");
    CHECK(geometry_compose(450, false, false) == ORIENT_ROT90, "compose(450)");
    CHECK(geometry_compose(0, true, true) == ORIENT_ROT180, "espejo H + V = 180");
    CHECK(geometry_compose(90, true, false) == geometry_then(ORIENT_ROT90, ORIENT_FLIP_H), "compose(90, H)");
}

// =================================================================================================
// CICP: matrices contra formulas escritas de nuevo
// =================================================================================================

// RGB -> YCbCr y vuelta con Kr/Kb, escrito sin matrices (la forma de las normas).
static void ycbcr_encode(double kr, double kb, const double rgb[3], double ycc[3]) {
    double kg = 1 - kr - kb, y = kr * rgb[0] + kg * rgb[1] + kb * rgb[2];
    ycc[0] = y;
    ycc[1] = (rgb[2] - y) / (2 * (1 - kb));
    ycc[2] = (rgb[0] - y) / (2 * (1 - kr));
}

static void ycbcr_decode(double kr, double kb, const double ycc[3], double rgb[3]) {
    double kg = 1 - kr - kb;
    rgb[0] = ycc[0] + 2 * (1 - kr) * ycc[2];
    rgb[2] = ycc[0] + 2 * (1 - kb) * ycc[1];
    rgb[1] = (ycc[0] - kr * rgb[0] - kb * rgb[2]) / kg;
}

static void apply3(const float m[9], const double in[3], double out[3]) {
    for (int r = 0; r < 3; r++) out[r] = m[3 * r] * in[0] + m[3 * r + 1] * in[1] + m[3 * r + 2] * in[2];
}

static void test_cicp_ycbcr(void) {
    float m[9];
    CHECK(cicp_ycbcr_correction(1, 1, 1, m), "709 -> 709 deberia existir");
    for (int i = 0; i < 9; i++) CHECK(fabsf(m[i] - (i % 4 == 0 ? 1.0f : 0.0f)) < 1e-6f, "709->709 no es la identidad (m[%d] = %g)", i, m[i]);
    // Un color codificado en BT.601 y decodificado (mal) como BT.709 vuelve con la correccion.
    struct { u32 declared; double kr, kb; } cases[] = { { 6, 0.299, 0.114 }, { 9, 0.2627, 0.0593 }, { 7, 0.212, 0.087 } };
    for (size_t c = 0; c < ARRAY_LEN(cases); c++) {
        CHECK(cicp_ycbcr_correction(1, cases[c].declared, 1, m), "correccion 709 -> %u", cases[c].declared);
        double worst = 0;
        for (int k = 0; k < 500; k++) {
            double rgb[3] = { rnd_unit(), rnd_unit(), rnd_unit() }, ycc[3], wrong[3], fixed[3];
            ycbcr_encode(cases[c].kr, cases[c].kb, rgb, ycc);
            ycbcr_decode(0.2126, 0.0722, ycc, wrong);
            apply3(m, wrong, fixed);
            for (int i = 0; i < 3; i++) worst = fmax(worst, fabs(fixed[i] - rgb[i]));
        }
        CHECK(worst < 1e-5, "matriz %u: error %g tras corregir", cases[c].declared, worst);
    }
    CHECK(!cicp_ycbcr_correction(1, 0, 1, m), "la identidad (GBR) no es una matriz YCbCr: no se corrige");
    CHECK(cicp_ycbcr_correction(1, 12, 12, m), "matriz derivada de los primarios (P3)");
}

static void test_cicp_gamut(void) {
    float m[9];
    CHECK(cicp_gamut_to_srgb(1, m), "sRGB -> sRGB");
    for (int i = 0; i < 9; i++) CHECK(fabsf(m[i] - (i % 4 == 0 ? 1.0f : 0.0f)) < 1e-5f, "sRGB->sRGB no es la identidad");
    // Display P3 -> sRGB: la matriz publicada (primarios de SMPTE EG 432-1 con blanco D65).
    static const float P3[9] = { 1.2249f, -0.2247f, 0.0f, -0.0420f, 1.0419f, 0.0f, -0.0197f, -0.0786f, 1.0979f };
    CHECK(cicp_gamut_to_srgb(12, m), "P3 -> sRGB");
    for (int i = 0; i < 9; i++) CHECK(fabsf(m[i] - P3[i]) < 2e-3f, "P3->sRGB m[%d] = %.4f, publicada %.4f", i, m[i], P3[i]);
    // El blanco del origen va al blanco de sRGB (tambien DCI-P3, con otro blanco: Bradford).
    u32 codes[] = { 5, 6, 9, 11, 12, 22 };
    for (size_t c = 0; c < ARRAY_LEN(codes); c++) {
        CHECK(cicp_gamut_to_srgb(codes[c], m), "primarios %u", codes[c]);
        for (int r = 0; r < 3; r++) CHECK(fabsf(m[3 * r] + m[3 * r + 1] + m[3 * r + 2] - 1.0f) < 2e-4f, "primarios %u: el blanco no queda blanco (fila %d)", codes[c], r);
    }
    CHECK(!cicp_gamut_to_srgb(99, m), "primarios desconocidos");
}

static void test_cicp_transfer(void) {
    colorspace_init();
    for (int i = 0; i <= 100; i++) {
        float v = i / 100.0f;
        CHECK(fabsf(cicp_to_linear(CICP_TRANSFER_SRGB, v) - srgb_to_linear(v)) < 1e-6f, "sRGB en %g", v);
        CHECK(cicp_to_linear(CICP_TRANSFER_LINEAR, v) == v, "lineal en %g", v);
    }
    float below = cicp_to_linear(CICP_TRANSFER_BT709, 0.0809f), above = cicp_to_linear(CICP_TRANSFER_BT709, 0.0811f);
    CHECK(fabsf(below - above) < 1e-4f, "BT.709 es continua en el quiebre (%g vs %g)", below, above);
    CHECK(cicp_transfer_invertible(CICP_TRANSFER_SRGB) && !cicp_transfer_invertible(CICP_TRANSFER_PQ), "invertibles");
    Cicp hdr = { 9, CICP_TRANSFER_PQ, 0, true }, srgb = { 1, 13, 0, true };
    CHECK(cicp_is_hdr(&hdr) && !cicp_is_hdr(&srgb) && cicp_is_srgb_like(&srgb) && !cicp_is_srgb_like(&hdr), "clasificacion");
}

// =================================================================================================
// deflate: adler32 combinado y tramos paralelos
// =================================================================================================

static u8 *random_bytes(size_t n, int kind) {
    u8 *p = mem_alloc(MAX(n, (size_t)1));
    if (!p) return NULL;
    for (size_t i = 0; i < n; i++)
        p[i] = kind == 0 ? 0 : kind == 1 ? (u8)rnd() : (u8)((i / 3) ^ (i >> 11) ^ (rnd_below(16) == 0 ? rnd() : 0));
    return p;
}

static void test_adler32_combine(void) {
    size_t sizes[] = { 0, 1, 2, 5552, 5553, 65536 + 7, 1 << 20 };
    for (size_t a = 0; a < ARRAY_LEN(sizes); a++)
        for (size_t b = 0; b < ARRAY_LEN(sizes); b++) {
            u8 *x = random_bytes(sizes[a], 1), *y = random_bytes(sizes[b], 1);
            if (!x || !y) { mem_free(x); mem_free(y); CHECK(false, "sin memoria"); return; }
            u32 whole = adler32_update(adler32_update(1, x, sizes[a]), y, sizes[b]);
            u32 combined = adler32_combine(adler32_update(1, x, sizes[a]), adler32_update(1, y, sizes[b]), sizes[b]);
            CHECK(whole == combined, "adler32_combine(%zu, %zu): %08X != %08X", sizes[a], sizes[b], combined, whole);
            mem_free(x);
            mem_free(y);
        }
}

static void test_deflate_roundtrip(void) {
    size_t sizes[] = { 0, 1, 100, 65536, ZLIB_PARALLEL_MIN_CHUNK - 1, ZLIB_PARALLEL_MIN_CHUNK + 1, 3 * ZLIB_PARALLEL_MIN_CHUNK + 12345 };
    for (size_t s = 0; s < ARRAY_LEN(sizes); s++)
        for (int kind = 0; kind < 3; kind++)
            for (int parallel = 0; parallel < 2; parallel++) {
                Diag d = { 0 };
                u8 *in = random_bytes(sizes[s], kind), *z = NULL, *back = NULL;
                size_t zlen = 0, blen = 0;
                Status st = in ? zlib_compress(in, sizes[s], 6, parallel, &z, &zlen, &d) : ST_NO_MEMORY;
                if (st == ST_OK) st = zlib_inflate_alloc(z, zlen, sizes[s] + 1, &back, &blen, &d);
                CHECK(st == ST_OK && blen == sizes[s] && (!sizes[s] || memcmp(back, in, sizes[s]) == 0),
                      "ida y vuelta de %zu bytes (tipo %d, paralelo %d): %s", sizes[s], kind, parallel, d.message);
                // Truncado a la mitad: se rescata un prefijo correcto, nunca basura.
                if (st == ST_OK && sizes[s] > 1000 && kind == 2) {
                    u8 *part = mem_alloc(sizes[s]);
                    size_t produced = 0;
                    ZSpan span = { z, zlen / 2 };
                    Diag d2 = { 0 };
                    Status t = zlib_inflate_exact(&span, 1, part, sizes[s], &produced, &d2);
                    CHECK(t == ST_TRUNCATED && produced < sizes[s] && memcmp(part, in, produced) == 0,
                          "rescate de %zu bytes: estado %d, %zu recuperados", sizes[s], (int)t, produced);
                    mem_free(part);
                }
                mem_free(in);
                mem_free(z);
                mem_free(back);
            }
}

// =================================================================================================
// filtros PNG: vectorizados contra la definicion escalar
// =================================================================================================

static u8 paeth(u8 a, u8 b, u8 c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

static void filter_reference(const u8 *row, const u8 *prev, size_t n, u32 bpp, u8 type, u8 *out) {
    for (size_t i = 0; i < n; i++) {
        u8 a = i >= bpp ? row[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
        u8 pred = type == 1 ? a : type == 2 ? b : type == 3 ? (u8)((a + b) / 2) : type == 4 ? paeth(a, b, c) : 0;
        out[i] = (u8)(row[i] - pred);
    }
}

static void test_unfilter_vs_reference(void) {
    u32 bpps[] = { 1, 2, 3, 4, 6, 8 };
    u8 row[200], prev[200], filtered[200];
    for (size_t k = 0; k < ARRAY_LEN(bpps); k++)
        for (size_t n = 1; n <= 70; n += (n < 20 ? 1 : 7))
            for (u8 type = 0; type <= 4; type++) {
                for (size_t i = 0; i < n; i++) { row[i] = (u8)rnd(); prev[i] = (u8)rnd(); }
                filter_reference(row, prev, n, bpps[k], type, filtered);
                bool ok = png_unfilter_row(filtered, prev, n, bpps[k], type);
                CHECK(ok && memcmp(filtered, row, n) == 0, "unfilter tipo %u, bpp %u, %zu bytes", type, bpps[k], n);
            }
    CHECK(!png_unfilter_row(row, prev, 10, 4, 5), "el filtro 5 no existe");
}

static void test_filter_strategies_roundtrip(void) {
    u32 bpps[] = { 1, 3, 4, 8 };
    for (size_t k = 0; k < ARRAY_LEN(bpps); k++)
        for (int s = FS_NONE; s < FS__COUNT; s++) {
            Packed p = { 0 };
            p.width = 37;
            p.height = 23;
            p.row_bytes = (size_t)p.width * bpps[k];
            p.rows = mem_alloc(p.row_bytes * p.height);
            if (!p.rows) { CHECK(false, "sin memoria"); return; }
            for (size_t i = 0; i < p.row_bytes * p.height; i++) p.rows[i] = (u8)((i % 97) * 3 + (rnd_below(8) == 0 ? rnd() : 0));
            Filtered f = { 0 };
            Diag d = { 0 };
            Status st = filter_packed(&p, bpps[k], (FilterStrategy)s, &f, &d);
            bool ok = st == ST_OK && f.size == p.height * (1 + p.row_bytes);
            u8 *prev = mem_calloc(p.row_bytes);
            for (u32 y = 0; ok && y < p.height; y++) {
                u8 *line = f.data + (size_t)y * (1 + p.row_bytes);
                ok = png_unfilter_row(line + 1, prev, p.row_bytes, bpps[k], line[0]) &&
                     memcmp(line + 1, p.rows + (size_t)y * p.row_bytes, p.row_bytes) == 0;
                memcpy(prev, line + 1, p.row_bytes);
            }
            CHECK(ok, "estrategia %s con bpp %u no vuelve a los datos", filter_strategy_name((FilterStrategy)s), bpps[k]);
            mem_free(prev);
            filtered_free(&f);
            mem_free(p.rows);
        }
}

// =================================================================================================
// texto
// =================================================================================================

static bool valid_utf8(const char *s) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, NULL, 0) > 0;
}

static void test_text_width(void) {
    CHECK(utf8_display_width("ñandú", strlen("ñandú")) == 5, "ancho de ñandú");
    CHECK(utf8_display_width("🎨", strlen("🎨")) == 2, "un emoji ocupa 2");
    CHECK(utf8_display_width("e\xCC\x81", 3) == 1, "e + acento combinante ocupa 1");
    CHECK(utf8_display_width("漢字", strlen("漢字")) == 4, "CJK ocupa 2 por caracter");
}

static void test_ellipsis(void) {
    const char *samples[] = {
        "C:/Users/ñandú/Fotos de las vacaciones 🎨/playa al atardecer con acentos áéíóú.png",
        "archivo.png", "漢字漢字漢字漢字漢字漢字.png", "🎨🎨🎨🎨🎨🎨🎨🎨🎨🎨🎨🎨🎨🎨🎨🎨🎨.avif", "",
    };
    char out[256];
    for (size_t k = 0; k < ARRAY_LEN(samples); k++)
        for (size_t cols = 1; cols <= 60; cols++) {
            utf8_ellipsize_middle(samples[k], cols, out, sizeof out);
            size_t w = utf8_display_width(out, strlen(out));
            bool fits = utf8_display_width(samples[k], strlen(samples[k])) <= cols;
            CHECK(valid_utf8(out), "UTF-8 invalido cortando \"%s\" a %zu", samples[k], cols);
            CHECK(w <= cols || cols < 2, "\"%s\" a %zu columnas ocupa %zu", samples[k], cols, w);
            CHECK(fits ? strcmp(out, samples[k]) == 0 : strstr(out, "…") != NULL, "\"%s\" a %zu: \"%s\"", samples[k], cols, out);
        }
    utf8_ellipsize_middle(samples[0], 40, out, sizeof out);
    CHECK(strstr(out, "acentos áéíóú.png") != NULL, "la elipsis del medio conserva el final: \"%s\"", out);
    // Prefijo visible con colores ANSI: las secuencias no ocupan y no se parten.
    const char *inked = "\x1b[38;2;1;2;3mhola\x1b[0m mundo";
    size_t cut = visible_prefix(inked, strlen(inked), 3);
    CHECK(cut == strlen("\x1b[38;2;1;2;3mhol"), "visible_prefix con ANSI corto en %zu", cut);
    CHECK(visible_width(inked, strlen(inked)) == 10, "ancho visible con ANSI");
}

static void test_formats(void) {
    char s[64];
    fmt_count(s, sizeof s, 1234567);
    CHECK(strcmp(s, "1.234.567") == 0, "fmt_count: %s", s);
    fmt_count(s, sizeof s, 0);
    CHECK(strcmp(s, "0") == 0, "fmt_count(0): %s", s);
    fmt_decimal(s, sizeof s, 13.7, 1);
    CHECK(strcmp(s, "13,7") == 0, "fmt_decimal: %s", s);
    fmt_decimal(s, sizeof s, -0.25, 2);
    CHECK(strcmp(s, "-0,25") == 0, "fmt_decimal negativo: %s", s);
    CHECK(edit_distance("kitten", "sitting") == 3, "Levenshtein clasico");
    CHECK(edit_distance("--resize", "--rseize") == 1, "Damerau: una transposicion adyacente cuenta 1");
    CHECK(edit_distance("--resize", "--rezise") == 2, "dos letras no vecinas intercambiadas: 2");
    CHECK(edit_distance("", "abc") == 3 && edit_distance("abc", "abc") == 0, "bordes de edit_distance");
}

// =================================================================================================
// ISOBMFF / HEIF: estructura sintetica y robustez ante truncamientos
// =================================================================================================

typedef struct ByteWriter { u8 data[1024]; size_t len; } ByteWriter;

static void put(ByteWriter *w, const void *p, size_t n) { memcpy(w->data + w->len, p, n); w->len += n; }
static void put8(ByteWriter *w, u32 v) { u8 b = (u8)v; put(w, &b, 1); }
static void put16(ByteWriter *w, u32 v) { u8 b[2]; store_be16(b, (u16)v); put(w, b, 2); }
static void put32(ByteWriter *w, u32 v) { u8 b[4]; store_be32(b, v); put(w, b, 4); }
static size_t box_open(ByteWriter *w, const char *type) { size_t at = w->len; put32(w, 0); put(w, type, 4); return at; }
static void box_close(ByteWriter *w, size_t at) { store_be32(w->data + at, (u32)(w->len - at)); }
static size_t fullbox_open(ByteWriter *w, const char *type, u32 version, u32 flags) {
    size_t at = box_open(w, type);
    put32(w, version << 24 | flags);
    return at;
}

// AVIF minimo: primaria 1 (colr nclx P3, irot 90 antihorario + espejo), alfa 2 (auxC alfa, oculto,
// irot) con auxl 2->1 y prem 1->2. Sin datos de imagen: solo estructura.
static size_t build_heif(ByteWriter *w, size_t *nclx_at, size_t *hidden_flags_at) {
    w->len = 0;
    size_t b = box_open(w, "ftyp"); put(w, "avif", 4); put32(w, 0); put(w, "avifmif1miaf", 12); box_close(w, b);
    size_t meta = fullbox_open(w, "meta", 0, 0);
    b = fullbox_open(w, "hdlr", 0, 0); put32(w, 0); put(w, "pict", 4); put32(w, 0); put32(w, 0); put32(w, 0); put8(w, 0); box_close(w, b);
    b = fullbox_open(w, "pitm", 0, 0); put16(w, 1); box_close(w, b);
    size_t iinf = fullbox_open(w, "iinf", 0, 0); put16(w, 2);
    b = fullbox_open(w, "infe", 2, 0); put16(w, 1); put16(w, 0); put(w, "av01", 4); put8(w, 0); box_close(w, b);
    b = fullbox_open(w, "infe", 2, 1); *hidden_flags_at = b + 11; put16(w, 2); put16(w, 0); put(w, "av01", 4); put8(w, 0); box_close(w, b);
    box_close(w, iinf);
    size_t iref = fullbox_open(w, "iref", 0, 0);
    b = box_open(w, "auxl"); put16(w, 2); put16(w, 1); put16(w, 1); box_close(w, b);
    b = box_open(w, "prem"); put16(w, 1); put16(w, 1); put16(w, 2); box_close(w, b);
    box_close(w, iref);
    size_t iprp = box_open(w, "iprp");
    size_t ipco = box_open(w, "ipco");
    b = box_open(w, "colr"); put(w, "nclx", 4); *nclx_at = w->len; put16(w, 12); put16(w, 13); put16(w, 6); put8(w, 0x80); box_close(w, b);   // 1
    static const char URN[] = "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha";
    b = fullbox_open(w, "auxC", 0, 0); put(w, URN, sizeof URN); box_close(w, b);                                                            // 2
    b = box_open(w, "irot"); put8(w, 1); box_close(w, b);                                                                                   // 3
    b = box_open(w, "imir"); put8(w, 0); box_close(w, b);                                                                                   // 4
    b = fullbox_open(w, "ispe", 0, 0); put32(w, 64); put32(w, 48); box_close(w, b);                                                        // 5
    box_close(w, ipco);
    b = fullbox_open(w, "ipma", 0, 0); put32(w, 2);
    put16(w, 1); put8(w, 4); put8(w, 5); put8(w, 1); put8(w, 3); put8(w, 4);
    put16(w, 2); put8(w, 3); put8(w, 5); put8(w, 2); put8(w, 3);
    box_close(w, b);
    box_close(w, iprp);
    box_close(w, meta);
    return w->len;
}

static void test_heif_structure(void) {
    ByteWriter w;
    size_t nclx_at, hidden_at;
    size_t n = build_heif(&w, &nclx_at, &hidden_at);
    HeifInfo info;
    if (!CHECK(heif_inspect(w.data, n, &info), "no se leyo el HEIF sintetico")) return;
    CHECK(info.primary_id == 1 && info.av1, "primaria %u, av1 %d", info.primary_id, info.av1);
    CHECK(info.has_nclx && info.cicp.primaries == 12 && info.cicp.transfer == 13 && info.cicp.matrix == 6 && info.cicp.full_range,
          "nclx %u/%u/%u rango %d", info.cicp.primaries, info.cicp.transfer, info.cicp.matrix, info.cicp.full_range);
    CHECK(info.nclx_offset == nclx_at, "offset de nclx %zu, esperado %zu", info.nclx_offset, nclx_at);
    CHECK(info.primary_orientation == geometry_then(ORIENT_ROT270, ORIENT_FLIP_H), "orientacion de la primaria %d", info.primary_orientation);
    CHECK(info.has_alpha && info.alpha_id == 2 && info.premultiplied, "alfa %d id %u prem %d", info.has_alpha, info.alpha_id, info.premultiplied);
    CHECK(info.alpha_orientation == ORIENT_ROT270, "orientacion del alfa %d", info.alpha_orientation);
    CHECK(info.alpha_flags_offset == hidden_at && w.data[hidden_at] == 1, "flags del infe del alfa en %zu (esperado %zu)", info.alpha_flags_offset, hidden_at);

    u8 *promoted = heif_promote_alpha(w.data, n, &info);
    HeifInfo after;
    CHECK(promoted && heif_inspect(promoted, n, &after) && after.primary_id == 2 && promoted[hidden_at] == 0,
          "promover el alfa: primaria %u, oculto %u", promoted ? after.primary_id : 0, promoted ? promoted[hidden_at] : 9);
    mem_free(promoted);

    Cicp neutral = { 6, 13, 6, true };
    u8 *rewritten = heif_rewrite_nclx(w.data, n, &info, &neutral);
    size_t differing = 0;
    for (size_t i = 0; rewritten && i < n; i++) differing += rewritten[i] != w.data[i];
    CHECK(rewritten && heif_inspect(rewritten, n, &after) && after.cicp.primaries == 6 && differing == 1,
          "reescribir la nclx: primarios %u, bytes cambiados %zu", rewritten ? after.cicp.primaries : 0, differing);
    mem_free(rewritten);
}

// Cada truncamiento y miles de corrupciones: nunca leer fuera (ASan lo verifica), nunca colgarse.
static void test_heif_robustness(void) {
    ByteWriter w;
    size_t nclx_at, hidden_at;
    size_t n = build_heif(&w, &nclx_at, &hidden_at);
    HeifInfo info;
    for (size_t len = 0; len < n; len++) {
        u8 *copy = mem_alloc(MAX(len, (size_t)1));       // con ASan la cola esta envenenada: el byte
        memcpy(copy, w.data, len);                       // siguiente al final explota si se lee
        heif_inspect(copy, len, &info);
        mem_free(copy);
    }
    u32 parsed = 0;
    for (int k = 0; k < 20000; k++) {
        u8 *copy = mem_alloc(n);
        memcpy(copy, w.data, n);
        for (u32 flips = 1 + rnd_below(4); flips; flips--) copy[rnd_below((u32)n)] = (u8)rnd();
        parsed += heif_inspect(copy, n, &info);
        mem_free(copy);
    }
    CHECK(parsed > 0, "ninguna corrupcion se pudo leer: el generador esta mal");
}

// =================================================================================================
// operaciones: cuantizacion y fondo, propiedades
// =================================================================================================

static void test_quantize_properties(void) {
    Image im;
    if (image_alloc(&im, 64, 48, 8, false, 0, &(Diag){ 0 }) != ST_OK) { CHECK(false, "sin memoria"); return; }
    for (u32 y = 0; y < im.height; y++)
        for (u32 x = 0; x < im.width; x++) {
            u8 *p = image_row(&im, y) + 4 * x;
            bool hole = (x / 8 + y / 8) % 3 == 0;
            p[0] = (u8)(x * 4); p[1] = (u8)(y * 5); p[2] = (u8)(x + y); p[3] = hole ? 0 : (u8)(128 + x);
        }
    Image *frames[1] = { &im };
    QuantizeOptions opt = { 16, DITHER_NONE, 1.0f };
    QuantizeReport rep;
    Diag d = { 0 };
    CHECK(quantize_frames(frames, 1, &opt, &rep, &d) == ST_OK, "cuantizar: %s", d.message);
    u32 seen[256], count = 0;
    bool holes_ok = true;
    for (u32 y = 0; y < im.height; y++)
        for (u32 x = 0; x < im.width; x++) {
            u32 px;
            memcpy(&px, image_row(&im, y) + 4 * x, 4);
            if ((x / 8 + y / 8) % 3 == 0) holes_ok &= px == 0;
            bool known = false;
            for (u32 i = 0; i < count && !known; i++) known = seen[i] == px;
            if (!known && count < 256) seen[count++] = px;
        }
    CHECK(count <= 16 && rep.palette <= 16, "%u colores distintos (paleta %u)", count, rep.palette);
    CHECK(holes_ok, "lo transparente tiene que quedar (0,0,0,0)");
    image_free(&im);
}

static void test_remove_bg_properties(void) {
    Image im;
    if (image_alloc(&im, 40, 30, 8, false, 0, &(Diag){ 0 }) != ST_OK) { CHECK(false, "sin memoria"); return; }
    for (u32 y = 0; y < im.height; y++)
        for (u32 x = 0; x < im.width; x++) {
            u8 *p = image_row(&im, y) + 4 * x;
            bool subject = x >= 12 && x < 28 && y >= 8 && y < 22;
            p[0] = subject ? 30 : 255; p[1] = subject ? 60 : 255; p[2] = subject ? 200 : 255; p[3] = 255;
        }
    BgRemoveOptions opt = { true, { 1, 1, 1, 1 }, 0.08f, 2 };
    BgRemoveReport rep;
    Diag d = { 0 };
    CHECK(background_remove(&im, &opt, &rep, &d) == ST_OK, "remove-bg: %s", d.message);
    CHECK(image_row(&im, 0)[3] == 0 && image_row(&im, 29)[4 * 39 + 3] == 0, "las esquinas quedan transparentes");
    const u8 *center = image_row(&im, 15) + 4 * 20;
    CHECK(center[3] == 255 && center[2] == 200, "el sujeto queda intacto");
    CHECK(rep.removed > 0 && rep.color.r > 0.99f, "detecto el fondo blanco (borro %llu)", (unsigned long long)rep.removed);
    image_free(&im);
}

static void test_diag(void) {
    Diag d = { 0 };
    diag_fail(&d, ST_CORRUPT, "primero");
    diag_fail(&d, ST_TRUNCATED, "segundo");
    CHECK(d.status == ST_CORRUPT && strcmp(d.message, "primero") == 0, "gana el primer error");
    for (int i = 0; i < 20; i++) diag_warn(&d, "repetido");
    diag_warn(&d, "otro");
    CHECK(d.warning_count == 2, "los avisos repetidos cuentan una vez (%u)", d.warning_count);
}

// =================================================================================================

static void register_all(void) {
    register_test("enteros", "u16 -> u8 exhaustivo (65.536 valores)", test_u16_to_u8);
    register_test("enteros", "division por 255 exhaustiva", test_div255);
    register_test("enteros", "guardas de desborde", test_overflow_guards);
    register_test("d4", "composicion de las 64 parejas", test_d4_composition);
    register_test("d4", "inversas y rotaciones", test_d4_inverse);
    register_test("cicp", "matrices YCbCr contra la norma", test_cicp_ycbcr);
    register_test("cicp", "gamas a sRGB (P3 publicada, Bradford)", test_cicp_gamut);
    register_test("cicp", "curvas de transferencia", test_cicp_transfer);
    register_test("deflate", "adler32 combinado", test_adler32_combine);
    register_test("deflate", "ida y vuelta en tramos paralelos + rescate", test_deflate_roundtrip);
    register_test("png", "unfilter SIMD contra la definicion", test_unfilter_vs_reference);
    register_test("png", "las 9 estrategias de filtro vuelven", test_filter_strategies_roundtrip);
    register_test("texto", "anchos de columna UTF-8", test_text_width);
    register_test("texto", "elipsis del medio y ANSI", test_ellipsis);
    register_test("texto", "formatos y Damerau-Levenshtein", test_formats);
    register_test("heif", "estructura sintetica completa", test_heif_structure);
    register_test("heif", "todos los truncamientos + 20.000 corrupciones", test_heif_robustness);
    register_test("ops", "cuantizacion: propiedades", test_quantize_properties);
    register_test("ops", "remove-bg: propiedades", test_remove_bg_properties);
    register_test("diag", "primer error y avisos", test_diag);
}

int main(int argc, char **argv) {
    term_init(TERM_COLOR_AUTO);
    pool_start(cpu_info()->logical_cores);
    register_all();
    const char *filter = argc > 1 ? argv[1] : NULL;
    Buf b;
    buf_init(&b);
    buf_puts(&b, "\n  ");
    buf_inked(&b, INK_LAVENDER, "prisma · pruebas unitarias");
    buf_inked(&b, INK_DIM, "  ·  %u pruebas  ·  %u hilos  ·  AVX2 %s\n\n", g_test_count, cpu_info()->logical_cores, cpu_info()->avx2 ? "si" : "no");
    term_emit(&b);
    buf_clear(&b);
    u32 run = 0, failed = 0;
    double t0 = now_seconds();
    for (u32 i = 0; i < g_test_count; i++) {
        const TestCase *t = &g_tests[i];
        if (filter && !strstr(t->group, filter) && !strstr(t->name, filter)) continue;
        g_current_failures = 0;
        u64 before = g_checks;
        double start = now_seconds();
        t->fn();
        double ms = (now_seconds() - start) * 1000.0;
        run++;
        bool ok = g_current_failures == 0;
        failed += !ok;
        size_t line = b.len;
        buf_inked(&b, ok ? INK_SAGE : INK_ROSE, "  %s ", ok ? "✓" : "✗");
        buf_inked(&b, INK_DIM, "%s · ", t->group);
        buf_inked(&b, ok ? INK_TEXT : INK_ROSE, "%s", t->name);
        buf_pad_to(&b, line, NAME_COLS + 14);
        char checks[32];
        fmt_count(checks, sizeof checks, g_checks - before);
        buf_inked(&b, INK_SKY, "%10s", checks);
        buf_inked(&b, INK_DIM, " chequeos  %8.1f ms\n", ms);
        if (!ok) buf_inked(&b, INK_CREAM, "      %s\n", g_first_failure);
        term_emit(&b);
        buf_clear(&b);
    }
    double secs = now_seconds() - t0;
    char total[32];
    fmt_count(total, sizeof total, g_checks);
    // Tarjeta: "│ " + CARD_COLS columnas + " │"; los tres bordes miden CARD_COLS + 4.
    static const char TITLE[] = "resultado";
    buf_puts(&b, "\n  ");
    buf_inked(&b, INK_FAINT, "╭─ ");
    buf_inked(&b, INK_LAVENDER, "%s", TITLE);
    buf_ink(&b, INK_FAINT);
    buf_puts(&b, " ");
    buf_repeat(&b, "─", CARD_COLS + 4 - 3 - (sizeof TITLE - 1) - 1 - 1);
    buf_puts(&b, "╮\n  │ ");
    buf_reset(&b);
    size_t line = b.len;
    buf_inked(&b, failed ? INK_ROSE : INK_SAGE, "%u/%u pruebas", run - failed, run);
    buf_inked(&b, INK_DIM, "   ·   %s chequeos   ·   %llu fallidos   ·   %.2f s", total, (unsigned long long)g_check_failures, secs);
    buf_pad_to(&b, line, CARD_COLS);
    buf_ink(&b, INK_FAINT);
    buf_puts(&b, " │\n  ╰");
    buf_repeat(&b, "─", CARD_COLS + 2);
    buf_puts(&b, "╯\n\n");
    buf_reset(&b);
    term_emit(&b);
    buf_free(&b);
    pool_stop();
    term_shutdown();
    return failed ? 1 : 0;
}
