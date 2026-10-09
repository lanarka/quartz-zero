#include "svfs.h"
#include "svfs_internal.h"

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

// ============================================================================
// This file implements the whole SVFS (Simple Virtual FAT FS) filesystem.
// It used to be split into
// one source file per command (my_create.c, my_open.c, my_write.c, ...);
// everything has been consolidated here since the individual files were
// mostly a handful of lines each and shared a lot of logic (filename
// parsing, directory-entry lookup, block allocation) that's easier to keep
// correct in one place. The public surface is svfs.h; everything below that
// is a private implementation detail.
// ============================================================================


// ============================================================================
// 1. Global state
// ============================================================================
// A single mounted volume's worth of state. Kept file-scope `static` (rather
// than the `extern` globals the previous multi-file version used) so nothing
// outside this file can reach into the filesystem's internals - the only way
// in is through the svfs_* functions declared in svfs.h.

static uint8_t      *virtual_disk = NULL;
static SuperBlock    *super_block = NULL;
static FatItem       *fat_table = NULL;
static OpenFileItem   open_file_table[OPEN_FILE_MAX];
static uint32_t       current_dir_block = 0;

// Backing storage for `virtual_disk`. Two modes:
//   - is_mmapped == 0: virtual_disk is a plain malloc()'d buffer with no file
//     tied to it yet (the state right after svfs_format()). Saving it means
//     writing the whole buffer out - unavoidable the first time, since the
//     file doesn't exist yet.
//   - is_mmapped == 1: virtual_disk is a MAP_SHARED mmap() of backing_fd,
//     mapped_size bytes long (the state after svfs_load(), or after the
//     first svfs_save() following a svfs_format() - see svfs_save()). Every
//     read/write to virtual_disk goes straight through to the OS's page
//     cache for that file; saving just means msync()'ing the mapping, which
//     only writes back pages that were actually touched (dirty) since the
//     last sync - not the whole file. This is what makes svfs_save() cheap
//     even on a large disk: a `mkdir` that only touches a handful of 1KB
//     blocks dirties only a handful of 4KB OS pages, regardless of whether
//     the disk image is 4KB or 4GB.
static int      is_mmapped = 0;
static int      backing_fd = -1;
static size_t   mapped_size = 0;

// Tears down whatever volume is currently mounted (malloc'd or mmap'd) and
// resets every piece of global state to "nothing mounted". Shared by
// svfs_format(), svfs_load() (before adopting a new mapping) and
// svfs_shutdown().
static void teardown_current_volume(void)
{
    if (is_mmapped) {
        if (virtual_disk != NULL && mapped_size > 0) {
            munmap(virtual_disk, mapped_size);
        }
        if (backing_fd >= 0) {
            close(backing_fd);
        }
    } else if (virtual_disk != NULL) {
        free(virtual_disk);
    }

    virtual_disk = NULL;
    super_block = NULL;
    fat_table = NULL;
    backing_fd = -1;
    mapped_size = 0;
    is_mmapped = 0;
}


// ============================================================================
// 2. Small shared helpers
// ============================================================================

// Copies up to `src_len` bytes of a fixed-width, space-padded on-disk field
// (like DirEntry.filename or DirEntry.ext) into a NUL-terminated buffer,
// trimming trailing spaces. Used anywhere an entry's name needs to be shown
// or compared as a normal C string (ls, fsck, rmdir, cd, ...).
static void trim_field(const char *src, size_t src_len, char *out, size_t out_size)
{
    size_t n = (src_len < out_size - 1) ? src_len : out_size - 1;
    memcpy(out, src, n);
    out[n] = '\0';
    while (n > 0 && out[n - 1] == ' ') {
        out[--n] = '\0';
    }
}

// Get the address of a directory entry by logical index.
// current_block: starting block of the directory "file" (from its DirEntry)
// logic_index: logical index of the directory entry (0, 1, 2...)
// Returns NULL once the index runs past the end of the directory's FAT chain.
static DirEntry* get_dir_entry_by_index(uint32_t first_block, int logic_index)
{
    int items_per_block = BLOCK_SIZE / sizeof(DirEntry);
    int block_offset = logic_index / items_per_block;
    int index_in_block = logic_index % items_per_block;

    uint32_t current_fat_block = first_block;

    if (block_offset != 0) {
        for (int i = 0; i < block_offset; i++) {
            uint32_t next_block = fat_table[current_fat_block];
            if (next_block == FAT_END || next_block == FAT_FREE) {
                return NULL;
            }
            current_fat_block = next_block;
        }
    }

    DirEntry *block_start_addr = (DirEntry*)GET_BLOCK_ADDR(current_fat_block);
    return block_start_addr + index_in_block;
}

// Splits "name.ext" (as typed by the caller) into fixed-size buffers. If there
// is no '.', ext is empty. Returns 1 if the name or extension was too long to
// fit and had to be truncated (i.e. the input as given is invalid and the
// caller should reject it with an error - NOT silently proceed with the
// truncated result, which would let e.g. two different too-long names collide
// on the same truncated on-disk entry). Returns 0 if everything fit cleanly.
static int fs_split_filename(const char *input, char *name, size_t name_size, char *ext, size_t ext_size)
{
    int truncated = 0;
    if (name_size > 0) name[0] = '\0';
    if (ext_size > 0) ext[0] = '\0';
    if (input == NULL) return 0;

    const char *dot = strrchr(input, '.');
    if (dot == NULL || dot == input) {
        size_t len = strlen(input);
        if (len >= name_size) { len = name_size - 1; truncated = 1; }
        memcpy(name, input, len);
        name[len] = '\0';
        return truncated;
    }

    size_t name_len = (size_t)(dot - input);
    if (name_len >= name_size) { name_len = name_size - 1; truncated = 1; }
    memcpy(name, input, name_len);
    name[name_len] = '\0';

    size_t ext_len = strlen(dot + 1);
    if (ext_len >= ext_size) { ext_len = ext_size - 1; truncated = 1; }
    memcpy(ext, dot + 1, ext_len);
    ext[ext_len] = '\0';

    return truncated;
}

static int is_valid_entry(const DirEntry *entry)
{
    return entry != NULL && entry->filename[0] != '\0';
}

static int entry_name_matches(const DirEntry *entry, const char *name, const char *ext)
{
    char entry_name[MAX_NAME_LEN + 1];
    char entry_ext[MAX_EXT_LEN + 1];
    trim_field(entry->filename, sizeof(entry->filename), entry_name, sizeof(entry_name));
    trim_field(entry->ext, sizeof(entry->ext), entry_ext, sizeof(entry_ext));
    return strcmp(entry_name, name) == 0 && strcmp(entry_ext, ext) == 0;
}

// Looks up an entry by (already-split) name/ext in the given directory block.
// Returns NULL if not found. Does not recurse into subdirectories.
static DirEntry* fs_find_entry(uint32_t dir_block, const char *name, const char *ext)
{
    for (int i = 0; i < DIR_ENTRY_MAX * 4; i++) {
        DirEntry *entry = get_dir_entry_by_index(dir_block, i);
        if (entry == NULL) break; // ran past the end of the directory's block chain
        if (!is_valid_entry(entry)) continue;
        if (entry_name_matches(entry, name, ext)) return entry;
    }
    return NULL;
}

// Finds one free block in the FAT, marks it FAT_END (allocated, empty chain) and
// updates super_block->free_blocks. Returns the block number, or -1 if the disk is full.
static int fs_alloc_block(void)
{
    for (uint32_t i = 0; i < super_block->total_blocks; i++) {
        if (fat_table[i] == FAT_FREE) {
            fat_table[i] = FAT_END;
            super_block->free_blocks--;
            return (int)i;
        }
    }
    return -1;
}

