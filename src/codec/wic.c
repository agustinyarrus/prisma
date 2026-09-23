// codec/wic.c - ver wic.h
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <propidl.h>
#include <math.h>
#include <stdio.h>
#include "wic.h"
#include "exif.h"
#include "heif.h"
#include "../core/text.h"
#include "../core/pool.h"
#include "../core/colorspace.h"
#include "../ops/geometry.h"
#include "../ops/pixelops.h"

enum { BAND_ROWS = 256, MAX_COLOR_CONTEXTS = 8 };

// HRESULT de WIC que conviene explicar con palabras.
#define HR_COMPONENT_NOT_FOUND ((HRESULT)0x88982F50L)
#define HR_BAD_HEADER          ((HRESULT)0x88982F61L)
#define HR_BAD_IMAGE           ((HRESULT)0x88982F60L)
#define HR_UNKNOWN_FORMAT      ((HRESULT)0x88982F07L)
#define HR_STREAM_READ         ((HRESULT)0x88982F72L)
#define HR_FRAME_MISSING       ((HRESULT)0x88982F62L)
#define HR_UNSUPPORTED_OP      ((HRESULT)0x88982F81L)

static _Thread_local bool                tls_com_ready;
static _Thread_local IWICImagingFactory *tls_factory;

static IWICImagingFactory *factory(void) {
    if (!tls_com_ready) {
        HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
        tls_com_ready = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }
    if (!tls_factory && tls_com_ready)
        CoCreateInstance(&CLSID_WICImagingFactory2, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&tls_factory);
    return tls_factory;
}

#define RELEASE(p) do { if (p) { IUnknown_Release((IUnknown *)(p)); (p) = NULL; } } while (0)

static Status fail_hr(Diag *diag, HRESULT hr, const char *what) {
    const char *why = hr == HR_COMPONENT_NOT_FOUND ? "no hay un codec de Windows instalado para este formato"
                    : hr == HR_BAD_HEADER || hr == HR_UNKNOWN_FORMAT ? "encabezado ilegible"
                    : hr == HR_BAD_IMAGE ? "datos de imagen dañados"
                    : hr == HR_STREAM_READ ? "el archivo termina antes de tiempo"
                    : hr == E_OUTOFMEMORY ? "sin memoria"
                    : NULL;
    Status status = hr == HR_COMPONENT_NOT_FOUND || hr == HR_UNKNOWN_FORMAT ? ST_UNKNOWN_FORMAT
                  : hr == HR_STREAM_READ ? ST_TRUNCATED
                  : hr == E_OUTOFMEMORY ? ST_NO_MEMORY : ST_CORRUPT;
    if (why) return diag_fail(diag, status, "%s: %s", what, why);
    return diag_fail(diag, status, "%s: error de WIC 0x%08lX", what, (unsigned long)hr);
}

// ---- nombre del contenedor -----------------------------------------------------------------

typedef struct ContainerName {
    const GUID *guid;
    const char *name;
} ContainerName;

static const char *container_name(const GUID *g) {
    static const ContainerName NAMES[] = {
        { &GUID_ContainerFormatJpeg, "JPEG" },   { &GUID_ContainerFormatTiff, "TIFF" },
        { &GUID_ContainerFormatWmp, "JPEG XR" }, { &GUID_ContainerFormatHeif, "HEIF" },
        { &GUID_ContainerFormatWebp, "WebP" },   { &GUID_ContainerFormatRaw, "RAW" },
        { &GUID_ContainerFormatAdng, "DNG" },    { &GUID_ContainerFormatDds, "DDS" },
        { &GUID_ContainerFormatJpegXL, "JPEG XL" }, { &GUID_ContainerFormatPng, "PNG" },
        { &GUID_ContainerFormatGif, "GIF" },     { &GUID_ContainerFormatBmp, "BMP" },
        { &GUID_ContainerFormatIco, "ICO" },
    };
    for (size_t i = 0; i < ARRAY_LEN(NAMES); i++)
        if (IsEqualGUID(g, NAMES[i].guid)) return NAMES[i].name;
    return NULL;
}

// Los contenedores HEIF (HEIC y AVIF) llevan la rotacion en las cajas irot/imir, que el codec
// ya aplica al decodificar; su EXIF de orientacion es solo informativo y aplicarlo de nuevo
// giraria dos veces la foto (el clasico bug de las fotos de iPhone).
static bool orientation_already_applied(const GUID *container) {
    return IsEqualGUID(container, &GUID_ContainerFormatHeif);
}

// ---- formato de pixeles de origen ----------------------------------------------------------

typedef struct PixelFormatFacts {
    u32  bits_per_pixel;
    u32  bits_per_channel;       // el canal mas ancho, contado en su mascara
    u32  channels;
    bool is_float;
    bool has_alpha;
    bool premultiplied;
    bool is_cmyk;
    bool indexed;
} PixelFormatFacts;

static bool guid_in(const GUID *g, const GUID *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) if (IsEqualGUID(g, list[i])) return true;
    return false;
}

