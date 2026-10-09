// =============================================================================
// Quartz - The Computer
// -----------------------------------------------------------------------------
// runtime.c - raylib-backed presentation layer (RunCtx implementation).
//
// This file owns two parallel "screens" that get composited together by
// quartz.c every frame:
//   - a pixel framebuffer (rt->fb, an RGBA8 CPU buffer mirrored into
//     rt->gfx_texture) driven by the OP_GFX_* opcodes, and
//   - a text-mode terminal (rt->term, a grid of TerminalCell rendered into
//     an offscreen RenderTexture2D) driven by the OP_TXT_* opcodes.
//
// Both are dumb: this file has no knowledge of the VM or opcodes at all, it
// just implements the rt_*() functions declared in runtime.h/runtime_setup.h
// which vm.c calls with raw pixel/cell data already prepared. Video-mode
// switches (rt_video()) reallocate both buffers at once via
// runtime_realloc_video(). Actual GPU texture updates only happen lazily in
// runtime_flush(), gated behind "dirty" flags (fb_dirty / term_changed), so a
// VM program that redraws nothing in a given frame costs nothing here.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "raylib.h"
#include "runtime.h"
#include "runtime_setup.h"
#include "bell_data.h"

#define CURSOR_BLINK_INTERVAL 0.5
#define CURSOR_COLOR (Color){28, 255, 14, 255}

typedef struct {
    int character;
    Color foreground;
    Color background;
} TerminalCell;

typedef struct {
    int cols;
    int rows;
    int charWidth;
    int charHeight;
    Font font;
    RenderTexture2D target;
    TerminalCell **cells;
    int cursorX, cursorY;
    int cursorVisible;
    double lastCursorBlink;
    int cursorEnabled;
} Terminal;

static int InitTerminal(Terminal *term, Font font, int cols, int rows, int fontSize) {
    memset(term, 0, sizeof(*term));
    term->cols = cols;
    term->rows = rows;
    term->font = font;
    term->charHeight = fontSize;
    term->charWidth = (int)MeasureTextEx(font, "W", (float)fontSize, 0).x;
    term->target = LoadRenderTexture(cols * term->charWidth, rows * term->charHeight);
    term->cursorX = 0;
    term->cursorY = 0;
    term->cursorVisible = 1;
    term->cursorEnabled = 0;
    term->lastCursorBlink = GetTime();

    term->cells = (TerminalCell **)malloc((size_t)rows * sizeof(TerminalCell *));
    if (!term->cells) return 0;
    for (int y = 0; y < rows; y++) term->cells[y] = NULL;

    for (int y = 0; y < rows; y++) {
        term->cells[y] = (TerminalCell *)malloc((size_t)cols * sizeof(TerminalCell));
        if (!term->cells[y]) return 0;
        for (int x = 0; x < cols; x++) {
            term->cells[y][x].character = -1;
            term->cells[y][x].foreground = LIGHTGRAY;
            term->cells[y][x].background = BLACK;
        }
    }
    return 1;
}

static void FreeTerminalBuffers(Terminal *term) {
    if (term->cells) {
        for (int y = 0; y < term->rows; y++) free(term->cells[y]);
        free(term->cells);
        term->cells = NULL;
    }
    UnloadRenderTexture(term->target);
}

static void FreeTerminal(Terminal *term) {
    FreeTerminalBuffers(term);
    UnloadFont(term->font);
}

static void TerminalSetCursor(Terminal *term, int x, int y) {
    term->cursorX = x;
    term->cursorY = y;
}

static void TerminalCursorEnabled(Terminal *term, int enabled) {
    term->cursorEnabled = enabled;
}

static Vector2 TerminalGetCursor(Terminal *term) {
    return (Vector2){ (float)term->cursorX, (float)term->cursorY };
}

static void DrawTerminalBuffer(Terminal *term) {
    for (int y = 0; y < term->rows; y++) {
        for (int x = 0; x < term->cols; x++) {
            TerminalCell cell = term->cells[y][x];
            Vector2 pos = { (float)(x * term->charWidth), (float)(y * term->charHeight) };
            Rectangle bgRect = { pos.x, pos.y, (float)term->charWidth, (float)term->charHeight };
            DrawRectangleRec(bgRect, cell.background);
            if (cell.character != -1)
                DrawTextCodepoint(term->font, cell.character, pos, (float)term->charHeight, cell.foreground);
        }
    }
    if (term->cursorEnabled > 0 && term->cursorVisible) {
        if (term->cursorEnabled == 1) {
            Rectangle cursor = {
                (float)(term->cursorX * term->charWidth), (float)(term->cursorY * term->charHeight),
                (float)term->charWidth, (float)term->charHeight
            };
            DrawRectangleRec(cursor, CURSOR_COLOR);
        }
        if (term->cursorEnabled == 2) {
            Rectangle cursor = {
                (float)(term->cursorX * term->charWidth), (float)(term->cursorY * term->charHeight),
                2, (float)term->charHeight
            };
            DrawRectangleRec(cursor, CURSOR_COLOR);
        }
    }
}