// Creates a new directory entry named name.ext of the given attribute inside dir_block:
// allocates one data block for it, fills in the entry (timestamps set to "now",
// file_size = 0), and returns a pointer to it. Returns NULL (printing an error) if the
// name already exists, the directory is full, or the disk is full.
static DirEntry* fs_create_entry(uint32_t dir_block, const char *name, const char *ext, uint8_t attribute)
{
    if (fs_find_entry(dir_block, name, ext) != NULL) {
        if (ext[0] != '\0') fprintf(stderr, "Error: '%s.%s' already exists\n", name, ext);
        else fprintf(stderr, "Error: '%s' already exists\n", name);
        return NULL;
    }

    uint8_t *dir_ptr = GET_BLOCK_ADDR(dir_block);
    int empty_offset = -1;
    for (int i = 0; i < DIR_ENTRY_MAX; i++) {
        DirEntry *slot = (DirEntry *)(dir_ptr + i * sizeof(DirEntry));
        if (slot->filename[0] == '\0') {
            empty_offset = i * (int)sizeof(DirEntry);
            break;
        }
    }
    if (empty_offset == -1) {
        fprintf(stderr, "Error: current directory is full, cannot create more entries\n");
        return NULL;
    }

    int new_block = fs_alloc_block();
    if (new_block == -1) {
        fprintf(stderr, "Error: disk is full, cannot create '%s'\n", name);
        return NULL;
    }

    DirEntry *entry = (DirEntry *)(dir_ptr + empty_offset);
    memset(entry, 0, sizeof(DirEntry));
    memcpy(entry->filename, name, strlen(name) > MAX_NAME_LEN ? MAX_NAME_LEN : strlen(name));
    memcpy(entry->ext, ext, strlen(ext) > MAX_EXT_LEN ? MAX_EXT_LEN : strlen(ext));
    entry->attribute = attribute;
    entry->protection = 0;
    entry->first_block = (uint32_t)new_block;
    entry->file_size = 0;

    time_t now = time(NULL);
    entry->ts_created = (uint32_t)now;
    entry->ts_modified = (uint32_t)now;

    return entry;
}

// (Re)writes the full content of an existing file entry with the given byte buffer.
// Grows/shrinks the block chain as needed, updates file_size and ts_modified.
// Returns 1 on full success, 0 if the disk ran out of space partway through.
static int fs_write_file_data(DirEntry *entry, const uint8_t *data, size_t len)
{
    if (entry->first_block == 0) {
        int new_blk = fs_alloc_block();
        if (new_blk == -1) {
            fprintf(stderr, "Error: no free space on disk\n");
            return 0;
        }
        entry->first_block = (uint32_t)new_blk;
    }

    const uint8_t *src = data;
    size_t remaining = len;
    uint32_t cur_block = entry->first_block;

    while (1) {
        size_t chunk = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : remaining;

        memset(GET_BLOCK_ADDR(cur_block), 0, BLOCK_SIZE);
        if (chunk > 0) memcpy(GET_BLOCK_ADDR(cur_block), src, chunk);

        src += chunk;
        remaining -= chunk;

        if (remaining == 0) {
            uint32_t next = fat_table[cur_block];
            fat_table[cur_block] = FAT_END;
            while (next != FAT_END && next != FAT_FREE && next != FAT_BAD) {
                uint32_t after = fat_table[next];
                fat_table[next] = FAT_FREE;
                super_block->free_blocks++;
                next = after;
            }
            break;
        }

        uint32_t next = fat_table[cur_block];
        if (next == FAT_END || next == FAT_FREE || next == FAT_BAD) {
            int new_blk = fs_alloc_block();
            if (new_blk == -1) {
                fat_table[cur_block] = FAT_END;
                entry->file_size = (uint32_t)(len - remaining);
                entry->ts_modified = (uint32_t)time(NULL);
                fprintf(stderr, "Error: disk is full, only wrote %u of %zu bytes\n", entry->file_size, len);
                return 0;
            }
            fat_table[cur_block] = (uint32_t)new_blk;
            cur_block = (uint32_t)new_blk;
        } else {
            cur_block = next;
        }
    }

    entry->file_size = (uint32_t)len;
    entry->ts_modified = (uint32_t)time(NULL);
    return 1;
}


// ============================================================================
// 3. Lifecycle: format / load / save / shutdown
// ============================================================================

uint64_t svfs_min_disk_size(void) { return MIN_DISK_SIZE; }
uint64_t svfs_max_disk_size(void) { return MAX_DISK_SIZE; }

void svfs_format(uint64_t disk_size_bytes)
{
    if (disk_size_bytes < MIN_DISK_SIZE || disk_size_bytes > MAX_DISK_SIZE) {
        fprintf(stderr,
                "Error: disk size must be between %llu and %llu bytes "
                "(got %llu bytes)\n",
                (unsigned long long)MIN_DISK_SIZE, (unsigned long long)MAX_DISK_SIZE,
                (unsigned long long)disk_size_bytes);
        return;
    }

    // Round down to a whole number of blocks - the on-disk layout only deals
    // in whole blocks, so a size that isn't already block-aligned would just
    // waste its last few bytes anyway.
    uint64_t total_blocks_u64 = disk_size_bytes / BLOCK_SIZE;
    uint64_t aligned_size = total_blocks_u64 * BLOCK_SIZE;
    if (aligned_size != disk_size_bytes) {
        fprintf(stderr, "Note: rounding disk size down to a whole number of %u-byte "
                "blocks (%llu -> %llu bytes)\n",
                BLOCK_SIZE, (unsigned long long)disk_size_bytes, (unsigned long long)aligned_size);
        disk_size_bytes = aligned_size;
    }

    fprintf(stderr, "Formatting virtual disk (%.2f MB)...\n", disk_size_bytes / 1024.0 / 1024.0);

    teardown_current_volume();

    virtual_disk = (uint8_t *)malloc((size_t)disk_size_bytes);
    if (virtual_disk == NULL) {
        fprintf(stderr, "Error: memory allocation failed!\n");
        return;
    }
    memset(virtual_disk, 0, (size_t)disk_size_bytes);

    // On-disk layout:
    // Block 0: SuperBlock
    // Block 1 .. (1+fat_blocks-1): FAT table
    // Block (1+fat_blocks): Root directory
    // Remaining blocks: Data area
    //
    // fat_blocks must be big enough to hold one FatItem (4 bytes) per block on
    // the disk. An earlier version of this code hardcoded both the disk size
    // and fat_blocks as compile-time constants, and got the FAT size wrong -
    // every fat_table[i] past the FAT's actual capacity silently aliased
    // directly onto the root directory and data area in memory, so writing
    // file content and writing FAT bookkeeping were corrupting each other.
    // Caught by running `fsck` on a freshly formatted disk. Computing it from
    // the real total_blocks here (rather than a fixed constant) also means
    // this same formula stays correct for any disk size the caller picks.
    uint32_t total_blocks = (uint32_t)total_blocks_u64;
    uint32_t fat_blocks = (uint32_t)(((uint64_t)total_blocks * sizeof(FatItem) + BLOCK_SIZE - 1) / BLOCK_SIZE);

    super_block = (SuperBlock *)GET_BLOCK_ADDR(0);
    super_block->magic = FS_MAGIC;
    super_block->version = FS_VERSION;
    super_block->disk_size = disk_size_bytes;
    super_block->block_size = BLOCK_SIZE;
    super_block->total_blocks = total_blocks;
    super_block->fat_start = 1;
    super_block->fat_size = fat_blocks;
    super_block->root_dir_block = 1 + fat_blocks;
    super_block->data_start = super_block->root_dir_block + 1;

    if (super_block->data_start >= total_blocks) {
        // Shouldn't be reachable given MIN_DISK_SIZE, but guard against it
        // directly rather than leaving a filesystem with zero free space.
        fprintf(stderr, "Error: disk is too small to hold a filesystem after overhead "
                "(needs at least %u blocks, have %u)\n", super_block->data_start + 1, total_blocks);
        free(virtual_disk);
        virtual_disk = NULL;
        super_block = NULL;
        return;
    }

    fat_table = (FatItem *)GET_BLOCK_ADDR(super_block->fat_start);

    for (uint32_t i = 0; i < super_block->total_blocks; i++) {
        fat_table[i] = FAT_FREE;
    }
    // Reserve every block up to (but not including) the data area.
    for (uint32_t b = 0; b < super_block->data_start; b++) {
        fat_table[b] = FAT_BAD;
    }
    // ... except the root directory's own block, a real single-block "file".
    fat_table[super_block->root_dir_block] = FAT_END;

    super_block->free_blocks = total_blocks - super_block->data_start;

    // Initialize the root directory: "." and ".." both point at itself.
    DirEntry *root_dir = (DirEntry *)GET_BLOCK_ADDR(super_block->root_dir_block);
    time_t now = time(NULL);

    // Note: filename[]/ext[] are fixed-width fields, not NUL-terminated C
    // strings, so we memcpy exact byte counts (a strcpy here would also write
    // a terminating NUL byte past the intended content - harmless once the
    // rest of the field is already zeroed by the memset above, but let's not
    // rely on that).
    memcpy(root_dir[0].filename, ".", 1);
    root_dir[0].attribute = FILE_ATTR_DIR;
    root_dir[0].first_block = super_block->root_dir_block;
    root_dir[0].file_size = 0;
    root_dir[0].ts_created = (uint32_t)now;
    root_dir[0].ts_modified = (uint32_t)now;

    memcpy(root_dir[1].filename, "..", 2);
    root_dir[1].attribute = FILE_ATTR_DIR;
    root_dir[1].first_block = super_block->root_dir_block;
    root_dir[1].file_size = 0;
    root_dir[1].ts_created = (uint32_t)now;
    root_dir[1].ts_modified = (uint32_t)now;

    current_dir_block = super_block->root_dir_block;
    super_block->current_dir = super_block->root_dir_block;
    memset(open_file_table, 0, sizeof(open_file_table));

    fprintf(stderr, "Formatting complete!\n");
    fprintf(stderr, "Total space: %.2f MB\n", disk_size_bytes / 1024.0 / 1024.0);
    fprintf(stderr, "Block size: %u B\n", BLOCK_SIZE);
    fprintf(stderr, "Total blocks: %u\n", total_blocks);
    fprintf(stderr, "Root directory starting block: %u\n", super_block->root_dir_block);
}