// Bits del canal mas ancho segun las mascaras de WIC. bpp / canales miente con los formatos con
// relleno: 32bppBGR son 3 canales de 8 (y 8 bits de nada), no "10 bits".
static u32 widest_channel_bits(IWICPixelFormatInfo2 *info, u32 channels, u32 bpp) {
    enum { MAX_MASK_BYTES = 32 };                       // 256 bits: el formato mas ancho de WIC es de 144
    BYTE mask[MAX_MASK_BYTES];
    UINT mask_bytes = (bpp + 7) / 8, widest = 0;
    if (!mask_bytes || mask_bytes > MAX_MASK_BYTES) return 0;
    for (UINT c = 0; c < channels; c++) {
        UINT got = 0, bits = 0;
        if (FAILED(IWICPixelFormatInfo2_GetChannelMask(info, c, mask_bytes, mask, &got))) return 0;
        for (UINT i = 0; i < got; i++) bits += (UINT)__builtin_popcount(mask[i]);
        widest = MAX(widest, bits);
    }
    return widest;
}

static PixelFormatFacts pixel_format_facts(IWICImagingFactory *f, const GUID *pf) {
    PixelFormatFacts facts = { 32, 8, 4, false, true, false, false, false };
    IWICComponentInfo *ci = NULL;
    IWICPixelFormatInfo2 *info = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateComponentInfo(f, pf, &ci)) &&
        SUCCEEDED(IWICComponentInfo_QueryInterface(ci, &IID_IWICPixelFormatInfo2, (void **)&info))) {
        UINT bpp = 0, ch = 0;
        BOOL transparency = FALSE;
        WICPixelFormatNumericRepresentation rep = WICPixelFormatNumericRepresentationUnspecified;
        IWICPixelFormatInfo2_GetBitsPerPixel(info, &bpp);
        IWICPixelFormatInfo2_GetChannelCount(info, &ch);
        IWICPixelFormatInfo2_SupportsTransparency(info, &transparency);
        IWICPixelFormatInfo2_GetNumericRepresentation(info, &rep);
        facts.bits_per_pixel = bpp;
        facts.channels = ch ? ch : 1;
        u32 widest = widest_channel_bits(info, facts.channels, bpp);
        facts.bits_per_channel = widest ? widest : bpp / facts.channels;
        facts.is_float = rep == WICPixelFormatNumericRepresentationFloat || rep == WICPixelFormatNumericRepresentationFixed;
        facts.indexed = rep == WICPixelFormatNumericRepresentationIndexed;
        facts.has_alpha = transparency != FALSE;
    }
    RELEASE(info);
    RELEASE(ci);
    static const GUID *const PREMULTIPLIED[] = {
        &GUID_WICPixelFormat32bppPBGRA, &GUID_WICPixelFormat32bppPRGBA, &GUID_WICPixelFormat64bppPRGBA,
        &GUID_WICPixelFormat64bppPBGRA, &GUID_WICPixelFormat128bppPRGBAFloat, &GUID_WICPixelFormat64bppPRGBAHalf,
    };
    static const GUID *const CMYK[] = {
        &GUID_WICPixelFormat32bppCMYK, &GUID_WICPixelFormat64bppCMYK, &GUID_WICPixelFormat40bppCMYKAlpha,
        &GUID_WICPixelFormat80bppCMYKAlpha,
    };
    facts.premultiplied = guid_in(pf, PREMULTIPLIED, ARRAY_LEN(PREMULTIPLIED));
    facts.is_cmyk = guid_in(pf, CMYK, ARRAY_LEN(CMYK));
    return facts;
}

// ---- metadatos ------------------------------------------------------------------------------

static u16 read_orientation(IWICBitmapFrameDecode *frame) {
    IWICMetadataQueryReader *qr = NULL;
    u16 value = 0;
    if (SUCCEEDED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &qr))) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        if (SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(qr, L"System.Photo.Orientation", &pv)) && pv.vt == VT_UI2)
            value = pv.uiVal >= 1 && pv.uiVal <= 8 ? pv.uiVal : 0;
        PropVariantClear(&pv);
    }
    RELEASE(qr);
    return value;
}

