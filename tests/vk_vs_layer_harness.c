// Harness for gfx_vk_vs_layer.c (tests/test_vk_vs_layer.py): a vertex shader's gl_Layer moved to an output location, and the geometry shaders that write it.
// The fixtures are glslang's SPIR-V for a synthetic vertex shader (no game's code):
//   layout(location=0) out vec4 a; layout(location=1) flat out ivec4 b; layout(location=3) out vec2 c[2];
//   out gl_PerVertex { vec4 gl_Position; float gl_PointSize; float gl_ClipDistance[2]; };
//   ... gl_Layer = gl_InstanceIndex;   (VS_NO_LAYER: the same without gl_Layer)
// With an output folder argument it writes the modules there, for spirv-val.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int spv_vs_writes_layer(const uint32_t *w, uint32_t nw);
uint32_t *spv_vs_layer_loc(const uint32_t *w, uint32_t nw, uint32_t nloc, uint32_t *loc, uint32_t *onw);
uint32_t *spv_layer_gs(const uint32_t *w, uint32_t nw, uint32_t loc, int cls, int psize, uint32_t *onw);
static const uint32_t VS_LAYER[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x00000033, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000020, 0x00020011, 0x00000045, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x000c000f, 0x00000000, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00000011, 0x0000001f, 0x00000023, 0x00000024, 0x0000002a, 0x00000031,
    0x00030003, 0x00000002, 0x000001c2, 0x000a0004, 0x415f4c47, 0x735f4252, 0x65646168, 0x69765f72,
    0x6f707765, 0x6c5f7472, 0x72657961, 0x7272615f, 0x00007961, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00060005, 0x0000000b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006,
    0x0000000b, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000000b, 0x00000001,
    0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x0000000b, 0x00000002, 0x435f6c67,
    0x4470696c, 0x61747369, 0x0065636e, 0x00030005, 0x0000000d, 0x00000000, 0x00060005, 0x00000011,
    0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00030005, 0x0000001f, 0x00000061, 0x00030005,
    0x00000023, 0x00000062, 0x00070005, 0x00000024, 0x495f6c67, 0x6174736e, 0x4965636e, 0x7865646e,
    0x00000000, 0x00030005, 0x0000002a, 0x00000063, 0x00050005, 0x00000031, 0x4c5f6c67, 0x72657961,
    0x00000000, 0x00030047, 0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b,
    0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000000b,
    0x00000002, 0x0000000b, 0x00000003, 0x00040047, 0x00000011, 0x0000000b, 0x0000002a, 0x00040047,
    0x0000001f, 0x0000001e, 0x00000000, 0x00030047, 0x00000023, 0x0000000e, 0x00040047, 0x00000023,
    0x0000001e, 0x00000001, 0x00040047, 0x00000024, 0x0000000b, 0x0000002b, 0x00040047, 0x0000002a,
    0x0000001e, 0x00000003, 0x00040047, 0x00000031, 0x0000000b, 0x00000009, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020, 0x00000000, 0x0004002b, 0x00000008,
    0x00000009, 0x00000002, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009, 0x0005001e, 0x0000000b,
    0x00000007, 0x00000006, 0x0000000a, 0x00040020, 0x0000000c, 0x00000003, 0x0000000b, 0x0004003b,
    0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e, 0x00000020, 0x00000001, 0x0004002b,
    0x0000000e, 0x0000000f, 0x00000000, 0x00040020, 0x00000010, 0x00000001, 0x0000000e, 0x0004003b,
    0x00000010, 0x00000011, 0x00000001, 0x00040020, 0x00000015, 0x00000003, 0x00000007, 0x0004002b,
    0x0000000e, 0x00000017, 0x00000001, 0x0004002b, 0x00000006, 0x00000018, 0x3f800000, 0x00040020,
    0x00000019, 0x00000003, 0x00000006, 0x0004002b, 0x0000000e, 0x0000001b, 0x00000002, 0x0004002b,
    0x00000006, 0x0000001c, 0x00000000, 0x0004003b, 0x00000015, 0x0000001f, 0x00000003, 0x0007002c,
    0x00000007, 0x00000020, 0x00000018, 0x00000018, 0x00000018, 0x00000018, 0x00040017, 0x00000021,
    0x0000000e, 0x00000004, 0x00040020, 0x00000022, 0x00000003, 0x00000021, 0x0004003b, 0x00000022,
    0x00000023, 0x00000003, 0x0004003b, 0x00000010, 0x00000024, 0x00000001, 0x00040017, 0x00000027,
    0x00000006, 0x00000002, 0x0004001c, 0x00000028, 0x00000027, 0x00000009, 0x00040020, 0x00000029,
    0x00000003, 0x00000028, 0x0004003b, 0x00000029, 0x0000002a, 0x00000003, 0x0005002c, 0x00000027,
    0x0000002b, 0x0000001c, 0x0000001c, 0x00040020, 0x0000002c, 0x00000003, 0x00000027, 0x0005002c,
    0x00000027, 0x0000002e, 0x00000018, 0x00000018, 0x00040020, 0x00000030, 0x00000003, 0x0000000e,
    0x0004003b, 0x00000030, 0x00000031, 0x00000003, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000e, 0x00000012, 0x00000011, 0x0004006f,
    0x00000006, 0x00000013, 0x00000012, 0x00070050, 0x00000007, 0x00000014, 0x00000013, 0x00000013,
    0x00000013, 0x00000013, 0x00050041, 0x00000015, 0x00000016, 0x0000000d, 0x0000000f, 0x0003003e,
    0x00000016, 0x00000014, 0x00050041, 0x00000019, 0x0000001a, 0x0000000d, 0x00000017, 0x0003003e,
    0x0000001a, 0x00000018, 0x00060041, 0x00000019, 0x0000001d, 0x0000000d, 0x0000001b, 0x0000000f,
    0x0003003e, 0x0000001d, 0x0000001c, 0x00060041, 0x00000019, 0x0000001e, 0x0000000d, 0x0000001b,
    0x00000017, 0x0003003e, 0x0000001e, 0x00000018, 0x0003003e, 0x0000001f, 0x00000020, 0x0004003d,
    0x0000000e, 0x00000025, 0x00000024, 0x00070050, 0x00000021, 0x00000026, 0x00000025, 0x00000025,
    0x00000025, 0x00000025, 0x0003003e, 0x00000023, 0x00000026, 0x00050041, 0x0000002c, 0x0000002d,
    0x0000002a, 0x0000000f, 0x0003003e, 0x0000002d, 0x0000002b, 0x00050041, 0x0000002c, 0x0000002f,
    0x0000002a, 0x00000017, 0x0003003e, 0x0000002f, 0x0000002e, 0x0004003d, 0x0000000e, 0x00000032,
    0x00000024, 0x0003003e, 0x00000031, 0x00000032, 0x000100fd, 0x00010038};
