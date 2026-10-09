// =============================================================================
// Quartz - The Computer
// -----------------------------------------------------------------------------
// quartz.c - application entry point.
//
// This file owns the raylib window/audio device and the outer "host" loop:
// it parses command-line options, sets up the raylib font/window, creates the
// runtime (RunCtx, see runtime.c / runtime_setup.h) and the VM (see vm.c /
// vm.h), then repeatedly feeds the VM a batch of instructions per frame and
// presents whatever the VM rendered into the runtime's framebuffer/terminal
// textures. It also handles host-level keyboard shortcuts (zoom, pause,
// reboot, FPS cycling) that live outside the VM's own instruction set.
//
// Overall flow of main():
//   1. Parse argv into runtime/VM configuration (video mode, scale, fps, ...).
//   2. Open the raylib window + audio device, load the built-in font.
//   3. runtime_create() -> allocate the framebuffer/terminal buffers (RunCtx).
//   4. vm_init() + vm.create() -> allocate VM RAM and load the program image.
//   5. Optionally attach a virtual SVFS disk image (vm_attach_disk()).
//   6. Main loop, once per raylib frame:
//        - handle host shortcuts (zoom/pause/reboot/fps),
//        - unless paused, run up to INSTRUCTIONS_PER_FRAME VM instructions
//          (vm.step()), stopping early if the VM itself requests a vsync
//          yield (OP_VSYNC, see vm.c) so a busy program can't starve the
//          host's frame pacing,
//        - flush the runtime's pixel/terminal buffers to GPU textures and
//          draw them to the screen.
//   7. On exit, optionally dump VM RAM to a file, then tear everything down.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <errno.h>

#include "raylib.h"
#include "codepoints.h"
#include "vm.h"
#include "runtime_setup.h"
#include "font_data.h"

#define FONT_SIZE 16
#define INSTRUCTIONS_PER_FRAME 200000   // Upper bound on VM instructions executed per host frame;
                                        // acts as a safety valve against runaway/infinite VM loops
                                        // that never yield via OP_VSYNC, keeping the host window
                                        // responsive.

static void show_help(const char *program) {
    printf("Usage: %s <program> [args...]\n\n", program);
    printf("Arguments:\n");
    printf("    <program>                 Path to program\n\n");
    printf("Optional arguments:\n");
    printf("    --ram <bytes>           RAM Size (default: 32768 Bytes)\n");
    printf("    --videomode <1|2|3>     Video mode (default: 1)\n");
    printf("    --scale <1.0-6.0>       Scale (default: 1.0)\n");
    printf("    --fps <60|120|240>      Set FPS speed (default: 120)\n");
    printf("    --cursor <0|1>          Show or hide mouse cursor (default: 1)\n");
    printf("    --dump <filename>       On exit, write the final contents of VM RAM to <filename>\n");
    printf("    --disk <path>           Attach an already-formatted SVFS virtual disk image.\n");
    printf("                            Not required - if omitted, no disk is attached and\n");
    printf("                            SVFS instructions simply do nothing. The image is\n");
    printf("                            NOT formatted by the VM (create it with the svfs CLI\n");
    printf("                            tool's 'format' command first).\n");
    printf("    --net <socket_path>     Attach the network card: connect to a HOST process\n");
    printf("                            (see qhost) already listening on the given UNIX\n");
    printf("                            socket path. Not required - if omitted, no network\n");
    printf("                            card is attached and OP_NET_* instructions simply\n");
    printf("                            report \"not attached\" instead of doing anything.\n");
    printf("    --help                  Show this help and exit\n\n");
    printf("Video modes:\n");
    printf("    1: 640x480    textmode 80x30 chars\n");
    printf("    2: 1024x768   textmode 128x48 chars\n");
    printf("    3: 1280x960   textmode 160x60 chars\n\n");
    printf("Keyboard shortcuts:\n");
    printf("    Ctrl+Alt+PgUp / PgDn / Wheel   Zoom In / Out\n");
    printf("    Ctrl+Alt+P                     Pause / Unpause\n");
    printf("    Ctrl+Alt+R                     Reboot\n");
    printf("    Ctrl+Alt+S                     Change cycle FPS (60/120/240)\n\n");
}