// Devuelve el contexto de color con perfil ICC (con AddRef) y copia el perfil a meta.
static IWICColorContext *read_icc(IWICImagingFactory *f, IWICBitmapFrameDecode *frame, Metadata *meta) {
    UINT count = 0;
    if (FAILED(IWICBitmapFrameDecode_GetColorContexts(frame, 0, NULL, &count)) || !count) return NULL;
    count = MIN(count, (UINT)MAX_COLOR_CONTEXTS);
    IWICColorContext *ctxs[MAX_COLOR_CONTEXTS] = { 0 };
    for (UINT i = 0; i < count; i++)
        if (FAILED(IWICImagingFactory_CreateColorContext(f, &ctxs[i]))) { count = i; break; }
    IWICColorContext *found = NULL;
    UINT actual = 0;
    if (SUCCEEDED(IWICBitmapFrameDecode_GetColorContexts(frame, count, ctxs, &actual))) {
        for (UINT i = 0; i < actual && !found; i++) {
            WICColorContextType type;
            if (FAILED(IWICColorContext_GetType(ctxs[i], &type)) || type != WICColorContextProfile) continue;
            UINT size = 0;
            if (FAILED(IWICColorContext_GetProfileBytes(ctxs[i], 0, NULL, &size)) || !size) continue;
            u8 *bytes = mem_alloc(size);
            if (bytes && SUCCEEDED(IWICColorContext_GetProfileBytes(ctxs[i], size, bytes, &size))) {
                blob_free(&meta->icc);
                meta->icc.data = bytes;
                meta->icc.size = size;
                found = ctxs[i];
                IWICColorContext_AddRef(found);
            } else {
                mem_free(bytes);
            }
        }
    }
    for (UINT i = 0; i < count; i++) RELEASE(ctxs[i]);
    return found;
}

// JPEG: el EXIF y el XMP crudos salen directo de los marcadores APP1 (WIC no da el bloque).
static void jpeg_scan_app_segments(const u8 *data, size_t size, Metadata *meta) {
    static const char XMP_NS[] = "http://ns.adobe.com/xap/1.0/";
    size_t at = 2;
    while (at + 4 <= size && data[at] == 0xFF) {
        u8 marker = data[at + 1];
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { at += 2; continue; }
        if (marker == 0xDA || marker == 0xD9) break;            // empieza la imagen: no hay mas APPn
        size_t len = load_be16(data + at + 2);
        if (len < 2 || at + 2 + len > size) break;
        const u8 *seg = data + at + 4;
        size_t seg_len = len - 2;
        if (marker == 0xE1) {
            size_t tiff = exif_tiff_offset(seg, seg_len);
            if (tiff && !meta->exif.size) blob_set(&meta->exif, seg + tiff, seg_len - tiff);
            else if (seg_len > sizeof XMP_NS && memcmp(seg, XMP_NS, sizeof XMP_NS) == 0 && !meta->xmp.size)
                blob_set(&meta->xmp, seg + sizeof XMP_NS, seg_len - sizeof XMP_NS);
        }
        at += 2 + len;
    }
}

// ---- copia de pixeles -------------------------------------------------------------------------

static Status copy_integer(IWICBitmapSource *src, Image *image, Diag *diag) {
    for (u32 y = 0; y < image->height; y += BAND_ROWS) {
        if (pool_cancelled()) return diag_fail(diag, ST_CANCELLED, "cancelado");
        u32 rows = MIN((u32)BAND_ROWS, image->height - y);
        WICRect rect = { 0, (INT)y, (INT)image->width, (INT)rows };
        HRESULT hr = IWICBitmapSource_CopyPixels(src, &rect, (UINT)image->stride, (UINT)(image->stride * rows), image_row(image, y));
        if (FAILED(hr)) return fail_hr(diag, hr, "WIC no pudo entregar los pixeles");
    }
    return ST_OK;
}

static Status copy_float(IWICBitmapSource *src, const DecodeOptions *opt, Image *image, ToneOperator *used, Diag *diag) {
    size_t pixels = (size_t)image->width * image->height, bytes;
    if (!mul_ok(pixels, 16, &bytes)) return diag_fail(diag, ST_TOO_LARGE, "imagen HDR demasiado grande");
    float *buf = mem_alloc(bytes);
    if (!buf) return diag_fail(diag, ST_NO_MEMORY, "sin memoria para la imagen HDR");
    size_t row_bytes = (size_t)image->width * 16;
    for (u32 y = 0; y < image->height; y += BAND_ROWS) {
        u32 rows = MIN((u32)BAND_ROWS, image->height - y);
        WICRect rect = { 0, (INT)y, (INT)image->width, (INT)rows };
        HRESULT hr = IWICBitmapSource_CopyPixels(src, &rect, (UINT)row_bytes, (UINT)(row_bytes * rows), (BYTE *)(buf + (size_t)y * image->width * 4));
        if (FAILED(hr)) { mem_free(buf); return fail_hr(diag, hr, "WIC no pudo entregar los pixeles HDR"); }
    }
    float scale = exp2f(opt->exposure);
    *used = tonemap_resolve(opt->tonemap, buf, pixels, scale);
    colorspace_init();
    for (u32 y = 0; y < image->height; y++)
        tonemap_row_rgba16(buf + (size_t)y * image->width * 4, image->width, *used, scale, (u16 *)image_row(image, y));
    mem_free(buf);
    return ST_OK;
}