struct RunCtx {
    uint16_t screen_w, screen_h;
    uint8_t *fb;
    Texture2D gfx_texture;
    int fb_dirty;
    Terminal term;
    int term_changed;
    Sound bell;
    int bell_loaded;
    Sound sound;
    int sound_loaded;
    float scale;
    uint8_t video_mode;
    int fps;
    int cursor_visible;
};

int runtime_video_mode_dims(int mode, int *screen_w, int *screen_h, int *term_cols, int *term_rows) {
    switch (mode) {
        case 1: *screen_w = 640;  *screen_h = 480; *term_cols = 80;  *term_rows = 30; return 1;
        case 2: *screen_w = 1024; *screen_h = 768; *term_cols = 128; *term_rows = 48; return 1;
        case 3: *screen_w = 1280; *screen_h = 960; *term_cols = 160; *term_rows = 60; return 1;
        default: return 0;
    }
}

// (Re)allocates the framebuffer and terminal buffers for a given resolution.
// Used both by runtime_create() on startup and by rt_video() when the VM
// switches video mode at runtime. Frees any previously-held buffers first,
// so it is safe to call on an already-initialized RunCtx.
static int runtime_realloc_video(RunCtx *rt, uint16_t screen_w, uint16_t screen_h,
                                  int term_cols, int term_rows, Font font, int font_size) {
    FreeTerminalBuffers(&rt->term);
    UnloadTexture(rt->gfx_texture);
    free(rt->fb);
    rt->fb = NULL;

    rt->screen_w = screen_w;
    rt->screen_h = screen_h;

    rt->fb = (uint8_t *)calloc((size_t)screen_w * screen_h * 4, 1);
    if (!rt->fb) return 0;

    Image img = {
        .data = rt->fb, .width = screen_w, .height = screen_h,
        .mipmaps = 1, .format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8
    };
    rt->gfx_texture = LoadTextureFromImage(img);
    rt->fb_dirty = 1;

    if (!InitTerminal(&rt->term, font, term_cols, term_rows, font_size)) {
        FreeTerminalBuffers(&rt->term);
        return 0;
    }
    rt->term_changed = 1;
    return 1;
}

RunCtx *runtime_create(uint16_t screen_w, uint16_t screen_h, Font font, int term_cols, int term_rows,
                        int font_size, float scale, int video_mode, int fps, int cursor) {
    RunCtx *rt = (RunCtx *)calloc(1, sizeof(RunCtx));
    if (!rt) return NULL;
    rt->scale = (scale > 0.0001f) ? scale : 1.0f;
    rt->video_mode = (uint8_t)video_mode;
    rt->fps = fps;
    rt->cursor_visible = cursor ? 1 : 0;
    if (!runtime_realloc_video(rt, screen_w, screen_h, term_cols, term_rows, font, font_size)) {
        fprintf(stderr, "Quartz: failed to allocate video buffers\n");
        UnloadFont(font);
        UnloadTexture(rt->gfx_texture);
        free(rt->fb);
        free(rt);
        return NULL;
    }
    Wave wave = LoadWaveFromMemory(".wav", bell_wav, (int)bell_wav_len);
    rt->bell = LoadSoundFromWave(wave);
    rt->bell_loaded = 1;
    return rt;
}

void runtime_destroy(RunCtx *rt) {
    if (!rt) return;
    if (rt->bell_loaded) UnloadSound(rt->bell);
    if (rt->sound_loaded) UnloadSound(rt->sound);
    FreeTerminal(&rt->term);
    UnloadTexture(rt->gfx_texture);
    free(rt->fb);
    free(rt);
}