void svfs_save(const char *filename)
{
    if (filename == NULL || strlen(filename) == 0) {
        fprintf(stderr, "Error: please specify a file name to save to\n");
        return;
    }
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "Error: filesystem is not initialized, cannot save\n");
        return;
    }

    // Persist the current directory so the next process to load this file
    // resumes wherever `cd` last left it (see the note on SuperBlock.current_dir).
    super_block->current_dir = current_dir_block;

    if (is_mmapped) {
        // Fast path (the common case: after svfs_load(), or after the first
        // svfs_save() below has already switched us into mmap mode). Only
        // the OS pages that were actually written to since the last sync get
        // flushed - a small operation like `mkdir` on a huge disk touches a
        // handful of blocks, not the whole file, so this is cheap regardless
        // of the disk's total size.
        if (msync(virtual_disk, mapped_size, MS_SYNC) != 0) {
            fprintf(stderr, "Error: failed to sync '%s' to disk: %s\n", filename, strerror(errno));
            return;
        }
        fprintf(stderr, "Filesystem saved successfully!\n");
        fprintf(stderr, "Location: %s\n", filename);
        fprintf(stderr, "Total size: %.2f MB\n", super_block->disk_size / 1024.0 / 1024.0);
        return;
    }

    // Slow path: virtual_disk is still a plain malloc()'d buffer (the state
    // right after svfs_format(), before this volume has ever been saved) -
    // the file doesn't exist yet, so there's no way around writing the whole
    // thing out at least this once. Immediately afterwards, re-open what was
    // just written as a memory-mapped file and adopt that as the new backing
    // store, so every *subsequent* svfs_save() call (in this same process,
    // or via svfs_load() in a later one) takes the fast msync() path instead.
    fprintf(stderr, "Saving filesystem to %s...\n", filename);

    FILE *file = fopen(filename, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: could not open %s for writing\n", filename);
        return;
    }

    size_t written = fwrite(virtual_disk, 1, (size_t)super_block->disk_size, file);
    if (written != (size_t)super_block->disk_size) {
        fprintf(stderr, "Error: incomplete write, only wrote %zu/%llu bytes\n",
                written, (unsigned long long)super_block->disk_size);
        fclose(file);
        return;
    }

    // Force this write out to the underlying storage now, while we already
    // know we're paying a "whole disk" cost anyway (there's no way around it
    // for a file that doesn't exist yet). Without this, fwrite()/fclose()
    // leave the data as *dirty* pages in the OS page cache - the upcoming
    // mmap() below would just reuse those same dirty pages, silently
    // deferring the full write-back cost onto whatever operation happens to
    // call msync() next (in this process or a later one), instead of paying
    // it here where the user is already expecting `format` to take a moment.
    if (fsync(fileno(file)) != 0) {
        fprintf(stderr, "Warning: fsync failed for '%s': %s\n", filename, strerror(errno));
    }
    fclose(file);

    uint64_t saved_disk_size = super_block->disk_size; // capture before freeing the old buffer below

    int fd = open(filename, O_RDWR);
    if (fd >= 0) {
        void *mapped = mmap(NULL, (size_t)saved_disk_size, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, 0);
        if (mapped != MAP_FAILED) {
            free(virtual_disk);
            virtual_disk = (uint8_t *)mapped;
            mapped_size = (size_t)saved_disk_size;
            backing_fd = fd;
            is_mmapped = 1;
            // GET_BLOCK_ADDR is relative to virtual_disk, which just moved -
            // every pointer into the old buffer must be recomputed. This is
            // now safe: virtual_disk points at the new mmap'd copy of the
            // exact same bytes we just wrote, so super_block/fat_table end
            // up pointing at equivalent (valid) data, just relocated.
            super_block = (SuperBlock *)GET_BLOCK_ADDR(0);
            fat_table = (FatItem *)GET_BLOCK_ADDR(super_block->fat_start);
        } else {
            // Not fatal - we already wrote the file successfully above, we
            // just didn't manage to switch to the fast path. The next
            // svfs_save() in this process will simply retry the full write.
            close(fd);
        }
    }

    fprintf(stderr, "Filesystem saved successfully!\n");
    fprintf(stderr, "Location: %s\n", filename);
    fprintf(stderr, "Total size: %.2f MB\n", saved_disk_size / 1024.0 / 1024.0);
}


