#ifndef SVFS_INTERNAL_H
#define SVFS_INTERNAL_H

// Internal, private implementation details of SVFS (Simple Virtual FAT FS). This
// header is included only by svfs.c - anything a consumer of the library needs
// is in the public API header, svfs.h, instead. Keeping this separate means the
// on-disk layout (block sizes, struct packing, FAT encoding, ...) can change
// freely without breaking code built against svfs.h.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

// ================= 1. Basic constants =================

#define BLOCK_SIZE      1024u             // Physical block size: 1 KB (fixed)

// The overall disk size is no longer a compile-time constant: svfs_format()
// takes it as a parameter (see MIN_DISK_SIZE/MAX_DISK_SIZE below for the
// allowed range), and the actual value in use for a mounted volume always
// lives in super_block->disk_size / super_block->total_blocks. Nothing in
// svfs.c should refer to a fixed "the disk size" outside of format-time
// validation - it always asks the mounted superblock instead.

// Smallest disk svfs_format() will create. Derived from the absolute minimum
// number of blocks (4) needed to fit: 1 superblock + 1 FAT block (which, at
// whole-block granularity, is the smallest a FAT can ever be, even though a
// disk this size only needs a handful of FAT bytes) + 1 root directory block
// + >=1 free data block to actually store anything in. Below this, svfs_format()
// physically cannot lay out a working filesystem.
#define MIN_DISK_SIZE   (4u * BLOCK_SIZE)               // 4 KiB

// Largest disk svfs_format() will create. The whole disk is malloc()'d in one
// go, so this is chosen to comfortably clear the "at least 4 GiB" target while
// staying a sane, deliberate upper bound (nothing about the on-disk format
// itself caps it much lower than the block-index width allows - see FatItem).
#define MAX_DISK_SIZE   (16ull * 1024 * 1024 * 1024)    // 16 GiB

// File attribute constants
#define FILE_ATTR_FILE  0x00              // Regular file
#define FILE_ATTR_DIR   0x01              // Directory
#define FILE_ATTR_HIDE  0x02              // Hidden file

// System state
#define OPEN_FILE_MAX   10                // Maximum number of files that can be open at once
#define FAT_END         0xFFFFFFFFu       // FAT chain end marker
#define FAT_FREE        0x00000000u       // Free block marker
#define FAT_BAD         0xFFFFFFF7u       // Bad / reserved system block marker

// On-disk layout (design)
// Block 0: SuperBlock - basic filesystem information
// Blocks 1..fat_size: FAT table
// Block after that: Root directory
// Remaining blocks: data area

// ================= 2. Data structure definitions =================

// On-disk format identifiers. Bumped whenever the layout of DirEntry / SuperBlock
// changes in a way that is not binary-compatible with previously saved disk images.
// svfs_load() checks these against a saved disk file and refuses to load a
// mismatched/old image instead of silently misinterpreting its bytes.
#define FS_MAGIC        0x53465653u   // "SVFS" as a little-endian 32-bit value
#define FS_VERSION      5u            // Bump this on every incompatible format change

// Maximum length of a file/directory name and extension. Widened from the
// original strict 8.3 convention (8+3) so names have real breathing room;
// still fixed-width on disk (simplicity), just a bigger fixed width.
#define MAX_NAME_LEN    32
#define MAX_EXT_LEN     8


// Directory entry structure (Directory Entry)
// Equivalent to a File Control Block (FCB)
typedef struct {
    char     filename[MAX_NAME_LEN]; // File name (not NUL-terminated if it fills the field)
    char     ext[MAX_EXT_LEN];       // Extension (not NUL-terminated if it fills the field)
    uint8_t  attribute;              // Attribute (0x00: file, 0x01: directory)
    uint8_t  protection;             // Protection bits (simple read/write protection)
    uint32_t first_block;            // Starting physical block number (index into the FAT)
    uint32_t file_size;              // File size (in bytes)
    uint32_t ts_created;             // Creation time (Unix timestamp, seconds since epoch)
    uint32_t ts_modified;            // Last modification time (Unix timestamp)
} DirEntry;

// Maximum number of directory entries that fit in one block.
// IMPORTANT: this is computed from the actual (aligned) size of DirEntry, not from an
// assumed byte count - struct padding around the uint32_t fields means sizeof(DirEntry)
// can be larger than the sum of its members, so hardcoding a fixed entry count would
// cause reads/writes past the end of the block (corrupting the next block in memory).
#define DIR_ENTRY_MAX   ((int)(BLOCK_SIZE / sizeof(DirEntry)))


// File Allocation Table entry (FAT Entry)
// Each entry is 4 bytes (uint32_t). Widened from the original 2 bytes (which
// could only address 65536 blocks - about 64 MiB of disk at a 1 KiB block
// size, nowhere near enough to support multi-gigabyte disks) so a block
// index can go well beyond what's needed for MAX_DISK_SIZE.
typedef uint32_t FatItem;


// SuperBlock
// Stored in block 0 of the virtual disk, describes the filesystem as a whole
typedef struct {
    uint32_t magic;          // Format identifier, see FS_MAGIC
    uint32_t version;        // Format version, see FS_VERSION
    uint64_t disk_size;      // Total disk size in bytes (needs 64 bits: a 4 GiB
                              // disk is one byte past what a 32-bit count can hold)
    uint32_t block_size;     // Block size
    uint32_t total_blocks;   // Total number of blocks
    uint32_t fat_start;      // Starting block of the FAT table
    uint32_t fat_size;       // Number of blocks used by the FAT
    uint32_t data_start;     // Starting block of the data area
    uint32_t root_dir_block; // Block that holds the root directory
    uint32_t free_blocks;    // Current number of free blocks
    // Persisted "current working directory", so that `svfs <disk> cd docs` run
    // as one process has an effect that `svfs <disk> ls` run as the next
    // process can see. Every CLI process is one-shot (load -> do one thing ->
    // save -> shut down), so anything that should survive between invocations
    // has to live on disk rather than in a process-local global.
    uint32_t current_dir;
} SuperBlock;


// Open File Table entry
// Tracks the state of currently open files within a single process run.
// Deliberately NOT persisted to disk: an open file descriptor is process-scoped
// state in a real OS too (nothing expects `open()` to survive a reboot), so it
// would be wrong to pretend a file opened in one process is still "open" by
// the time the next, separate process starts.
typedef struct {
    DirEntry *dir_entry;    // Pointer to the directory entry (in-memory copy)
    uint32_t current_pos;   // Current read/write pointer position within the file
    uint8_t  is_write;      // Open mode (0: read, 1: write)
    int8_t   fd;            // File descriptor (index)
} OpenFileItem;

// ================= 3. Helper macros =================

// Get the starting address of a block in memory (relative to a given disk base pointer)
#define GET_BLOCK_ADDR(block_num) (virtual_disk + (uint64_t)(block_num) * BLOCK_SIZE)

#endif