static const uint32_t VS_NO_LAYER[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x00000030, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000020, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x00000000, 0x00000001, 0x000b000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d,
    0x00000011, 0x0000001f, 0x00000023, 0x00000024, 0x0000002a, 0x00030003, 0x00000002, 0x000001c2,
    0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005, 0x0000000b, 0x505f6c67, 0x65567265,
    0x78657472, 0x00000000, 0x00060006, 0x0000000b, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69,
    0x00070006, 0x0000000b, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006,
    0x0000000b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e, 0x00030005, 0x0000000d,
    0x00000000, 0x00060005, 0x00000011, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00030005,
    0x0000001f, 0x00000061, 0x00030005, 0x00000023, 0x00000062, 0x00070005, 0x00000024, 0x495f6c67,
    0x6174736e, 0x4965636e, 0x7865646e, 0x00000000, 0x00030005, 0x0000002a, 0x00000063, 0x00030047,
    0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b, 0x00000000, 0x00050048,
    0x0000000b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000000b, 0x00000002, 0x0000000b,
    0x00000003, 0x00040047, 0x00000011, 0x0000000b, 0x0000002a, 0x00040047, 0x0000001f, 0x0000001e,
    0x00000000, 0x00030047, 0x00000023, 0x0000000e, 0x00040047, 0x00000023, 0x0000001e, 0x00000001,
    0x00040047, 0x00000024, 0x0000000b, 0x0000002b, 0x00040047, 0x0000002a, 0x0000001e, 0x00000003,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020, 0x00000000,
    0x0004002b, 0x00000008, 0x00000009, 0x00000002, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009,
    0x0005001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x00040020, 0x0000000c, 0x00000003,
    0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e, 0x00000020,
    0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040020, 0x00000010, 0x00000001,
    0x0000000e, 0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00040020, 0x00000015, 0x00000003,
    0x00000007, 0x0004002b, 0x0000000e, 0x00000017, 0x00000001, 0x0004002b, 0x00000006, 0x00000018,
    0x3f800000, 0x00040020, 0x00000019, 0x00000003, 0x00000006, 0x0004002b, 0x0000000e, 0x0000001b,
    0x00000002, 0x0004002b, 0x00000006, 0x0000001c, 0x00000000, 0x0004003b, 0x00000015, 0x0000001f,
    0x00000003, 0x0007002c, 0x00000007, 0x00000020, 0x00000018, 0x00000018, 0x00000018, 0x00000018,
    0x00040017, 0x00000021, 0x0000000e, 0x00000004, 0x00040020, 0x00000022, 0x00000003, 0x00000021,
    0x0004003b, 0x00000022, 0x00000023, 0x00000003, 0x0004003b, 0x00000010, 0x00000024, 0x00000001,
    0x00040017, 0x00000027, 0x00000006, 0x00000002, 0x0004001c, 0x00000028, 0x00000027, 0x00000009,
    0x00040020, 0x00000029, 0x00000003, 0x00000028, 0x0004003b, 0x00000029, 0x0000002a, 0x00000003,
    0x0005002c, 0x00000027, 0x0000002b, 0x0000001c, 0x0000001c, 0x00040020, 0x0000002c, 0x00000003,
    0x00000027, 0x0005002c, 0x00000027, 0x0000002e, 0x00000018, 0x00000018, 0x00050036, 0x00000002,
    0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000e, 0x00000012,
    0x00000011, 0x0004006f, 0x00000006, 0x00000013, 0x00000012, 0x00070050, 0x00000007, 0x00000014,
    0x00000013, 0x00000013, 0x00000013, 0x00000013, 0x00050041, 0x00000015, 0x00000016, 0x0000000d,
    0x0000000f, 0x0003003e, 0x00000016, 0x00000014, 0x00050041, 0x00000019, 0x0000001a, 0x0000000d,
    0x00000017, 0x0003003e, 0x0000001a, 0x00000018, 0x00060041, 0x00000019, 0x0000001d, 0x0000000d,
    0x0000001b, 0x0000000f, 0x0003003e, 0x0000001d, 0x0000001c, 0x00060041, 0x00000019, 0x0000001e,
    0x0000000d, 0x0000001b, 0x00000017, 0x0003003e, 0x0000001e, 0x00000018, 0x0003003e, 0x0000001f,
    0x00000020, 0x0004003d, 0x0000000e, 0x00000025, 0x00000024, 0x00070050, 0x00000021, 0x00000026,
    0x00000025, 0x00000025, 0x00000025, 0x00000025, 0x0003003e, 0x00000023, 0x00000026, 0x00050041,
    0x0000002c, 0x0000002d, 0x0000002a, 0x0000000f, 0x0003003e, 0x0000002d, 0x0000002b, 0x00050041,
    0x0000002c, 0x0000002f, 0x0000002a, 0x00000017, 0x0003003e, 0x0000002f, 0x0000002e, 0x000100fd,
    0x00010038};