int svfs_load(const char *filename)
{
    if (filename == NULL || strlen(filename) == 0) {
        fprintf(stderr, "Error: please specify a file name to load\n");
        return 0;
    }

    fprintf(stderr, "Loading filesystem from %s...\n", filename);

    int fd = open(filename, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "Note: file %s does not exist, a new filesystem will be created\n", filename);
        return 0;
    }

    struct stat st;
    if (fstat(fd, &st) != 0) {
        fprintf(stderr, "Error: could not stat '%s': %s\n", filename, strerror(errno));
        close(fd);
        return 0;
    }
    off_t file_size = st.st_size;

    // The disk size is no longer a fixed constant (see svfs_format), so we
    // trust the actual file size here instead of comparing against one -
    // just sanity-check it's in the range svfs_format() could ever have
    // produced, and block-aligned.
    if (file_size < 0 || (uint64_t)file_size < MIN_DISK_SIZE || (uint64_t)file_size > MAX_DISK_SIZE) {
        fprintf(stderr, "Error: file size (%lld bytes) is outside the valid disk size range "
                "[%llu, %llu] bytes\n", (long long)file_size,
                (unsigned long long)MIN_DISK_SIZE, (unsigned long long)MAX_DISK_SIZE);
        close(fd);
        return 0;
    }
    if ((uint64_t)file_size % BLOCK_SIZE != 0) {
        fprintf(stderr, "Error: file size (%lld bytes) is not a whole number of %u-byte blocks\n",
                (long long)file_size, BLOCK_SIZE);
        close(fd);
        return 0;
    }

    // Memory-map the file directly rather than reading it all into a malloc'd
    // buffer: mapping is essentially instant regardless of file size (pages
    // are faulted in from disk lazily, only as they're actually touched), and
    // - more importantly for svfs_save() - it means writes made through
    // virtual_disk go straight to the OS page cache for this file, so saving
    // later is just an msync() of whichever pages actually changed.
    void *mapped = mmap(NULL, (size_t)file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "Error: could not memory-map '%s': %s\n", filename, strerror(errno));
        close(fd);
        return 0;
    }

    // Sanity-check the on-disk format before trusting it (see FS_MAGIC/FS_VERSION).
    SuperBlock *loaded_sb = (SuperBlock *)mapped;
    if (loaded_sb->magic != FS_MAGIC || loaded_sb->version != FS_VERSION) {
        fprintf(stderr, "Note: %s uses an incompatible or outdated filesystem format "
               "(magic=0x%08X version=%u, expected magic=0x%08X version=%u).\n",
               filename, loaded_sb->magic, loaded_sb->version, FS_MAGIC, FS_VERSION);
        fprintf(stderr, "A new filesystem will be created instead.\n");
        munmap(mapped, (size_t)file_size);
        close(fd);
        return 0;
    }
    if (loaded_sb->disk_size != (uint64_t)file_size) {
        fprintf(stderr, "Error: %s looks corrupted or truncated (superblock says %llu bytes, "
                "the file is %lld bytes)\n", filename,
                (unsigned long long)loaded_sb->disk_size, (long long)file_size);
        munmap(mapped, (size_t)file_size);
        close(fd);
        return 0;
    }

    // Everything checks out - release whatever volume was mounted before
    // (if any) and adopt this mapping.
    teardown_current_volume();

    virtual_disk = (uint8_t *)mapped;
    mapped_size = (size_t)file_size;
    backing_fd = fd;
    is_mmapped = 1;

    super_block = (SuperBlock *)GET_BLOCK_ADDR(0);
    fat_table = (FatItem *)GET_BLOCK_ADDR(super_block->fat_start);

    // Resume in the directory `cd` last left us in (persisted by svfs_save),
    // falling back to root if the stored value looks out of range - e.g. an
    // image saved by a build that predates SuperBlock.current_dir would read
    // this field as zero, which is never a valid directory block.
    uint32_t cd = super_block->current_dir;
    if (cd == super_block->root_dir_block ||
        (cd >= super_block->data_start && cd < super_block->total_blocks)) {
        current_dir_block = cd;
    } else {
        current_dir_block = super_block->root_dir_block;
    }

    memset(open_file_table, 0, sizeof(open_file_table));

    fprintf(stderr, "Filesystem loaded successfully!\n");
    fprintf(stderr, "Total size: %.2f MB\n", super_block->disk_size / 1024.0 / 1024.0);
    fprintf(stderr, "Block size: %u B\n", super_block->block_size);
    fprintf(stderr, "Free blocks: %u\n", super_block->free_blocks);

    return 1;
}

void svfs_shutdown(void)
{
    teardown_current_volume();
    current_dir_block = 0;
    memset(open_file_table, 0, sizeof(open_file_table));
}


// ============================================================================
// 4. Navigation & listing
// ============================================================================

static void format_ts(uint32_t ts, char *out, size_t out_size)
{
    if (ts == 0) {
        snprintf(out, out_size, "-");
        return;
    }
    time_t t = (time_t)ts;
    struct tm *tm_info = localtime(&t);
    if (tm_info == NULL) {
        snprintf(out, out_size, "-");
        return;
    }
    strftime(out, out_size, "%Y-%m-%d %H:%M", tm_info);
}

// Reconstructs the absolute path of the current directory (e.g. "/docs/sub",
// or "/" for the root itself), purely for a friendly status line - nothing
// else in SVFS needs or parses this back.
//
// A directory never stores its own name - only its PARENT's entry for it
// does - so the only way to recover it is to walk upward one level at a
// time via each directory's own ".." entry, and in each parent look up
// which entry's first_block points back down at the child we just came
// from. Repeating this until the root is reached, then printing the
// collected names root-first, rebuilds the full path.
static void build_current_path(char *out, size_t out_size)
{
    if (out_size == 0) return;
    out[0] = '\0';
    if (out_size < 2) return;

    char names[64][MAX_NAME_LEN + 1];
    int depth = 0;

    uint32_t block = current_dir_block;
    while (block != super_block->root_dir_block && depth < 64) {
        DirEntry *dotdot = get_dir_entry_by_index(block, 1); // ".." entry
        if (dotdot == NULL || !is_valid_entry(dotdot)) break;
        uint32_t parent_block = dotdot->first_block;

        // Find, in the parent, the entry whose first_block is `block` -
        // that entry's name is this directory's own name.
        const char *found_name = NULL;
        for (int i = 0; i < DIR_ENTRY_MAX * 4; i++) {
            DirEntry *e = get_dir_entry_by_index(parent_block, i);
            if (e == NULL) break;
            if (!is_valid_entry(e)) continue;
            if (e->attribute == FILE_ATTR_DIR && e->first_block == block) {
                trim_field(e->filename, sizeof(e->filename), names[depth], sizeof(names[depth]));
                found_name = names[depth];
                break;
            }
        }
        if (found_name == NULL) break; // shouldn't happen on a consistent filesystem
        depth++;
        block = parent_block;
    }

    // Print root-first: "/" then each collected name, deepest (current
    // directory) last - names[] was filled innermost-first, so walk it
    // back to front.
    size_t pos = 0;
    out[pos++] = '/';
    out[pos] = '\0';
    for (int i = depth - 1; i >= 0; i--) {
        size_t name_len = strlen(names[i]);
        int need_sep = (pos > 1);
        size_t need = name_len + (size_t)(need_sep ? 1 : 0);
        if (pos + need >= out_size) break; // would overflow - keep what we have so far
        if (need_sep) out[pos++] = '/';
        memcpy(out + pos, names[i], name_len);
        pos += name_len;
        out[pos] = '\0';
    }
}

void svfs_ls(void)
{
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "ls: filesystem is not initialized\n");
        return;
    }

    char path[512];
    build_current_path(path, sizeof(path));
    printf("Current directory: %s\n", path);

    uint8_t *curr = GET_BLOCK_ADDR(current_dir_block);

    // Column widths: NAME needs room for the widest possible "name.ext"
    // (MAX_NAME_LEN + '.' + MAX_EXT_LEN), plus a little breathing room.
    printf("%-44s%-8s%12s  %s\n", "NAME", "TYPE", "SIZE", "MODIFIED");
    printf("%-44s%-8s%12s  %s\n",
           "--------------------------------------------",
           "--------", "------------", "----------------");

    for (int i = 0; i < DIR_ENTRY_MAX; i++) {
        DirEntry *e = (DirEntry *)(curr + i * sizeof(DirEntry));
        if (e->filename[0] == '\0') continue;

        char name[MAX_NAME_LEN + 1], ext[MAX_EXT_LEN + 1];
        trim_field(e->filename, sizeof(e->filename), name, sizeof(name));
        trim_field(e->ext, sizeof(e->ext), ext, sizeof(ext));

        char display[MAX_NAME_LEN + 1 + MAX_EXT_LEN + 1];
        if (ext[0] != '\0') snprintf(display, sizeof(display), "%s.%s", name, ext);
        else snprintf(display, sizeof(display), "%s", name);

        const char *type_str;
        char size_str[16];
        if (e->attribute == FILE_ATTR_DIR) {
            type_str = "dir";
            snprintf(size_str, sizeof(size_str), "-");
        } else if (e->attribute == FILE_ATTR_HIDE) {
            type_str = "hidden";
            snprintf(size_str, sizeof(size_str), "%u", (unsigned)e->file_size);
        } else {
            type_str = "file";
            snprintf(size_str, sizeof(size_str), "%u", (unsigned)e->file_size);
        }

        char modified[20];
        format_ts(e->ts_modified, modified, sizeof(modified));

        printf("%-44s%-8s%12s  %s\n", display, type_str, size_str, modified);
    }
    printf("\n");
    fflush(stdout);
}

