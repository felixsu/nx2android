// The layer a vertex shader writes, written by a geometry shader instead (the vs_layer_gs quirk, gfx_vk_quirks.c), without Vulkan dependencies.
// Mali-G720's driver (r49p1, POCO 2511FPC34G) draws a primitive whose vertex shader writes gl_Layer only into layers 0-7, though it reports
// maxFramebufferLayers 256; a geometry shader's gl_Layer reaches every layer. A standalone Vulkan test drew 32 instances with layer = instance into a
// 32-layer 2D array and into a 3D image's 2D-array view: slices 0-7 were written from the vertex shader, all 32 from a geometry shader, in every format
// and size tried. Pokémon Legends: Z-A fills its 32x32x32 colour-grading LUT that way (vertex shader b29f08619f379a83, 32 instances, layer = instance + 1):
// slices 8-31 stayed empty, and the tone map turned every colour but those with almost no blue to black (the whole game dark).
// spv_vs_layer_loc: the vertex shader's gl_Layer output becomes an ordinary output at a free location. spv_layer_gs: a geometry shader for one kind of
// input primitive that passes the vertex shader's outputs on, as they are, and writes gl_Layer from that location.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum { OP_EXTENSION = 10, OP_MEMORY_MODEL = 14, OP_ENTRY_POINT = 15, OP_CAPABILITY = 17, OP_TYPE_VOID = 19, OP_TYPE_INT = 21, OP_TYPE_ARRAY = 28,
       OP_TYPE_STRUCT = 30, OP_TYPE_POINTER = 32, OP_TYPE_FUNCTION = 33, OP_CONSTANT = 43, OP_FUNCTION = 54, OP_FUNCTION_END = 56, OP_VARIABLE = 59,
       OP_LOAD = 61, OP_STORE = 62, OP_ACCESS_CHAIN = 65, OP_DECORATE = 71, OP_MEMBER_DECORATE = 72, OP_DECORATION_GROUP = 73, OP_GROUP_DECORATE = 74,
       OP_GROUP_MEMBER_DECORATE = 75, OP_EMIT_VERTEX = 218, OP_END_PRIMITIVE = 219, OP_LABEL = 248, OP_RETURN = 253, OP_DECORATE_ID = 332,
       OP_DECORATE_STRING = 5632, OP_MEMBER_DECORATE_STRING = 5633 };
enum { SC_INPUT = 1, SC_OUTPUT = 3, DEC_BLOCK = 2, DEC_BUILTIN = 11, DEC_INVARIANT = 18, DEC_LOCATION = 30, DEC_COMPONENT = 31,
       BI_POSITION = 0, BI_POINT_SIZE = 1, BI_CLIP = 3, BI_CULL = 4, BI_LAYER = 9, CAP_GEOMETRY = 2, EXEC_GEOMETRY = 3 };

static int is_type_or_const(uint32_t op) { return (op >= 19 && op <= 39) || (op >= 41 && op <= 46) || (op >= 48 && op <= 52) || op == 322 || op == 1; }   // (OpUndef: 1)
static uint32_t pre_end(const uint32_t *w, uint32_t nw) {   // the first function, or 0 when the module is malformed
    for (uint32_t i = 5; i < nw;) { uint32_t n = w[i] >> 16; if (!n || i + n > nw) return 0; if ((w[i] & 0xffff) == OP_FUNCTION) return i; i += n; }
    return 0;
}
static uint32_t storage_of(const uint32_t *w, uint32_t fn, uint32_t var) {   // an OpVariable's storage class, ~0u when var is not one
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_VARIABLE && (w[i] >> 16) >= 4 && w[i + 2] == var) return w[i + 3];
    return ~0u;
}
static uint32_t layer_decoration(const uint32_t *w, uint32_t fn) {   // the word index of "OpDecorate <output variable> BuiltIn Layer", 0 when there is none
    for (uint32_t i = 5; i < fn; i += w[i] >> 16)
        if ((w[i] & 0xffff) == OP_DECORATE && (w[i] >> 16) == 4 && w[i + 2] == DEC_BUILTIN && w[i + 3] == BI_LAYER && storage_of(w, fn, w[i + 1]) == SC_OUTPUT) return i;
    return 0;
}
int spv_vs_writes_layer(const uint32_t *w, uint32_t nw) { uint32_t fn = nw > 5 ? pre_end(w, nw) : 0; return fn && layer_decoration(w, fn) != 0; }

