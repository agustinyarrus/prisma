// codec/exif.h - lo minimo de EXIF que hace falta: leer y reescribir la orientacion.
//
// La orientacion (tag 0x0112) es la unica etiqueta que cambia como se VE la imagen: una foto de
// celular en vertical viene guardada acostada con orientacion 6. prisma rota los pixeles al
// convertir (--no-auto-orient lo evita) y, si se conserva el EXIF, lo reescribe a 1 para que
// ningun visor la vuelva a rotar.
#pragma once
#include "../core/base.h"

// `tiff` apunta al encabezado TIFF ("II*\0" o "MM\0*"), sin el prefijo "Exif\0\0" de JPEG.
u16  exif_orientation(const u8 *tiff, size_t size);          // 1..8, o 0 si no hay
bool exif_set_orientation(u8 *tiff, size_t size, u16 value); // en el lugar; false si no estaba

// Si el bloque empieza con "Exif\0\0" devuelve el desplazamiento del TIFF (6), si no 0.
size_t exif_tiff_offset(const u8 *data, size_t size);