int svfs_dir(SvfsDirEntry *out, int max_entries)
{
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "dir: filesystem is not initialized\n");
        return -1;
    }
    if (out == NULL || max_entries <= 0) return 0;

    uint8_t *curr = GET_BLOCK_ADDR(current_dir_block);
    int count = 0;

    for (int i = 0; i < DIR_ENTRY_MAX && count < max_entries; i++) {
        DirEntry *e = (DirEntry *)(curr + i * sizeof(DirEntry));
        if (!is_valid_entry(e)) continue;

        SvfsDirEntry *dst = &out[count];
        trim_field(e->filename, sizeof(e->filename), dst->name, sizeof(dst->name));
        trim_field(e->ext, sizeof(e->ext), dst->ext, sizeof(dst->ext));
        dst->is_dir     = (e->attribute & FILE_ATTR_DIR)  ? 1 : 0;
        dst->is_hidden  = (e->attribute & FILE_ATTR_HIDE) ? 1 : 0;
        dst->size       = dst->is_dir ? 0 : e->file_size;
        dst->ts_created  = e->ts_created;
        dst->ts_modified = e->ts_modified;
        count++;
    }
    return count;
}

int svfs_cd(const char *filename)
{
    if (virtual_disk == NULL || fat_table == NULL || super_block == NULL) {
        fprintf(stderr, "cd: filesystem is not initialized\n");
        return 0;
    }
    if (filename == NULL || filename[0] == '\0') {
        fprintf(stderr, "cd: missing directory name\n");
        return 0;
    }

    if (strcmp(filename, ".") == 0) {
        return 1;
    }
    if (strcmp(filename, "/") == 0) {
        current_dir_block = super_block->root_dir_block;
        return 1;
    }
    if (strcmp(filename, "..") == 0) {
        if (current_dir_block == super_block->root_dir_block) return 1;

        for (int i = 0; i < DIR_ENTRY_MAX; ++i) {
            DirEntry *entry = get_dir_entry_by_index(current_dir_block, i);
            if (!is_valid_entry(entry)) continue;
            if ((entry->attribute & FILE_ATTR_DIR) && entry_name_matches(entry, "..", "")) {
                current_dir_block = entry->first_block;
                return 1;
            }
        }
        fprintf(stderr, "cd: parent directory entry not found\n");
        return 0;
    }

    char name[MAX_NAME_LEN + 1], ext[MAX_EXT_LEN + 1];
    if (fs_split_filename(filename, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "cd: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, filename);
        return 0;
    }

    DirEntry *entry = fs_find_entry(current_dir_block, name, ext);
    if (entry == NULL) {
        fprintf(stderr, "cd: no such directory: %s\n", filename);
        return 0;
    }
    if ((entry->attribute & FILE_ATTR_DIR) == 0) {
        fprintf(stderr, "cd: %s is not a directory\n", filename);
        return 0;
    }
    current_dir_block = entry->first_block;
    return 1;
}


// ============================================================================
// 5. Directories: mkdir / rmdir
// ============================================================================

void svfs_mkdir(const char *name)
{
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "mkdir: filesystem is not initialized\n");
        return;
    }
    if (name == NULL || strlen(name) > MAX_NAME_LEN) {
        fprintf(stderr, "Error: directory name cannot exceed %d characters\n", MAX_NAME_LEN);
        return;
    }

    DirEntry *new_dir_entry = fs_create_entry(current_dir_block, name, "", FILE_ATTR_DIR);
    if (new_dir_entry == NULL) return;

    uint32_t new_block_idx = new_dir_entry->first_block;
    uint8_t *new_dir_ptr = GET_BLOCK_ADDR(new_block_idx);
    memset(new_dir_ptr, 0, BLOCK_SIZE);

    time_t now = time(NULL);

    DirEntry *self_entry = (DirEntry *)new_dir_ptr;
    strcpy(self_entry->filename, ".");
    self_entry->attribute = FILE_ATTR_DIR;
    self_entry->first_block = (uint32_t)new_block_idx;
    self_entry->file_size = 0;
    self_entry->ts_created = (uint32_t)now;
    self_entry->ts_modified = (uint32_t)now;

    DirEntry *parent_entry = (DirEntry *)(new_dir_ptr + sizeof(DirEntry));
    strcpy(parent_entry->filename, "..");
    parent_entry->attribute = FILE_ATTR_DIR;
    parent_entry->first_block = (uint32_t)current_dir_block;
    parent_entry->file_size = 0;
    parent_entry->ts_created = (uint32_t)now;
    parent_entry->ts_modified = (uint32_t)now;

    fprintf(stderr, "Directory created successfully: %s\n", name);
}

static int rmdir_is_directory_empty(uint32_t dir_block)
{
    const int items_per_block = BLOCK_SIZE / sizeof(DirEntry);
    uint32_t block = dir_block;

    while (1) {
        DirEntry *entries = (DirEntry *)GET_BLOCK_ADDR(block);
        for (int i = 0; i < items_per_block; ++i) {
            if (!is_valid_entry(&entries[i])) continue;
            char name[MAX_NAME_LEN + 1];
            trim_field(entries[i].filename, sizeof(entries[i].filename), name, sizeof(name));
            if (strcmp(name, ".") != 0 && strcmp(name, "..") != 0) return 0;
        }
        uint32_t next = fat_table[block];
        if (next == FAT_END || next == FAT_FREE || next == FAT_BAD) break;
        block = next;
    }
    return 1;
}

static void rmdir_free_directory_blocks(uint32_t dir_block)
{
    uint32_t block = dir_block;
    while (1) {
        uint32_t next = fat_table[block];
        fat_table[block] = FAT_FREE;
        super_block->free_blocks++;
        if (next == FAT_END || next == FAT_FREE || next == FAT_BAD) break;
        block = next;
    }
}

static void rmdir_recursive_delete(uint32_t dir_block)
{
    const int items_per_block = BLOCK_SIZE / sizeof(DirEntry);
    uint32_t block = dir_block;

    while (1) {
        DirEntry *entries = (DirEntry *)GET_BLOCK_ADDR(block);
        for (int i = 0; i < items_per_block; ++i) {
            DirEntry *entry = &entries[i];
            if (!is_valid_entry(entry)) continue;

            char name[MAX_NAME_LEN + 1];
            trim_field(entry->filename, sizeof(entry->filename), name, sizeof(name));
            if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

            if (entry->attribute == FILE_ATTR_DIR) {
                rmdir_recursive_delete(entry->first_block);
            }
            rmdir_free_directory_blocks(entry->first_block);
            memset(entry, 0, sizeof(DirEntry));
        }
        uint32_t next = fat_table[block];
        if (next == FAT_END || next == FAT_FREE || next == FAT_BAD) break;
        block = next;
    }
}

void svfs_rmdir(const char *name, int recursive)
{
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "rmdir: filesystem is not initialized\n");
        return;
    }
    if (name == NULL || name[0] == '\0') {
        fprintf(stderr, "Error: please specify a directory name to delete\n");
        return;
    }
    if (strlen(name) > MAX_NAME_LEN) {
        fprintf(stderr, "Error: directory name cannot exceed %d characters\n", MAX_NAME_LEN);
        return;
    }

    const int items_per_block = BLOCK_SIZE / sizeof(DirEntry);
    uint32_t block = current_dir_block;

    uint32_t target_block = 0;
    uint32_t parent_block = 0;
    int entry_index = -1;

    while (1) {
        DirEntry *entries = (DirEntry *)GET_BLOCK_ADDR(block);
        for (int i = 0; i < items_per_block; ++i) {
            DirEntry *entry = &entries[i];
            if (!is_valid_entry(entry)) continue;

            char ename[MAX_NAME_LEN + 1];
            trim_field(entry->filename, sizeof(entry->filename), ename, sizeof(ename));
            if (strcmp(ename, name) == 0) {
                if (entry->attribute != FILE_ATTR_DIR) {
                    fprintf(stderr, "Error: '%s' is not a directory\n", name);
                    return;
                }
                target_block = entry->first_block;
                parent_block = block;
                entry_index = i;
                break;
            }
        }
        if (entry_index != -1) break;

        uint32_t next = fat_table[block];
        if (next == FAT_END || next == FAT_FREE || next == FAT_BAD) break;
        block = next;
    }

    if (entry_index == -1) {
        fprintf(stderr, "Error: directory '%s' does not exist\n", name);
        return;
    }

    if (!rmdir_is_directory_empty(target_block)) {
        if (!recursive) {
            fprintf(stderr, "Error: directory '%s' is not empty\n", name);
            return;
        }
        rmdir_recursive_delete(target_block);
    }

    rmdir_free_directory_blocks(target_block);

    DirEntry *entries = (DirEntry *)GET_BLOCK_ADDR(parent_block);
    memset(&entries[entry_index], 0, sizeof(DirEntry));

    fprintf(stderr, "Directory deleted successfully: %s\n", name);
}


