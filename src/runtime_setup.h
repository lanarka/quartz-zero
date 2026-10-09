// =============================================================================
// Quartz - The Computer
// -----------------------------------------------------------------------------
// runtime_setup.h - lifecycle and host-facing control interface for RunCtx.
//
// =============================================================================

#ifndef RUNTIME_SETUP_H
#define RUNTIME_SETUP_H

#include "raylib.h"
#include "runtime.h"

typedef struct RunCtx RunCtx;  // Opaque handle; full definition lives in runtime.c.

RunCtx *runtime_create(uint16_t screen_w, uint16_t screen_h, Font font,
                        int term_cols, int term_rows, int font_size,
                        float scale, int video_mode, int fps, int cursor);
void runtime_destroy(RunCtx *rt);
int runtime_video_mode_dims(int mode, int *screen_w, int *screen_h, int *term_cols, int *term_rows);
void runtime_get_video(RunCtx *rt, int *video_mode, int *screen_w, int *screen_h, float *scale, int *fps);
void runtime_set_fps(RunCtx *rt, int fps);
void runtime_set_scale(RunCtx *rt, float scale);
void runtime_flush(RunCtx *rt);
Texture2D runtime_gfx_texture(RunCtx *rt);
RenderTexture2D runtime_term_texture(RunCtx *rt);
void runtime_clear_all(RunCtx *rt);

#endif
