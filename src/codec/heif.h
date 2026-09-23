// codec/heif.h - la estructura ISOBMFF de HEIF/HEIC/AVIF, sin decodificar ni un pixel.
//
// El codec HEIF de Windows entrega solo la imagen PRIMARIA y la convierte a RGB a su manera. Dos
// cosas se pierden en el camino y este modulo las recupera leyendo las cajas:
//
//  1. El ALFA. Viaja como imagen auxiliar aparte (un item con la propiedad auxC ".../alpha" y una
//     referencia auxl a la primaria) y WIC la ignora: sin esto, todo AVIF transparente sale opaco.
//     El truco: una copia del archivo con 'pitm' apuntando al item del alfa (y el flag de oculto
//     apagado) hace que el MISMO codec lo decodifique como una imagen gris normal, con su
//     profundidad, su rango y sus mosaicos (grid).
//  2. El COLOR. Con AV1 el codec no respeta la matriz YCbCr que declara la caja colr 'nclx': la
//     deduce de los primarios (medido: 5/6 -> BT.601, 1 -> BT.709) y con primarios 9, 10 y 12
//     ademas convierte de gama por su cuenta. Un AVIF BT.601 con primarios BT.709 (lo normal de
//     libavif) salia hasta 35 niveles corrido. Solucion: decodificar una copia con la nclx
//     reescrita (heif_rewrite_nclx) para que el codec use la matriz correcta y no toque la gama;
//     el color real se declara despues en el PNG.
//
// Tambien se leen los giros/espejos (irot/imir) de cada item, porque el alfa tiene que quedar
// alineado con el color ya orientado, y si el color viene premultiplicado (referencia 'prem').
#pragma once
#include "../core/base.h"
#include "../core/cicp.h"
#include "../ops/geometry.h"

typedef struct HeifInfo {
    u32         primary_id;
    bool        av1;                      // marca avif/avis: las imagenes son AV1
    bool        has_nclx;                 // colr 'nclx' asociada a la primaria
    Cicp        cicp;                     // la de nclx, o la de MIAF por omision (1, 13, 6, completo)
    size_t      nclx_offset;              // primer byte de los codigos CICP de esa nclx (0 = no hay)
    bool        has_icc;                  // colr 'prof'/'rICC' asociada a la primaria
    Orientation primary_orientation;      // irot/imir que el codec aplica al primario

    bool        has_alpha;
    u32         alpha_id;
    bool        premultiplied;            // 'prem': el color del primario viene multiplicado por el alfa
    Orientation alpha_orientation;        // irot/imir del alfa (el formato permite que difieran)
    size_t      pitm_id_offset;           // item_ID de 'pitm' dentro del archivo
    u8          pitm_id_size;             // 2 (version 0) o 4 (version 1)
    size_t      alpha_flags_offset;       // ultimo byte de flags del 'infe' del alfa (0 = no hallado)
} HeifInfo;

// Lee la estructura. O(cajas + items + propiedades), sin reservar mas que el indice de
// propiedades. false si no es ISOBMFF/HEIF legible (entonces no hay nada que corregir).
bool heif_inspect(const u8 *data, size_t size, HeifInfo *out);

// Copia del archivo (mem_alloc, `size` bytes) en la que el alfa es la imagen primaria visible.
// Requiere info->has_alpha. NULL sin memoria.
u8 *heif_promote_alpha(const u8 *data, size_t size, const HeifInfo *info);

// Copia del archivo con la nclx de la primaria reescrita (mismo tamaño, se edita en el lugar).
// Requiere info->nclx_offset. NULL sin memoria.
u8 *heif_rewrite_nclx(const u8 *data, size_t size, const HeifInfo *info, const Cicp *cicp);
