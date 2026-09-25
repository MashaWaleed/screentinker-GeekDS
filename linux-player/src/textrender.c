/* textrender.c — draws text straight into a display rect using a small,
 * self-contained "14-segment display" style font (the same geometric
 * convention used by real alphanumeric LED/LCD displays — a small set of
 * straight strokes on a 5x7 grid, not a bitmap copied from anywhere). This
 * keeps the player's only two must-have UI elements — the on-screen pairing
 * code and a clock widget — dependency-free: no FreeType, no font files to
 * ship or find on a stripped-down box.
 *
 * It's intentionally blocky. A "text" widget with long strings will look
 * basic; that's a fine trade for zero external font dependencies. Swapping
 * in FreeType + a real .ttf later is a self-contained change to this file.
 */
#include "stp.h"
#include "textrender.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Grid is 5 wide (x:0..4) by 7 tall (y:0..6). Segment endpoints below. */
typedef struct { int8_t x1, y1, x2, y2; } seg_t;
enum {
    SEG_A, SEG_B, SEG_C, SEG_D, SEG_E, SEG_F,  /* outer rectangle, clockwise from top */
    SEG_G1, SEG_G2,                             /* middle bar, left half / right half */
    SEG_H, SEG_I, SEG_J,                        /* upper-left diag, vertical mid-top, upper-right diag */
    SEG_K, SEG_L, SEG_M,                        /* lower-left diag, vertical mid-bot, lower-right diag */
    SEG_COUNT
};
static const seg_t SEG[SEG_COUNT] = {
    [SEG_A]  = {0,0, 4,0},   [SEG_B]  = {4,0, 4,3},   [SEG_C]  = {4,3, 4,6},
    [SEG_D]  = {0,6, 4,6},   [SEG_E]  = {0,3, 0,6},   [SEG_F]  = {0,0, 0,3},
    [SEG_G1] = {0,3, 2,3},   [SEG_G2] = {2,3, 4,3},
    [SEG_H]  = {0,0, 2,3},   [SEG_I]  = {2,0, 2,3},   [SEG_J]  = {4,0, 2,3},
    [SEG_K]  = {0,6, 2,3},   [SEG_L]  = {2,3, 2,6},   [SEG_M]  = {4,6, 2,3},
};
#define B(seg) (1u << (seg))

static uint32_t glyph_mask(char c) {
    switch (toupper((unsigned char)c)) {
    case '0': return B(SEG_A)|B(SEG_B)|B(SEG_C)|B(SEG_D)|B(SEG_E)|B(SEG_F)|B(SEG_M);
    case '1': return B(SEG_B)|B(SEG_C);
    case '2': return B(SEG_A)|B(SEG_B)|B(SEG_G1)|B(SEG_G2)|B(SEG_E)|B(SEG_D);
    case '3': return B(SEG_A)|B(SEG_B)|B(SEG_G1)|B(SEG_G2)|B(SEG_C)|B(SEG_D);
    case '4': return B(SEG_F)|B(SEG_B)|B(SEG_G1)|B(SEG_G2)|B(SEG_C);
    case '5': return B(SEG_A)|B(SEG_F)|B(SEG_G1)|B(SEG_G2)|B(SEG_C)|B(SEG_D);
    case '6': return B(SEG_A)|B(SEG_F)|B(SEG_G1)|B(SEG_G2)|B(SEG_E)|B(SEG_C)|B(SEG_D);
    case '7': return B(SEG_A)|B(SEG_B)|B(SEG_C);
    case '8': return B(SEG_A)|B(SEG_B)|B(SEG_C)|B(SEG_D)|B(SEG_E)|B(SEG_F)|B(SEG_G1)|B(SEG_G2);
    case '9': return B(SEG_A)|B(SEG_F)|B(SEG_B)|B(SEG_G1)|B(SEG_G2)|B(SEG_C)|B(SEG_D);
    case 'A': return B(SEG_A)|B(SEG_B)|B(SEG_C)|B(SEG_E)|B(SEG_F)|B(SEG_G1)|B(SEG_G2);
    case 'B': return B(SEG_A)|B(SEG_B)|B(SEG_C)|B(SEG_D)|B(SEG_I)|B(SEG_L)|B(SEG_G1)|B(SEG_G2);
    case 'C': return B(SEG_A)|B(SEG_F)|B(SEG_E)|B(SEG_D);
    case 'D': return B(SEG_A)|B(SEG_B)|B(SEG_C)|B(SEG_D)|B(SEG_I)|B(SEG_L);
    case 'E': return B(SEG_A)|B(SEG_F)|B(SEG_E)|B(SEG_D)|B(SEG_G1)|B(SEG_G2);
    case 'F': return B(SEG_A)|B(SEG_F)|B(SEG_E)|B(SEG_G1)|B(SEG_G2);
    case 'G': return B(SEG_A)|B(SEG_F)|B(SEG_E)|B(SEG_D)|B(SEG_C)|B(SEG_G2);
    case 'H': return B(SEG_B)|B(SEG_C)|B(SEG_E)|B(SEG_F)|B(SEG_G1)|B(SEG_G2);
    case 'I': return B(SEG_A)|B(SEG_D)|B(SEG_I)|B(SEG_L);
    case 'J': return B(SEG_B)|B(SEG_C)|B(SEG_D)|B(SEG_E);
    case 'K': return B(SEG_F)|B(SEG_E)|B(SEG_H)|B(SEG_K)|B(SEG_M);
    case 'L': return B(SEG_F)|B(SEG_E)|B(SEG_D);
    case 'M': return B(SEG_F)|B(SEG_E)|B(SEG_B)|B(SEG_C)|B(SEG_H)|B(SEG_J);
    case 'N': return B(SEG_F)|B(SEG_E)|B(SEG_B)|B(SEG_C)|B(SEG_H)|B(SEG_M);
    case 'O': return B(SEG_A)|B(SEG_B)|B(SEG_C)|B(SEG_D)|B(SEG_E)|B(SEG_F);
    case 'P': return B(SEG_A)|B(SEG_F)|B(SEG_E)|B(SEG_B)|B(SEG_G1)|B(SEG_G2);
    case 'Q': return B(SEG_A)|B(SEG_B)|B(SEG_C)|B(SEG_D)|B(SEG_E)|B(SEG_F)|B(SEG_M);
    case 'R': return B(SEG_A)|B(SEG_F)|B(SEG_E)|B(SEG_B)|B(SEG_G1)|B(SEG_G2)|B(SEG_M);
    case 'S': return B(SEG_A)|B(SEG_F)|B(SEG_G1)|B(SEG_G2)|B(SEG_C)|B(SEG_D);
    case 'T': return B(SEG_A)|B(SEG_I)|B(SEG_L);
    case 'U': return B(SEG_B)|B(SEG_C)|B(SEG_D)|B(SEG_E)|B(SEG_F);
    case 'V': return B(SEG_F)|B(SEG_B)|B(SEG_K)|B(SEG_M);
    case 'W': return B(SEG_F)|B(SEG_E)|B(SEG_B)|B(SEG_C)|B(SEG_K)|B(SEG_M);
    case 'X': return B(SEG_H)|B(SEG_J)|B(SEG_K)|B(SEG_M);
    case 'Y': return B(SEG_H)|B(SEG_J)|B(SEG_L);
    case 'Z': return B(SEG_A)|B(SEG_D)|B(SEG_H)|B(SEG_M);
    case ':': return B(SEG_I)|B(SEG_L); /* short center stroke stands in for the two dots */
    case '-': return B(SEG_G1)|B(SEG_G2);
    case '.': return B(SEG_D);
    case '/': return B(SEG_K)|B(SEG_J);
    case '_': return B(SEG_D);
    case '%': return B(SEG_H)|B(SEG_M)|B(SEG_K)|B(SEG_J);
    case ' ': default: return 0;
    }
}