// ============================================================================
// 6. Files: create / rm / open / close / write / read
// ============================================================================

void svfs_create(const char *filename)
{
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "create: filesystem is not initialized\n");
        return;
    }

    char name[MAX_NAME_LEN + 1] = {0};
    char ext[MAX_EXT_LEN + 1] = {0};
    if (fs_split_filename(filename, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "create: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, filename);
        return;
    }

    DirEntry *new_file_entry = fs_create_entry(current_dir_block, name, ext, FILE_ATTR_FILE);
    if (new_file_entry == NULL) return;

    fprintf(stderr, "File created successfully: %s\n", filename);
    fprintf(stderr, "  [debug] File name: %.*s\n", MAX_NAME_LEN, new_file_entry->filename);
    fprintf(stderr, "  [debug] Extension: %.*s\n", MAX_EXT_LEN, new_file_entry->ext);
    fprintf(stderr, "  [debug] Attribute: %s\n",
           new_file_entry->attribute == FILE_ATTR_FILE ? "file" : "directory");
    fprintf(stderr, "  [debug] Starting block: %u\n", new_file_entry->first_block);
    fprintf(stderr, "  [debug] File size: %u bytes\n", new_file_entry->file_size);
}

void svfs_rm(const char *arg)
{
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "rm: filesystem is not initialized\n");
        return;
    }
    if (arg == NULL || arg[0] == '\0') {
        fprintf(stderr, "Usage: rm <file name>\n");
        return;
    }

    char name[MAX_NAME_LEN + 1] = {0};
    char ext[MAX_EXT_LEN + 1] = {0};
    if (fs_split_filename(arg, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "rm: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, arg);
        return;
    }


    DirEntry *entry = fs_find_entry(current_dir_block, name, ext);
    if (entry == NULL) {
        fprintf(stderr, "Error: file '%s' not found\n", arg);
        return;
    }
    if (entry->attribute == FILE_ATTR_DIR) {
        fprintf(stderr, "Error: '%s' is a directory, use rmdir to remove it\n", arg);
        return;
    }

    uint32_t blk = entry->first_block;
    int steps = 0;
    while (blk != 0 && blk < super_block->total_blocks && steps < (int)super_block->total_blocks) {
        steps++;
        if (blk < super_block->data_start) {
            fprintf(stderr, "Error: file has an invalid data block number\n");
            return;
        }
        uint32_t next = fat_table[blk];
        if (fat_table[blk] == FAT_FREE) break;
        fat_table[blk] = FAT_FREE;
        super_block->free_blocks++;
        if (next == FAT_END) break;
        blk = next;
    }

    for (int j = 0; j < OPEN_FILE_MAX; j++) {
        if (open_file_table[j].dir_entry == entry)
            memset(&open_file_table[j], 0, sizeof(OpenFileItem));
    }

    memset(entry, 0, sizeof(DirEntry));
    fprintf(stderr, "File deleted successfully: %s\n", arg);
}

void svfs_open(const char *filename)
{
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "open: filesystem is not initialized\n");
        return;
    }

    char name[MAX_NAME_LEN + 1] = {0};
    char ext[MAX_EXT_LEN + 1] = {0};
    if (fs_split_filename(filename, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "open: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, filename);
        return;
    }


    DirEntry *target_entry = fs_find_entry(current_dir_block, name, ext);
    if (target_entry == NULL) {
        fprintf(stderr, "Error: file '%s' does not exist\n", filename);
        return;
    }
    if (target_entry->attribute == FILE_ATTR_DIR) {
        fprintf(stderr, "Error: '%s' is a directory, cannot be opened as a file\n", filename);
        return;
    }

    int fd = -1;
    for (int i = 0; i < OPEN_FILE_MAX; i++) {
        if (open_file_table[i].dir_entry == NULL) { fd = i; break; }
    }
    if (fd == -1) {
        fprintf(stderr, "Error: open file table is full, cannot open more files\n");
        return;
    }

    for (int i = 0; i < OPEN_FILE_MAX; i++) {
        if (open_file_table[i].dir_entry != NULL &&
            open_file_table[i].dir_entry->first_block == target_entry->first_block) {
            fprintf(stderr, "Error: file '%s' is already open (file descriptor: %d)\n", filename, i);
            return;
        }
    }

    open_file_table[fd].dir_entry = target_entry;
    open_file_table[fd].current_pos = 0;
    open_file_table[fd].is_write = 1;
    open_file_table[fd].fd = fd;

    fprintf(stderr, "File opened successfully: %s (file descriptor: %d)\n", filename, fd);
    fprintf(stderr, "  [debug] File starting block: %u\n", open_file_table[fd].dir_entry->first_block);
    fprintf(stderr, "  [debug] Current read/write position: %d\n", open_file_table[fd].current_pos);
    fprintf(stderr, "  [debug] Open mode: %s\n", open_file_table[fd].is_write ? "read/write" : "read-only");
}

