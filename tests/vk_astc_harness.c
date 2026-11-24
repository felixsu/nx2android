// Harness for gfx_vk_astc.c (tests/test_vk_astc.py): BC1 blocks rewritten as ASTC 4x4 blocks, and RGBA8 blocks encoded as ASTC 4x4. The expected blocks were decoded with an independent decoder
// (texture2ddecoder's ASTC and BC1, 2026-11-23): the colour and alpha differences from the BC1 block's own decode are the last two numbers of each case.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
void bc1_block_astc(const uint8_t in[8], int rgb, uint8_t out[16]);
void bc1_to_astc(const uint8_t *src, size_t pitch, uint32_t bx, uint32_t by, uint32_t z, int rgb, uint8_t *dst);
void rgba_block_astc(const uint8_t *rgba, size_t row, uint16_t valid, uint8_t out[16]);
void rgba_to_astc(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t z, uint8_t *dst);
// RGBA8 blocks encoded (BC7 decoded on the CPU, then encoded): an opaque gradient off the grey axis (largest difference from the source after an independent decode: 34,
// a gradient in two directions on one line), a colour over an alpha ramp (26: 4 steps for 16 alphas), an edge block with 2x2 texels inside (45: a square of 4 colours)
static const char *const RGBA_ASTC[3] = {"530051a681abe4fc0000aff99a8bb008", "4280696979783c3c34ca0100ff55aa00", "530015dc5e5f01010100000000c0011c"};
static void rgba_case(int k, uint8_t px[64], uint16_t *valid) {
    static const uint16_t V[3] = {0xFFFF, 0xFFFF, 0x0033}; *valid = V[k];
    for (int i = 0; i < 16; i++) {
        int x = i & 3, y = i >> 2; uint8_t *p = px + i * 4;
        if (k == 0) { p[0] = (uint8_t)(40 + 50 * x + 7 * y); p[1] = (uint8_t)(200 - 30 * x - 11 * y); p[2] = (uint8_t)(90 + 20 * y); p[3] = 255; }
        if (k == 1) { p[0] = 180; p[1] = 60; p[2] = 30; p[3] = (uint8_t)(17 * i); }
        if (k == 2) { p[0] = (uint8_t)(x < 2 && y < 2 ? 10 + 100 * x : 0); p[1] = (uint8_t)(x < 2 && y < 2 ? 220 - 90 * y : 0); p[2] = 128; p[3] = 255; }
    }
}
static const struct { const char *name; uint16_t c0, c1; uint32_t idx; int rgb; const char *astc; int colour_diff, alpha_diff; } CASES[] = {
    {"four colours", 0xF81F, 0x07E0, 0x1B1B1B1Bu, 0, "420001feff0100fe0100000093939393", 1, 0},
    {"solid, three-colour mode", 0x52AA, 0x52AA, 0x00000000u, 0, "4200a5a4aaaaa4a40000000000000000", 0, 0},
    {"three colours, line past c1", 0x4208, 0x6318, 0x20820821u, 0, "420065c664c2048c01000000a6699ae6", 2, 0},
    {"three colours, both lines clamped", 0x001F, 0xF800, 0x00061824u, 0, "420001fe0100fe010000000000b02c38", 44, 0},
    {"transparent texels (BC1 with alpha)", 0x2104, 0xC618, 0xF0F0F0F0u, 0, "428001420040004200fe0100f0f0f0f0", 0, 0},
    {"black texels (BC1 without alpha)", 0x2104, 0xC618, 0xF0F0F0F0u, 1, "420001420040004200000000f0f0f0f0", 0, 0},
};
static void block(int i, uint8_t b[8]) { b[0] = (uint8_t)CASES[i].c0; b[1] = (uint8_t)(CASES[i].c0 >> 8); b[2] = (uint8_t)CASES[i].c1; b[3] = (uint8_t)(CASES[i].c1 >> 8);
    for (int k = 0; k < 4; k++) b[4 + k] = (uint8_t)(CASES[i].idx >> (8 * k)); }
