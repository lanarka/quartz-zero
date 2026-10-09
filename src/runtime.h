// =============================================================================
// Quartz - The Computer
// -----------------------------------------------------------------------------
// runtime.h - low-level "device" interface exposed to the VM.
//
// =============================================================================

#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdint.h>

#define RT_VIDEO_MODE_MIN 1
#define RT_VIDEO_MODE_MAX 3
#define RT_MIN_SCALE 1.0f
#define RT_MAX_SCALE 6.0f

typedef struct { uint8_t r, g, b, a; } rt_color;

void rt_frame(void *rt, const uint8_t *data, uint16_t x, uint16_t y, uint16_t h, uint16_t w);
void rt_pix(void *rt, uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void rt_raw(void *rt, const uint8_t *data, uint32_t len, uint32_t offset);
void rt_clear(void *rt);
void rt_clear_color(void *rt, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
void rt_getres(void *rt, uint16_t *screen_h, uint16_t *screen_w);
void rt_bell(void *rt);
void rt_sound(void *rt, const uint8_t *data, uint32_t len);
void rt_char(void *rt, uint16_t x, uint16_t y, uint16_t cp, rt_color fg, rt_color bg);
void rt_txtframe(void *rt, const uint8_t *data, uint16_t x, uint16_t y, uint16_t h, uint16_t w);
void rt_getermres(void *rt, uint16_t *cols, uint16_t *rows);
void rt_txtclear(void *rt);
void rt_curset(void *rt, uint16_t x, uint16_t y);
void rt_curget(void *rt, uint16_t *x, uint16_t *y);
void rt_curena(void *rt, uint8_t enabled);
void rt_mouse(void *rt, uint16_t *x, uint16_t *y, uint8_t *b);   // Pixel coordinates (0..screen_w-1, 0..screen_h-1), already converted from window space back through the current scale factor.
void rt_keyboard(void *rt, int *count, int *keys, int max_keys);
void rt_poll(void *rt, uint16_t *mx, uint16_t *my, uint8_t *mb, int *kcount, uint16_t keys[4]);
void rt_txt_mouse(void *rt, uint16_t *col, uint16_t *row, uint8_t *b);
void rt_txt_poll(void *rt, uint16_t *col, uint16_t *row, uint8_t *mb, int *kcount, uint16_t keys[4]);
int rt_video(void *rt, uint8_t video_mode, uint16_t fps, float scale, uint8_t cursor);
void rt_video_info(void *rt, uint8_t *video_mode, uint16_t *fps, float *scale, uint8_t *cursor);
void rt_screen_reset(void *rt);

#endif
