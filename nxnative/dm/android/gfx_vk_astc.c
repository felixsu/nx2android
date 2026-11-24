// BC1 blocks rewritten as ASTC 4x4 blocks, for GPUs with ASTC and no BC (Mali): an image of ASTC 4x4 takes 1 byte a texel where BC1 decoded on the CPU took 4
// (gfx_vk_tex.c bc_target). Pokemon Legends Z-A on a Mali-G720 phone (7.5 GB): its textures, BC decoded to RGBA8 and RG8, took 1.2 GB of GPU memory in Lumiose City,
// the game's own memory went to swap and frames took seconds (2026-11-23).
//
// A BC1 block is two RGB565 endpoints and a 2-bit index a texel. In its four-colour mode (c0 > c1) the palette is c0, c1, 2/3 c0 + 1/3 c1 and 1/3 c0 + 2/3 c1: one
// ASTC partition with RGB direct endpoints (colour endpoint mode 8) at 8 bits and 4x4 weights of 2 bits (0, 21, 43 and 64 / 64 along the line) holds it, the endpoints
// exactly and the two middle colours within 1/192 of the line. The three-colour mode (c0 <= c1: c0, c1, their mean and black, transparent in BC1 with alpha) is
// rewritten on a line through c0 and c1 that puts them on weights 0 and 43 (or 21 and 64) and the mean on 21 (43), 0.5 of the way within 2.3%, where that line stays
// within 0-255, else on c0 to c1 with the mean on a third of the way; a block that uses its
// fourth index becomes a line from black (transparent: RGBA direct, mode 12) to the mean of its other texels, so alpha stays exact and colour is approximated there.
//
// The block (ASTC specification, "Integer Sequence Encoding", "Block Mode"): bits 0-10 block mode 0x042 (4x4 weights, range 0-3, one plane), 11-12 partitions - 1 = 0,
// 13-16 the endpoint mode, then the endpoint values of 8 bits from bit 17 (128 - 17 - 32 weight bits leave 79: 6 or 8 values of 8 bits fit, so the decoder takes that
// range), and the 16 weights of 2 bits from bit 127 down, texel (x, y) the (4y + x)th
#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

static void put(uint8_t *b, unsigned pos, unsigned n, unsigned v) { for (unsigned i = 0; i < n; i++) if (v >> i & 1) b[(pos + i) >> 3] |= (uint8_t)(1u << ((pos + i) & 7)); }
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// e0, e1: RGBA endpoints (alpha used with rgba), w: 16 weights of wbits bits (2: 0-3, block mode 0x042; 3: 0-7, block mode 0x053, which leaves 63 bits: the 6 RGB values
// of 8 bits still fit, the 8 RGBA ones would not)
static void astc_block(uint8_t out[16], const int e0[4], const int e1[4], const uint8_t w[16], int rgba, int wbits) {
    const int *a = e0, *b = e1; uint8_t ww[16]; memcpy(ww, w, 16); const int top = (1 << wbits) - 1;
    if (b[0] + b[1] + b[2] < a[0] + a[1] + a[2]) {   // the decoder reads a smaller second sum as blue contraction: the endpoints swapped, the weights turned round
        a = e1; b = e0; for (int i = 0; i < 16; i++) ww[i] = (uint8_t)(top - ww[i]); }
    memset(out, 0, 16);
    put(out, 0, 11, wbits == 3 ? 0x053 : 0x042); put(out, 13, 4, rgba ? 12 : 8);
    unsigned p = 17;
    for (int c = 0; c < (rgba ? 4 : 3); c++) { put(out, p, 8, (unsigned)a[c]); put(out, p + 8, 8, (unsigned)b[c]); p += 16; }
    for (int i = 0; i < 16; i++) for (int k = 0; k < wbits; k++) if (ww[i] >> k & 1) { unsigned q = 127 - ((unsigned)wbits * (unsigned)i + (unsigned)k); out[q >> 3] |= (uint8_t)(1u << (q & 7)); }
}
static void astc_line(uint8_t out[16], const int e0[4], const int e1[4], const uint8_t w[16], int rgba) { astc_block(out, e0, e1, w, rgba, 2); }