void svfs_close(const char *filename)
{
    if (virtual_disk == NULL || super_block == NULL) {
        fprintf(stderr, "close: filesystem is not initialized\n");
        return;
    }
    if (filename == NULL || filename[0] == '\0') {
        fprintf(stderr, "Usage: close <file name>\n");
        return;
    }

    char name[MAX_NAME_LEN + 1] = {0};
    char ext[MAX_EXT_LEN + 1] = {0};
    if (fs_split_filename(filename, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "close: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, filename);
        return;
    }

    DirEntry *entry = fs_find_entry(current_dir_block, name, ext);
    if (entry == NULL) {
        fprintf(stderr, "Error: file '%s' does not exist\n", filename);
        return;
    }

    for (int i = 0; i < OPEN_FILE_MAX; i++) {
        if (open_file_table[i].dir_entry == entry) {
            memset(&open_file_table[i], 0, sizeof(OpenFileItem));
            fprintf(stderr, "File closed successfully: %s\n", filename);
            return;
        }
    }

    fprintf(stderr, "Error: file '%s' is not open\n", filename);
}

// Binary-safe write: writes exactly `len` bytes regardless of their content
// (including embedded zero bytes). This is what svfs_write() below actually
// calls; it's also directly usable by API consumers who have raw binary data
// rather than a NUL-terminated string.
int svfs_write_bytes(const char *filename, const void *data, size_t len)
{
    if (virtual_disk == NULL || fat_table == NULL || super_block == NULL) {
        fprintf(stderr, "write: filesystem is not initialized\n");
        return 0;
    }
    if (filename == NULL || filename[0] == '\0') {
        fprintf(stderr, "write: missing file name\n");
        return 0;
    }
    if (data == NULL && len > 0) {
        fprintf(stderr, "write: null data pointer with nonzero length\n");
        return 0;
    }

    char name[MAX_NAME_LEN + 1], ext[MAX_EXT_LEN + 1];
    if (fs_split_filename(filename, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "write_bytes: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, filename);
        return 0;
    }

    DirEntry *entry = fs_find_entry(current_dir_block, name, ext);
    if (entry == NULL) {
        fprintf(stderr, "write: file '%s' does not exist (create it first with 'create')\n", filename);
        return 0;
    }
    if (entry->attribute & FILE_ATTR_DIR) {
        fprintf(stderr, "write: '%s' is a directory, cannot write to it as a file\n", filename);
        return 0;
    }

    int ok = fs_write_file_data(entry, (const uint8_t *)data, len);
    if (ok) {
        fprintf(stderr, "Wrote to file successfully: %s (%zu bytes)\n", filename, len);
    }
    return ok;
}

// Text convenience wrapper: NUL-terminated string, so an embedded '\0' byte
// would truncate the write. Use svfs_write_bytes() directly for binary data.
void svfs_write(const char *filename, const char *content)
{
    if (content == NULL) content = "";
    svfs_write_bytes(filename, content, strlen(content));
}

// Binary-safe read into a caller-provided buffer. Returns 1 and sets *out_len
// on success (truncating to buffer_size if the file is larger); returns 0 on
// error, printing a message, and leaves *out_len untouched.
int svfs_read_bytes(const char *filename, void *buffer, size_t buffer_size, size_t *out_len)
{
    if (virtual_disk == NULL || fat_table == NULL || super_block == NULL) {
        fprintf(stderr, "read: filesystem is not initialized\n");
        return 0;
    }
    if (filename == NULL || filename[0] == '\0') {
        fprintf(stderr, "read: missing file name\n");
        return 0;
    }
    if (buffer == NULL && buffer_size > 0) {
        fprintf(stderr, "read: null buffer with nonzero size\n");
        return 0;
    }

    char name[MAX_NAME_LEN + 1], ext[MAX_EXT_LEN + 1];
    if (fs_split_filename(filename, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "read_bytes: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, filename);
        return 0;
    }

    DirEntry *entry = fs_find_entry(current_dir_block, name, ext);
    if (entry == NULL) {
        fprintf(stderr, "read: no such file: %s\n", filename);
        return 0;
    }
    if (entry->attribute & FILE_ATTR_DIR) {
        fprintf(stderr, "read: %s is a directory\n", filename);
        return 0;
    }

    size_t to_copy = entry->file_size;
    if (to_copy > buffer_size) to_copy = buffer_size;

    uint8_t *dst = (uint8_t *)buffer;
    size_t remaining = to_copy;
    uint32_t block = entry->first_block;

    while (remaining > 0) {
        if (block == FAT_FREE || block == FAT_END || block >= super_block->total_blocks) {
            fprintf(stderr, "read: broken FAT chain\n");
            return 0;
        }

        size_t chunk = (remaining > BLOCK_SIZE) ? BLOCK_SIZE : remaining;
        memcpy(dst, GET_BLOCK_ADDR(block), chunk);
        dst += chunk;
        remaining -= chunk;
        if (remaining == 0) break;

        uint32_t next_block = fat_table[block];
        if (next_block == block) {
            fprintf(stderr, "read: FAT loop detected\n");
            return 0;
        }
        block = next_block;
    }

    if (out_len) *out_len = to_copy;
    return 1;
}

// stdout-streaming reader. Binary-safe on output (writes exactly file_size
// bytes via fwrite, not up to the first '\0', and appends no extra bytes of
// its own), so it's fine to use on binary files too - only svfs_write()'s
// *input* side is text-only. All status/error messages go to stderr so
// stdout carries nothing but the file's actual bytes.
void svfs_read(const char *filename)
{
    if (virtual_disk == NULL || fat_table == NULL || super_block == NULL) {
        fprintf(stderr, "read: filesystem is not initialized\n");
        return;
    }
    if (filename == NULL || filename[0] == '\0') {
        fprintf(stderr, "read: missing file name\n");
        return;
    }

    char name[MAX_NAME_LEN + 1], ext[MAX_EXT_LEN + 1];
    if (fs_split_filename(filename, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "read: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, filename);
        return;
    }

    DirEntry *entry = fs_find_entry(current_dir_block, name, ext);
    if (entry == NULL) {
        fprintf(stderr, "read: no such file: %s\n", filename);
        return;
    }
    if (entry->attribute & FILE_ATTR_DIR) {
        fprintf(stderr, "read: %s is a directory\n", filename);
        return;
    }
    if (entry->file_size == 0) {
        // A genuinely empty file has zero bytes of content - print nothing to
        // stdout (so `svfs read empty.txt > out` produces a real empty file),
        // just note it on stderr for anyone watching interactively.
        fprintf(stderr, "[empty file]\n");
        return;
    }

    uint32_t block = entry->first_block;
    uint32_t remain = entry->file_size;

    while (remain > 0) {
        if (block == FAT_FREE || block == FAT_END || block >= super_block->total_blocks) {
            fprintf(stderr, "read: broken FAT chain\n");
            return;
        }

        size_t chunk = (remain > BLOCK_SIZE) ? BLOCK_SIZE : (size_t)remain;
        fwrite(GET_BLOCK_ADDR(block), 1, chunk, stdout);

        remain -= (uint32_t)chunk;
        if (remain == 0) break;

        uint32_t next_block = fat_table[block];
        if (next_block == block) {
            fprintf(stderr, "read: FAT loop detected\n");
            return;
        }
        block = next_block;
    }
}



// ============================================================================
// 7. Host filesystem interop: import / export
// ============================================================================

void svfs_import(const char *host_path, const char *vfs_name)
{
    if (virtual_disk == NULL || fat_table == NULL || super_block == NULL) {
        fprintf(stderr, "import: filesystem is not initialized\n");
        return;
    }
    if (host_path == NULL || host_path[0] == '\0' || vfs_name == NULL || vfs_name[0] == '\0') {
        fprintf(stderr, "Usage: import <host file> <destination file name>\n");
        return;
    }

    char name[MAX_NAME_LEN + 1];
    char ext[MAX_EXT_LEN + 1];
    if (fs_split_filename(vfs_name, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "import: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, vfs_name);
        return;
    }

    FILE *f = fopen(host_path, "rb");
    if (f == NULL) {
        fprintf(stderr, "import: could not open host file '%s'\n", host_path);
        return;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fprintf(stderr, "import: could not determine the size of '%s'\n", host_path);
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size < 0) {
        fprintf(stderr, "import: could not determine the size of '%s'\n", host_path);
        fclose(f);
        return;
    }
    rewind(f);

    uint8_t *buffer = NULL;
    if (size > 0) {
        buffer = (uint8_t *)malloc((size_t)size);
        if (buffer == NULL) {
            fprintf(stderr, "import: out of memory (file is %ld bytes)\n", size);
            fclose(f);
            return;
        }
        size_t read = fread(buffer, 1, (size_t)size, f);
        if (read != (size_t)size) {
            fprintf(stderr, "import: short read on '%s' (%zu of %ld bytes)\n", host_path, read, size);
            free(buffer);
            fclose(f);
            return;
        }
    }
    fclose(f);

    DirEntry *entry = fs_find_entry(current_dir_block, name, ext);
    if (entry == NULL) {
        entry = fs_create_entry(current_dir_block, name, ext, FILE_ATTR_FILE);
        if (entry == NULL) {
            free(buffer);
            return;
        }
    } else if (entry->attribute & FILE_ATTR_DIR) {
        fprintf(stderr, "import: '%s' is a directory, cannot overwrite it with a file\n", vfs_name);
        free(buffer);
        return;
    }

    if (fs_write_file_data(entry, buffer, (size_t)size)) {
        fprintf(stderr, "Imported '%s' -> '%s' (%ld bytes)\n", host_path, vfs_name, size);
    }

    free(buffer);
}

void svfs_export(const char *vfs_name, const char *host_path)
{
    if (virtual_disk == NULL || fat_table == NULL || super_block == NULL) {
        fprintf(stderr, "export: filesystem is not initialized\n");
        return;
    }
    if (vfs_name == NULL || vfs_name[0] == '\0' || host_path == NULL || host_path[0] == '\0') {
        fprintf(stderr, "Usage: export <source file name> <host file>\n");
        return;
    }

    char name[MAX_NAME_LEN + 1];
    char ext[MAX_EXT_LEN + 1];
    if (fs_split_filename(vfs_name, name, sizeof(name), ext, sizeof(ext))) {
        fprintf(stderr, "export: name or extension too long (max %d/%d characters): %s\n",
                MAX_NAME_LEN, MAX_EXT_LEN, vfs_name);
        return;
    }

    DirEntry *entry = fs_find_entry(current_dir_block, name, ext);
    if (entry == NULL) {
        fprintf(stderr, "export: file '%s' does not exist\n", vfs_name);
        return;
    }
    if (entry->attribute & FILE_ATTR_DIR) {
        fprintf(stderr, "export: '%s' is a directory, cannot export it as a file\n", vfs_name);
        return;
    }

    FILE *f = fopen(host_path, "wb");
    if (f == NULL) {
        fprintf(stderr, "export: could not open '%s' for writing\n", host_path);
        return;
    }

    uint32_t remain = entry->file_size;
    uint32_t block = entry->first_block;

    while (remain > 0) {
        if (block == FAT_FREE || block == FAT_END || block >= super_block->total_blocks) {
            fprintf(stderr, "export: broken FAT chain while reading '%s'\n", vfs_name);
            fclose(f);
            return;
        }

        size_t chunk = (remain > BLOCK_SIZE) ? BLOCK_SIZE : (size_t)remain;
        size_t written = fwrite(GET_BLOCK_ADDR(block), 1, chunk, f);
        if (written != chunk) {
            fprintf(stderr, "export: short write to '%s'\n", host_path);
            fclose(f);
            return;
        }

        remain -= (uint32_t)chunk;
        if (remain == 0) break;

        uint32_t next = fat_table[block];
        if (next == block) {
            fprintf(stderr, "export: FAT loop detected while reading '%s'\n", vfs_name);
            fclose(f);
            return;
        }
        block = next;
    }

    fclose(f);
    fprintf(stderr, "Exported '%s' -> '%s' (%u bytes)\n", vfs_name, host_path, entry->file_size);
}


// ============================================================================
// 8. Maintenance: fsck
// ============================================================================

typedef struct {
    int errors;
    int warnings;
    int file_count;
    int dir_count;
} FsckStats;

static void fsck_mark_block(uint8_t *visited, uint32_t block, FsckStats *stats, const char *context)
{
    if (block >= super_block->total_blocks) {
        printf("  [ERROR] %s references out-of-range block %u\n", context, block);
        stats->errors++;
        return;
    }
    if (visited[block]) {
        printf("  [ERROR] Block %u is referenced by more than one file/directory "
               "(cross-linked chain) - detected while checking %s\n", block, context);
        stats->errors++;
        return;
    }
    visited[block] = 1;
}

static void fsck_walk_dir(uint32_t dir_block, uint8_t *visited, FsckStats *stats, int depth)
{
    if (depth > 64) {
        printf("  [ERROR] Directory nesting exceeds the safety limit (possible cycle) "
               "at block %u\n", dir_block);
        stats->errors++;
        return;
    }

    uint32_t blk = dir_block;
    int steps = 0;
    while (1) {
        if (blk >= super_block->total_blocks) {
            printf("  [ERROR] Directory chain references out-of-range block %u\n", blk);
            stats->errors++;
            return;
        }
        fsck_mark_block(visited, (uint32_t)blk, stats, "a directory's own block chain");
        uint32_t next = fat_table[blk];
        if (next == FAT_END) break;
        if (next == FAT_FREE || next == FAT_BAD) {
            printf("  [ERROR] Directory block chain at block %u ends abnormally "
                   "(points to a free/reserved block %u)\n", blk, next);
            stats->errors++;
            break;
        }
        blk = next;
        if (++steps > (int)super_block->total_blocks) {
            printf("  [ERROR] FAT chain loop detected in directory starting at block %u\n", dir_block);
            stats->errors++;
            break;
        }
    }

    for (int i = 0; ; i++) {
        DirEntry *entry = get_dir_entry_by_index(dir_block, i);
        if (entry == NULL) break;
        if (entry->filename[0] == '\0') continue;

        char name[MAX_NAME_LEN + 1], ext[MAX_EXT_LEN + 1];
        trim_field(entry->filename, sizeof(entry->filename), name, sizeof(name));
        trim_field(entry->ext, sizeof(entry->ext), ext, sizeof(ext));

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char display[MAX_NAME_LEN + 1 + MAX_EXT_LEN + 1];
        if (ext[0] != '\0') snprintf(display, sizeof(display), "%s.%s", name, ext);
        else snprintf(display, sizeof(display), "%s", name);

        if (entry->first_block < super_block->data_start ||
            entry->first_block >= super_block->total_blocks) {
            printf("  [ERROR] '%s' has an invalid starting block (%u)\n", display, entry->first_block);
            stats->errors++;
            continue;
        }

        if (entry->attribute == FILE_ATTR_DIR) {
            stats->dir_count++;
            fsck_walk_dir(entry->first_block, visited, stats, depth + 1);
            continue;
        }

        stats->file_count++;

        uint32_t bytes_capacity = 0;
        int fsteps = 0;
        uint32_t cur = entry->first_block;
        while (1) {
            fsck_mark_block(visited, cur, stats, display);
            bytes_capacity += BLOCK_SIZE;
            uint32_t next = fat_table[cur];
            if (next == FAT_END) break;
            if (next == FAT_FREE || next == FAT_BAD) {
                printf("  [ERROR] File '%s' FAT chain ends abnormally at block %u "
                       "(points to a free/reserved block %u)\n", display, cur, next);
                stats->errors++;
                break;
            }
            cur = next;
            if (++fsteps > (int)super_block->total_blocks) {
                printf("  [ERROR] FAT chain loop detected in file '%s'\n", display);
                stats->errors++;
                break;
            }
        }

        if (bytes_capacity < entry->file_size) {
            printf("  [ERROR] File '%s': file_size=%u exceeds its allocated capacity "
                   "(%u bytes across its block chain)\n", display, entry->file_size, bytes_capacity);
            stats->errors++;
        }

        if (entry->ts_modified != 0 && entry->ts_created != 0 && entry->ts_modified < entry->ts_created) {
            printf("  [WARN] File '%s' has ts_modified older than ts_created\n", display);
            stats->warnings++;
        }
    }
}

void svfs_fsck(void)
{
    if (virtual_disk == NULL || fat_table == NULL || super_block == NULL) {
        printf("fsck: filesystem is not initialized\n");
        return;
    }

    printf("=== Filesystem consistency check ===\n");

    if (super_block->magic != FS_MAGIC || super_block->version != FS_VERSION) {
        printf("  [ERROR] Superblock magic/version mismatch (magic=0x%08X version=%u, "
               "expected magic=0x%08X version=%u)\n",
               super_block->magic, super_block->version, FS_MAGIC, FS_VERSION);
    }

    FsckStats stats = {0, 0, 0, 0};
    uint8_t *visited = calloc(super_block->total_blocks, 1);
    if (visited == NULL) {
        printf("fsck: out of memory\n");
        return;
    }

    visited[0] = 1;
    for (uint32_t b = super_block->fat_start; b < super_block->fat_start + super_block->fat_size; b++) {
        if (b < super_block->total_blocks) visited[b] = 1;
    }

    printf("Checking directory tree from root (block %u)...\n", super_block->root_dir_block);
    fsck_walk_dir(super_block->root_dir_block, visited, &stats, 0);

    uint32_t counted_free = 0;
    uint32_t orphaned = 0;
    for (uint32_t b = 0; b < super_block->total_blocks; b++) {
        int marked_used = visited[b];
        uint32_t fat_val = fat_table[b];

        if (fat_val == FAT_FREE) {
            counted_free++;
            if (marked_used) {
                printf("  [ERROR] Block %u is used by a file/directory but the FAT marks it FREE\n", b);
                stats.errors++;
            }
        } else if (!marked_used && fat_val != FAT_BAD) {
            printf("  [WARN] Block %u is allocated in the FAT but is not reachable from any "
                   "file or directory (orphaned/lost block)\n", b);
            stats.warnings++;
            orphaned++;
        }
    }

    if (counted_free != super_block->free_blocks) {
        printf("  [ERROR] Superblock free_blocks=%u does not match the actual free block "
               "count=%u\n", super_block->free_blocks, counted_free);
        stats.errors++;
    }

    printf("\nSummary:\n");
    printf("  Files checked:       %d\n", stats.file_count);
    printf("  Directories checked: %d\n", stats.dir_count);
    printf("  Orphaned blocks:     %u\n", orphaned);
    printf("  Errors:              %d\n", stats.errors);
    printf("  Warnings:            %d\n", stats.warnings);

    if (stats.errors == 0 && stats.warnings == 0) {
        printf("Result: OK - filesystem is consistent\n");
    } else if (stats.errors == 0) {
        printf("Result: OK, but with warnings\n");
    } else {
        printf("Result: INCONSISTENT - %d error(s) found\n", stats.errors);
    }

    free(visited);
}
