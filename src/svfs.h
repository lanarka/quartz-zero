#ifndef SVFS_H
#define SVFS_H

#include <stddef.h>
#include <stdint.h>

// ============================================================================
// SVFS - Simple Virtual FAT FS
//
// A small in-memory FAT-style virtual filesystem, backed by a single flat
// "disk image" file on the host.
//
// This header is the ONLY thing another project needs to include to embed
// SVFS: it does not expose any on-disk structures (those live in the private
// svfs_internal.h, used only by svfs.c), just a plain function-call API.
//
// A typical host program looks like:
//
//     if (!svfs_load("virtual_disk.dat")) {
//         svfs_format(64ull * 1024 * 1024);   // 64 MiB
//     }
//     svfs_create("hello.txt");
//     svfs_write("hello.txt", "Hi there");
//     svfs_save("virtual_disk.dat");
//     svfs_shutdown();
//
// The reference command-line tool (main.c) follows exactly this pattern: it
// is a one-shot process that loads the disk image, performs a single
// requested operation, saves (only if the operation actually changed
// anything), and shuts down.
//
// The filesystem holds exactly one mounted volume at a time (there is no
// "handle" object to pass around) - this matches how the reference CLI uses
// it, and keeps the API small. If you need multiple independent volumes in
// the same process, svfs.c would need to be extended to carry its state in
// an explicit struct instead of static globals.
// ============================================================================

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

// Loads a previously saved virtual disk from disk_file into memory.
// Returns 1 on success, 0 if the file doesn't exist, has the wrong size, or
// was written by an incompatible version of SVFS (in all of these cases no
// filesystem is mounted yet - call svfs_format() to create a fresh one).
int svfs_load(const char *disk_file);

// The smallest / largest disk size, in bytes, that svfs_format() will accept.
// Exposed so callers (like the CLI's size-argument validation) don't have to
// duplicate these bounds - they always match what svfs_format() itself will
// actually enforce.
uint64_t svfs_min_disk_size(void);
uint64_t svfs_max_disk_size(void);

// Formats a brand new, empty virtual disk of exactly `disk_size_bytes` bytes
// in memory (discarding whatever was mounted before, if anything). Must be
// called before any other operation if svfs_load() did not succeed.
// disk_size_bytes must be within [svfs_min_disk_size(), svfs_max_disk_size()];
// out-of-range values are rejected (an error is printed to stderr and nothing
// is mounted - any previously mounted volume, if any, is left untouched).
void svfs_format(uint64_t disk_size_bytes);

// Saves the current in-memory virtual disk to disk_file (a single flat binary
// file that a later svfs_load() call can read back). This also persists the
// current directory (see svfs_cd) so the next process to svfs_load() this
// file resumes in the same directory.
void svfs_save(const char *disk_file);

// Frees the in-memory virtual disk. Safe to call even if nothing is mounted.
// After this call, svfs_load() or svfs_format() must be called again before
// using any other function.
void svfs_shutdown(void);

// ---------------------------------------------------------------------------
// Navigation & listing
// ---------------------------------------------------------------------------

// Prints an aligned table of the current directory's contents to stdout.
void svfs_ls(void);

// Fixed max length (+1 for NUL) of the `name`/`ext` fields below. Mirrors
// the on-disk limits (see svfs_internal.h's MAX_NAME_LEN/MAX_EXT_LEN) -
// duplicated here, rather than pulled in from the private header, to keep
// this header self-contained and free of on-disk-layout details.
#define SVFS_DIR_NAME_LEN 33  // 32 chars + NUL
#define SVFS_DIR_EXT_LEN  9   // 8 chars + NUL

// One directory entry, in a small, stable, public shape - the
// machine-readable counterpart to what svfs_ls() prints as a table row.
// Deliberately NOT the same layout as the on-disk DirEntry (that's a
// private implementation detail, see svfs_internal.h): a caller that needs
// to *use* a listing programmatically (rather than just print it) gets
// this instead.
typedef struct {
    char     name[SVFS_DIR_NAME_LEN]; // NUL-terminated name, WITHOUT extension
    char     ext[SVFS_DIR_EXT_LEN];   // NUL-terminated extension, "" if none
    uint8_t  is_dir;                  // 1 = directory, 0 = file
    uint8_t  is_hidden;               // 1 = hidden attribute set
    uint32_t size;                    // file size in bytes (always 0 for directories)
    uint32_t ts_created;              // Unix timestamp
    uint32_t ts_modified;             // Unix timestamp
} SvfsDirEntry;