// Transforma con el motor de color de Windows (perfil de origen -> sRGB) y entrega BGRA.
static IWICBitmapSource *make_srgb_transform(IWICImagingFactory *f, IWICBitmapSource *src, IWICColorContext *from, const GUID *bgra) {
    IWICColorContext *to = NULL;
    IWICColorTransform *xf = NULL;
    IWICBitmapSource *result = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateColorContext(f, &to)) &&
        SUCCEEDED(IWICColorContext_InitializeFromExifColorSpace(to, 1)) &&
        SUCCEEDED(IWICImagingFactory_CreateColorTransformer(f, &xf)) &&
        SUCCEEDED(IWICColorTransform_Initialize(xf, src, from, to, bgra))) {
        result = (IWICBitmapSource *)xf;
        xf = NULL;
    }
    RELEASE(xf);
    RELEASE(to);
    return result;
}

static void swap_red_blue(Image *image) {
    for (u32 y = 0; y < image->height; y++) {
        u8 *row = image_row(image, y);
        if (image->depth == 8) {
            for (u32 x = 0; x < image->width; x++) { u8 t = row[4 * x]; row[4 * x] = row[4 * x + 2]; row[4 * x + 2] = t; }
        } else {
            u16 *p = (u16 *)row;
            for (u32 x = 0; x < image->width; x++) { u16 t = p[4 * x]; p[4 * x] = p[4 * x + 2]; p[4 * x + 2] = t; }
        }
    }
}

// ---- decodificacion ---------------------------------------------------------------------------

typedef struct WicSession {
    IWICImagingFactory *f;
    IWICStream         *stream;
    IWICBitmapDecoder  *decoder;
    GUID                container;
} WicSession;

static void session_close(WicSession *s) {
    RELEASE(s->decoder);
    RELEASE(s->stream);
}