// The locations a value of this type takes (arrays of vectors, matrices: one a row; 64-bit vectors are not split here: the translator writes none)
static uint32_t loc_span(const uint32_t *w, uint32_t fn, uint32_t t, int depth) {
    if (depth > 4) return 1;
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if (w[i + 1] != t || !is_type_or_const(op)) continue;
        if (op == OP_TYPE_ARRAY) {
            uint32_t len = 1;
            for (uint32_t j = 5; j < fn; j += w[j] >> 16) if ((w[j] & 0xffff) == OP_CONSTANT && w[j + 2] == w[i + 3]) { len = w[j + 3]; break; }
            return len * loc_span(w, fn, w[i + 2], depth + 1);
        }
        if (op == 24) return w[i + 3];   // OpTypeMatrix: its columns
        return 1;
    }
    return 1;
}
static uint32_t pointee(const uint32_t *w, uint32_t fn, uint32_t ptr) {
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_TYPE_POINTER && w[i + 1] == ptr) return w[i + 3];
    return 0;
}
static uint32_t var_type(const uint32_t *w, uint32_t fn, uint32_t var) {   // the type an OpVariable holds
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_VARIABLE && w[i + 2] == var) return pointee(w, fn, w[i + 1]);
    return 0;
}

// A copy of a vertex shader with its gl_Layer output at the highest location below nloc that no output takes (*loc), NULL when there is none free
uint32_t *spv_vs_layer_loc(const uint32_t *w, uint32_t nw, uint32_t nloc, uint32_t *loc, uint32_t *onw) {
    uint32_t fn = nw > 5 ? pre_end(w, nw) : 0, at = fn ? layer_decoration(w, fn) : 0;
    if (!at || nloc > 64) return NULL;
    uint64_t used = 0;
    for (uint32_t i = 5; i < fn; i += w[i] >> 16)
        if ((w[i] & 0xffff) == OP_DECORATE && (w[i] >> 16) == 4 && w[i + 2] == DEC_LOCATION && storage_of(w, fn, w[i + 1]) == SC_OUTPUT) {
            uint32_t span = loc_span(w, fn, var_type(w, fn, w[i + 1]), 0);
            for (uint32_t k = 0; k < span && w[i + 3] + k < 64; k++) used |= 1ull << (w[i + 3] + k);
        }
    int l = (int)nloc - 1; while (l >= 0 && (used >> l & 1)) l--;
    if (l < 0) return NULL;
    uint32_t *o = malloc((size_t)nw * 4); if (!o) return NULL;
    memcpy(o, w, (size_t)nw * 4); o[at + 2] = DEC_LOCATION; o[at + 3] = (uint32_t)l;   // BuiltIn Layer → Location l (the same four words)
    *loc = (uint32_t)l; *onw = nw; return o;
}

typedef struct { uint32_t *p; size_t n, cap; int bad; } vbuf;
static void put(vbuf *b, uint32_t x) {
    if (b->bad) return;
    if (b->n == b->cap) { size_t c = b->cap ? b->cap * 2 : 256; uint32_t *q = realloc(b->p, c * 4); if (!q) { b->bad = 1; return; } b->p = q; b->cap = c; }
    b->p[b->n++] = x;
}
static void put_n(vbuf *b, const uint32_t *x, uint32_t k) { for (uint32_t i = 0; i < k; i++) put(b, x[i]); }

typedef struct { uint32_t var, type, loc, comp; int layer; uint32_t in_var, out_var, in_ptr, in_elem_ptr, out_ptr; } item;
typedef struct { uint32_t mtype, builtin; int invariant; uint32_t in_ptr, out_ptr; uint32_t src_index; } member;

