#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "svfs.h"

// ============================================================================
// SVFS command-line tool.
//
// There is no interactive shell and no default disk file: every invocation
// names the disk image explicitly as its first argument and does exactly one
// thing, then exits. The pattern is always the same:
//
//     load the named disk image (this only happens for commands other than
//     `format` - `format` itself always creates a brand new image of the
//     exact size given, and never loads an existing one)
//       -> perform exactly the one operation the command line asked for
//       -> save the disk image back to the same file, but ONLY if that
//          operation actually changed something (mkdir/rmdir/import) - a
//          read-only command like `ls`, `fsck` or `export` doesn't rewrite
//          the file it just looked at
//       -> shut down (free everything)
//
// This CLI intentionally only exposes the filesystem's structural/admin
// operations (format, ls, mkdir, rmdir, rm, import, export, fsck) - not the
// lower-level svfs_read/svfs_write/svfs_create/svfs_open/svfs_close, which
// remain part of the public svfs.h API for programs that embed SVFS as a
// library, just not as separate CLI subcommands here.
//
// There's no interactive `cd` either, but names given to mkdir/rmdir/rm/
// import/export/ls CAN contain '/' to reach into subdirectories (e.g.
// "docs/notes/todo.txt") - internally this walks in with svfs_cd() one
// segment at a time, starting fresh from the root every time, and is reset
// back to the root before saving so the persisted "current directory" always
// stays root for the next invocation (see SuperBlock.current_dir).
// ============================================================================

static void format_size(uint64_t bytes, char *out, size_t out_size);

static void print_usage(const char *prog)
{
    char min_buf[32], max_buf[32];
    format_size(svfs_min_disk_size(), min_buf, sizeof(min_buf));
    format_size(svfs_max_disk_size(), max_buf, sizeof(max_buf));

    fprintf(stderr,
        "Usage: %s <disk_file> <command> [args...]\n"
        "\n"
        "Paths (for mkdir/rmdir/import/export/ls) may contain '/' to reach into\n"
        "subdirectories, e.g. \"docs/notes/todo.txt\". There is no interactive `cd`.\n"
        "\n"
        "Commands:\n"
        "  format <size>          create a brand new, empty disk image of the given\n"
        "                         size, e.g. 1024K, 20M, 1G, or a plain byte count.\n"
        "                         Allowed range: %s - %s.\n"
        "  ls [path]              list a directory (the root, if path is omitted)\n"
        "  mkdir <path>           create a directory; its parent must already exist\n"
        "  rmdir [-r] <path>      remove a directory; refuses if it still has\n"
        "                         files/subdirectories in it unless -r is given,\n"
        "                         in which case its entire contents are removed too\n"
        "  rm <path>              remove a file\n"
        "  import <host_file> <path>   copy a file from the host machine in\n"
        "  export <path> <host_file>   copy a file out to the host machine\n"
        "  fsck                   check the whole disk image for consistency\n\n",
        prog, min_buf, max_buf);
}

// Formats a byte count as a short human-readable size (e.g. "64.00 MB").
static void format_size(uint64_t bytes, char *out, size_t out_size)
{
    if (bytes >= 1024ull * 1024 * 1024) {
        snprintf(out, out_size, "%.2f GB", bytes / 1024.0 / 1024.0 / 1024.0);
    } else if (bytes >= 1024ull * 1024) {
        snprintf(out, out_size, "%.2f MB", bytes / 1024.0 / 1024.0);
    } else if (bytes >= 1024ull) {
        snprintf(out, out_size, "%.2f KB", bytes / 1024.0);
    } else {
        snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
    }
}