// `data` es el archivo (o una copia retocada): WIC lo lee sin copiar, tiene que vivir mas que la sesion.
static Status session_open(const u8 *data, size_t size, Diag *diag, WicSession *s) {
    memset(s, 0, sizeof *s);
    s->f = factory();
    if (!s->f) return diag_fail(diag, ST_UNSUPPORTED, "WIC no esta disponible en este sistema");
    if (size > 0xFFFFFFFFu) return diag_fail(diag, ST_TOO_LARGE, "WIC no acepta archivos de mas de 4 GB");
    HRESULT hr = IWICImagingFactory_CreateStream(s->f, &s->stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(s->stream, (BYTE *)data, (DWORD)size);
    if (FAILED(hr)) { session_close(s); return fail_hr(diag, hr, "WIC"); }
    hr = IWICImagingFactory_CreateDecoderFromStream(s->f, (IStream *)s->stream, NULL, WICDecodeMetadataCacheOnDemand, &s->decoder);
    if (FAILED(hr)) { session_close(s); return fail_hr(diag, hr, "formato no reconocido"); }
    IWICBitmapDecoder_GetContainerFormat(s->decoder, &s->container);
    return ST_OK;
}

static void describe_codec(WicSession *s, Decoded *out) {
    char label[64] = "WIC";
    IWICBitmapDecoderInfo *info = NULL;
    if (SUCCEEDED(IWICBitmapDecoder_GetDecoderInfo(s->decoder, &info))) {
        wchar_t wname[96];
        UINT got = 0;
        if (SUCCEEDED(IWICBitmapDecoderInfo_GetFriendlyName(info, ARRAY_LEN(wname), wname, &got))) {
            char *name = utf16_to_utf8(wname);
            if (name) {
                // "Microsoft HEIF Decoder" -> "WIC · HEIF Decoder" (el "Microsoft" no aporta).
                const char *shown = str_starts_with(name, "Microsoft ") ? name + 10 : name;
                snprintf(label, sizeof label, "WIC · %s", shown);
                mem_free(name);
            }
        }
    }
    RELEASE(info);
    const char *format = container_name(&s->container);
    source_set(&out->source, format ? format : "WIC", label);
}

static Status decode_frame(const DecodeContext *ctx, WicSession *s, u32 index, bool is_first, Decoded *out) {
    Diag *diag = ctx->diag;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    IWICColorContext *icc_ctx = NULL;
    IWICBitmapSource *xform = NULL;
    Metadata scratch_meta = { 0 };           // los cuadros 2..n no pisan los metadatos del primero
    Status status = ST_OK;
    HRESULT hr = IWICBitmapDecoder_GetFrame(s->decoder, index, &frame);
    if (FAILED(hr)) return fail_hr(diag, hr, "WIC no pudo abrir el cuadro");

    UINT w = 0, h = 0;
    GUID pf;
    IWICBitmapFrameDecode_GetSize(frame, &w, &h);
    IWICBitmapFrameDecode_GetPixelFormat(frame, &pf);
    PixelFormatFacts facts = pixel_format_facts(s->f, &pf);
    u32 bits_per_channel = facts.indexed ? 8 : facts.bits_per_channel;

    if (is_first) {
        SourceInfo *src = &out->source;
        src->bits = bits_per_channel;
        src->channels = facts.channels;
        src->has_alpha = facts.has_alpha && !facts.is_cmyk;
        src->alpha_origin = !src->has_alpha ? ALPHA_ORIGIN_NONE : facts.premultiplied ? ALPHA_ORIGIN_PREMULTIPLIED : ALPHA_ORIGIN_CHANNEL;
        src->is_float = facts.is_float;
        source_detail(src, "%s%u canales · %u bits%s%s", facts.is_cmyk ? "CMYK · " : "", facts.channels, bits_per_channel,
                      facts.is_float ? " flotante" : "", facts.premultiplied ? " · premultiplicado" : "");
        if (!orientation_already_applied(&s->container)) out->meta.orientation = read_orientation(frame);
    }
    // El perfil se lee siempre (se conserva en iCCP), pero solo se aplica con --icc srgb o si
    // el origen es CMYK: la conversion ingenua de CMYK a RGB sin perfil queda muy lejos.
    icc_ctx = read_icc(s->f, frame, is_first ? &out->meta : &scratch_meta);
    bool transform = icc_ctx && (ctx->opt->to_srgb || facts.is_cmyk) && !facts.is_float;

    bool wide = bits_per_channel > 8 && !facts.indexed;
    const GUID *target = facts.is_float ? &GUID_WICPixelFormat128bppRGBAFloat
                       : transform ? (wide ? &GUID_WICPixelFormat64bppBGRA : &GUID_WICPixelFormat32bppBGRA)
                       : wide ? &GUID_WICPixelFormat64bppRGBA : &GUID_WICPixelFormat32bppRGBA;
    IWICBitmapSource *source = (IWICBitmapSource *)frame;
    if (transform) {
        xform = make_srgb_transform(s->f, source, icc_ctx, target);
        if (xform) source = xform;
        else { transform = false; target = wide ? &GUID_WICPixelFormat64bppRGBA : &GUID_WICPixelFormat32bppRGBA; }
    }
    hr = IWICImagingFactory_CreateFormatConverter(s->f, &conv);
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(conv, source, target, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { status = fail_hr(diag, hr, "WIC no sabe convertir este formato de pixel"); goto done; }

    Frame *out_frame = decoded_add_frame(out);
    if (!out_frame) { status = diag_fail(diag, ST_NO_MEMORY, "sin memoria"); goto done; }
    out_frame->index = index;
    status = image_alloc(&out_frame->image, w, h, wide || facts.is_float ? 16 : 8, false, ctx->opt->max_pixels, diag);
    if (status != ST_OK) goto done;
    if (facts.is_float) {
        ToneOperator used;
        status = copy_float((IWICBitmapSource *)conv, ctx->opt, &out_frame->image, &used, diag);
        if (status == ST_OK && used == TONE_ACES && is_first) diag_warn(diag, "HDR: luces por encima de 1,0 comprimidas con ACES (--tonemap)");
    } else {
        status = copy_integer((IWICBitmapSource *)conv, &out_frame->image, diag);
        if (status == ST_OK && transform) swap_red_blue(&out_frame->image);
    }
    if (status == ST_OK && transform && is_first) {
        // Los pixeles ya estan en sRGB: el perfil original ya no los describe.
        blob_free(&out->meta.icc);
        out->meta.has_srgb = true;
        out->meta.srgb_intent = 0;
    }
done:
    RELEASE(conv);
    RELEASE(xform);
    RELEASE(icc_ctx);
    RELEASE(frame);
    metadata_free(&scratch_meta);
    return status;
}

// ---- alfa auxiliar de HEIF / AVIF ------------------------------------------------------------

// Decodifica el cuadro 0 de un archivo en memoria como RGBA (un gris se replica en R, G y B), a
// 16 bits si el original o `min_depth` lo piden. `bits` = bits por canal del original.
static Status decode_first_frame(IWICImagingFactory *f, const u8 *data, size_t size, u32 min_depth, u64 max_pixels,
                                 Image *out, u32 *bits, Diag *diag) {
    IWICStream *stream = NULL;
    IWICBitmapDecoder *decoder = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    GUID pf;
    u32 depth = min_depth;
    HRESULT hr = IWICImagingFactory_CreateStream(f, &stream);
    if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)size);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateDecoderFromStream(f, (IStream *)stream, NULL, WICDecodeMetadataCacheOnDemand, &decoder);
    if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(decoder, 0, &frame);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetSize(frame, &w, &h);
    if (SUCCEEDED(hr)) hr = IWICBitmapFrameDecode_GetPixelFormat(frame, &pf);
    if (SUCCEEDED(hr)) {
        PixelFormatFacts facts = pixel_format_facts(f, &pf);
        *bits = facts.indexed ? 8 : facts.bits_per_channel;
        if (*bits > 8) depth = 16;
        hr = IWICImagingFactory_CreateFormatConverter(f, &conv);
    }
    const GUID *target = depth == 16 ? &GUID_WICPixelFormat64bppRGBA : &GUID_WICPixelFormat32bppRGBA;
    if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame, target, WICBitmapDitherTypeNone, NULL, 0.0, WICBitmapPaletteTypeCustom);
    Status status = FAILED(hr) ? fail_hr(diag, hr, "WIC") : image_alloc(out, w, h, depth, false, max_pixels, diag);
    if (status == ST_OK) status = copy_integer((IWICBitmapSource *)conv, out, diag);
    RELEASE(conv);
    RELEASE(frame);
    RELEASE(decoder);
    RELEASE(stream);
    return status;
}