int main(void) {
    int n = 0, bad = 0; const int nc = (int)(sizeof CASES / sizeof CASES[0]);
    for (int i = 0; i < nc; i++) {
        uint8_t in[8], out[16]; char hex[33]; block(i, in); bc1_block_astc(in, CASES[i].rgb, out);
        for (int k = 0; k < 16; k++) snprintf(hex + 2 * k, 3, "%02x", out[k]);
        n++; if (strcmp(hex, CASES[i].astc)) { bad++; printf("FAIL %s: %s, expected %s\n", CASES[i].name, hex, CASES[i].astc); }
        n++; if ((out[0] | (out[1] & 7) << 8) != 0x042 || (out[1] >> 3 & 3)) { bad++; printf("FAIL %s: block mode or partitions\n", CASES[i].name); }   // 4x4 weights of 2 bits, one partition
    }
    // rows of blocks with a pitch wider than the row, two slices: each block lands where the tight layout puts it
    uint8_t src[2 * 3 * 40], dst[2 * 3 * 4 * 16]; memset(src, 0, sizeof src);
    for (uint32_t s = 0; s < 2; s++) for (uint32_t y = 0; y < 3; y++) for (uint32_t x = 0; x < 4; x++) block((int)((s * 12 + y * 4 + x) % (uint32_t)nc), src + (s * 3 + y) * 40 + x * 8);
    bc1_to_astc(src, 40, 4, 3, 2, 0, dst);
    for (uint32_t s = 0; s < 2; s++) for (uint32_t y = 0; y < 3; y++) for (uint32_t x = 0; x < 4; x++) {
        int i = (int)((s * 12 + y * 4 + x) % (uint32_t)nc); uint8_t in[8], one[16]; block(i, in); bc1_block_astc(in, 0, one);
        n++; if (memcmp(one, dst + ((s * 3 + y) * 4 + x) * 16, 16)) { bad++; printf("FAIL layout: block %u,%u slice %u\n", x, y, s); }
    }
    for (int k = 0; k < 3; k++) {
        uint8_t px[64], out[16]; uint16_t valid; char hex[33]; rgba_case(k, px, &valid); rgba_block_astc(px, 16, valid, out);
        for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", out[i]);
        n++; if (strcmp(hex, RGBA_ASTC[k])) { bad++; printf("FAIL rgba block %d: %s, expected %s\n", k, hex, RGBA_ASTC[k]); }
    }
    {   // a texture big enough for the threads (256x260: 16,640 blocks), 2 texels short of whole blocks across: each block as the one-block encoder makes it
        enum { W = 1022, H = 65 * 4 }; static uint8_t tex[W * H * 4], out[256 * 65 * 16];
        for (int i = 0; i < W * H * 4; i++) tex[i] = (uint8_t)((i * 2654435761u) >> 24 | ((i & 3) == 3 ? 0xF0 : 0));
        rgba_to_astc(tex, W, H, 1, out);
        int wrong = 0;
        for (int by = 0; by < 65; by++) for (int bx = 0; bx < 256; bx++) {
            uint8_t blk[64] = {0}, one[16]; uint16_t valid = 0;
            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) if (bx * 4 + x < W) { memcpy(blk + y * 16 + x * 4, tex + ((size_t)(by * 4 + y) * W + (size_t)bx * 4 + (size_t)x) * 4, 4); valid |= (uint16_t)(1u << (y * 4 + x)); }
            rgba_block_astc(blk, 16, valid, one); wrong += memcmp(one, out + ((size_t)by * 256 + (size_t)bx) * 16, 16) != 0;
        }
        n++; if (wrong) { bad++; printf("FAIL threaded texture: %d blocks differ\n", wrong); }
    }
    if (bad) { printf("%d of %d cases failed\n", bad, n); return 1; }
    printf("%d cases passed\n", n); return 0;
}
