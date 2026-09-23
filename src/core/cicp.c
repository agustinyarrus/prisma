// core/cicp.c - ver cicp.h
#include <math.h>
#include "cicp.h"
#include "colorspace.h"

// ---- algebra 3x3 en doble precision ------------------------------------------------------------

typedef struct Mat3 { double m[3][3]; } Mat3;

static Mat3 mat3_mul(Mat3 a, Mat3 b) {
    Mat3 r = { 0 };
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// Inversa por cofactores (3x3: mas simple y exacta que eliminar). false si es singular.
static bool mat3_inverse(Mat3 a, Mat3 *out) {
    double (*m)[3] = a.m;                // a es una copia local
    double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (fabs(det) < 1e-12) return false;
    double k = 1.0 / det;
    *out = (Mat3){ {
        { c00 * k, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k },
        { c01 * k, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k },
        { c02 * k, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k },
    } };
    return true;
}

static void mat3_store(Mat3 a, float out[9]) {
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) out[3 * i + j] = (float)a.m[i][j];
}

static Mat3 diagonal(double a, double b, double c) { return (Mat3){ { { a, 0, 0 }, { 0, b, 0 }, { 0, 0, c } } }; }

// ---- primarios -------------------------------------------------------------------------------

typedef struct Chromaticity { double x, y; } Chromaticity;

typedef struct PrimariesSpec {
    u8           code;
    const char  *name;
    Chromaticity r, g, b, white;
} PrimariesSpec;

#define D65 { 0.3127, 0.3290 }
#define ILLUMINANT_C { 0.3100, 0.3160 }

static const PrimariesSpec PRIMARIES[] = {
    { 1,  "BT.709 / sRGB",     { 0.640, 0.330 }, { 0.300, 0.600 }, { 0.150, 0.060 }, D65 },
    { 4,  "BT.470 M",          { 0.670, 0.330 }, { 0.210, 0.710 }, { 0.140, 0.080 }, ILLUMINANT_C },
    { 5,  "BT.601 PAL",        { 0.640, 0.330 }, { 0.290, 0.600 }, { 0.150, 0.060 }, D65 },
    { 6,  "BT.601 NTSC",       { 0.630, 0.340 }, { 0.310, 0.595 }, { 0.155, 0.070 }, D65 },
    { 7,  "SMPTE 240M",        { 0.630, 0.340 }, { 0.310, 0.595 }, { 0.155, 0.070 }, D65 },
    { 8,  "pelicula generica", { 0.681, 0.319 }, { 0.243, 0.692 }, { 0.145, 0.049 }, ILLUMINANT_C },
    { 9,  "BT.2020",           { 0.708, 0.292 }, { 0.170, 0.797 }, { 0.131, 0.046 }, D65 },
    { 11, "DCI-P3",            { 0.680, 0.320 }, { 0.265, 0.690 }, { 0.150, 0.060 }, { 0.3140, 0.3510 } },
    { 12, "Display P3",        { 0.680, 0.320 }, { 0.265, 0.690 }, { 0.150, 0.060 }, D65 },
    { 22, "EBU 3213",          { 0.630, 0.340 }, { 0.295, 0.605 }, { 0.155, 0.077 }, D65 },
};

static const PrimariesSpec *primaries_spec(u32 code) {
    for (size_t i = 0; i < ARRAY_LEN(PRIMARIES); i++)
        if (PRIMARIES[i].code == code) return &PRIMARIES[i];
    return NULL;
}

INLINE double xyz_x(Chromaticity c) { return c.x / c.y; }
INLINE double xyz_z(Chromaticity c) { return (1.0 - c.x - c.y) / c.y; }

// RGB lineal -> XYZ: las columnas son los primarios, escalados para que (1,1,1) sea el blanco.
static bool rgb_to_xyz(const PrimariesSpec *p, Mat3 *out) {
    Mat3 cols = { {
        { xyz_x(p->r), xyz_x(p->g), xyz_x(p->b) },
        { 1.0, 1.0, 1.0 },
        { xyz_z(p->r), xyz_z(p->g), xyz_z(p->b) },
    } }, inv;
    if (!mat3_inverse(cols, &inv)) return false;
    double w[3] = { xyz_x(p->white), 1.0, xyz_z(p->white) }, s[3];
    for (int i = 0; i < 3; i++) s[i] = inv.m[i][0] * w[0] + inv.m[i][1] * w[1] + inv.m[i][2] * w[2];
    *out = mat3_mul(cols, diagonal(s[0], s[1], s[2]));
    return true;
}

// Adaptacion cromatica de Bradford entre dos blancos (identidad si son iguales).
static Mat3 bradford(Chromaticity from, Chromaticity to) {
    static const Mat3 B = { { { 0.8951, 0.2664, -0.1614 }, { -0.7502, 1.7135, 0.0367 }, { 0.0389, -0.0685, 1.0296 } } };
    Mat3 b_inv;
    mat3_inverse(B, &b_inv);
    double src[3], dst[3], wf[3] = { xyz_x(from), 1.0, xyz_z(from) }, wt[3] = { xyz_x(to), 1.0, xyz_z(to) };
    for (int i = 0; i < 3; i++) {
        src[i] = B.m[i][0] * wf[0] + B.m[i][1] * wf[1] + B.m[i][2] * wf[2];
        dst[i] = B.m[i][0] * wt[0] + B.m[i][1] * wt[1] + B.m[i][2] * wt[2];
    }
    return mat3_mul(b_inv, mat3_mul(diagonal(dst[0] / src[0], dst[1] / src[1], dst[2] / src[2]), B));
}