// Lists the current directory into caller-supplied storage: writes up to
// max_entries SvfsDirEntry records into out[0..return value-1], in no
// particular order, and returns how many were actually written. Returns
// -1 if no filesystem is mounted (out is left untouched). Entries beyond
// max_entries are simply not returned (not an error) - a directory can
// only ever hold a small, fixed number of entries (they live in a single
// block, see svfs_internal.h), so a modestly sized `out` array is always
// enough to see everything in practice.
//
// This is svfs_ls()'s machine-readable counterpart: svfs_ls() formats a
// table straight to stdout for a human/CLI to read, while svfs_dir() hands
// the same information back as plain data for a caller - such as a VM
// instruction that needs to copy it into its own memory - to use however
// it needs to.
int svfs_dir(SvfsDirEntry *out, int max_entries);

// Changes the current directory. Accepts a plain name, ".", ".." or "/".
// Returns 1 on success, 0 on error (prints why to stderr) - the current
// directory is left unchanged on failure. Persisted across process runs by
// svfs_save() - see the note there.
int svfs_cd(const char *name);

// ---------------------------------------------------------------------------
// Directories
// ---------------------------------------------------------------------------

// Creates a subdirectory named `name` inside the current directory.
void svfs_mkdir(const char *name);

// Removes the subdirectory named `name` from the current directory.
// If `recursive` is 0 (the default from the CLI's plain `rmdir`), the
// directory must already be empty (only "." and ".." in it) - if it still
// has files or subdirectories in it, nothing is deleted and an error is
// printed. If `recursive` is non-zero (the CLI's `rmdir -r`), its entire
// contents (files and subdirectories, recursively) are deleted first, and
// then the now-empty directory itself is removed.
void svfs_rmdir(const char *name, int recursive);

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// Creates an empty file named `name` in the current directory.
void svfs_create(const char *name);

// Deletes the file named `name` from the current directory.
void svfs_rm(const char *name);

// Opens the file named `name`, tracking it in the (process-local, not
// persisted - see svfs_internal.h) open file table.
void svfs_open(const char *name);

// Closes a file previously opened with svfs_open() in this same process.
void svfs_close(const char *name);

// --- Text convenience wrappers (NUL-terminated strings) ---

// Overwrites the entire content of an existing file with `content`, a
// NUL-terminated string. The file must already exist (see svfs_create).
// Equivalent to svfs_write_bytes(name, content, strlen(content)) - so any
// embedded '\0' byte in binary data would truncate the write. Use
// svfs_write_bytes() directly for content that isn't plain text.
void svfs_write(const char *name, const char *content);

// Writes a file's entire content to stdout, exactly as stored (no added
// newline or other framing - this is what makes it binary-safe: reading a
// binary file and redirecting stdout to a host file reproduces it exactly).
// Status/error messages ("no such file", etc.) go to stderr, never stdout.
void svfs_read(const char *name);

// --- Binary-safe variants (explicit length, no NUL-termination assumed) ---

// Overwrites the entire content of an existing file with exactly `len` bytes
// from `data`, regardless of what those bytes are (including embedded zero
// bytes). The file must already exist (see svfs_create). Returns 1 on full
// success, 0 if the disk ran out of space partway through (file_size then
// reflects how much was actually written).
int svfs_write_bytes(const char *name, const void *data, size_t len);

// Reads up to `buffer_size` bytes of a file's content into `buffer`.
// On success, returns 1 and sets *out_len to the number of bytes copied
// (min(file_size, buffer_size) - if the file is larger than the buffer, the
// content is truncated and *out_len == buffer_size). Returns 0 if the file
// doesn't exist, is a directory, or its FAT chain is broken; *out_len is
// left untouched in that case. Pass NULL for out_len if you don't need it.
int svfs_read_bytes(const char *name, void *buffer, size_t buffer_size, size_t *out_len);

// ---------------------------------------------------------------------------
// Host filesystem interop
// ---------------------------------------------------------------------------

// Copies a file from the host filesystem (host_path) into the current
// directory of the virtual disk as `vfs_name`, creating it if it doesn't
// already exist there, or overwriting it if it does. Always binary-safe.
void svfs_import(const char *host_path, const char *vfs_name);

// Copies a file named `vfs_name` from the current directory of the virtual
// disk out to the host filesystem at host_path. Always binary-safe.
void svfs_export(const char *vfs_name, const char *host_path);

// ---------------------------------------------------------------------------
// Maintenance
// ---------------------------------------------------------------------------

// Walks the whole filesystem and checks it for internal consistency (bad
// block references, cross-linked/looping FAT chains, orphaned blocks, and a
// free-block count that doesn't match the superblock's bookkeeping). Prints
// a human-readable report to stdout.
void svfs_fsck(void);

#endif