// A geometry shader for primitives of class cls (0 points, 1 lines, 2 triangles: lists, strips and fans) after the vertex shader w (from spv_vs_layer_loc,
// its layer at location loc): every vertex's outputs are passed on and gl_Layer is that location. psize: the device writes PointSize from a geometry
// shader (shaderTessellationAndGeometryPointSize); without it the point size is left out, as declaring it would be invalid. NULL when the vertex shader's
// outputs are not all located variables and one gl_PerVertex block (what the translator writes)
uint32_t *spv_layer_gs(const uint32_t *w, uint32_t nw, uint32_t loc, int cls, int psize, uint32_t *onw) {
    uint32_t fn = nw > 5 ? pre_end(w, nw) : 0, bound = nw > 5 ? w[3] : 0, nv = cls == 0 ? 1 : cls == 1 ? 2 : 3;
    if (!fn || cls < 0 || cls > 2) return NULL;
    uint32_t ep = 0, t_void = 0, t_fn = 0, t_int = 0, has_geom = 0;
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff, n = w[i] >> 16;
        if (op == OP_DECORATION_GROUP || op == OP_GROUP_DECORATE || op == OP_GROUP_MEMBER_DECORATE) return NULL;
        if (op == OP_ENTRY_POINT && n >= 3 && w[i + 1] == 0 && !ep) ep = i;
        if (op == OP_CAPABILITY && w[i + 1] == CAP_GEOMETRY) has_geom = 1;
        if (op == OP_TYPE_VOID) t_void = w[i + 1];
        if (op == OP_TYPE_INT && n == 4 && w[i + 2] == 32 && w[i + 3] == 1) t_int = w[i + 1];
    }
    if (!ep) return NULL;
    if (t_void) for (uint32_t i = 5; i < fn; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_TYPE_FUNCTION && (w[i] >> 16) == 3 && w[i + 2] == t_void) t_fn = w[i + 1];
    // the entry point's interface: its operands after the name (a nul-terminated string in words)
    uint32_t epn = w[ep] >> 16, k = ep + 3; while (k < ep + epn && (w[k] >> 24)) k++; k++;
    item it[40]; int ni = 0; uint32_t blk_var = 0; member mb[8]; int nm = 0, layer_seen = 0;
    for (; k < ep + epn; k++) {
        uint32_t v = w[k]; if (storage_of(w, fn, v) != SC_OUTPUT) continue;
        uint32_t t = var_type(w, fn, v), l = ~0u, c = ~0u, bi = ~0u;
        for (uint32_t i = 5; i < fn; i += w[i] >> 16)
            if ((w[i] & 0xffff) == OP_DECORATE && (w[i] >> 16) == 4 && w[i + 1] == v) { if (w[i + 2] == DEC_LOCATION) l = w[i + 3]; else if (w[i + 2] == DEC_COMPONENT) c = w[i + 3]; else if (w[i + 2] == DEC_BUILTIN) bi = w[i + 3]; }
        if (bi != ~0u || !t) return NULL;   // a built-in output outside the block: not what the translator writes
        if (l != ~0u) { if (ni == 40) return NULL; it[ni] = (item){.var = v, .type = t, .loc = l, .comp = c, .layer = l == loc}; layer_seen |= l == loc; ni++; continue; }
        // the gl_PerVertex block: its members by their built-ins
        if (blk_var) return NULL;
        blk_var = v;
        for (uint32_t i = 5; i < fn; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_TYPE_STRUCT && w[i + 1] == t) {
            uint32_t nmem = (w[i] >> 16) - 2;
            for (uint32_t m = 0; m < nmem; m++) {
                uint32_t b = ~0u; int inv = 0;
                for (uint32_t j = 5; j < fn; j += w[j] >> 16) if ((w[j] & 0xffff) == OP_MEMBER_DECORATE && (w[j] >> 16) >= 4 && w[j + 1] == t && w[j + 2] == m) {
                    if (w[j + 3] == DEC_BUILTIN && (w[j] >> 16) == 5) b = w[j + 4]; else if (w[j + 3] == DEC_INVARIANT) inv = 1; }
                if (b == BI_POSITION || b == BI_CLIP || b == BI_CULL || (b == BI_POINT_SIZE && psize)) { if (nm == 8) return NULL; mb[nm++] = (member){.mtype = w[i + 2 + m], .builtin = b, .invariant = inv, .src_index = m}; }
            }
        }
        if (!nm) return NULL;
    }
    if (!layer_seen) return NULL;
    // new ids
    uint32_t id = bound;
    if (!t_void) t_void = id++;
    if (!t_fn) t_fn = id++;
    int new_int = !t_int; if (new_int) t_int = id++;
    uint32_t nconst = nv + 1 > 8 ? nv + 1 : 8, cst[9]; for (uint32_t i = 0; i < nconst; i++) cst[i] = id++;   // int constants 0 .. nconst - 1
    uint32_t f_main = id++;
    uint32_t arr_of[40];   // each output's per-vertex input array type
    for (int i = 0; i < ni; i++) { it[i].in_var = id++; it[i].out_var = id++; it[i].in_ptr = id++; it[i].in_elem_ptr = id++; it[i].out_ptr = id++; arr_of[i] = id++; }
    uint32_t s_in = 0, s_out = 0, s_arr = 0, s_in_ptr = 0, s_out_ptr = 0, b_in = 0, b_out = 0;
    if (blk_var) { s_in = id++; s_out = id++; s_arr = id++; s_in_ptr = id++; s_out_ptr = id++; b_in = id++; b_out = id++; for (int m = 0; m < nm; m++) { mb[m].in_ptr = id++; mb[m].out_ptr = id++; } }
    vbuf hd = {0}, dec = {0}, ty = {0}, fnb = {0};
    // capabilities, extensions, the memory model: as the vertex shader's (its types need them), with Geometry
    put_n(&hd, w, 5);
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_CAPABILITY) put_n(&hd, &w[i], w[i] >> 16);
    if (!has_geom) { put(&hd, 2u << 16 | OP_CAPABILITY); put(&hd, CAP_GEOMETRY); }
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_EXTENSION) put_n(&hd, &w[i], w[i] >> 16);
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) if ((w[i] & 0xffff) == OP_MEMORY_MODEL) put_n(&hd, &w[i], w[i] >> 16);
    // OpEntryPoint Geometry %main "main" <inputs> <outputs>
    uint32_t nif = (uint32_t)ni * 2 + (blk_var ? 2 : 0);
    put(&hd, (5 + nif) << 16 | OP_ENTRY_POINT); put(&hd, EXEC_GEOMETRY); put(&hd, f_main); put(&hd, 0x6e69616d); put(&hd, 0);   // "main"
    for (int i = 0; i < ni; i++) { put(&hd, it[i].in_var); put(&hd, it[i].out_var); }
    if (blk_var) { put(&hd, b_in); put(&hd, b_out); }
    static const uint32_t in_mode[3] = {19, 20, 22}, out_mode[3] = {27, 28, 29};   // InputPoints, InputLines, Triangles; OutputPoints, OutputLineStrip, OutputTriangleStrip
    put(&hd, 3u << 16 | 16); put(&hd, f_main); put(&hd, in_mode[cls]);
    put(&hd, 4u << 16 | 16); put(&hd, f_main); put(&hd, 0); put(&hd, 1);   // Invocations 1
    put(&hd, 3u << 16 | 16); put(&hd, f_main); put(&hd, out_mode[cls]);
    put(&hd, 4u << 16 | 16); put(&hd, f_main); put(&hd, 26); put(&hd, nv);   // OutputVertices
    // the vertex shader's decorations of its types and constants (array strides, block layouts), then the new variables' and blocks'
    uint8_t *tc = calloc(bound, 1); if (!tc) return NULL;
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) {   // the ids the copied types and constants define (a type's is its first operand, a constant's its second)
        uint32_t op = w[i] & 0xffff, r = (op >= 19 && op <= 39) || op == 322 ? w[i + 1] : (w[i] >> 16) >= 3 ? w[i + 2] : ~0u;
        if (is_type_or_const(op) && r < bound) tc[r] = 1;
    }
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) {
        uint32_t op = w[i] & 0xffff;
        if ((op == OP_DECORATE || op == OP_MEMBER_DECORATE || op == OP_DECORATE_ID || op == OP_DECORATE_STRING || op == OP_MEMBER_DECORATE_STRING) && w[i + 1] < bound && tc[w[i + 1]])
            put_n(&dec, &w[i], w[i] >> 16);
    }
    free(tc);
    for (int i = 0; i < ni; i++) {
        if (it[i].layer) { put(&dec, 4u << 16 | OP_DECORATE); put(&dec, it[i].out_var); put(&dec, DEC_BUILTIN); put(&dec, BI_LAYER); }
        else { put(&dec, 4u << 16 | OP_DECORATE); put(&dec, it[i].out_var); put(&dec, DEC_LOCATION); put(&dec, it[i].loc);
               if (it[i].comp != ~0u) { put(&dec, 4u << 16 | OP_DECORATE); put(&dec, it[i].out_var); put(&dec, DEC_COMPONENT); put(&dec, it[i].comp); } }
        put(&dec, 4u << 16 | OP_DECORATE); put(&dec, it[i].in_var); put(&dec, DEC_LOCATION); put(&dec, it[i].loc);
        if (it[i].comp != ~0u) { put(&dec, 4u << 16 | OP_DECORATE); put(&dec, it[i].in_var); put(&dec, DEC_COMPONENT); put(&dec, it[i].comp); }
    }
    if (blk_var) for (int s = 0; s < 2; s++) {
        uint32_t st = s ? s_out : s_in;
        put(&dec, 3u << 16 | OP_DECORATE); put(&dec, st); put(&dec, DEC_BLOCK);
        for (int m = 0; m < nm; m++) {
            put(&dec, 5u << 16 | OP_MEMBER_DECORATE); put(&dec, st); put(&dec, (uint32_t)m); put(&dec, DEC_BUILTIN); put(&dec, mb[m].builtin);
            if (s && mb[m].invariant) { put(&dec, 4u << 16 | OP_MEMBER_DECORATE); put(&dec, st); put(&dec, (uint32_t)m); put(&dec, DEC_INVARIANT); }
        }
    }
    // types and constants: the vertex shader's, then the new ones; then the variables
    for (uint32_t i = 5; i < fn; i += w[i] >> 16) if (is_type_or_const(w[i] & 0xffff)) put_n(&ty, &w[i], w[i] >> 16);
    if (t_void >= bound) { put(&ty, 2u << 16 | OP_TYPE_VOID); put(&ty, t_void); }
    if (t_fn >= bound) { put(&ty, 3u << 16 | OP_TYPE_FUNCTION); put(&ty, t_fn); put(&ty, t_void); }
    if (new_int) { put(&ty, 4u << 16 | OP_TYPE_INT); put(&ty, t_int); put(&ty, 32); put(&ty, 1); }
    for (uint32_t i = 0; i < nconst; i++) { put(&ty, 4u << 16 | OP_CONSTANT); put(&ty, t_int); put(&ty, cst[i]); put(&ty, i); }
    for (int i = 0; i < ni; i++) {
        put(&ty, 4u << 16 | OP_TYPE_ARRAY); put(&ty, arr_of[i]); put(&ty, it[i].type); put(&ty, cst[nv]);
        put(&ty, 4u << 16 | OP_TYPE_POINTER); put(&ty, it[i].in_ptr); put(&ty, SC_INPUT); put(&ty, arr_of[i]);
        put(&ty, 4u << 16 | OP_TYPE_POINTER); put(&ty, it[i].in_elem_ptr); put(&ty, SC_INPUT); put(&ty, it[i].type);
        put(&ty, 4u << 16 | OP_TYPE_POINTER); put(&ty, it[i].out_ptr); put(&ty, SC_OUTPUT); put(&ty, it[i].type);
    }
    if (blk_var) {
        for (int s = 0; s < 2; s++) { put(&ty, (2u + (uint32_t)nm) << 16 | OP_TYPE_STRUCT); put(&ty, s ? s_out : s_in); for (int m = 0; m < nm; m++) put(&ty, mb[m].mtype); }
        put(&ty, 4u << 16 | OP_TYPE_ARRAY); put(&ty, s_arr); put(&ty, s_in); put(&ty, cst[nv]);
        put(&ty, 4u << 16 | OP_TYPE_POINTER); put(&ty, s_in_ptr); put(&ty, SC_INPUT); put(&ty, s_arr);
        put(&ty, 4u << 16 | OP_TYPE_POINTER); put(&ty, s_out_ptr); put(&ty, SC_OUTPUT); put(&ty, s_out);
        for (int m = 0; m < nm; m++) {
            put(&ty, 4u << 16 | OP_TYPE_POINTER); put(&ty, mb[m].in_ptr); put(&ty, SC_INPUT); put(&ty, mb[m].mtype);
            put(&ty, 4u << 16 | OP_TYPE_POINTER); put(&ty, mb[m].out_ptr); put(&ty, SC_OUTPUT); put(&ty, mb[m].mtype);
        }
    }
    for (int i = 0; i < ni; i++) {
        put(&ty, 4u << 16 | OP_VARIABLE); put(&ty, it[i].in_ptr); put(&ty, it[i].in_var); put(&ty, SC_INPUT);
        put(&ty, 4u << 16 | OP_VARIABLE); put(&ty, it[i].out_ptr); put(&ty, it[i].out_var); put(&ty, SC_OUTPUT);
    }
    if (blk_var) {
        put(&ty, 4u << 16 | OP_VARIABLE); put(&ty, s_in_ptr); put(&ty, b_in); put(&ty, SC_INPUT);
        put(&ty, 4u << 16 | OP_VARIABLE); put(&ty, s_out_ptr); put(&ty, b_out); put(&ty, SC_OUTPUT);
    }
    // main: each vertex's outputs copied, the vertex emitted; one primitive
    put(&fnb, 5u << 16 | OP_FUNCTION); put(&fnb, t_void); put(&fnb, f_main); put(&fnb, 0); put(&fnb, t_fn);
    put(&fnb, 2u << 16 | OP_LABEL); put(&fnb, id++);
    for (uint32_t v = 0; v < nv; v++) {
        for (int i = 0; i < ni; i++) {
            uint32_t p = id++, x = id++;
            put(&fnb, 5u << 16 | OP_ACCESS_CHAIN); put(&fnb, it[i].in_elem_ptr); put(&fnb, p); put(&fnb, it[i].in_var); put(&fnb, cst[v]);
            put(&fnb, 4u << 16 | OP_LOAD); put(&fnb, it[i].type); put(&fnb, x); put(&fnb, p);
            put(&fnb, 3u << 16 | OP_STORE); put(&fnb, it[i].out_var); put(&fnb, x);
        }
        for (int m = 0; m < nm; m++) {
            uint32_t p = id++, x = id++, q = id++;
            put(&fnb, 6u << 16 | OP_ACCESS_CHAIN); put(&fnb, mb[m].in_ptr); put(&fnb, p); put(&fnb, b_in); put(&fnb, cst[v]); put(&fnb, cst[m]);
            put(&fnb, 4u << 16 | OP_LOAD); put(&fnb, mb[m].mtype); put(&fnb, x); put(&fnb, p);
            put(&fnb, 5u << 16 | OP_ACCESS_CHAIN); put(&fnb, mb[m].out_ptr); put(&fnb, q); put(&fnb, b_out); put(&fnb, cst[m]);
            put(&fnb, 3u << 16 | OP_STORE); put(&fnb, q); put(&fnb, x);
        }
        put(&fnb, 1u << 16 | OP_EMIT_VERTEX);
    }
    put(&fnb, 1u << 16 | OP_END_PRIMITIVE); put(&fnb, 1u << 16 | OP_RETURN); put(&fnb, 1u << 16 | OP_FUNCTION_END);
    uint32_t *o = NULL;
    if (!hd.bad && !dec.bad && !ty.bad && !fnb.bad && (o = malloc((hd.n + dec.n + ty.n + fnb.n) * 4))) {
        size_t n = 0;
        memcpy(o + n, hd.p, hd.n * 4); n += hd.n; memcpy(o + n, dec.p, dec.n * 4); n += dec.n;
        memcpy(o + n, ty.p, ty.n * 4); n += ty.n; memcpy(o + n, fnb.p, fnb.n * 4); n += fnb.n;
        o[3] = id; *onw = (uint32_t)n;
    }
    free(hd.p); free(dec.p); free(ty.p); free(fnb.p);
    return o;
}
