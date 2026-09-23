// codec/heif.c - ver heif.h
//
// Estructura que se recorre (ISO/IEC 14496-12 y 23008-12):
//
//   ftyp                      marcas: avif, avis (AV1), heic, heix, mif1, msf1...
//   meta (FullBox)
//     pitm                    item_ID de la imagen primaria
//     iinf > infe...          tipo y flags (bit 0 = oculto) de cada item
//     iref > auxl, prem...    referencias: alfa --auxl--> primaria, primaria --prem--> alfa
//     iprp > ipco             propiedades (auxC, irot, imir, ispe...), indice base 1
//          > ipma             que propiedades tiene cada item, EN ORDEN
//
// Todo acceso pasa por un lector con limites: un archivo roto o malicioso devuelve false, nunca lee
// fuera del buffer.
#include "heif.h"
#include "../core/mem.h"

#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))

enum {
    BOX_HEADER = 8,
    BOX_LARGE_HEADER = 16,
    FULLBOX_HEADER = 4,                   // version (1) + flags (3)
    MAX_REFERENCES = 16,                  // candidatos a alfa y 'prem' que se recuerdan
    INFE_FLAG_HIDDEN = 1,
};

// URN del alfa segun el codec de la imagen (AVIF, HEVC, AVC).
static const char *const ALPHA_URNS[] = {
    "urn:mpeg:mpegB:cicp:systems:auxiliary:alpha",
    "urn:mpeg:hevc:2015:auxid:1",
    "urn:mpeg:avc:2015:auxid:1",
};

// ---- lectura con limites -------------------------------------------------------------------

typedef struct Span {
    const u8 *p;
    size_t    n;
} Span;

typedef struct Reader {
    const u8 *at, *end;
    bool      ok;                         // se apaga en la primera lectura fuera de rango
} Reader;

INLINE Reader reader_of(Span s) { return (Reader){ s.p, s.p + s.n, true }; }

INLINE bool reader_need(Reader *r, size_t n) {
    if (r->ok && (size_t)(r->end - r->at) >= n) return true;
    r->ok = false;
    return false;
}

static u32 read_u8(Reader *r)  { if (!reader_need(r, 1)) return 0; return *r->at++; }
static u32 read_u16(Reader *r) { if (!reader_need(r, 2)) return 0; u32 v = load_be16(r->at); r->at += 2; return v; }
static u32 read_u32(Reader *r) { if (!reader_need(r, 4)) return 0; u32 v = load_be32(r->at); r->at += 4; return v; }
static u32 read_id(Reader *r, bool wide) { return wide ? read_u32(r) : read_u16(r); }

// ---- cajas -----------------------------------------------------------------------------------

typedef struct Box {
    u32  type;
    Span body;                            // contenido sin el encabezado
} Box;

// Siguiente caja del rango. size 0 = "hasta el final"; size 1 = tamano de 64 bits.
static bool box_next(Reader *r, Box *box) {
    if (!r->ok || (size_t)(r->end - r->at) < BOX_HEADER) return false;
    u64 size = load_be32(r->at);
    u32 type = load_be32(r->at + 4);
    size_t header = BOX_HEADER, left = (size_t)(r->end - r->at);
    if (size == 1) {
        if (left < BOX_LARGE_HEADER) return false;
        size = load_be64(r->at + 8);
        header = BOX_LARGE_HEADER;
    } else if (size == 0) {
        size = left;
    }
    if (size < header || size > left) { r->ok = false; return false; }
    box->type = type;
    box->body = (Span){ r->at + header, (size_t)size - header };
    r->at += size;
    return true;
}

static bool box_find(Span range, u32 type, Box *found) {
    Reader r = reader_of(range);
    Box b;
    while (box_next(&r, &b))
        if (b.type == type) { *found = b; return true; }
    return false;
}

// Contenido de una FullBox despues de version y flags.
INLINE Span fullbox_payload(Span body) {
    return body.n < FULLBOX_HEADER ? (Span){ body.p, 0 } : (Span){ body.p + FULLBOX_HEADER, body.n - FULLBOX_HEADER };
}

// ---- propiedades -------------------------------------------------------------------------------

typedef struct Properties {
    Box   *items;                         // ipco, en orden: la propiedad N es items[N-1]
    size_t count;
} Properties;