int main(int argc, char *argv[]) {
    SetTraceLogLevel(LOG_ERROR);
    printf("QUARTZ (The Computer) - Version 0.0.5a [%d]\n", RUNTIME_VERSION);
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    srand((unsigned int)time(NULL));

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) { show_help(argv[0]); return 0; }
    }
    if (argc < 2) {
        fprintf(stderr, "Error: required parameter missing <program>\n\n");
        show_help(argv[0]);
        return 1;
    }
    if (strncmp(argv[1], "--", 2) == 0) {
        fprintf(stderr, "Error: first parameter must be <program>\n\n");
        show_help(argv[0]);
        return 1;
    }
    const char *binary_name = argv[1];

    int video_mode = 1;
    float scale = 1.0f;
    int fps = 120;
    int cursor = 1;
    int ram_size = 1024*1024;
    const char *dump_path = NULL;
    const char *disk_path = NULL;
    const char *net_path = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--videomode") == 0 && i + 1 < argc) {
            video_mode = atoi(argv[++i]);
            int dummy_w, dummy_h, dummy_c, dummy_r;
            if (!runtime_video_mode_dims(video_mode, &dummy_w, &dummy_h, &dummy_c, &dummy_r)) {
                fprintf(stderr, "Error: --videomode must be 1, 2 or 3\n"); return 1;
            }
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = (float)atof(argv[++i]);
            if (scale < RT_MIN_SCALE || scale > RT_MAX_SCALE) {
                fprintf(stderr, "Error: --scale must be between %.1f and %.1f\n", RT_MIN_SCALE, RT_MAX_SCALE); return 1;
            }
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            fps = atoi(argv[++i]);
            if (fps != 60 && fps != 120 && fps != 240) {
                fprintf(stderr, "Error: --fps must be 60, 120 or 240\n"); return 1;
            }
        } else if (strcmp(argv[i], "--cursor") == 0 && i + 1 < argc) {
            cursor = atoi(argv[++i]);
            if (cursor != 0 && cursor != 1) {
                fprintf(stderr, "Error: --cursor must be 0 or 1\n"); return 1;
            }
        } else if (strcmp(argv[i], "--ram") == 0 && i + 1 < argc) {
            ram_size = atoi(argv[++i]);
            if (ram_size <= 0) {
                fprintf(stderr, "Error: --ram must be positive number\n"); return 1;
            }
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            dump_path = argv[++i];
        } else if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc) {
            disk_path = argv[++i];
        } else if (strcmp(argv[i], "--net") == 0 && i + 1 < argc) {
            net_path = argv[++i];
        } else {
            fprintf(stderr, "Unknown or incomplete option: %s\n\n", argv[i]);
            show_help(argv[0]);
            return 1;
        }
    }

    int screen_w, screen_h, term_cols, term_rows;
    runtime_video_mode_dims(video_mode, &screen_w, &screen_h, &term_cols, &term_rows);

    int window_w = (int)((float)screen_w * scale);
    int window_h = (int)((float)screen_h * scale);
    InitWindow(window_w, window_h, "Quartz");
    SetTargetFPS(fps);
    SetExitKey(0);
    InitAudioDevice();
    if (!cursor) HideCursor();

    Font font = LoadFontFromMemory(".ttf", PxPlus_IBM_VGA8_ttf, (int)PxPlus_IBM_VGA8_ttf_len,
                                    FONT_SIZE, (int*)codepoints, codepointCount);
    if (font.texture.id == 0) {
        fprintf(stderr, "Quartz: cannot load font\n");
        CloseWindow();
        return 1;
    }

    RunCtx *rt = runtime_create((uint16_t)screen_w, (uint16_t)screen_h, font,
                                 term_cols, term_rows, FONT_SIZE, scale, video_mode, fps, cursor);
    if (!rt) {
        fprintf(stderr, "Quartz: failed to initialize runtime\n");
        CloseWindow();
        return 1;
    }

    VM vm;
    vm_init(&vm);
    vm.create(&vm, ram_size, binary_name, rt);
    if (!vm.running) {
        fprintf(stderr, "Quartz: failed to start VM\n");
        vm.destroy(&vm);
        runtime_destroy(rt);
        CloseWindow();
        return 1;
    }

    // Attach the virtual disk (if one was given) BEFORE the main loop starts,
    // i.e. before the first vm.step(). This is optional: without --disk,
    // nothing is attached and the program keeps running normally - the
    // OP_SVFS_* instructions then simply become no-ops (see vm_do_destroy()/
    // vm_attach_disk() in vm.c). The image is assumed to already be
    // formatted; this call only loads it, it never formats it.
    if (disk_path) {
        if (!vm_attach_disk(&vm, disk_path)) {
            fprintf(stderr, "Quartz: failed to attach virtual disk, aborting\n");
            vm.destroy(&vm);
            runtime_destroy(rt);
            CloseWindow();
            return 1;
        }
    }

    // Attach the network card (if requested) BEFORE the main loop starts,
    // same as the disk above. This is optional: without --net, nothing is
    // attached and the program keeps running normally - the OP_NET_*
    // instructions then simply report "not attached" (see vm_attach_net()/
    // OP_NET_ATTACHED in vm.c). A HOST process (qhost, or src-net/host.c)
    // must already be listening on net_path before the VM connects.
    if (net_path) {
        if (!vm_attach_net(&vm, net_path)) {
            fprintf(stderr, "Quartz: failed to attach network card, aborting\n");
            vm.destroy(&vm);
            runtime_destroy(rt);
            CloseWindow();
            return 1;
        }
    }

    int paused = 0;
    while (!WindowShouldClose() && vm.running) {
        int ctrl_alt = (IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)) &&
                       (IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT));

        if (ctrl_alt && (IsKeyPressed(KEY_PAGE_UP) || GetMouseWheelMove() > 0)) {
            scale += .1f;
            if (scale > RT_MAX_SCALE) scale = RT_MAX_SCALE;
            int window_w = (int)((float)screen_w * scale);
            int window_h = (int)((float)screen_h * scale);
            SetWindowSize(window_w, window_h);
            runtime_set_scale(rt, scale);
        } else if (ctrl_alt && (IsKeyPressed(KEY_PAGE_DOWN) || GetMouseWheelMove() < 0)) {
            scale -= .1f;
            if (scale < RT_MIN_SCALE) scale = RT_MIN_SCALE;
            int window_w = (int)((float)screen_w * scale);
            int window_h = (int)((float)screen_h * scale);
            SetWindowSize(window_w, window_h);
            runtime_set_scale(rt, scale);
        } else if (ctrl_alt && IsKeyPressed(KEY_P)) {
            paused = !paused;
        } else if (ctrl_alt && IsKeyPressed(KEY_R)) {
            vm.reset(&vm, &vm.prog);
            vm.running = 1;
            runtime_clear_all(rt);
            paused = 0;
        } else if (ctrl_alt && IsKeyPressed(KEY_S)) {
            fps = (fps == 60) ? 120 : (fps == 120) ? 240 : 60;
            SetTargetFPS(fps);
            runtime_set_fps(rt, fps);
        }

        if (!paused && vm.running) {
            vm.vsync_yield = 0;
            for (int i = 0; i < INSTRUCTIONS_PER_FRAME && vm.running; i++) {
                vm.step(&vm);
                if (vm.vsync_yield) break;
            }
        }

        runtime_get_video(rt, &video_mode, &screen_w, &screen_h, &scale, &fps);
        runtime_flush(rt);

        char title[160];
        const char *state = paused ? "Paused" : "Running";
        snprintf(title, sizeof(title), "Quartz: %s - %s (res=%dx%d win=%dx%d scale=%.1f fps=%d)",
                 binary_name, state, screen_w, screen_h, GetScreenWidth(), GetScreenHeight(),
                 scale, GetFPS());
        SetWindowTitle(title);

        BeginDrawing();
        ClearBackground(BLACK);
        {
            RenderTexture2D term_tex = runtime_term_texture(rt);
            Rectangle tsrc = {0, 0, (float)term_tex.texture.width, -(float)term_tex.texture.height};
            Rectangle tdst = {0, 0, (float)term_tex.texture.width * scale, (float)term_tex.texture.height * scale};
            DrawTexturePro(term_tex.texture, tsrc, tdst, (Vector2){0, 0}, 0.0f, WHITE);

            Texture2D gfx_tex = runtime_gfx_texture(rt);
            DrawTextureEx(gfx_tex, (Vector2){0, 0}, 0.0f, scale, WHITE);
        }
        EndDrawing();
    }

    if (vm.running) {
        printf("Quartz: window closed by user, stopping VM\n");
        vm.running = 0;
    } else {
        printf("Quartz: VM halted, closing window\n");
    }

    if (dump_path) {
        FILE *df = fopen(dump_path, "wb");
        if (!df) {
            fprintf(stderr, "Quartz: cannot open dump file '%s': %s\n", dump_path, strerror(errno));
        } else {
            size_t written = fwrite(vm.ram, 1, (size_t)vm.ram_size, df);
            fclose(df);
            if (written != (size_t)vm.ram_size) {
                fprintf(stderr, "Quartz: dump write incomplete (%zu/%d bytes) for '%s'\n",
                        written, vm.ram_size, dump_path);
            } else {
                printf("Quartz: dumped %d bytes of RAM to '%s'\n", vm.ram_size, dump_path);
            }
        }
    }

    vm.destroy(&vm);
    runtime_destroy(rt);
    CloseAudioDevice();
    CloseWindow();
    printf("Quartz: shut down cleanly\n");
    return 0;
}