static void draw_seg_line(stp_display_t *d, int x0, int y0, int cell_w, int cell_h,
                           int stroke, const seg_t *s, uint32_t color) {
    color = stp_pack_color(d, color);
    int x1 = x0 + s->x1 * cell_w, y1 = y0 + s->y1 * cell_h;
    int x2 = x0 + s->x2 * cell_w, y2 = y0 + s->y2 * cell_h;
    int dx = x2 - x1, dy = y2 - y1;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps == 0) steps = 1;
    for (int i = 0; i <= steps; i++) {
        int px = x1 + dx * i / steps, py = y1 + dy * i / steps;
        for (int oy = -stroke/2; oy <= stroke/2; oy++)
            for (int ox = -stroke/2; ox <= stroke/2; ox++) {
                int fx = px + ox, fy = py + oy;
                if (fx < 0 || fy < 0 || fx >= d->width || fy >= d->height) continue;
                *(uint32_t *)(d->pixels + (size_t)fy * d->stride + (size_t)fx * 4) = color;
            }
    }
}

/* One glyph advances by 6*scale pixels; full height is 8*scale pixels. */
int text_width(const char *s, int scale) { return (int)strlen(s) * scale * 6; }
int text_height(int scale) { return scale * 8; }

void text_draw(stp_display_t *d, int x, int y, const char *s, int scale,
               uint32_t fg, uint32_t bg, bool opaque_bg) {
    int cell = scale, stroke = scale < 2 ? 1 : scale;
    if (opaque_bg) {
        stp_rect_t r = { x, y, text_width(s, scale), text_height(scale) };
        rect_fill(d, r, bg);
    }
    int cx = x;
    for (const char *p = s; *p; p++) {
        uint32_t mask = glyph_mask(*p);
        for (int i = 0; i < SEG_COUNT; i++)
            if (mask & (1u << i)) draw_seg_line(d, cx, y, cell, cell, stroke, &SEG[i], fg);
        cx += 6 * scale;
    }
}

void text_draw_centered(stp_display_t *d, stp_rect_t rect, const char *s, int scale,
                         uint32_t fg, uint32_t bg, bool opaque_bg) {
    int w = text_width(s, scale), h = text_height(scale);
    int x = rect.x + (rect.w - w) / 2;
    int y = rect.y + (rect.h - h) / 2;
    text_draw(d, x, y, s, scale, fg, bg, opaque_bg);
}

void rect_fill(stp_display_t *d, stp_rect_t r, uint32_t bgra) {
    bgra = stp_pack_color(d, bgra);
    for (int y = 0; y < r.h; y++) {
        uint32_t *row = (uint32_t *)(d->pixels + (size_t)(r.y + y) * d->stride + (size_t)r.x * 4);
        for (int x = 0; x < r.w; x++) row[x] = bgra;
    }
}
