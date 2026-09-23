// ops/geometry.c - ver geometry.h
#include "geometry.h"
#include "../core/pool.h"

enum { TILE = 32 };

typedef struct OrientJob {
    const Image *src;
    Image       *dst;
    Orientation  op;
} OrientJob;

// Coordenada de origen para el destino (x, y). W y H son del ORIGEN.
INLINE void source_of(Orientation op, u32 x, u32 y, u32 W, u32 H, u32 *sx, u32 *sy) {
    switch (op) {
    case ORIENT_FLIP_H:     *sx = W - 1 - x; *sy = y; break;
    case ORIENT_ROT180:     *sx = W - 1 - x; *sy = H - 1 - y; break;
    case ORIENT_FLIP_V:     *sx = x; *sy = H - 1 - y; break;
    case ORIENT_TRANSPOSE:  *sx = y; *sy = x; break;
    case ORIENT_ROT90:      *sx = y; *sy = H - 1 - x; break;
    case ORIENT_TRANSVERSE: *sx = W - 1 - y; *sy = H - 1 - x; break;
    case ORIENT_ROT270:     *sx = W - 1 - y; *sy = x; break;
    default:                *sx = x; *sy = y; break;
    }
}

static void orient_rows(void *arg, size_t ty0, size_t ty1) {
    const OrientJob *j = arg;
    u32 bpp = image_bytes_per_pixel(j->dst), W = j->src->width, H = j->src->height;
    for (size_t ty = ty0; ty < ty1; ty++) {
        u32 y0 = (u32)ty * TILE, y1 = MIN(y0 + TILE, j->dst->height);
        for (u32 x0 = 0; x0 < j->dst->width; x0 += TILE) {
            u32 x1 = MIN(x0 + TILE, j->dst->width);
            for (u32 y = y0; y < y1; y++) {
                u8 *drow = image_row(j->dst, y);
                for (u32 x = x0; x < x1; x++) {
                    u32 sx, sy;
                    source_of(j->op, x, y, W, H, &sx, &sy);
                    memcpy(drow + (size_t)x * bpp, image_row(j->src, sy) + (size_t)sx * bpp, bpp);
                }
            }
        }
    }
}

Status geometry_orient(Image *im, Orientation op, Diag *diag) {
    if (op <= ORIENT_NONE || op > ORIENT_ROT270) return ST_OK;
    bool swap = op >= ORIENT_TRANSPOSE;
    Image out;
    Status s = image_alloc(&out, swap ? im->height : im->width, swap ? im->width : im->height, im->depth, false, 0, diag);
    if (s != ST_OK) return s;
    OrientJob job = { im, &out, op };
    parallel_range((out.height + TILE - 1) / TILE, 4, orient_rows, &job);
    image_free(im);
    *im = out;
    return ST_OK;
}

// Las 8 orientaciones son el grupo diedrico D4. Cada una es una matriz 2x2 que lleva coordenadas
// (centradas) del destino al origen; componer = multiplicar matrices. Sin tablas a mano que puedan
// estar mal: la composicion sale del algebra y despues se busca que orientacion es.
typedef struct Mat2 { int a, b, c, d; } Mat2;      // [a b; c d]

static const Mat2 ORIENT_MATRIX[9] = {
    [ORIENT_NONE] = { 1, 0, 0, 1 },      [ORIENT_FLIP_H] = { -1, 0, 0, 1 },
    [ORIENT_ROT180] = { -1, 0, 0, -1 },  [ORIENT_FLIP_V] = { 1, 0, 0, -1 },
    [ORIENT_TRANSPOSE] = { 0, 1, 1, 0 }, [ORIENT_ROT90] = { 0, 1, -1, 0 },
    [ORIENT_TRANSVERSE] = { 0, -1, -1, 0 }, [ORIENT_ROT270] = { 0, -1, 1, 0 },
};

static Mat2 mat_mul(Mat2 x, Mat2 y) {
    return (Mat2){ x.a * y.a + x.b * y.c, x.a * y.b + x.b * y.d, x.c * y.a + x.d * y.c, x.c * y.b + x.d * y.d };
}

static Mat2 matrix_of(Orientation o) {
    return o >= ORIENT_NONE && o <= ORIENT_ROT270 ? ORIENT_MATRIX[o] : ORIENT_MATRIX[ORIENT_NONE];
}

