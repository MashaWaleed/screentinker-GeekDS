#ifndef STP_TEXTRENDER_H
#define STP_TEXTRENDER_H
#include "stp.h"

int text_width(const char *s, int scale);
int text_height(int scale);
void text_draw(stp_display_t *d, int x, int y, const char *s, int scale,
               uint32_t fg, uint32_t bg, bool opaque_bg);
void text_draw_centered(stp_display_t *d, stp_rect_t rect, const char *s, int scale,
                         uint32_t fg, uint32_t bg, bool opaque_bg);
void rect_fill(stp_display_t *d, stp_rect_t r, uint32_t bgra);

#endif