// El canal R de `mask` (gris replicado) pasa a ser el alfa de `image`. Misma profundidad.
static void take_alpha_from_red(Image *image, const Image *mask) {
    for (u32 y = 0; y < image->height; y++) {
        if (image->depth == 8) {
            u8 *dst = image_row(image, y);
            const u8 *src = image_row(mask, y);
            for (u32 x = 0; x < image->width; x++) dst[4 * x + 3] = src[4 * x];
        } else {
            u16 *dst = (u16 *)image_row(image, y);
            const u16 *src = (const u16 *)image_row(mask, y);
            for (u32 x = 0; x < image->width; x++) dst[4 * x + 3] = src[4 * x];
        }
    }
}

// ---- color de AVIF ------------------------------------------------------------------------------

// Que hacer con el color de un HEIF antes y despues de pasarlo por el codec (ver heif.h).
typedef struct HeifColorPlan {
    bool  neutralize;            // decodificar una copia con la nclx reescrita a `decode_as`
    Cicp  decode_as;
    bool  correct;               // y despues aplicar `correction` (RGB lineal en valores codificados)
    float correction[9];
    bool  declare;               // declarar el color real en el PNG (chunk sRGB o cICP)
} HeifColorPlan;

// El codec AV1 de Windows no respeta la matriz de la nclx: la deduce sobre todo de los PRIMARIOS
// (medido con un decodificador WIC crudo contra libavif: primarios 5/6 -> BT.601 siempre,
// 1 -> BT.709 siempre; con primarios sin especificar depende de otros campos, y con 9, 10 y 12
// convierte de gama por su cuenta). Por eso la copia se reescribe con combinaciones donde
// primarios y matriz dicen LO MISMO: (6, sRGB, 6) para BT.601 y (1, sRGB, 1) para BT.709. Asi
// sale bien hoy y seguiria saliendo bien si el codec algun dia respetara la matriz. La
// transferencia se le da como sRGB para que no toque la curva. Las demas matrices lineales se
// decodifican como BT.709 y se corrigen con C = M_declarada^-1 · M_709 (exacta salvo lo que el
// codec recorto). HDR (PQ/HLG) se deja al codec: entrega flotante y se mapean tonos.
static void plan_heif_color(const DecodeContext *ctx, const HeifInfo *info, HeifColorPlan *plan) {
    memset(plan, 0, sizeof *plan);
    if (!info->av1) return;
    if (!info->has_nclx) { plan->declare = true; return; }   // MIAF: sRGB, y el codec ya usa BT.601
    const Cicp *c = &info->cicp;
    if (cicp_is_hdr(c)) return;
    u32 m = c->matrix;
    bool bt601 = m == CICP_UNSPECIFIED || m == 5 || m == 6;
    plan->neutralize = plan->declare = true;
    plan->decode_as = bt601 ? (Cicp){ CICP_PRIMARIES_BT601, CICP_TRANSFER_SRGB, CICP_MATRIX_BT601, c->full_range }
                            : (Cicp){ CICP_PRIMARIES_BT709, CICP_TRANSFER_SRGB, CICP_MATRIX_BT709, c->full_range };
    if (bt601 || m == CICP_MATRIX_BT709) return;
    plan->correct = cicp_ycbcr_correction(CICP_MATRIX_BT709, m, c->primaries, plan->correction);
    if (!plan->correct)
        diag_warn(ctx->diag, "AVIF con matriz CICP %u (identidad, YCgCo o luminancia constante): el codec de Windows no la "
                  "sabe decodificar y no se puede corregir despues; los colores pueden salir alterados", m);
}

// Primarios y transferencia: WIC no convierte de gama, asi que el RGB sigue en los del archivo.
// sRGB se declara con el chunk sRGB; cualquier otro (Display P3, BT.2020, HDR) viaja en cICP. Si
// el archivo trae un perfil ICC, el perfil ya lo describe (y va en iCCP).
static void declare_heif_color(const HeifInfo *info, Decoded *out) {
    if (out->meta.icc.size || out->meta.has_srgb) return;
    const Cicp *c = &info->cicp;
    if (cicp_is_srgb_like(c)) {
        out->meta.has_srgb = c->primaries == CICP_PRIMARIES_BT709 && c->transfer == CICP_TRANSFER_SRGB;
        out->meta.srgb_intent = 0;
        return;
    }
    out->meta.has_cicp = true;
    out->meta.cicp[0] = c->primaries == CICP_UNSPECIFIED ? CICP_PRIMARIES_BT709 : c->primaries;
    out->meta.cicp[1] = c->transfer == CICP_UNSPECIFIED ? CICP_TRANSFER_SRGB : c->transfer;
    out->meta.cicp[2] = CICP_MATRIX_IDENTITY;            // el PNG guarda RGB
    out->meta.cicp[3] = 1;                               // y a rango completo
}