// Backing implementation of the OP_VIDEO opcode: switches resolution/fps/
// scale/cursor visibility at runtime. Only reallocates the framebuffer and
// terminal buffers (via runtime_realloc_video()) when the video mode
// actually changes; fps/scale/cursor are cheap to apply unconditionally.
int rt_video(void *rtp, uint8_t video_mode, uint16_t fps, float scale, uint8_t cursor) {
    RunCtx *rt = (RunCtx *)rtp;
    int screen_w, screen_h, term_cols, term_rows;
    if (!runtime_video_mode_dims(video_mode, &screen_w, &screen_h, &term_cols, &term_rows)) {
        return 0;
    }
    if (video_mode != rt->video_mode) {
        Font font = rt->term.font;
        int  font_size = rt->term.charHeight;
        if (!runtime_realloc_video(rt, (uint16_t)screen_w, (uint16_t)screen_h,
                                    term_cols, term_rows, font, font_size)) {
            return 0;
        }
        rt->video_mode = video_mode;
    }
    rt->fps = fps;
    SetTargetFPS(fps);
    rt->scale = (scale < RT_MIN_SCALE) ? RT_MIN_SCALE : (scale > RT_MAX_SCALE) ? RT_MAX_SCALE : scale;
    SetWindowSize((int)((float)rt->screen_w * rt->scale), (int)((float)rt->screen_h * rt->scale));
    rt->cursor_visible = cursor ? 1 : 0;
    if (rt->cursor_visible) ShowCursor(); else HideCursor();
    return 1;
}

void rt_video_info(void *rtp, uint8_t *video_mode, uint16_t *fps, float *scale, uint8_t *cursor) {
    RunCtx *rt = (RunCtx *)rtp;
    *video_mode = rt->video_mode;
    *fps = (uint16_t)rt->fps;
    *scale = rt->scale;
    *cursor = (uint8_t)rt->cursor_visible;
}

void runtime_get_video(RunCtx *rt, int *video_mode, int *screen_w, int *screen_h, float *scale, int *fps) {
    *video_mode = rt->video_mode;
    *screen_w   = rt->screen_w;
    *screen_h   = rt->screen_h;
    *scale      = rt->scale;
    *fps        = rt->fps;
}

void runtime_set_fps(RunCtx *rt, int fps) {
    rt->fps = fps;
}

void runtime_set_scale(RunCtx *rt, float scale) {
    rt->scale = (scale > 0.0001f) ? scale : 1.0f;
}

// Called once per host frame by quartz.c, after all VM instructions for the
// frame have run. Pushes the CPU-side framebuffer to the GPU texture only if
// something actually changed (fb_dirty), advances the cursor blink timer,
// and re-renders the terminal's offscreen texture only if its contents or
// the cursor's visibility changed (term_changed) - keeping idle frames cheap.
void runtime_flush(RunCtx *rt) {
    if (rt->fb_dirty) {
        UpdateTexture(rt->gfx_texture, rt->fb);
        rt->fb_dirty = 0;
    }
    double now = GetTime();
    if (now - rt->term.lastCursorBlink >= CURSOR_BLINK_INTERVAL) {
        rt->term.cursorVisible = !rt->term.cursorVisible;
        rt->term.lastCursorBlink = now;
        rt->term_changed = 1;
    }
    if (rt->term_changed) {
        BeginTextureMode(rt->term.target);
        ClearBackground(BLACK);
        DrawTerminalBuffer(&rt->term);
        EndTextureMode();
        rt->term_changed = 0;
    }
}

Texture2D runtime_gfx_texture(RunCtx *rt) { return rt->gfx_texture; }
RenderTexture2D runtime_term_texture(RunCtx *rt) { return rt->term.target; }

void runtime_clear_all(RunCtx *rt) {
    rt_screen_reset(rt);
}

void rt_screen_reset(void *rtp) {
    RunCtx *rt = (RunCtx *)rtp;
    memset(rt->fb, 0, (size_t)rt->screen_w * rt->screen_h * 4);
    rt->fb_dirty = 1;
    for (int y = 0; y < rt->term.rows; y++) {//
        for (int x = 0; x < rt->term.cols; x++) {
            rt->term.cells[y][x].character = -1;
            rt->term.cells[y][x].foreground = LIGHTGRAY;
            rt->term.cells[y][x].background = BLACK;
        }
    }
    rt->term.cursorEnabled = 0;
    rt->term_changed = 1;
}