// Dos pasadas (contar, llenar): un solo bloque exacto. O(propiedades).
static bool properties_index(Span ipco, Properties *props) {
    size_t count = 0;
    Box b;
    Reader r = reader_of(ipco);
    while (box_next(&r, &b)) count++;
    props->count = count;
    props->items = count ? mem_alloc(count * sizeof(Box)) : NULL;
    if (count && !props->items) return false;
    r = reader_of(ipco);
    for (size_t i = 0; i < count && box_next(&r, &b); i++) props->items[i] = b;
    return true;
}

static bool is_alpha_urn(const Box *auxc) {
    Span urn = fullbox_payload(auxc->body);
    for (size_t i = 0; i < ARRAY_LEN(ALPHA_URNS); i++) {
        size_t len = strlen(ALPHA_URNS[i]);
        if (urn.n > len && memcmp(urn.p, ALPHA_URNS[i], len) == 0 && urn.p[len] == 0) return true;
    }
    return false;
}

// irot: giro ANTIHORARIO en cuartos de vuelta. imir: eje 0 = vertical (espejo izquierda-derecha),
// 1 = horizontal (arriba-abajo). Se componen en el orden de ipma, que es el que aplica el codec.
static Orientation fold_transform(Orientation so_far, const Box *prop) {
    if (prop->type == FOURCC('i', 'r', 'o', 't') && prop->body.n >= 1)
        return geometry_then(so_far, geometry_compose(-90 * (int)(prop->body.p[0] & 3), false, false));
    if (prop->type == FOURCC('i', 'm', 'i', 'r') && prop->body.n >= 1) {
        bool vertical_axis = (prop->body.p[0] & 1) == 0;
        return geometry_then(so_far, geometry_compose(0, vertical_axis, !vertical_axis));
    }
    return so_far;
}

// ---- referencias -------------------------------------------------------------------------------

typedef struct References {
    u32 alpha_candidates[MAX_REFERENCES]; // items con auxl -> primaria
    u32 candidate_count;
    u32 premultiplied_by[MAX_REFERENCES]; // destinos de 'prem' desde la primaria
    u32 prem_count;
} References;

static void read_references(Span iref, u32 primary, References *refs) {
    memset(refs, 0, sizeof *refs);
    bool wide = iref.n >= 1 && iref.p[0] != 0;         // version 1: ids de 32 bits
    Reader r = reader_of(fullbox_payload(iref));
    Box b;
    while (box_next(&r, &b)) {
        bool auxl = b.type == FOURCC('a', 'u', 'x', 'l'), prem = b.type == FOURCC('p', 'r', 'e', 'm');
        if (!auxl && !prem) continue;
        Reader e = reader_of(b.body);
        u32 from = read_id(&e, wide), count = read_u16(&e);
        for (u32 i = 0; i < count && e.ok; i++) {
            u32 to = read_id(&e, wide);
            if (!e.ok) break;
            if (auxl && to == primary && refs->candidate_count < MAX_REFERENCES)
                refs->alpha_candidates[refs->candidate_count++] = from;
            if (prem && from == primary && refs->prem_count < MAX_REFERENCES)
                refs->premultiplied_by[refs->prem_count++] = to;
        }
    }
}

static bool id_in(u32 id, const u32 *list, u32 n) {
    for (u32 i = 0; i < n; i++) if (list[i] == id) return true;
    return false;
}

// ---- asociaciones (ipma) --------------------------------------------------------------------

typedef struct ItemFacts {
    bool        seen;
    bool        alpha_urn;
    Orientation orientation;
    bool        has_nclx, has_icc;
    Cicp        cicp;
    const u8   *nclx_codes;               // donde empiezan los codigos dentro del archivo
} ItemFacts;

// colr: 'nclx' = codigos CICP + rango; 'prof'/'rICC' = perfil ICC (lo lee el codec).
static void read_colour(ItemFacts *facts, const Box *colr) {
    Reader r = reader_of(colr->body);
    u32 kind = read_u32(&r);
    if (kind == FOURCC('p', 'r', 'o', 'f') || kind == FOURCC('r', 'I', 'C', 'C')) { facts->has_icc = r.ok; return; }
    if (kind != FOURCC('n', 'c', 'l', 'x')) return;
    u32 primaries = read_u16(&r), transfer = read_u16(&r), matrix = read_u16(&r), flags = read_u8(&r);
    if (!r.ok || primaries > 255 || transfer > 255 || matrix > 255) return;
    facts->has_nclx = true;
    facts->cicp = (Cicp){ (u8)primaries, (u8)transfer, (u8)matrix, (flags & 0x80) != 0 };
    facts->nclx_codes = colr->body.p + 4;
}