static Orientation orientation_of(Mat2 m) {
    for (int o = ORIENT_NONE; o <= ORIENT_ROT270; o++) {
        Mat2 k = ORIENT_MATRIX[o];
        if (k.a == m.a && k.b == m.b && k.c == m.c && k.d == m.d) return (Orientation)o;
    }
    return ORIENT_NONE;                  // inalcanzable: D4 es cerrado bajo el producto
}

// Aplicar `first` y despues `second`: origen = M_first · (M_second · destino).
Orientation geometry_then(Orientation first, Orientation second) {
    return orientation_of(mat_mul(matrix_of(first), matrix_of(second)));
}

// Las 8 matrices son ortogonales: la inversa es la transpuesta.
Orientation geometry_inverse(Orientation o) {
    Mat2 m = matrix_of(o);
    return orientation_of((Mat2){ m.a, m.c, m.b, m.d });
}

// Primero se rota (sentido horario) y despues se espeja en H y/o V la imagen ya rotada.
Orientation geometry_compose(int rotate, bool flip_h, bool flip_v) {
    static const Orientation ROT[4] = { ORIENT_NONE, ORIENT_ROT90, ORIENT_ROT180, ORIENT_ROT270 };
    Orientation o = ROT[((rotate / 90) % 4 + 4) % 4];
    if (flip_h) o = geometry_then(o, ORIENT_FLIP_H);
    if (flip_v) o = geometry_then(o, ORIENT_FLIP_V);
    return o;
}

Status geometry_crop(Image *im, u32 x, u32 y, u32 w, u32 h, Diag *diag) {
    if (!w || !h || x >= im->width || y >= im->height)
        return diag_fail(diag, ST_INVALID, "el recorte %u,%u %ux%u queda fuera de la imagen de %ux%u", x, y, w, h, im->width, im->height);
    w = MIN(w, im->width - x);
    h = MIN(h, im->height - y);
    Image out;
    Status s = image_alloc(&out, w, h, im->depth, false, 0, diag);
    if (s != ST_OK) return s;
    u32 bpp = image_bytes_per_pixel(im);
    for (u32 r = 0; r < h; r++) memcpy(image_row(&out, r), image_row(im, y + r) + (size_t)x * bpp, (size_t)w * bpp);
    image_free(im);
    *im = out;
    return ST_OK;
}

bool geometry_content_box(const Image *im, u32 threshold8, u32 *x0, u32 *y0, u32 *x1, u32 *y1) {
    u32 min_x = im->width, max_x = 0, min_y = im->height, max_y = 0;
    u32 thr = im->depth == 8 ? threshold8 : threshold8 * 257;
    for (u32 y = 0; y < im->height; y++) {
        const u8 *row = image_row(im, y);
        u32 left = im->width, right = 0;
        for (u32 x = 0; x < im->width; x++) {
            u32 a = im->depth == 8 ? row[4 * (size_t)x + 3] : ((const u16 *)row)[4 * (size_t)x + 3];
            if (a > thr) { left = x; break; }
        }
        if (left == im->width) continue;
        for (u32 x = im->width; x-- > left;) {
            u32 a = im->depth == 8 ? row[4 * (size_t)x + 3] : ((const u16 *)row)[4 * (size_t)x + 3];
            if (a > thr) { right = x; break; }
        }
        min_x = MIN(min_x, left);
        max_x = MAX(max_x, right);
        min_y = MIN(min_y, y);
        max_y = y;
    }
    if (min_y > max_y) return false;
    *x0 = min_x; *y0 = min_y; *x1 = max_x + 1; *y1 = max_y + 1;
    return true;
}

Status geometry_pad(Image *im, u32 left, u32 top, u32 right, u32 bottom, const u16 color16[4], Diag *diag) {
    u64 w = (u64)im->width + left + right, h = (u64)im->height + top + bottom;
    if (w > 0x7FFFFFFF || h > 0x7FFFFFFF) return diag_fail(diag, ST_TOO_LARGE, "el margen deja una imagen demasiado grande");
    Image out;
    Status s = image_alloc(&out, (u32)w, (u32)h, im->depth, false, 0, diag);
    if (s != ST_OK) return s;
    image_fill(&out, color16);
    u32 bpp = image_bytes_per_pixel(im);
    for (u32 r = 0; r < im->height; r++)
        memcpy(image_row(&out, top + r) + (size_t)left * bpp, image_row(im, r), (size_t)im->width * bpp);
    image_free(im);
    *im = out;
    return ST_OK;
}