// Standard "source over destination" alpha blend of one RGBA8 pixel into
// another, used by rt_frame() so that OP_GFX_FRAME can draw semi-transparent
// sprites onto the existing framebuffer instead of always overwriting it.
static inline void blend_pixel(uint8_t *dst, const uint8_t *src) {
    uint8_t sa = src[3];
    if (sa == 255) {
        dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
        return;
    }
    if (sa == 0) return;
    uint8_t da = dst[3];
    uint32_t inv_sa = 255 - sa;
    uint32_t out_a = sa + (uint32_t)(da * inv_sa) / 255;
    for (int c = 0; c < 3; c++) {
        uint32_t num = (uint32_t)src[c] * sa + (uint32_t)dst[c] * ((uint32_t)(da * inv_sa) / 255);
        dst[c] = (out_a > 0) ? (uint8_t)(num / out_a) : 0;
    }
    dst[3] = (uint8_t)out_a;
}

void rt_frame(void *rtp, const uint8_t *data, uint16_t x, uint16_t y, uint16_t h, uint16_t w) {
    RunCtx *rt = (RunCtx *)rtp;
    if (x >= rt->screen_w || y >= rt->screen_h) return;
    uint16_t src_stride = w;
    if (x + w > rt->screen_w) w = rt->screen_w - x;
    if (y + h > rt->screen_h) h = rt->screen_h - y;
    for (uint16_t row = 0; row < h; row++) {
        uint32_t fb_offset = (uint32_t)((y + row) * rt->screen_w + x) * 4;
        const uint8_t *src = data + (size_t)row * src_stride * 4;
        uint8_t *dst = &rt->fb[fb_offset];
        for (uint16_t col = 0; col < w; col++) {
            blend_pixel(&dst[col * 4], &src[col * 4]);
        }
    }
    rt->fb_dirty = 1;
}

void rt_pix(void *rtp, uint16_t x, uint16_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    RunCtx *rt = (RunCtx *)rtp;
    if (x >= rt->screen_w || y >= rt->screen_h) return;
    uint32_t i = ((uint32_t)y * rt->screen_w + x) * 4;
    rt->fb[i + 0] = r; rt->fb[i + 1] = g; rt->fb[i + 2] = b; rt->fb[i + 3] = a;
    rt->fb_dirty = 1;
}

void rt_raw(void *rtp, const uint8_t *data, uint32_t len, uint32_t offset) {
    RunCtx *rt = (RunCtx *)rtp;
    uint32_t fb_size = (uint32_t)rt->screen_w * rt->screen_h * 4;
    if ((uint64_t)offset + (uint64_t)len > (uint64_t)fb_size) return;
    memcpy(&rt->fb[offset], data, len);
    rt->fb_dirty = 1;
}

void rt_clear(void *rtp) {
    RunCtx *rt = (RunCtx *)rtp;
    memset(rt->fb, 0, (size_t)rt->screen_w * rt->screen_h * 4);
    rt->fb_dirty = 1;
}

void rt_clear_color(void *rtp, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    RunCtx *rt = (RunCtx *)rtp;
    if (r == 0 && g == 0 && b == 0 && a == 0) { rt_clear(rtp); return; }
    size_t count = (size_t)rt->screen_w * rt->screen_h;
    uint8_t *dst = rt->fb;
    for (size_t i = 0; i < count; i++) {
        dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = a;
        dst += 4;
    }
    rt->fb_dirty = 1;
}

void rt_getres(void *rtp, uint16_t *screen_h, uint16_t *screen_w) {
    RunCtx *rt = (RunCtx *)rtp;
    *screen_h = rt->screen_h;
    *screen_w = rt->screen_w;
}

void rt_bell(void *rtp) {
    RunCtx *rt = (RunCtx *)rtp;
    if (rt->bell_loaded) PlaySound(rt->bell);
}

void rt_sound(void *rtp, const uint8_t *data, uint32_t len) {
    RunCtx *rt = (RunCtx *)rtp;
    if (rt->sound_loaded) {
        StopSound(rt->sound);
        UnloadSound(rt->sound);
        rt->sound_loaded = 0;
    }
    Wave wave = LoadWaveFromMemory(".wav", data, (int)len);
    if (wave.data == NULL) {
        return;
    }
    rt->sound = LoadSoundFromWave(wave);//
    UnloadWave(wave);
    rt->sound_loaded = 1;
    PlaySound(rt->sound);
}

void rt_char(void *rtp, uint16_t x, uint16_t y, uint16_t cp, rt_color fg, rt_color bg) {
    RunCtx *rt = (RunCtx *)rtp;
    if (x >= (uint16_t)rt->term.cols || y >= (uint16_t)rt->term.rows) return;
    rt->term.cells[y][x].character = cp;
    rt->term.cells[y][x].foreground = (Color){fg.r, fg.g, fg.b, fg.a};
    rt->term.cells[y][x].background = (Color){bg.r, bg.g, bg.b, bg.a};
    rt->term_changed = 1;
}