// Recorre TODAS las cajas ipma (puede haber varias) y junta, para la primaria y para cada
// candidato, si tiene la URN de alfa y su orientacion. O(asociaciones).
static void read_associations(Span iprp, const Properties *props, u32 primary, const References *refs,
                              ItemFacts *primary_facts, ItemFacts *candidate_facts) {
    Reader boxes = reader_of(iprp);
    Box ipma;
    while (box_next(&boxes, &ipma)) {
        if (ipma.type != FOURCC('i', 'p', 'm', 'a') || ipma.body.n < FULLBOX_HEADER) continue;
        bool wide_ids = ipma.body.p[0] >= 1, wide_index = (ipma.body.p[3] & 1) != 0;
        Reader r = reader_of(fullbox_payload(ipma.body));
        u32 entries = read_u32(&r);
        for (u32 e = 0; e < entries && r.ok; e++) {
            u32 item = read_id(&r, wide_ids), count = read_u8(&r);
            ItemFacts *facts = item == primary ? primary_facts : NULL;
            for (u32 c = 0; !facts && c < refs->candidate_count; c++)
                if (refs->alpha_candidates[c] == item) facts = &candidate_facts[c];
            if (facts && !facts->seen) { facts->seen = true; facts->orientation = ORIENT_NONE; }
            for (u32 a = 0; a < count && r.ok; a++) {
                u32 index = wide_index ? (read_u16(&r) & 0x7FFF) : (read_u8(&r) & 0x7F);
                if (!facts || !index || index > props->count) continue;
                const Box *prop = &props->items[index - 1];
                if (prop->type == FOURCC('a', 'u', 'x', 'C')) facts->alpha_urn |= is_alpha_urn(prop);
                else if (prop->type == FOURCC('c', 'o', 'l', 'r')) read_colour(facts, prop);
                facts->orientation = fold_transform(facts->orientation, prop);
            }
        }
    }
}

// ---- infe: el flag de oculto ----------------------------------------------------------------

// Devuelve el offset (en el archivo) del byte de flags bajo del 'infe' de `item`, o 0.
static size_t find_infe_flags(const u8 *file, Span iinf, u32 item) {
    if (iinf.n < FULLBOX_HEADER) return 0;
    bool wide_count = iinf.p[0] != 0;
    Reader r = reader_of(fullbox_payload(iinf));
    read_id(&r, wide_count);                                 // entry_count: se recorre por cajas
    Box infe;
    while (box_next(&r, &infe)) {
        if (infe.type != FOURCC('i', 'n', 'f', 'e') || infe.body.n < FULLBOX_HEADER + 2) continue;
        u32 version = infe.body.p[0];
        if (version < 2) continue;                           // v0/v1 no nombran items de imagen
        Reader e = reader_of(fullbox_payload(infe.body));
        if (read_id(&e, version >= 3) == item && e.ok) return (size_t)(infe.body.p + 3 - file);
    }
    return 0;
}

// ---- ftyp ----------------------------------------------------------------------------------------

// ¿Alguna marca (la principal o una compatible) dice AV1?
static bool brands_say_av1(Span ftyp) {
    Reader r = reader_of(ftyp);
    for (u32 i = 0; r.ok && (size_t)(r.end - r.at) >= 4; i++) {
        u32 brand = read_u32(&r);
        if (i == 1) continue;                                   // la 2.a palabra es la version menor
        if (brand == FOURCC('a', 'v', 'i', 'f') || brand == FOURCC('a', 'v', 'i', 's')) return true;
    }
    return false;
}

// ---- API ---------------------------------------------------------------------------------------

// MIAF 7.3.6.4: sin colr, la imagen es BT.709 / sRGB / matriz BT.601 / rango completo.
static const Cicp MIAF_DEFAULT_CICP = { CICP_PRIMARIES_BT709, CICP_TRANSFER_SRGB, CICP_MATRIX_BT601, true };

