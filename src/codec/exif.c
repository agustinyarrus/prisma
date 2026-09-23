// codec/exif.c - ver exif.h
#include "exif.h"

enum { TAG_ORIENTATION = 0x0112, TYPE_SHORT = 3, IFD_ENTRY_SIZE = 12, MAX_IFD_ENTRIES = 1024 };

typedef struct TiffView {
    const u8 *data;
    size_t    size;
    bool      big_endian;
} TiffView;

static u16 tv16(const TiffView *t, size_t at) { return t->big_endian ? load_be16(t->data + at) : load_le16(t->data + at); }
static u32 tv32(const TiffView *t, size_t at) { return t->big_endian ? load_be32(t->data + at) : load_le32(t->data + at); }

// Busca la entrada de orientacion en el IFD0; devuelve el desplazamiento de su valor o 0.
static size_t find_orientation(const u8 *tiff, size_t size, TiffView *view) {
    if (size < 8) return 0;
    TiffView t = { tiff, size, false };
    if (memcmp(tiff, "II*\0", 4) == 0) t.big_endian = false;
    else if (memcmp(tiff, "MM\0*", 4) == 0) t.big_endian = true;
    else return 0;
    u32 ifd = tv32(&t, 4);
    if (ifd < 8 || (size_t)ifd + 2 > size) return 0;
    u32 count = tv16(&t, ifd);
    if (count > MAX_IFD_ENTRIES || (size_t)ifd + 2 + (size_t)count * IFD_ENTRY_SIZE > size) return 0;
    for (u32 i = 0; i < count; i++) {
        size_t e = (size_t)ifd + 2 + (size_t)i * IFD_ENTRY_SIZE;
        if (tv16(&t, e) == TAG_ORIENTATION && tv16(&t, e + 2) == TYPE_SHORT && tv32(&t, e + 4) >= 1) {
            *view = t;
            return e + 8;          // el SHORT va en los primeros 2 bytes del campo de valor
        }
    }
    return 0;
}

u16 exif_orientation(const u8 *tiff, size_t size) {
    TiffView t;
    size_t at = find_orientation(tiff, size, &t);
    if (!at) return 0;
    u16 v = tv16(&t, at);
    return v >= 1 && v <= 8 ? v : 0;
}

bool exif_set_orientation(u8 *tiff, size_t size, u16 value) {
    TiffView t;
    size_t at = find_orientation(tiff, size, &t);
    if (!at) return false;
    if (t.big_endian) store_be16(tiff + at, value);
    else store_le16(tiff + at, value);
    return true;
}

size_t exif_tiff_offset(const u8 *data, size_t size) {
    return size >= 6 && memcmp(data, "Exif\0\0", 6) == 0 ? 6 : 0;
}