void rt_txtclear(void *rtp) {
    RunCtx *rt = (RunCtx *)rtp;
    for (int y = 0; y < rt->term.rows; y++) {
        for (int x = 0; x < rt->term.cols; x++) {
            rt->term.cells[y][x].character = -1;
            rt->term.cells[y][x].foreground = LIGHTGRAY;
            rt->term.cells[y][x].background = BLACK;
        }
    }
    rt->term_changed = 1;
}

void rt_txtframe(void *rtp, const uint8_t *data, uint16_t x, uint16_t y, uint16_t h, uint16_t w) {
    RunCtx *rt = (RunCtx *)rtp;
    uint16_t render_w = w, render_h = h;
    if (x >= (uint16_t)rt->term.cols || y >= (uint16_t)rt->term.rows) return;
    if (x + render_w > (uint16_t)rt->term.cols) render_w = (uint16_t)rt->term.cols - x;
    if (y + render_h > (uint16_t)rt->term.rows) render_h = (uint16_t)rt->term.rows - y;

    for (uint16_t dy = 0; dy < render_h; dy++) {
        for (uint16_t dx = 0; dx < render_w; dx++) {
            uint32_t off = (uint32_t)(dy * w + dx) * 10;
            uint16_t cp = data[off] | (data[off + 1] << 8);
            Color fg = { data[off+2], data[off+3], data[off+4], data[off+5] };
            Color bg = { data[off+6], data[off+7], data[off+8], data[off+9] };
            rt->term.cells[y + dy][x + dx].character = cp;
            rt->term.cells[y + dy][x + dx].foreground = fg;
            rt->term.cells[y + dy][x + dx].background = bg;
        }
    }
    rt->term_changed = 1;
}

void rt_getermres(void *rtp, uint16_t *cols, uint16_t *rows) {
    RunCtx *rt = (RunCtx *)rtp;
    *cols = (uint16_t)rt->term.cols;
    *rows = (uint16_t)rt->term.rows;
}

void rt_curset(void *rtp, uint16_t x, uint16_t y) {
    RunCtx *rt = (RunCtx *)rtp;
    TerminalSetCursor(&rt->term, x, y);
    rt->term_changed = 1;
}

void rt_curget(void *rtp, uint16_t *x, uint16_t *y) {
    RunCtx *rt = (RunCtx *)rtp;
    Vector2 v = TerminalGetCursor(&rt->term);
    *x = (uint16_t)v.x;
    *y = (uint16_t)v.y;
}

void rt_curena(void *rtp, uint8_t enabled) {
    RunCtx *rt = (RunCtx *)rtp;
    TerminalCursorEnabled(&rt->term, enabled);//
    rt->term_changed = 1;
}

void rt_mouse(void *rtp, uint16_t *x, uint16_t *y, uint8_t *b) {
    RunCtx *rt = (RunCtx *)rtp;
    Vector2 m = GetMousePosition();
    int lx = (int)(m.x / rt->scale);
    int ly = (int)(m.y / rt->scale);
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (rt->screen_w > 0 && lx >= rt->screen_w) lx = rt->screen_w - 1;
    if (rt->screen_h > 0 && ly >= rt->screen_h) ly = rt->screen_h - 1;
    *x = (uint16_t)lx;
    *y = (uint16_t)ly;

    uint8_t btn = 0;
    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT))   btn |= 1;
    if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT))  btn |= 2;
    if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) btn |= 3;
    *b = btn;
}