// Parses a human-friendly size like "20M", "1G", "1024K", or a plain byte
// count like "4096", into a byte count. Returns 1 on success, 0 on a
// malformed string (no digits, garbage after the suffix, more than one
// suffix character, etc).
static int parse_size(const char *s, uint64_t *out_bytes)
{
    if (s == NULL || s[0] == '\0') return 0;

    char *end = NULL;
    unsigned long long value = strtoull(s, &end, 10);
    if (end == s) return 0; // no digits at all

    uint64_t multiplier = 1;
    if (*end != '\0') {
        if (end[1] != '\0') return 0; // more than one character after the digits
        switch (*end) {
            case 'k': case 'K': multiplier = 1024ull; break;
            case 'm': case 'M': multiplier = 1024ull * 1024; break;
            case 'g': case 'G': multiplier = 1024ull * 1024 * 1024; break;
            default: return 0;
        }
    }

    *out_bytes = (uint64_t)value * multiplier;
    return 1;
}

// Splits "docs/sub/file.txt" into dir_prefix="docs/sub" and *basename="file.txt".
// A path with no '/' (e.g. "file.txt") splits into dir_prefix="" and
// *basename="file.txt" - an empty dir_prefix means "the root", handled by
// navigate_to_dir_path() below. *basename always points into the original
// `path` string (no copy needed for it).
static void split_last_slash(const char *path, char *dir_prefix, size_t dir_prefix_cap,
                              const char **basename)
{
    const char *last_slash = strrchr(path, '/');
    if (last_slash == NULL) {
        if (dir_prefix_cap > 0) dir_prefix[0] = '\0';
        *basename = path;
        return;
    }

    size_t len = (size_t)(last_slash - path);
    if (len >= dir_prefix_cap) len = dir_prefix_cap - 1;
    memcpy(dir_prefix, path, len);
    dir_prefix[len] = '\0';
    *basename = last_slash + 1;
}