// El codec HEIF de Windows entrega solo el color: el alfa (imagen auxiliar, ver heif.h) se
// decodifica aparte promoviendolo a primaria, se lleva a la orientacion del color y se copia al
// canal alfa; si el color venia premultiplicado ('prem'), se des-premultiplica. Cualquier falla
// deja la imagen opaca con un aviso: el color ya esta bien y el problema es del codec externo.
static void attach_heif_alpha(const DecodeContext *ctx, WicSession *s, const HeifInfo *info, const u8 *base, Decoded *out) {
    Frame *frame = decoded_frame(out, 0);
    if (!info->has_alpha || out->source.has_alpha) return;

    Diag local = { 0 };
    Image mask = { 0 };
    u32 alpha_bits = 8;
    u8 *copy = heif_promote_alpha(base, ctx->size, info);
    Status st = copy ? ST_OK : diag_fail(&local, ST_NO_MEMORY, "sin memoria");
    if (st == ST_OK)
        st = decode_first_frame(s->f, copy, ctx->size, frame->image.depth, ctx->opt->max_pixels, &mask, &alpha_bits, &local);
    // El codec giro cada item con SUS irot/imir: se deshace la del alfa y se aplica la del color.
    Orientation align = geometry_then(geometry_inverse(info->alpha_orientation), info->primary_orientation);
    if (st == ST_OK) st = geometry_orient(&mask, align, &local);
    if (st == ST_OK && (mask.width != frame->image.width || mask.height != frame->image.height))
        st = diag_fail(&local, ST_UNSUPPORTED, "mide %ux%u y el color %ux%u", mask.width, mask.height,
                       frame->image.width, frame->image.height);
    // Alfa de 10/12 bits sobre color de 8: el color sube a 16 para no tirar la precision del alfa.
    if (st == ST_OK) st = image_set_depth(&frame->image, mask.depth, &local);
    if (st == ST_OK) {
        take_alpha_from_red(&frame->image, &mask);
        if (info->premultiplied) image_unpremultiply(&frame->image);
        SourceInfo *src = &out->source;
        src->has_alpha = true;
        src->alpha_origin = info->premultiplied ? ALPHA_ORIGIN_PREMULTIPLIED : ALPHA_ORIGIN_CHANNEL;
        size_t used = strlen(src->detail);
        snprintf(src->detail + used, sizeof src->detail - used, " · alfa auxiliar de %u bits%s", alpha_bits,
                 info->premultiplied ? " (color premultiplicado)" : "");
    } else {
        diag_warn(ctx->diag, "no se pudo leer el alfa auxiliar del HEIF/AVIF (%s): queda opaca", local.message);
    }
    image_free(&mask);
    mem_free(copy);
}

// Todo lo que el codec HEIF de Windows deja a medias, sobre la imagen primaria (cuadro 0).
// `base`: el archivo tal como se decodifico (con la nclx retocada si hizo falta), para que el alfa
// pase por el codec con el mismo trato que el color.
static void finish_heif(const DecodeContext *ctx, WicSession *s, const HeifInfo *info, const HeifColorPlan *plan,
                        const u8 *base, Decoded *out) {
    if (!IsEqualGUID(&s->container, &GUID_ContainerFormatHeif) || !decoded_frame_count(out)) return;
    Frame *frame = decoded_frame(out, 0);
    if (frame->index != 0) return;
    if (plan->correct) {                                // antes del alfa: la correccion es lineal y
        Kernel k = kernel_color_matrix(plan->correction);   // conmuta con la premultiplicacion
        pixelops_run(&frame->image, &k, 1);
    }
    if (plan->declare) declare_heif_color(info, out);
    attach_heif_alpha(ctx, s, info, base, out);
}