static void rgb565(unsigned c, int o[4]) {
    unsigned r = c >> 11 & 31, g = c >> 5 & 63, b = c & 31;
    o[0] = (int)(r << 3 | r >> 2); o[1] = (int)(g << 2 | g >> 4); o[2] = (int)(b << 3 | b >> 2); o[3] = 255;
}

// One BC1 block (8 bytes) as one ASTC 4x4 block (16 bytes). rgb: BC1 without alpha (the fourth colour of the three-colour mode is opaque black)
void bc1_block_astc(const uint8_t in[8], int rgb, uint8_t out[16]) {
    unsigned c0 = (unsigned)in[0] | (unsigned)in[1] << 8, c1 = (unsigned)in[2] | (unsigned)in[3] << 8;
    uint32_t idx = (uint32_t)in[4] | (uint32_t)in[5] << 8 | (uint32_t)in[6] << 16 | (uint32_t)in[7] << 24;
    int p0[4], p1[4]; rgb565(c0, p0); rgb565(c1, p1);
    uint8_t w[16];
    if (c0 > c1) {   // four colours: c0, c1, 2/3 c0 + 1/3 c1, 1/3 c0 + 2/3 c1 on weights 0, 3, 1, 2
        static const uint8_t map[4] = {0, 3, 1, 2};
        for (int i = 0; i < 16; i++) w[i] = map[idx >> (2 * i) & 3];
        astc_line(out, p0, p1, w, 0); return;
    }
    int used3 = 0; for (int i = 0; i < 16; i++) used3 |= (idx >> (2 * i) & 3) == 3;
    if (!used3) {   // c0, c1 and their mean: of three lines, the one closest to the block's texels
        // (c0 on 0, the mean on 21, c1 on 43, the line running on past c1; c0 on 21, the mean on 43, c1 on 64; or c0 to c1, the mean on the nearer of 21 and 43, off by 1/6
        // of c1 - c0 at most: the fallback where both longer lines leave 0-255)
        int t[3][4], cand[3][2][4];
        for (int c = 0; c < 4; c++) { t[0][c] = p0[c]; t[1][c] = p1[c]; t[2][c] = (p0[c] + p1[c]) / 2; }
        for (int c = 0; c < 4; c++) {
            cand[0][0][c] = p0[c]; cand[0][1][c] = c < 3 ? clamp8(p0[c] + (p1[c] - p0[c]) * 64 / 43) : 255;
            cand[1][0][c] = c < 3 ? clamp8(p1[c] - (p1[c] - p0[c]) * 64 / 43) : 255; cand[1][1][c] = p1[c];
            cand[2][0][c] = p0[c]; cand[2][1][c] = p1[c];
        }
        static const int W[4] = {0, 21, 43, 64};
        long best = -1; int bi = 0; uint8_t bm[3] = {0, 0, 0};
        for (int k = 0; k < 3; k++) {
            long err = 0; uint8_t m[3];
            for (int j = 0; j < 3; j++) {   // each of the three colours on its nearest weight
                long e = -1;
                for (int q = 0; q < 4; q++) {
                    long d = 0;
                    for (int c = 0; c < 3; c++) { int v = (cand[k][0][c] * (64 - W[q]) + cand[k][1][c] * W[q] + 32) >> 6; d += (long)(v - t[j][c]) * (v - t[j][c]); }
                    if (e < 0 || d < e) { e = d; m[j] = (uint8_t)q; }
                }
                err += e;
            }
            if (best < 0 || err < best) { best = err; bi = k; memcpy(bm, m, 3); }
        }
        for (int i = 0; i < 16; i++) w[i] = bm[idx >> (2 * i) & 3];
        astc_line(out, cand[bi][0], cand[bi][1], w, 0);
        return;
    }
    // the fourth index (black; transparent with alpha): a line from it to the mean of the block's other texels
    int sum[3] = {0, 0, 0}, n = 0;
    for (int i = 0; i < 16; i++) {
        unsigned k = idx >> (2 * i) & 3; if (k == 3) continue;
        for (int c = 0; c < 3; c++) sum[c] += k == 0 ? p0[c] : k == 1 ? p1[c] : (p0[c] + p1[c]) / 2;
        n++;
    }
    int lo[4] = {0, 0, 0, rgb ? 255 : 0}, hi[4] = {n ? (sum[0] + n / 2) / n : 0, n ? (sum[1] + n / 2) / n : 0, n ? (sum[2] + n / 2) / n : 0, 255};
    for (int i = 0; i < 16; i++) w[i] = (idx >> (2 * i) & 3) == 3 ? 0 : 3;
    astc_line(out, lo, hi, w, !rgb);
}