#define N(a) ((uint32_t)(sizeof a / sizeof *a))
static int fails, cases;
static void check(int ok, const char *what) { cases++; if (!ok) { fails++; printf("FAIL %s\n", what); } }
static int count_op(const uint32_t *w, uint32_t nw, uint32_t op, int words, const uint32_t *ops) {   // instructions op whose first words operands are ops
    int n = 0;
    for (uint32_t i = 5; i < nw;) { uint32_t k = w[i] >> 16; if (!k) return -1; if ((w[i] & 0xffff) == op && (int)k > words) { int m = 1; for (int j = 0; j < words; j++) if (ops[j] != ~0u && w[i + 1 + j] != ops[j]) m = 0; n += m; } i += k; }
    return n;
}
static void write_spv(const char *dir, const char *name, const uint32_t *w, uint32_t n) {
    if (!dir) return; char p[1024]; snprintf(p, sizeof p, "%s/%s.spv", dir, name); FILE *f = fopen(p, "wb"); if (f) { fwrite(w, 4, n, f); fclose(f); }
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : NULL;
    check(spv_vs_writes_layer(VS_LAYER, N(VS_LAYER)) == 1, "the layer is seen");
    check(spv_vs_writes_layer(VS_NO_LAYER, N(VS_NO_LAYER)) == 0, "no layer is seen in a shader without one");
    uint32_t loc = 99, n = 0, *vs = spv_vs_layer_loc(VS_NO_LAYER, N(VS_NO_LAYER), 32, &loc, &n);
    check(!vs, "no variant for a shader without a layer");
    vs = spv_vs_layer_loc(VS_LAYER, N(VS_LAYER), 32, &loc, &n);
    check(vs && loc == 31 && n == N(VS_LAYER), "the layer goes to the highest location");
    if (!vs) { printf("%d of %d cases failed\n", fails + 1, cases); return 1; }
    check(spv_vs_writes_layer(vs, n) == 0, "the variant writes no built-in layer");
    check(count_op(vs, n, 71, 3, (uint32_t[]){~0u, 30, 31}) == 1, "one output at location 31");
    write_spv(dir, "vs", vs, n);
    // locations 0, 1, 3 and 4 (c[2]) are taken: below 5 the only free one is 2; below 2 none
    uint32_t l2 = 0, n2 = 0, *v2 = spv_vs_layer_loc(VS_LAYER, N(VS_LAYER), 5, &l2, &n2);
    check(v2 && l2 == 2, "an array output's locations are all taken"); free(v2);
    v2 = spv_vs_layer_loc(VS_LAYER, N(VS_LAYER), 2, &l2, &n2); check(!v2, "no variant when no location is free"); free(v2);
    static const char *cls_name[3] = {"points", "lines", "triangles"};
    static const uint32_t in_mode[3] = {19, 20, 22}, out_mode[3] = {27, 28, 29};
    for (int c = 0; c < 3; c++) for (int ps = 0; ps < 2; ps++) {
        uint32_t gn = 0, *g = spv_layer_gs(vs, n, loc, c, ps, &gn); char what[128], name[64];
        snprintf(what, sizeof what, "a geometry shader for %s%s", cls_name[c], ps ? " with point size" : ""); check(g != NULL, what);
        if (!g) continue;
        snprintf(name, sizeof name, "gs_%s%s", cls_name[c], ps ? "_psize" : ""); write_spv(dir, name, g, gn);
        check(count_op(g, gn, 15, 1, (uint32_t[]){3}) == 1, "a geometry entry point");
        check(count_op(g, gn, 16, 2, (uint32_t[]){~0u, in_mode[c]}) == 1 && count_op(g, gn, 16, 2, (uint32_t[]){~0u, out_mode[c]}) == 1, "its input and output primitives");
        check(count_op(g, gn, 16, 3, (uint32_t[]){~0u, 26, (uint32_t)c + 1}) == 1, "one vertex a point, two a line, three a triangle");
        check(count_op(g, gn, 218, 0, NULL) == c + 1 && count_op(g, gn, 219, 0, NULL) == 1, "each vertex emitted, one primitive");
        check(count_op(g, gn, 71, 3, (uint32_t[]){~0u, 11, 9}) == 1, "gl_Layer written");
        check(count_op(g, gn, 71, 3, (uint32_t[]){~0u, 30, 31}) == 1, "the layer read from location 31 (and not passed on)");
        for (uint32_t l = 0; l <= 3; l++) if (l != 2) { snprintf(what, sizeof what, "location %u passed on", l);   // (c[2] at 3 takes 4 too)
            check(count_op(g, gn, 71, 3, (uint32_t[]){~0u, 30, l}) == 2, what); }
        check(count_op(g, gn, 71, 3, (uint32_t[]){~0u, 31, ~0u}) == 0, "no component decorations made up");
        // the new blocks: Position and ClipDistance, PointSize only where the device writes it from a geometry shader (the vertex shader's own block is copied as is: 1)
        check(count_op(g, gn, 72, 4, (uint32_t[]){~0u, ~0u, 11, 0}) == 3, "Position in and out");
        check(count_op(g, gn, 72, 4, (uint32_t[]){~0u, ~0u, 11, 3}) == 3, "ClipDistance in and out");
        check(count_op(g, gn, 72, 4, (uint32_t[]){~0u, ~0u, 11, 1}) == (ps ? 3 : 1), "PointSize only with the device feature");
        check(g[3] > vs[3], "its bound covers the new ids");
        free(g);
    }
    uint32_t gn = 0; check(spv_layer_gs(vs, n, loc, 3, 0, &gn) == NULL, "no shader for another kind of primitive");
    check(spv_layer_gs(vs, n, 7, 2, 0, &gn) == NULL, "none when the location holds no layer");
    free(vs);
    if (fails) { printf("%d of %d cases failed\n", fails, cases); return 1; }
    printf("%d cases passed\n", cases); return 0;
}