void rt_keyboard(void *rtp, int *count, int *keys, int max_keys) {
    (void)rtp;
    int kc = 0;//
    for (int k = 0; k < 512 && kc < max_keys && kc < 4; k++) {
        // Lock keys are handled separately below (see comment) - IsKeyDown()
        // for them gets stuck "true" forever after the first press due to
        // the missing release, so they must NOT be reported via this scan
        // at all, or that stuck "true" would mask every later press/release
        // pulse we synthesize from the event queue further down.
        if (k == KEY_CAPS_LOCK || k == KEY_SCROLL_LOCK || k == KEY_NUM_LOCK) continue;
        if (IsKeyDown(k)) keys[kc++] = k;
    }

    // --- Lock-key workaround (Caps Lock / Scroll Lock / Num Lock) ----------
    // raylib/GLFW does not reliably deliver a key-RELEASE event for lock
    // keys on desktop platforms (confirmed upstream bug, see
    // https://github.com/raysan5/raylib/issues/4078 - "Caps Lock release
    // is not registered on Desktop platforms"). The OS reports the PRESS
    // that engages the lock just fine, but never sends the matching
    // release, so CORE.Input.Keyboard.currentKeyState[KEY_CAPS_LOCK] gets
    // stuck at "down" forever after the very first tap - IsKeyDown() would
    // report it as continuously held from then on, EVEN AFTER the user
    // physically taps it again to disengage. That's why these three keys
    // are explicitly skipped in the scan above instead of just being left
    // to fall through: if we didn't skip them, this stuck "true" would
    // permanently mask any edge we try to synthesize below, and a quartz
    // program watching for "currently held" state could detect the lock
    // turning ON but never detect it turning back OFF again.
    //
    // raylib's separate key-press EVENT QUEUE (drained via GetKeyPressed)
    // is fed directly from the same low-level callback at the moment of
    // the PRESS action itself, independent of whatever bookkeeping the
    // continuous currentKeyState array is stuck on. Empirically (and per
    // the linked issue) the PRESS side is reported correctly every single
    // time the key is physically actuated - engaging AND disengaging the
    // lock are both genuine press actions - so draining this queue lets us
    // recover a clean one-shot "pressed" pulse for lock keys on every tap,
    // regardless of the broken release.
    //
    // We synthesize that pulse into our own "currently held" report as a
    // single-frame blip - present only on the exact frame the queue gave
    // us the press, gone again the very next frame (since we no longer
    // fall back to the stuck IsKeyDown state at all). From a quartz
    // program's point of view (comparing this frame's code against last
    // frame's to detect a fresh press) Caps Lock/Scroll Lock/Num Lock then
    // behave exactly like a normal, ordinary key.
    //
    // Nothing else in this codebase calls GetKeyPressed() (host-level
    // shortcuts use IsKeyPressed(), which reads the separate current/
    // previous state array), so it's safe for rt_keyboard to be the sole
    // consumer that drains this queue every frame.
    int pk;
    while ((pk = GetKeyPressed()) != 0) {
        if (pk == KEY_CAPS_LOCK || pk == KEY_SCROLL_LOCK || pk == KEY_NUM_LOCK) {
            if (kc < max_keys && kc < 4) {
                int already = 0;
                for (int i = 0; i < kc; i++) {
                    if (keys[i] == pk) { already = 1; break; }
                }
                if (!already) keys[kc++] = pk;
            }
        }
    }

    *count = kc;
}

void rt_poll(void *rtp, uint16_t *mx, uint16_t *my, uint8_t *mb, int *kcount, uint16_t keys[4]) {
    uint16_t x, y; uint8_t b;
    rt_mouse(rtp, &x, &y, &b);
    *mx = x; *my = y; *mb = b;

    int ikeys[4] = {0,0,0,0};
    int count = 0;
    rt_keyboard(rtp, &count, ikeys, 4);
    *kcount = count;
    for (int i = 0; i < 4; i++) keys[i] = (uint16_t)ikeys[i];
}

void rt_txt_mouse(void *rtp, uint16_t *col, uint16_t *row, uint8_t *b) {
    RunCtx *rt = (RunCtx *)rtp;
    uint16_t px, py;
    rt_mouse(rtp, &px, &py, b);

    uint32_t c = (rt->screen_w > 0) ? ((uint32_t)px * (uint32_t)rt->term.cols) / rt->screen_w : 0;
    uint32_t r = (rt->screen_h > 0) ? ((uint32_t)py * (uint32_t)rt->term.rows) / rt->screen_h : 0;
    if (c >= (uint32_t)rt->term.cols) c = (uint32_t)rt->term.cols - 1;
    if (r >= (uint32_t)rt->term.rows) r = (uint32_t)rt->term.rows - 1;

    *col = (uint16_t)c;
    *row = (uint16_t)r;
}

void rt_txt_poll(void *rtp, uint16_t *col, uint16_t *row, uint8_t *mb, int *kcount, uint16_t keys[4]) {
    rt_txt_mouse(rtp, col, row, mb);

    int ikeys[4] = {0,0,0,0};
    int count = 0;
    rt_keyboard(rtp, &count, ikeys, 4);
    *kcount = count;
    for (int i = 0; i < 4; i++) keys[i] = (uint16_t)ikeys[i];
}