// Rows of BC1 blocks (pitch bytes apart, by rows a slice, z slices) into tight ASTC 4x4 blocks
void bc1_to_astc(const uint8_t *src, size_t pitch, uint32_t bx, uint32_t by, uint32_t z, int rgb, uint8_t *dst) {
    for (uint32_t s = 0; s < z; s++) for (uint32_t y = 0; y < by; y++) {
        const uint8_t *row = src + ((size_t)s * by + y) * pitch;
        for (uint32_t x = 0; x < bx; x++, dst += 16) bc1_block_astc(row + (size_t)x * 8, rgb, dst);
    }
}

// Any 4x4 RGBA8 block as one ASTC 4x4 block: BC7 (and BC3) textures, decoded on the CPU, then written as ASTC (gfx_vk_tex.c bc_decode). One partition: an opaque block
// is a line in RGB with 8 steps (weights 0, 9, 18, 27, 37, 46, 55, 64 / 64) and endpoints of 8 bits, a block with alpha below 255 a line in RGBA with 4 steps. The line
// is the texels' principal axis (power iteration on their covariance), its ends the texels' extremes along it, then twice: each texel on its nearest step, the ends
// fitted to those steps by least squares. A block of several colours that no line passes near (BC7 gives such a block up to three partitions) loses most there.
// valid: the texels inside the texture (an edge block of a texture not a multiple of 4 wide or high); the others are left out of the fit
static const int W8[8] = {0, 9, 18, 27, 37, 46, 55, 64}, W4[4] = {0, 21, 43, 64};
// Each texel on the step nearest its projection onto the line e0-e1 (the nearest of the palette: the steps lie on that line), and the squared error of the block
static long fit_err(const int px[16][4], uint16_t valid, int nc, const int e0[4], const int e1[4], const int *W, int nw, uint8_t w[16]) {
    int pal[8][4], d[4], dd = 0; long err = 0;
    for (int k = 0; k < nw; k++) for (int c = 0; c < nc; c++) pal[k][c] = (e0[c] * (64 - W[k]) + e1[c] * W[k] + 32) >> 6;
    for (int c = 0; c < nc; c++) { d[c] = e1[c] - e0[c]; dd += d[c] * d[c]; }
    for (int i = 0; i < 16; i++) {
        if (!(valid >> i & 1)) { w[i] = 0; continue; }
        int k = 0;
        if (dd) {
            int t = 0; for (int c = 0; c < nc; c++) t += (px[i][c] - e0[c]) * d[c];
            int a = t <= 0 ? 0 : t >= dd ? 64 : (int)(((long)t * 64 + dd / 2) / dd);   // 0-64 along the line
            while (k + 1 < nw && W[k + 1] <= a) k++;
            if (k + 1 < nw && a - W[k] > W[k + 1] - a) k++;
        }
        w[i] = (uint8_t)k;
        for (int c = 0; c < nc; c++) { int v = pal[k][c] - px[i][c]; err += (long)v * v; }
    }
    return err;
}
void rgba_block_astc(const uint8_t *rgba, size_t row, uint16_t valid, uint8_t out[16]) {
    int px[16][4], nc = 3, n = 0; float m[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; i++) {
        const uint8_t *p = rgba + (size_t)(i >> 2) * row + (size_t)(i & 3) * 4;
        if (valid >> i & 1) { for (int c = 0; c < 4; c++) px[i][c] = p[c]; if (p[3] != 255) nc = 4; n++; }
        else for (int c = 0; c < 4; c++) px[i][c] = 0;
    }
    const int *W = nc == 4 ? W4 : W8; const int nw = nc == 4 ? 4 : 8;
    int e0[4] = {0, 0, 0, 255}, e1[4] = {0, 0, 0, 255}; uint8_t w[16] = {0};
    if (!n) { astc_block(out, e0, e1, w, nc == 4, nc == 4 ? 2 : 3); return; }
    for (int i = 0; i < 16; i++) if (valid >> i & 1) for (int c = 0; c < nc; c++) m[c] += (float)px[i][c];
    for (int c = 0; c < nc; c++) m[c] /= (float)n;
    float cv[4][4] = {{0}};
    for (int i = 0; i < 16; i++) if (valid >> i & 1) for (int a = 0; a < nc; a++) for (int b = 0; b < nc; b++) cv[a][b] += ((float)px[i][a] - m[a]) * ((float)px[i][b] - m[b]);
    float ax[4] = {1, 1, 1, 1};
    { int big = 0; for (int c = 1; c < nc; c++) if (cv[c][c] > cv[big][big]) big = c; for (int c = 0; c < nc; c++) ax[c] = cv[big][c] + (c == big ? 1e-3f : 0); }
    for (int it = 0; it < 8; it++) {
        float v[4] = {0, 0, 0, 0}, l = 0;
        for (int a = 0; a < nc; a++) { for (int b = 0; b < nc; b++) v[a] += cv[a][b] * ax[b]; l += v[a] * v[a]; }
        if (l < 1e-12f) break;
        float r = 1.0f / sqrtf(l); for (int a = 0; a < nc; a++) ax[a] = v[a] * r;
    }
    { float l = 0; for (int c = 0; c < nc; c++) l += ax[c] * ax[c]; if (l < 1e-12f) { for (int c = 0; c < nc; c++) ax[c] = 0; } else { float r = 1.0f / sqrtf(l); for (int c = 0; c < nc; c++) ax[c] *= r; } }
    float lo = 0, hi = 0; int first = 1;
    for (int i = 0; i < 16; i++) if (valid >> i & 1) {
        float t = 0; for (int c = 0; c < nc; c++) t += ((float)px[i][c] - m[c]) * ax[c];
        if (first || t < lo) lo = t; if (first || t > hi) hi = t; first = 0;
    }
    for (int c = 0; c < nc; c++) { e0[c] = clamp8((int)lrintf(m[c] + ax[c] * lo)); e1[c] = clamp8((int)lrintf(m[c] + ax[c] * hi)); }
    int b0[4], b1[4]; uint8_t bw[16]; long best = fit_err(px, valid, nc, e0, e1, W, nw, w);
    memcpy(b0, e0, sizeof b0); memcpy(b1, e1, sizeof b1); memcpy(bw, w, 16);
    for (int it = 0; it < 2 && best > 0; it++) {   // the ends refitted to the steps the texels took: least squares on (1 - a) e0 + a e1
        float s00 = 0, s01 = 0, s11 = 0, r0[4] = {0, 0, 0, 0}, r1[4] = {0, 0, 0, 0};
        for (int i = 0; i < 16; i++) if (valid >> i & 1) {
            float a = (float)W[w[i]] / 64.0f, b = 1.0f - a;
            s00 += b * b; s01 += a * b; s11 += a * a;
            for (int c = 0; c < nc; c++) { r0[c] += b * (float)px[i][c]; r1[c] += a * (float)px[i][c]; }
        }
        float det = s00 * s11 - s01 * s01;
        if (det < 1e-6f) break;
        for (int c = 0; c < nc; c++) { e0[c] = clamp8((int)lrintf((r0[c] * s11 - r1[c] * s01) / det)); e1[c] = clamp8((int)lrintf((r1[c] * s00 - r0[c] * s01) / det)); }
        long err = fit_err(px, valid, nc, e0, e1, W, nw, w);
        if (err < best) { best = err; memcpy(b0, e0, sizeof b0); memcpy(b1, e1, sizeof b1); memcpy(bw, w, 16); }
        else break;
    }
    if (nc == 3) { b0[3] = b1[3] = 255; }
    astc_block(out, b0, b1, bw, nc == 4, nc == 4 ? 2 : 3);
}