bool cicp_gamut_to_srgb(u32 primaries, float out[9]) {
    const PrimariesSpec *from = primaries_spec(primaries), *srgb = primaries_spec(CICP_PRIMARIES_BT709);
    Mat3 to_xyz, srgb_to_xyz, xyz_to_srgb;
    if (!from || !rgb_to_xyz(from, &to_xyz) || !rgb_to_xyz(srgb, &srgb_to_xyz) || !mat3_inverse(srgb_to_xyz, &xyz_to_srgb))
        return false;
    mat3_store(mat3_mul(xyz_to_srgb, mat3_mul(bradford(from->white, srgb->white), to_xyz)), out);
    return true;
}

// ---- matrices YCbCr --------------------------------------------------------------------------

// Coeficientes de luma (Kr, Kb) de cada matriz de H.273. La 2 (sin especificar) se lee como
// BT.601, igual que libavif y lo que MIAF define por omision.
static bool luma_coefficients(u32 matrix, u32 primaries, double *kr, double *kb) {
    switch (matrix) {
    case 1:                 *kr = 0.2126; *kb = 0.0722; return true;
    case 2: case 5: case 6: *kr = 0.299;  *kb = 0.114;  return true;
    case 4:                 *kr = 0.30;   *kb = 0.11;   return true;
    case 7:                 *kr = 0.212;  *kb = 0.087;  return true;
    case 9:                 *kr = 0.2627; *kb = 0.0593; return true;
    case 12: {              // derivada de los primarios (luminancia no constante)
        const PrimariesSpec *p = primaries_spec(primaries ? primaries : CICP_PRIMARIES_BT709);
        Mat3 m;
        if (!p || !rgb_to_xyz(p, &m)) return false;
        *kr = m.m[1][0];
        *kb = m.m[1][2];
        return true;
    }
    default: return false;  // 0 identidad (GBR), 8 YCgCo, 10/13 luminancia constante, 11, 14 ICtCp
    }
}

// RGB -> (Y, Cb, Cr) con Cb y Cr centrados en 0 (el rango y el desplazamiento se cancelan al
// componer dos de estas matrices).
static Mat3 ycbcr_matrix(double kr, double kb) {
    double kg = 1.0 - kr - kb;
    return (Mat3){ {
        { kr, kg, kb },
        { -kr / (2.0 * (1.0 - kb)), -kg / (2.0 * (1.0 - kb)), 0.5 },
        { 0.5, -kg / (2.0 * (1.0 - kr)), -kb / (2.0 * (1.0 - kr)) },
    } };
}

bool cicp_ycbcr_correction(u32 used, u32 declared, u32 primaries, float out[9]) {
    double kr_u, kb_u, kr_d, kb_d;
    if (!luma_coefficients(used, primaries, &kr_u, &kb_u) || !luma_coefficients(declared, primaries, &kr_d, &kb_d)) return false;
    Mat3 declared_inv;
    if (!mat3_inverse(ycbcr_matrix(kr_d, kb_d), &declared_inv)) return false;
    mat3_store(mat3_mul(declared_inv, ycbcr_matrix(kr_u, kb_u)), out);
    return true;
}

// ---- nombres y transferencias -----------------------------------------------------------------

const char *cicp_primaries_name(u32 primaries) {
    const PrimariesSpec *p = primaries_spec(primaries);
    return p ? p->name : NULL;
}

const char *cicp_transfer_name(u32 transfer) {
    switch (transfer) {
    case 1: case 6: case 14: case 15: return "BT.709";
    case 4:  return "gamma 2,2";
    case 5:  return "gamma 2,8";
    case 7:  return "SMPTE 240M";
    case 8:  return "lineal";
    case 13: return "sRGB";
    case 16: return "PQ (HDR)";
    case 17: return "SMPTE 428";
    case 18: return "HLG (HDR)";
    default: return NULL;
    }
}

bool cicp_is_srgb_like(const Cicp *c) {
    return (c->primaries == CICP_PRIMARIES_BT709 || c->primaries == CICP_UNSPECIFIED) &&
           (c->transfer == CICP_TRANSFER_SRGB || c->transfer == CICP_UNSPECIFIED);
}

bool cicp_is_hdr(const Cicp *c) { return c->transfer == CICP_TRANSFER_PQ || c->transfer == CICP_TRANSFER_HLG; }

bool cicp_transfer_invertible(u32 transfer) {
    switch (transfer) {
    case 1: case 4: case 5: case 6: case 7: case 8: case 13: case 14: case 15: return true;
    default: return false;
    }
}

// Inversa de la curva de codificacion (sin HDR). BT.709/601/2020 usan la inversa de la OETF,
// como skcms y los navegadores al mostrar AVIF.
float cicp_to_linear(u32 transfer, float v) {
    switch (transfer) {
    case 13: return srgb_to_linear(v);
    case 1: case 6: case 14: case 15: return v < 0.081f ? v / 4.5f : powf((v + 0.099f) / 1.099f, 1.0f / 0.45f);
    case 7:  return v < 0.0913f ? v / 4.0f : powf((v + 0.1115f) / 1.1115f, 1.0f / 0.45f);
    case 4:  return powf(v, 2.2f);
    case 5:  return powf(v, 2.8f);
    default: return v;
    }
}