bool heif_inspect(const u8 *data, size_t size, HeifInfo *out) {
    memset(out, 0, sizeof *out);
    out->primary_orientation = out->alpha_orientation = ORIENT_NONE;
    Box ftyp, meta, pitm, iref, iprp, ipco, iinf;
    Span file = { data, size };
    Reader top = reader_of(file);
    if (!box_next(&top, &ftyp) || ftyp.type != FOURCC('f', 't', 'y', 'p')) return false;
    if (!box_find(file, FOURCC('m', 'e', 't', 'a'), &meta)) return false;
    Span children = fullbox_payload(meta.body);
    if (!box_find(children, FOURCC('p', 'i', 't', 'm'), &pitm) || pitm.body.n < FULLBOX_HEADER + 2) return false;
    if (!box_find(children, FOURCC('i', 'p', 'r', 'p'), &iprp) || !box_find(iprp.body, FOURCC('i', 'p', 'c', 'o'), &ipco))
        return false;

    bool wide_pitm = pitm.body.p[0] != 0;
    Reader pr = reader_of(fullbox_payload(pitm.body));
    u32 primary = read_id(&pr, wide_pitm);
    if (!pr.ok) return false;

    References refs = { 0 };
    if (box_find(children, FOURCC('i', 'r', 'e', 'f'), &iref)) read_references(iref.body, primary, &refs);

    Properties props;
    if (!properties_index(ipco.body, &props)) return false;
    ItemFacts primary_facts = { 0 }, candidate_facts[MAX_REFERENCES] = { 0 };
    read_associations(iprp.body, &props, primary, &refs, &primary_facts, candidate_facts);
    mem_free(props.items);

    out->primary_id = primary;
    out->av1 = brands_say_av1(ftyp.body);
    out->has_nclx = primary_facts.has_nclx;
    out->cicp = primary_facts.has_nclx ? primary_facts.cicp : MIAF_DEFAULT_CICP;
    out->nclx_offset = primary_facts.has_nclx ? (size_t)(primary_facts.nclx_codes - data) : 0;
    out->has_icc = primary_facts.has_icc;
    out->primary_orientation = primary_facts.seen ? primary_facts.orientation : ORIENT_NONE;
    out->pitm_id_offset = (size_t)(pitm.body.p + FULLBOX_HEADER - data);
    out->pitm_id_size = wide_pitm ? 4 : 2;

    for (u32 c = 0; c < refs.candidate_count; c++) {
        if (!candidate_facts[c].seen || !candidate_facts[c].alpha_urn) continue;
        u32 alpha = refs.alpha_candidates[c];
        if (!wide_pitm && alpha > 0xFFFF) continue;            // 'pitm' de 16 bits no puede apuntarlo
        out->has_alpha = true;
        out->alpha_id = alpha;
        out->premultiplied = id_in(alpha, refs.premultiplied_by, refs.prem_count);
        out->alpha_orientation = candidate_facts[c].orientation;
        if (box_find(children, FOURCC('i', 'i', 'n', 'f'), &iinf))
            out->alpha_flags_offset = find_infe_flags(data, iinf.body, alpha);
        break;
    }
    return true;
}

u8 *heif_rewrite_nclx(const u8 *data, size_t size, const HeifInfo *info, const Cicp *cicp) {
    u8 *copy = mem_alloc(size);
    if (!copy) return NULL;
    memcpy(copy, data, size);
    u8 *codes = copy + info->nclx_offset;
    store_be16(codes, cicp->primaries);
    store_be16(codes + 2, cicp->transfer);
    store_be16(codes + 4, cicp->matrix);
    codes[6] = cicp->full_range ? 0x80 : 0;
    return copy;
}

u8 *heif_promote_alpha(const u8 *data, size_t size, const HeifInfo *info) {
    u8 *copy = mem_alloc(size);
    if (!copy) return NULL;
    memcpy(copy, data, size);
    if (info->pitm_id_size == 4) store_be32(copy + info->pitm_id_offset, info->alpha_id);
    else store_be16(copy + info->pitm_id_offset, (u16)info->alpha_id);
    if (info->alpha_flags_offset) copy[info->alpha_flags_offset] &= (u8)~INFE_FLAG_HIDDEN;
    return copy;
}