// Rows of blocks [r0, r1) of w x h x z RGBA8 texels (rows of w * 4 bytes, rows of blocks numbered through the slices) into tight ASTC 4x4 blocks
static void rows_to_astc(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t r0, uint32_t r1, uint8_t *dst) {
    const size_t row = (size_t)w * 4; const uint32_t bx_n = (w + 3) / 4, by_n = (h + 3) / 4;
    for (uint32_t rr = r0; rr < r1; rr++) {
        uint32_t s = rr / by_n, by = rr % by_n; uint8_t *o = dst + (size_t)rr * bx_n * 16;
        for (uint32_t bx = 0; bx < bx_n; bx++, o += 16) {
            uint16_t valid = 0; uint8_t tmp[64]; const uint8_t *src = rgba + ((size_t)s * h + (size_t)by * 4) * row + (size_t)bx * 16; size_t r = row;
            if (bx * 4 + 4 > w || by * 4 + 4 > h) {   // an edge block: its texels inside the texture copied out, the rest left out of the fit
                memset(tmp, 0, sizeof tmp);
                for (uint32_t y = 0; y < 4; y++) for (uint32_t x = 0; x < 4; x++) if (bx * 4 + x < w && by * 4 + y < h) {
                    memcpy(tmp + y * 16 + x * 4, rgba + ((size_t)s * h + by * 4 + y) * row + (size_t)(bx * 4 + x) * 4, 4); valid |= (uint16_t)(1u << (y * 4 + x)); }
                src = tmp; r = 16;
            } else valid = 0xFFFF;
            rgba_block_astc(src, r, valid, o);
        }
    }
}
typedef struct { const uint8_t *rgba; uint32_t w, h, r0, r1; uint8_t *dst; } rows_job;
static void *rows_main(void *a) { rows_job *j = a; rows_to_astc(j->rgba, j->w, j->h, j->r0, j->r1, j->dst); return NULL; }
// w x h x z RGBA8 texels (rows of w * 4 bytes) into tight ASTC 4x4 blocks; a texture of 16,384 blocks or more on four threads (about 28 million texels a second
// a thread on an M-series Mac: Lumiose City's BC7 textures, 83 million texels, were 3 s of one thread)
void rgba_to_astc(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t z, uint8_t *dst) {
    const uint32_t rows = (h + 3) / 4 * z; const uint64_t blocks = (uint64_t)rows * ((w + 3) / 4);
    enum { NT = 4 };
    if (blocks < 16384 || rows < NT) { rows_to_astc(rgba, w, h, 0, rows, dst); return; }
    rows_job j[NT]; pthread_t t[NT]; int ok[NT] = {0};
    for (int i = 0; i < NT; i++) {
        j[i] = (rows_job){rgba, w, h, rows * (uint32_t)i / NT, rows * (uint32_t)(i + 1) / NT, dst};
        if (i) ok[i] = !pthread_create(&t[i], NULL, rows_main, &j[i]);
    }
    rows_main(&j[0]);
    for (int i = 1; i < NT; i++) { if (ok[i]) pthread_join(t[i], NULL); else rows_main(&j[i]); }
}