Status wic_decode(const DecodeContext *ctx, Decoded *out) {
    // HEIF/AVIF: la estructura se lee antes de decodificar, porque puede hacer falta decodificar
    // una copia retocada (ver plan_heif_color). Para cualquier otro formato cuesta un 'ftyp' que no esta.
    HeifInfo heif;
    HeifColorPlan plan = { 0 };
    bool is_heif = heif_inspect(ctx->data, ctx->size, &heif);
    if (is_heif) plan_heif_color(ctx, &heif, &plan);
    u8 *retouched = plan.neutralize ? heif_rewrite_nclx(ctx->data, ctx->size, &heif, &plan.decode_as) : NULL;
    if (plan.neutralize && !retouched) {
        diag_warn(ctx->diag, "sin memoria para corregir el color del AVIF: se decodifica tal cual");
        plan = (HeifColorPlan){ 0 };
    }

    WicSession s;
    Status status = session_open(retouched ? retouched : ctx->data, ctx->size, ctx->diag, &s);
    if (status != ST_OK) { mem_free(retouched); return status; }
    describe_codec(&s, out);
    if (IsEqualGUID(&s.container, &GUID_ContainerFormatJpeg)) jpeg_scan_app_segments(ctx->data, ctx->size, &out->meta);

    UINT count = 0;
    IWICBitmapDecoder_GetFrameCount(s.decoder, &count);
    if (!count) { session_close(&s); mem_free(retouched); return diag_fail(ctx->diag, ST_CORRUPT, "el archivo no tiene imagenes"); }
    out->source.total_frames = count;
    const DecodeOptions *opt = ctx->opt;
    if (opt->frame_mode == FRAMES_ONE && opt->frame_index >= count) {
        session_close(&s);
        mem_free(retouched);
        return diag_fail(ctx->diag, ST_INVALID, "--frame %u: el archivo tiene %u (0..%u)", opt->frame_index, count, count - 1);
    }
    bool first = true;
    for (u32 i = 0; i < count && status == ST_OK; i++) {
        if (!frame_wanted(opt, i, count)) continue;
        status = decode_frame(ctx, &s, i, first, out);
        first = false;
    }
    if (status == ST_OK && is_heif) finish_heif(ctx, &s, &heif, &plan, retouched ? retouched : ctx->data, out);
    if (status == ST_OK && count > 1 && opt->frame_mode == FRAMES_DEFAULT)
        diag_warn(ctx->diag, "el archivo tiene %u imagenes: se convirtio la primera (--frames all para todas)", count);
    session_close(&s);
    mem_free(retouched);
    return status;
}

// ---- enumeracion para --formats ---------------------------------------------------------------

void wic_enumerate_decoders(WicCodecVisitor visit, void *ctx) {
    IWICImagingFactory *f = factory();
    if (!f) return;
    IEnumUnknown *e = NULL;
    if (FAILED(IWICImagingFactory_CreateComponentEnumerator(f, WICDecoder, WICComponentEnumerateDefault, &e))) return;
    IUnknown *unk = NULL;
    ULONG fetched = 0;
    while (IEnumUnknown_Next(e, 1, &unk, &fetched) == S_OK && fetched) {
        IWICBitmapCodecInfo *info = NULL;
        if (SUCCEEDED(IUnknown_QueryInterface(unk, &IID_IWICBitmapCodecInfo, (void **)&info))) {
            wchar_t wname[128] = L"", wext[512] = L"";
            UINT got;
            IWICBitmapCodecInfo_GetFriendlyName(info, ARRAY_LEN(wname), wname, &got);
            IWICBitmapCodecInfo_GetFileExtensions(info, ARRAY_LEN(wext), wext, &got);
            char *name = utf16_to_utf8(wname), *ext = utf16_to_utf8(wext);
            if (name && ext) visit(ctx, name, ext);
            mem_free(name);
            mem_free(ext);
        }
        RELEASE(info);
        RELEASE(unk);
    }
    RELEASE(e);
}

// ---- conversion de perfil de una imagen ya decodificada ----------------------------------------

Status wic_convert_to_srgb(Image *image, const u8 *icc, size_t icc_size, Diag *diag) {
    IWICImagingFactory *f = factory();
    if (!f) return diag_fail(diag, ST_UNSUPPORTED, "WIC no esta disponible para convertir el perfil");
    const GUID *bgra = image->depth == 16 ? &GUID_WICPixelFormat64bppBGRA : &GUID_WICPixelFormat32bppBGRA;
    IWICBitmap *bitmap = NULL;
    IWICColorContext *from = NULL;
    IWICBitmapSource *xf = NULL;
    Status status = ST_OK;
    swap_red_blue(image);                                    // RGBA -> BGRA
    HRESULT hr = IWICImagingFactory_CreateBitmapFromMemory(f, image->width, image->height, bgra, (UINT)image->stride,
                                                           (UINT)(image->stride * image->height), image->pixels, &bitmap);
    if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateColorContext(f, &from);
    if (SUCCEEDED(hr)) hr = IWICColorContext_InitializeFromMemory(from, icc, (UINT)icc_size);
    if (SUCCEEDED(hr)) {
        xf = make_srgb_transform(f, (IWICBitmapSource *)bitmap, from, bgra);
        if (!xf) hr = E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        for (u32 y = 0; y < image->height && SUCCEEDED(hr); y += BAND_ROWS) {
            u32 rows = MIN((u32)BAND_ROWS, image->height - y);
            WICRect rect = { 0, (INT)y, (INT)image->width, (INT)rows };
            // Se escribe sobre el mismo buffer: cada banda ya fue leida por el transformador antes
            // de pisarla (WIC transforma por pedido, banda por banda).
            hr = IWICBitmapSource_CopyPixels(xf, &rect, (UINT)image->stride, (UINT)(image->stride * rows), image_row(image, y));
        }
    }
    swap_red_blue(image);                                    // BGRA -> RGBA
    if (FAILED(hr)) status = diag_fail(diag, ST_UNSUPPORTED, "no se pudo convertir el perfil ICC a sRGB (0x%08lX)", (unsigned long)hr);
    RELEASE(xf);
    RELEASE(from);
    RELEASE(bitmap);
    return status;
}