// Walks from the root through each '/'-separated segment of dir_path (which
// may be "" to mean "stay at the root"), using svfs_cd() one segment at a
// time. Returns 1 if the whole path was successfully walked (the volume's
// current directory is now the target directory), 0 if any segment doesn't
// exist or isn't a directory - svfs_cd() already printed why.
static int navigate_to_dir_path(const char *dir_path)
{
    if (!svfs_cd("/")) return 0; // always start from a known point: the root

    if (dir_path == NULL || dir_path[0] == '\0') return 1;

    char buf[4096];
    strncpy(buf, dir_path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *tok = strtok(buf, "/");
    while (tok != NULL) {
        if (!svfs_cd(tok)) return 0;
        tok = strtok(NULL, "/");
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0 ||
        strcmp(argv[1], "help") == 0) {
        print_usage(argv[0]);
        return (argc < 2) ? 1 : 0;
    }

    const char *disk_file = argv[1];

    if (argc < 3) {
        fprintf(stderr, "svfs: missing command\n\n");
        print_usage(argv[0]);
        return 1;
    }

    const char *cmd = argv[2];
    int argi = 3;

    // `format` is special: it doesn't load whatever is already at disk_file
    // (it's explicitly asking to replace it with a fresh volume), and it
    // always saves - there is nothing to conditionally skip.
    if (strcmp(cmd, "format") == 0) {
        if (argi >= argc) {
            char min_buf[32], max_buf[32];
            format_size(svfs_min_disk_size(), min_buf, sizeof(min_buf));
            format_size(svfs_max_disk_size(), max_buf, sizeof(max_buf));
            fprintf(stderr, "Usage: %s %s format <size>   (e.g. 1024K, 20M, 1G; "
                    "allowed range %s - %s)\n", argv[0], disk_file, min_buf, max_buf);
            return 1;
        }

        uint64_t size_bytes = 0;
        if (!parse_size(argv[argi], &size_bytes)) {
            fprintf(stderr, "svfs: invalid size '%s' (expected e.g. 1024K, 20M, 1G, "
                    "or a plain byte count)\n", argv[argi]);
            return 1;
        }

        svfs_format(size_bytes);
        svfs_save(disk_file);
        svfs_shutdown();
        return 0;
    }

    // Every other command operates on an existing volume - it must already
    // have been created with `format`.
    if (!svfs_load(disk_file)) {
        fprintf(stderr, "svfs: could not load '%s' - run '%s %s format <size>' first\n",
                disk_file, argv[0], disk_file);
        return 1;
    }

    int rc = 0;
    int mutated = 0; // only save if the command actually changes the volume

    if (strcmp(cmd, "ls") == 0) {
        const char *path = (argi < argc) ? argv[argi] : NULL;
        if (path == NULL || navigate_to_dir_path(path)) {
            svfs_ls();
        } else {
            rc = 1;
        }
    }
    else if (strcmp(cmd, "mkdir") == 0) {
        if (argi >= argc) {
            fprintf(stderr, "Usage: %s %s mkdir <path>\n", argv[0], disk_file);
            rc = 1;
        } else {
            char dir_prefix[4096];
            const char *basename;
            split_last_slash(argv[argi], dir_prefix, sizeof(dir_prefix), &basename);
            if (navigate_to_dir_path(dir_prefix)) {
                svfs_mkdir(basename);
                mutated = 1;
            } else {
                rc = 1;
            }
        }
    }
    else if (strcmp(cmd, "rmdir") == 0) {
        int recursive = 0;
        if (argi < argc && strcmp(argv[argi], "-r") == 0) { recursive = 1; argi++; }
        if (argi >= argc) {
            fprintf(stderr, "Usage: %s %s rmdir [-r] <path>\n", argv[0], disk_file);
            rc = 1;
        } else {
            char dir_prefix[4096];
            const char *basename;
            split_last_slash(argv[argi], dir_prefix, sizeof(dir_prefix), &basename);
            if (navigate_to_dir_path(dir_prefix)) {
                svfs_rmdir(basename, recursive);
                mutated = 1;
            } else {
                rc = 1;
            }
        }
    }
    else if (strcmp(cmd, "rm") == 0) {
        if (argi >= argc) {
            fprintf(stderr, "Usage: %s %s rm <path>\n", argv[0], disk_file);
            rc = 1;
        } else {
            char dir_prefix[4096];
            const char *basename;
            split_last_slash(argv[argi], dir_prefix, sizeof(dir_prefix), &basename);
            if (navigate_to_dir_path(dir_prefix)) {
                svfs_rm(basename);
                mutated = 1;
            } else {
                rc = 1;
            }
        }
    }
    else if (strcmp(cmd, "import") == 0) {
        if (argi + 1 >= argc) {
            fprintf(stderr, "Usage: %s %s import <host file> <destination path>\n",
                    argv[0], disk_file);
            rc = 1;
        } else {
            char dir_prefix[4096];
            const char *basename;
            split_last_slash(argv[argi + 1], dir_prefix, sizeof(dir_prefix), &basename);
            if (navigate_to_dir_path(dir_prefix)) {
                svfs_import(argv[argi], basename);
                mutated = 1;
            } else {
                rc = 1;
            }
        }
    }
    else if (strcmp(cmd, "export") == 0) {
        if (argi + 1 >= argc) {
            fprintf(stderr, "Usage: %s %s export <source path> <host file>\n",
                    argv[0], disk_file);
            rc = 1;
        } else {
            char dir_prefix[4096];
            const char *basename;
            split_last_slash(argv[argi], dir_prefix, sizeof(dir_prefix), &basename);
            if (navigate_to_dir_path(dir_prefix)) {
                svfs_export(basename, argv[argi + 1]);
            } else {
                rc = 1;
            }
        }
    }
    else if (strcmp(cmd, "fsck") == 0) {
        svfs_fsck();
    }
    else {
        fprintf(stderr, "Unknown command: %s\n\n", cmd);
        print_usage(argv[0]);
        rc = 1;
    }

    // However far a path argument navigated us in, always leave the volume's
    // persisted current directory at the root before saving - this CLI has
    // no interactive `cd`, so there's no reason a later, unrelated invocation
    // should implicitly start anywhere else.
    svfs_cd("/");

    if (mutated) {
        svfs_save(disk_file);
    }
    svfs_shutdown();
    return rc;
}
