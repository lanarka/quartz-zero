// =============================================================================
// Quartz - The Computer
// -----------------------------------------------------------------------------
// vm.c - the Quartz virtual machine: a simple register-based bytecode
// interpreter driven one instruction at a time by vm_do_step().
//
// Rough layout of this file, top to bottom:
//   1. Small host-side helpers: busy/sleep waiting (wait_us), wall-clock time
//      (now_ms), little-endian read/write helpers, and RAM bounds checking
//      (ram_bounds_ok) used by every memory-touching opcode.
//   2. SVFS helpers: since the VM only ever sees SVFS file/directory names as
//      NUL-terminated strings living inside VM RAM (never as host pointers),
//      vm_ram_cstr()/vm_ram_cstr_len() safely locate the terminator and hand
//      back a `const char *` straight into vm->ram for the OP_SVFS_* opcodes
//      to pass on to svfs_*().
//   3. Typed-value helpers (type_size/type_normalize/type_add/.../do_compare):
//      every arithmetic/comparison opcode carries an explicit ValType operand
//      (see vm.h) so the same opcode can operate on i8..i64/u8..u64/f32/f64;
//      these helpers centralize the per-type width truncation and float vs.
//      integer dispatch so the opcode handlers themselves stay short.
//   4. Instruction-stream fetch helpers (fetch_u8/u16/u32/u64/reg/type/imm):
//      read the next operand from prog->code at vm->pc, advancing pc, and
//      report failure on truncated/out-of-range programs.
//   5. Program loading (load_program) and VM lifecycle (vm_do_create,
//      vm_do_reset, vm_attach_disk, vm_do_destroy) - see comments at each.
//   6. vm_do_step(): fetches one opcode and dispatches to its handler. This
//      is the interpreter's core loop, called repeatedly by quartz.c's main
//      loop (vm.step()). Per-opcode comments intentionally stay light here
//      since the instruction set itself is documented separately; comments
//      in this switch focus on control-flow/lifecycle subtleties instead
//      (OP_VMX's "warm restart" semantics, OP_RESET vs. OP_VMX vs. boot_prog,
//      the SVFS text vs. binary-safe opcode pairs, etc.).
//   7. vm_init(): wires up the VM struct's function-pointer "vtable".
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <math.h>
#include <sys/time.h>
#include <time.h>
#include <errno.h>

#include "vm.h"
#include "runtime.h"
#include "svfs.h"
#include "vmnet.h"

#define VM_WAIT_MAX_US 5000000ULL   // Hard cap on how long a single OP_WAIT/OP_WAITI can block the
                                    // host thread, so a VM program can never freeze the whole
                                    // emulator (window, audio, host shortcuts) indefinitely.

static void wait_us(unsigned long long us) {
    struct timeval start, now;
    unsigned long long elapsed;
    gettimeofday(&start, NULL);
    const unsigned long long spin_tail_us = 1000ULL;
    if (us > spin_tail_us) {
        unsigned long long sleep_us = us - spin_tail_us;
        struct timespec ts;
        ts.tv_sec  = (time_t)(sleep_us / 1000000ULL);
        ts.tv_nsec = (long)((sleep_us % 1000000ULL) * 1000ULL);
        nanosleep(&ts, NULL);
    }
    do {
        gettimeofday(&now, NULL);
        elapsed = (unsigned long long)(now.tv_sec - start.tv_sec) * 1000000ULL
                + (unsigned long long)(now.tv_usec - start.tv_usec);

    } while (elapsed < us);
}

static int64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000LL + (int64_t)(tv.tv_usec / 1000);
}

static uint32_t read_u32_le(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint16_t read_u16_le(const uint8_t *b) {
    return (uint16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8));
}

static void write_u32_le(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)(v & 0xFF);
    b[1] = (uint8_t)((v >> 8) & 0xFF);
    b[2] = (uint8_t)((v >> 16) & 0xFF);
    b[3] = (uint8_t)((v >> 24) & 0xFF);
}

static uint64_t read_u64_le(const uint8_t *b) {
    uint64_t lo = read_u32_le(b);
    uint64_t hi = read_u32_le(b + 4);
    return lo | (hi << 32);
}

static void write_u64_le(uint8_t *b, uint64_t v) {
    write_u32_le(b, (uint32_t)(v & 0xFFFFFFFFu));
    write_u32_le(b + 4, (uint32_t)(v >> 32));
}

static uint64_t read_sized_le(const uint8_t *b, int size) {
    switch (size) {
        case 1: return (uint64_t)b[0];
        case 2: return (uint64_t)b[0] | ((uint64_t)b[1] << 8);
        case 4: return (uint64_t)read_u32_le(b);
        case 8: return read_u64_le(b);
        default: return 0;
    }
}

static void write_sized_le(uint8_t *b, uint64_t v, int size) {
    switch (size) {
        case 1: b[0] = (uint8_t)(v & 0xFF); break;
        case 2: b[0] = (uint8_t)(v & 0xFF); b[1] = (uint8_t)((v >> 8) & 0xFF); break;
        case 4: write_u32_le(b, (uint32_t)(v & 0xFFFFFFFFu)); break;
        case 8: write_u64_le(b, v); break;
        default: break;
    }
}

static int ram_bounds_ok(const VM *self, uint32_t addr, uint64_t size) {
    if (size == 0) return 1;
    if (addr >= (uint32_t)self->ram_size) return 0;
    if ((uint64_t)addr + size > (uint64_t)self->ram_size) return 0;
    return 1;
}

static void vm_fault(VM *self, const char *msg) {
    fprintf(stderr, "VM: fault at pc=%u: %s (halting)\n", self->pc, msg);
    self->running = 0;
}

// ---------------------------------------------------------------------------
// SVFS (virtual disk) helpers
//
// The OP_SVFS_* instructions below pass file/directory names and text
// content to the svfs_*() functions as NUL-terminated C strings, but the VM
// operand is always just a REGISTER holding an ADDRESS into vm->ram - never
// a direct host pointer. Since vm->ram is ordinary host memory, nothing
// needs to be copied: it's enough to locate where (if at all) a NUL
// terminator lies within safe bounds, and then hand back &vm->ram[addr]
// directly as a `const char *`.
// ---------------------------------------------------------------------------

// Upper bound for scanning a file/directory name in RAM. SVFS itself already
// limits names to MAX_NAME_LEN + '.' + MAX_EXT_LEN = 32+1+8 = 41 characters -
// this is just a safety net so an unterminated string can never force us to
// scan all of RAM before giving up.
#define SVFS_VM_NAME_SCAN_MAX 256u

// OP_SVFS_DIR: fixed size (in bytes) of one serialized directory entry
// written into VM RAM - see the record format in the OP_SVFS_DIR comment
// in vm.h.
#define SVFS_VM_DIR_RECORD_SIZE 64u

// OP_SVFS_DIR: size of the host-side scratch array that svfs_dir() writes
// into before we serialize it into VM RAM. Comfortably above SVFS's own cap
// on entries per directory (a directory is a single fixed block, see
// svfs_internal.h), so the entire real directory always fits.
#define SVFS_VM_DIR_HOST_CAP 32

// Scans up to max_len bytes of VM RAM starting at addr looking for '\0'.
// Never reads past the end of RAM. On success returns 1 and sets *out_len to
// the string's length (WITHOUT the NUL byte). On failure (addr already out
// of RAM, or no NUL found within max_len/RAM) returns 0 and leaves *out_len
// untouched.
static int vm_ram_cstr_len(VM *self, uint32_t addr, uint32_t max_len, size_t *out_len) {
    if (addr >= (uint32_t)self->ram_size) return 0;
    uint32_t room  = (uint32_t)self->ram_size - addr;
    uint32_t limit = (room < max_len) ? room : max_len;
    for (uint32_t i = 0; i < limit; i++) {
        if (self->ram[addr + i] == 0) {
            *out_len = (size_t)i;
            return 1;
        }
    }
    return 0;
}

// Convenience wrapper around vm_ram_cstr_len(): directly returns a usable
// `const char *` (pointing straight into VM RAM) instead of a length.
// Returns NULL on the same failure conditions.
static const char *vm_ram_cstr(VM *self, uint32_t addr, uint32_t max_len) {
    size_t len;
    if (!vm_ram_cstr_len(self, addr, max_len, &len)) return NULL;
    return (const char *)&self->ram[addr];
}

// ---------------------------------------------------------------------------
// Network card helpers
//
// Talks to the src-net "sietovka" library (vmnet.h). See NETHELP.md for
// what each OP_NET_* instruction does. Implementation note: network I/O
// must never block the VM's step loop, so OP_NET_RECV always polls with a
// 0ms timeout before ever calling the otherwise-blocking vmnet_recv().
// ---------------------------------------------------------------------------

// Thin accessor: vm->net is stored as `void *` in vm.h (same reasoning as
// the `rt` field) so this is the only place that casts it back to its real
// type.
static VmNetChannel *vm_net(VM *self) {
    return (VmNetChannel *)self->net;
}

// Tears down the network channel after an unrecoverable error or a clean
// disconnect from the HOST side: closes/frees the channel and clears
// vm->net so every subsequent OP_NET_* opcode simply reports "not
// attached" again, instead of repeatedly failing against a dead socket.
static void vm_net_detach(VM *self) {
    VmNetChannel *ch = vm_net(self);
    if (!ch) return;
    vmnet_channel_close(ch);
    free(ch);
    self->net = NULL;
}

static int is_unsigned_type(ValType t) {
    return t == TYPE_U8 || t == TYPE_U16 || t == TYPE_U32 || t == TYPE_U64;
}

static int is_float_type(ValType t) {
    return t == TYPE_F32 || t == TYPE_F64;
}

static int type_size(ValType t) {
    switch (t) {
        case TYPE_I8:  case TYPE_U8:  return 1;
        case TYPE_I16: case TYPE_U16: return 2;
        case TYPE_I32: case TYPE_U32: case TYPE_F32: return 4;
        case TYPE_I64: case TYPE_U64: case TYPE_F64: return 8;
        default: return 8;
    }
}

static const char *type_name(ValType t) {
    switch (t) {
        case TYPE_I8:  return "i8";
        case TYPE_I16: return "i16";
        case TYPE_I32: return "i32";
        case TYPE_U8:  return "u8";
        case TYPE_U16: return "u16";
        case TYPE_U32: return "u32";
        case TYPE_I64: return "i64";
        case TYPE_U64: return "u64";
        case TYPE_F32: return "f32";
        case TYPE_F64: return "f64";
        default:       return "?";
    }
}

static int64_t type_normalize(int64_t v, ValType t) {
    uint64_t u = (uint64_t)v;
    switch (t) {
        case TYPE_I8:  return (int64_t)(int8_t)(uint8_t)u;
        case TYPE_U8:  return (int64_t)(uint8_t)u;
        case TYPE_I16: return (int64_t)(int16_t)(uint16_t)u;
        case TYPE_U16: return (int64_t)(uint16_t)u;
        case TYPE_I32: return (int64_t)(int32_t)(uint32_t)u;
        case TYPE_U32: return (int64_t)(uint32_t)u;
        case TYPE_I64:
        case TYPE_U64: return v;
        case TYPE_F32: return (int64_t)(uint32_t)u;
        case TYPE_F64: return v;
        default:       return v;
    }
}

static float f32_from_bits(int64_t raw) {
    uint32_t bits = (uint32_t)(uint64_t)raw;
    float f; memcpy(&f, &bits, sizeof(f)); return f;
}
static int64_t f32_to_raw(float f) {
    uint32_t bits; memcpy(&bits, &f, sizeof(bits));
    return (int64_t)(uint64_t)bits;
}
static double f64_from_bits(int64_t raw) {
    uint64_t bits = (uint64_t)raw;
    double d; memcpy(&d, &bits, sizeof(d)); return d;
}
static int64_t f64_to_raw(double d) {
    uint64_t bits; memcpy(&bits, &d, sizeof(bits));
    return (int64_t)bits;
}

static int64_t type_add(int64_t a, int64_t b, ValType t) {
    if (t == TYPE_F32) return f32_to_raw(f32_from_bits(a) + f32_from_bits(b));
    if (t == TYPE_F64) return f64_to_raw(f64_from_bits(a) + f64_from_bits(b));
    return type_normalize(a + b, t);
}
static int64_t type_sub(int64_t a, int64_t b, ValType t) {
    if (t == TYPE_F32) return f32_to_raw(f32_from_bits(a) - f32_from_bits(b));
    if (t == TYPE_F64) return f64_to_raw(f64_from_bits(a) - f64_from_bits(b));
    return type_normalize(a - b, t);
}
static int64_t type_mul(int64_t a, int64_t b, ValType t) {
    if (t == TYPE_F32) return f32_to_raw(f32_from_bits(a) * f32_from_bits(b));
    if (t == TYPE_F64) return f64_to_raw(f64_from_bits(a) * f64_from_bits(b));
    return type_normalize(a * b, t);
}

static int64_t type_div(int64_t a, int64_t b, ValType t) {
    if (t == TYPE_F32) return f32_to_raw(f32_from_bits(a) / f32_from_bits(b));
    if (t == TYPE_F64) return f64_to_raw(f64_from_bits(a) / f64_from_bits(b));
    if (is_unsigned_type(t))
        return type_normalize((int64_t)((uint64_t)a / (uint64_t)b), t);
    return type_normalize(a / b, t);
}
static int64_t type_mod(int64_t a, int64_t b, ValType t) {
    if (t == TYPE_F32) return f32_to_raw(fmodf(f32_from_bits(a), f32_from_bits(b)));
    if (t == TYPE_F64) return f64_to_raw(fmod(f64_from_bits(a), f64_from_bits(b)));
    if (is_unsigned_type(t))
        return type_normalize((int64_t)((uint64_t)a % (uint64_t)b), t);
    return type_normalize(a % b, t);
}

static int64_t type_shl(int64_t a, int64_t b, ValType t) {
    int shift = (int)(b & 63);
    return type_normalize((int64_t)((uint64_t)a << shift), t);
}
static int64_t type_shr(int64_t a, int64_t b, ValType t) {
    int shift = (int)(b & 63);
    if (is_unsigned_type(t))
        return type_normalize((int64_t)((uint64_t)a >> shift), t);
    return type_normalize(a >> shift, t);
}

static void do_compare(VM *self, int64_t raw_a, int64_t raw_b, ValType t) {
    int64_t na = type_normalize(raw_a, t);
    int64_t nb = type_normalize(raw_b, t);
    if (t == TYPE_F32) {
        float fa = f32_from_bits(na), fb = f32_from_bits(nb);
        self->flag_zero = (fa == fb);
        self->flag_neg  = (fa < fb);
    } else if (t == TYPE_F64) {
        double da = f64_from_bits(na), db = f64_from_bits(nb);
        self->flag_zero = (da == db);
        self->flag_neg  = (da < db);
    } else if (is_unsigned_type(t)) {
        uint64_t ua = (uint64_t)na, ub = (uint64_t)nb;
        self->flag_zero = (ua == ub);
        self->flag_neg  = (ua < ub);
    } else {
        self->flag_zero = (na == nb);
        self->flag_neg  = (na < nb);
    }
}

static int fetch_u8(const Program *prog, VM *vm, uint8_t *out) {
    if (vm->pc >= prog->code_len) return 0;
    *out = prog->code[vm->pc++];
    return 1;
}

static int fetch_u32(const Program *prog, VM *vm, uint32_t *out) {
    if ((uint64_t)vm->pc + 4 > prog->code_len) return 0;
    *out = read_u32_le(&prog->code[vm->pc]);
    vm->pc += 4;
    return 1;
}

static int fetch_u16(const Program *prog, VM *vm, uint16_t *out) {
    if ((uint64_t)vm->pc + 2 > prog->code_len) return 0;
    *out = read_u16_le(&prog->code[vm->pc]);
    vm->pc += 2;
    return 1;
}

static int fetch_f32(const Program *prog, VM *vm, float *out) {
    uint32_t raw;
    if (!fetch_u32(prog, vm, &raw)) return 0;
    memcpy(out, &raw, sizeof(*out));
    return 1;
}

static int fetch_u64(const Program *prog, VM *vm, uint64_t *out) {
    if ((uint64_t)vm->pc + 8 > prog->code_len) return 0;
    *out = read_u64_le(&prog->code[vm->pc]);
    vm->pc += 8;
    return 1;
}

static int fetch_reg(const Program *prog, VM *vm, int *out) {
    uint8_t b;
    if (!fetch_u8(prog, vm, &b)) return 0;
    if (b >= NUM_REGS) return 0;
    *out = b;
    return 1;
}

static int fetch_type(const Program *prog, VM *vm, ValType *out) {
    uint8_t b;
    if (!fetch_u8(prog, vm, &b)) return 0;
    if (b >= TYPE_COUNT) return 0;
    *out = (ValType)b;
    return 1;
}

static int fetch_imm(const Program *prog, VM *vm, ValType t, int64_t *out) {
    if (type_size(t) == 8) {
        uint64_t raw;
        if (!fetch_u64(prog, vm, &raw)) return 0;
        *out = (int64_t)raw;
        return 1;
    } else {
        uint32_t raw;
        if (!fetch_u32(prog, vm, &raw)) return 0;
        *out = (int64_t)(int32_t)raw;
        return 1;
    }
}

// Loads a Quartz .bin image from disk into a freshly allocated Program.
// Validates the fixed 22-byte header (magic number, BIN_VERSION and
// RUNTIME_VERSION - see vm.h) before trusting any of the size fields that
// follow, so a corrupt or incompatible file is rejected up front rather than
// causing an out-of-bounds read further down. On any failure the file is
// closed and no memory is leaked; *prog is only touched on success.
static int load_program(const char *path, Program *prog) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Error: cannot open '%s'\n", path);
        return 0;
    }

    uint8_t header[22];
    if (fread(header, 1, 22, f) != 22) {
        fprintf(stderr, "Error: '%s' is too small to be a valid image file\n", path);
        fclose(f);
        return 0;
    }

    if (header[0] != BIN_MAGIC0 || header[1] != BIN_MAGIC1 ||
        header[2] != BIN_MAGIC2 || header[3] != BIN_MAGIC3) {
        fprintf(stderr, "Error: '%s' has a bad magic number\n", path);
        fclose(f);
        return 0;
    }

    uint8_t bin_version = header[4];
    uint8_t runtime_version = header[5];
    uint32_t entry = read_u32_le(&header[6]);
    uint32_t code_len = read_u32_le(&header[10]);
    uint32_t data_len = read_u32_le(&header[14]);
    uint32_t data_addr = read_u32_le(&header[18]);

    if (bin_version != BIN_VERSION) {
        fprintf(stderr, "Error: unsupported image version\n");
        fclose(f);
        return 0;
    }

    if (runtime_version != RUNTIME_VERSION) {
        fprintf(stderr, "Error: unsupported runtime version\n");
        fclose(f);
        return 0;
    }

    uint8_t *code = (uint8_t *)malloc(code_len > 0 ? code_len : 1);
    if (!code) {
        fprintf(stderr, "Error: out of memory loading '%s'\n", path);
        fclose(f);
        return 0;
    }

    if (code_len > 0 && fread(code, 1, code_len, f) != code_len) {
        fprintf(stderr, "Error: '%s' is truncated (expected %u bytes of code)\n",
                path, (unsigned)code_len);
        free(code);
        fclose(f);
        return 0;
    }

    uint8_t *data = NULL;
    if (data_len > 0) {
        data = (uint8_t *)malloc(data_len);
        if (!data) {
            fprintf(stderr, "Error: out of memory loading data segment of '%s'\n", path);
            free(code);
            fclose(f);
            return 0;
        }
        size_t got = fread(data, 1, data_len, f);
        if (got != data_len) {
            fprintf(stderr,
                    "Error: '%s' data segment is truncated (expected %u bytes, got %zu)\n",
                    path, (unsigned)data_len, got);
            free(code);
            free(data);
            fclose(f);
            return 0;
        }
    }

    fclose(f);

    prog->code = code;
    prog->code_len = code_len;
    prog->entry = entry;
    prog->data = data;
    prog->data_len = data_len;
    prog->data_addr = data_addr;
    return 1;
}

// Performs a full "cold" reset of the VM: registers, RAM, stacks, flags and
// the tick counter are all cleared, and vm->prog is reloaded from an
// independent copy of vm->boot_prog (see the boot_prog field comment in
// vm.h) so that any program installed at runtime via OP_VMX is discarded.
// Triggered by OP_RESET and by the host's Ctrl+Alt+R shortcut in quartz.c.
static void vm_do_reset(VM *vm, const Program *prog) {
    // The "prog" parameter is no longer used for its code/data contents. A
    // "cold" reset (OP_RESET, Ctrl+Alt+R) must always bring the VM back to
    // the program loaded from the file given on the command line at startup
    // (vm->boot_prog) - not to the current vm->prog, which may have since
    // been overwritten via OP_VMX. That's why vm->prog is repopulated on
    // every reset from an independent copy of vm->boot_prog.
    (void)prog;

    uint8_t *code_copy = NULL;
    if (vm->boot_prog.code_len > 0 && vm->boot_prog.code != NULL) {
        code_copy = (uint8_t *)malloc(vm->boot_prog.code_len);
        if (code_copy) {
            memcpy(code_copy, vm->boot_prog.code, vm->boot_prog.code_len);
        }
    }

    uint8_t *data_copy = NULL;
    if (vm->boot_prog.data_len > 0 && vm->boot_prog.data != NULL) {
        data_copy = (uint8_t *)malloc(vm->boot_prog.data_len);
        if (data_copy) {
            memcpy(data_copy, vm->boot_prog.data, vm->boot_prog.data_len);
        }
    }

    if (vm->prog.code) free(vm->prog.code);
    if (vm->prog.data) free(vm->prog.data);

    vm->prog.code      = code_copy;
    vm->prog.code_len  = vm->boot_prog.code_len;
    vm->prog.entry     = vm->boot_prog.entry;
    vm->prog.data      = data_copy;
    vm->prog.data_len  = vm->boot_prog.data_len;
    vm->prog.data_addr = vm->boot_prog.data_addr;

    memset(vm->reg, 0, sizeof(vm->reg));
    memset(vm->ram, 0, (size_t)vm->ram_size);

    vm->pc = vm->prog.entry;
    vm->running = 0;
    vm->flag_zero = 0;
    vm->flag_neg = 0;
    vm->call_sp = 0;
    vm->data_sp = 0;
    vm->vsync_yield = 0;
    vm->tick_count = 0;

    if (vm->prog.data_len > 0 && vm->prog.data != NULL) {
        if ((uint64_t)vm->prog.data_addr + (uint64_t)vm->prog.data_len <= (uint64_t)vm->ram_size) {
            memcpy(vm->ram + vm->prog.data_addr, vm->prog.data, vm->prog.data_len);
        }
    }
}

// Mounts an SVFS virtual disk image for the OP_SVFS_* opcodes to operate on.
// Optional: if never called, disk_path stays NULL and every OP_SVFS_*
// opcode becomes an effective no-op (or reports "not attached", see
// OP_SVFS_ATTACHED). Called once from quartz.c's main() after vm.create()
// but before the main loop starts, i.e. before the first vm.step().
int vm_attach_disk(VM *vm, const char *disk_path) {
    if (disk_path == NULL || disk_path[0] == '\0') return 0;

    if (!svfs_load(disk_path)) {
        fprintf(stderr,
                "VM: failed to attach virtual disk '%s' (missing file, wrong size, "
                "or incompatible SVFS version)\n", disk_path);
        return 0;
    }

    vm->disk_path = strdup(disk_path);
    printf("VM: virtual disk attached: %s\n", disk_path);
    return 1;
}

// Mounts the network card for the OP_NET_* opcodes: allocates a
// VmNetChannel and connects out to a HOST process that must already be
// listening on sock_path. See NETHELP.md for behavior/status codes and
// vm.h for the call-site contract.
int vm_attach_net(VM *vm, const char *sock_path) {
    if (sock_path == NULL || sock_path[0] == '\0') return 0;

    VmNetChannel *ch = (VmNetChannel *)malloc(sizeof(*ch));
    if (!ch) {
        fprintf(stderr, "VM: failed to allocate network channel\n");
        return 0;
    }

    if (vmnet_channel_init(ch, 0) < 0) {
        fprintf(stderr, "VM: failed to initialize network channel: %s\n", strerror(errno));
        free(ch);
        return 0;
    }

    if (vmnet_connect(ch, sock_path) < 0) {
        fprintf(stderr,
                "VM: failed to attach network card '%s': %s (is a HOST process listening?)\n",
                sock_path, strerror(errno));
        vmnet_channel_close(ch);
        free(ch);
        return 0;
    }

    vm->net = ch;
    printf("VM: network card attached: %s (max_packet=%zu)\n",
           sock_path, vmnet_channel_max_packet(ch));
    return 1;
}

// Allocates VM RAM, loads the boot program image from disk, snapshots it
// into self->boot_prog for later "cold" resets (see vm_do_reset()), and
// performs the initial reset that actually copies the program's data
// segment into RAM and sets pc = entry. Sets self->running = 1 on success;
// on any failure it leaves self->running = 0 for the caller (quartz.c) to
// detect and abort startup.
static void vm_do_create(VM *self, int ram_size, const char *image_name, void *rt_handle) {
    self->ram_size = ram_size;
    self->ram = (uint8_t *)calloc((size_t)ram_size, 1);
    if (!self->ram) {
        fprintf(stderr, "VM: failed to allocate %d bytes of RAM\n", ram_size);
        self->running = 0;
        return;
    }

    self->image_name = image_name ? strdup(image_name) : NULL;
    self->rt = rt_handle;
    self->pc = 0;

    rt_getres(self->rt, &self->screen_h, &self->screen_w);
    rt_getermres(self->rt, &self->term_w, &self->term_h);
    self->vsync_yield = 0;

    if (!load_program(image_name, &self->prog)) {
        fprintf(stderr, "VM: failed to load program\n");
        self->running = 0;
        return;
    }

    if (self->prog.data_len > 0) {
        if ((uint64_t)self->prog.data_addr + (uint64_t)self->prog.data_len > (uint64_t)self->ram_size) {
            fprintf(stderr,
                    "VM: data segment does not fit into RAM (data_addr=%u data_len=%u ram_size=%d)\n",
                    self->prog.data_addr, self->prog.data_len, self->ram_size);
            free(self->prog.code);
            free(self->prog.data);
            self->prog.code = NULL;
            self->prog.data = NULL;
            self->running = 0;
            return;
        }
    }

    // Keep an independent copy of the original program (from the file given
    // on the command line) for future "cold" resets (OP_RESET, Ctrl+Alt+R) -
    // self->prog may later be overwritten via OP_VMX, but self->boot_prog
    // stays untouched.
    self->boot_prog.entry = self->prog.entry;
    self->boot_prog.data_addr = self->prog.data_addr;

    self->boot_prog.code_len = self->prog.code_len;
    self->boot_prog.code = NULL;
    if (self->prog.code_len > 0) {
        self->boot_prog.code = (uint8_t *)malloc(self->prog.code_len);
        if (self->boot_prog.code) {
            memcpy(self->boot_prog.code, self->prog.code, self->prog.code_len);
        }
    }

    self->boot_prog.data_len = self->prog.data_len;
    self->boot_prog.data = NULL;
    if (self->prog.data_len > 0) {
        self->boot_prog.data = (uint8_t *)malloc(self->prog.data_len);
        if (self->boot_prog.data) {
            memcpy(self->boot_prog.data, self->prog.data, self->prog.data_len);
        }
    }

    self->reset(self, &self->prog);
    self->running = 1;

    if (self->prog.data_len > 0) {
        printf("VM: Loaded %u bytes to RAM (data segment @ 0x%08X)\n",
               self->prog.data_len, self->prog.data_addr);
    } else {
        printf("VM: No data segment found.\n");
    }

    printf("VM: created (ram_size=%d, image=\"%s\", code_len=%u, entry=%u)\n",
           self->ram_size, self->image_name,
           self->prog.code_len, self->prog.entry);
}

// Executes exactly one instruction: fetches the opcode byte at the current
// pc, advances pc past it, then decodes and executes whatever operands that
// opcode needs (each handler below fetches its own operands via the
// fetch_*() helpers). Called in a tight loop by quartz.c (vm.step()) up to
// INSTRUCTIONS_PER_FRAME times per host frame, or until the VM signals a
// vsync yield (see OP_VSYNC) or a fault/halt sets self->running = 0.
//
// Opcode-level comments below are intentionally light where the instruction
// set is already documented elsewhere; comments here mostly cover
// control-flow/lifecycle subtleties (OP_VMX's "warm restart" vs. OP_RESET's
// "cold reset", the SVFS text vs. binary-safe opcode pairs, etc.).
static void vm_do_step(VM *self) {
    if (!self->running) return;
    const Program *prog = &self->prog;

    uint8_t op;
    if (!fetch_u8(prog, self, &op)) {
        vm_fault(self, "PC out of scope (unexpected end of code)");
        return;
    }

    switch (op) {
        // ESSENTIALS
        case OP_NOP:
            break;

        case OP_HALT:
            self->running = 0;
            break;

        case OP_RAND: {
            int dst;
            uint32_t max;
            if (!fetch_reg(prog, self, &dst) || !fetch_u32(prog, self, &max)) {
                vm_fault(self, "OP_RAND: bad operand"); return;
            }
            self->reg[dst] = (int64_t)(int32_t)((uint32_t)rand() % (max ? max : 1U));
            break;
        }

        case OP_RAND_SEED: {
            int rseed;
            if (!fetch_reg(prog, self, &rseed)) {
                vm_fault(self, "OP_RAND_SEED: bad operand"); return;
            }
            srand((unsigned int)self->reg[rseed]);
            break;
        }

        case OP_WAIT: {
            int dst;
            if (!fetch_reg(prog, self, &dst)) {
                vm_fault(self, "OP_WAIT: bad operand"); return;
            }
            uint32_t time = (uint32_t)self->reg[dst];
            unsigned long long us = time;
            if (us > VM_WAIT_MAX_US) us = VM_WAIT_MAX_US;
            wait_us(us);
            break;
        }

        case OP_WAITI: {
            uint32_t time;
            if (!fetch_u32(prog, self, &time)) {
                vm_fault(self, "OP_WAITI: bad operand"); return;
            }
            unsigned long long us = time;
            if (us > VM_WAIT_MAX_US) us = VM_WAIT_MAX_US;
            wait_us(us);
            break;
        }

        case OP_TIME: {
            int dst;
            if (!fetch_reg(prog, self, &dst)) {
                vm_fault(self, "OP_TIME: bad operand"); return;
            }
            self->reg[dst] = now_ms();
            break;
        }

        case OP_TICK: {
            int dst;
            if (!fetch_reg(prog, self, &dst)) {
                vm_fault(self, "OP_TICK: bad operand"); return;
            }
            self->reg[dst] = (int64_t)self->tick_count;
            break;
        }

        case OP_RAMSIZE: {
            int dst;
            if (!fetch_reg(prog, self, &dst)) {
                vm_fault(self, "OP_RAMSIZE: bad operand"); return;
            }
            self->reg[dst] = (int64_t)self->ram_size;
            break;
        }

        case OP_VMX: {
            int raddr, rlen, rentry;
            if (!fetch_reg(prog, self, &raddr) || !fetch_reg(prog, self, &rlen) ||
                !fetch_reg(prog, self, &rentry)) {
                vm_fault(self, "OP_VMX: bad operand"); return;
            }
            uint32_t addr  = (uint32_t)self->reg[raddr];
            uint32_t len   = (uint32_t)self->reg[rlen];
            uint32_t entry = (uint32_t)self->reg[rentry];

            if (len == 0) {
                vm_fault(self, "OP_VMX: zero-length code segment"); return;
            }
            if (!ram_bounds_ok(self, addr, len)) {
                vm_fault(self, "OP_VMX: code segment out of RAM"); return;
            }
            if (entry >= len) {
                vm_fault(self, "OP_VMX: entrypoint out of code segment"); return;
            }

            // The code is COPIED from RAM into a separate buffer (the same
            // model as load_program() from a file, just the source is now
            // RAM) - the copy is necessary because the newly launched code
            // may, via OP_STORE, overwrite this very same RAM region it was
            // originally read from (e.g. when chaining several VMX calls in
            // a row); without the copy, that would corrupt the code
            // currently being executed (self->prog.code).
            uint8_t *new_code = (uint8_t *)malloc(len);
            if (!new_code) {
                vm_fault(self, "OP_VMX: out of memory"); return;
            }
            memcpy(new_code, &self->ram[addr], len);

            if (self->prog.code) free(self->prog.code);
            self->prog.code = new_code;
            self->prog.code_len = len;
            self->prog.entry = entry;

            // The original program's data segment (self->prog.data/data_len/
            // data_addr) is NOT touched and NOT replayed - unlike a .bin
            // header, OP_VMX has no data segment of its own; the new code
            // continues with whatever contents the previous run left behind
            // in RAM.
            //
            // "Warm restart": only registers/flags/stacks are zeroed and PC
            // jumps to the new entrypoint. Unlike self->reset() (used e.g.
            // for Ctrl+Alt+R in quartz.c), RAM is NOT cleared.
            memset(self->reg, 0, sizeof(self->reg));
            self->pc = entry;
            self->flag_zero = 0;
            self->flag_neg = 0;
            self->call_sp = 0;
            self->data_sp = 0;
            self->vsync_yield = 0;
            break;
        }

        case OP_RESET: {
            self->reset(self, &self->prog);
            self->running = 1;
            rt_screen_reset(self->rt);
            break;
        }

        case OP_VIDEO: {
            uint8_t mode;
            uint16_t fps;
            float scale;
            uint8_t cursor;
            if (!fetch_u8(prog, self, &mode) || !fetch_u16(prog, self, &fps) ||
                !fetch_f32(prog, self, &scale) || !fetch_u8(prog, self, &cursor)) {
                vm_fault(self, "OP_VIDEO: bad operand"); return;
            }
            if (!rt_video(self->rt, mode, fps, scale, cursor)) {
                vm_fault(self, "OP_VIDEO: invalid video mode (or out of memory while switching)"); return;
            }
            break;
        }

        case OP_VIDEO_INFO: {
            int rmode, rfps, rscale, rcursor;
            if (!fetch_reg(prog, self, &rmode) || !fetch_reg(prog, self, &rfps) ||
                !fetch_reg(prog, self, &rscale) || !fetch_reg(prog, self, &rcursor)) {
                vm_fault(self, "OP_VIDEO_INFO: bad operand"); return;
            }
            uint8_t mode;
            uint16_t fps;
            float scale;
            uint8_t cursor;
            rt_video_info(self->rt, &mode, &fps, &scale, &cursor);
            self->reg[rmode]   = (int64_t)mode;
            self->reg[rfps]    = (int64_t)fps;
            self->reg[rscale]  = f32_to_raw(scale);
            self->reg[rcursor] = (int64_t)cursor;
            break;
        }

        // DEBUG
        case OP_PRINT: {
            int r; ValType t;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &r)) {
                vm_fault(self, "OP_PRINT: bad operand"); return;
            }
            int64_t raw = type_normalize(self->reg[r], t);
            if (t == TYPE_F32) {
                printf("[OP_PRINT] R%d (f32) = %.9g\n", r, (double)f32_from_bits(raw));
            } else if (t == TYPE_F64) {
                printf("[OP_PRINT] R%d (f64) = %.17g\n", r, f64_from_bits(raw));
            } else if (is_unsigned_type(t)) {
                printf("[OP_PRINT] R%d (%s) = %" PRIu64 "\n", r, type_name(t), (uint64_t)raw);
            } else {
                printf("[OP_PRINT] R%d (%s) = %" PRId64 "\n", r, type_name(t), raw);
            }
            fflush(stdout);
            break;
        }

        // DATA MOVE
        case OP_MOV: {
            int dst, src;
            if (!fetch_reg(prog, self, &dst) || !fetch_reg(prog, self, &src)) {
                vm_fault(self, "MOV: bad operand"); return;
            }
            self->reg[dst] = self->reg[src];
            break;
        }

        case OP_MOVI: {
            int dst; ValType t; int64_t imm;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &dst) ||
                !fetch_imm(prog, self, t, &imm)) {
                vm_fault(self, "MOVI: bad operand"); return;
            }
            self->reg[dst] = type_normalize(imm, t);
            break;
        }

        // ARITHMETIC (REGISTER, REGISTER)
        case OP_ADD: 
        case OP_SUB: 
        case OP_MUL: 
        case OP_DIV: 
        case OP_MOD:
        case OP_AND: 
        case OP_OR:
        case OP_XOR:
        case OP_SHL:
        case OP_SHR: {
            int dst, src; ValType t;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &dst) ||
                !fetch_reg(prog, self, &src)) {
                vm_fault(self, "arithmetic instruction: bad operand"); return;
            }
            if (is_float_type(t) &&
                (op == OP_AND || op == OP_OR || op == OP_XOR || op == OP_SHL || op == OP_SHR)) {
                vm_fault(self, "bitwise operation not supported for float type"); return;
            }
            int64_t a = type_normalize(self->reg[dst], t);
            int64_t b = type_normalize(self->reg[src], t);
            switch (op) {
                case OP_ADD: self->reg[dst] = type_add(a, b, t); break;
                case OP_SUB: self->reg[dst] = type_sub(a, b, t); break;
                case OP_MUL: self->reg[dst] = type_mul(a, b, t); break;
                case OP_DIV:
                    if (!is_float_type(t) && b == 0) { vm_fault(self, "OP_DIV: division by zero"); return; }
                    self->reg[dst] = type_div(a, b, t);
                    break;
                case OP_MOD:
                    if (!is_float_type(t) && b == 0) { vm_fault(self, "OP_MOD: division by zero"); return; }
                    self->reg[dst] = type_mod(a, b, t);
                    break;
                case OP_AND: self->reg[dst] = type_normalize(a & b, t); break;
                case OP_OR:  self->reg[dst] = type_normalize(a | b, t); break;
                case OP_XOR: self->reg[dst] = type_normalize(a ^ b, t); break;
                case OP_SHL: self->reg[dst] = type_shl(a, b, t); break;
                case OP_SHR: self->reg[dst] = type_shr(a, b, t); break;
                default: break;
            }
            break;
        }

        // ARITHMETIC (REGISTER, IMMEDIATE)
        case OP_ADDI:
        case OP_SUBI: {
            int dst; ValType t; int64_t imm;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &dst) ||
                !fetch_imm(prog, self, t, &imm)) {
                vm_fault(self, "OP_ADDI/OP_SUBI: bad operand"); return;
            }
            int64_t a  = type_normalize(self->reg[dst], t);
            int64_t im = type_normalize(imm, t);
            self->reg[dst] = (op == OP_ADDI) ? type_add(a, im, t) : type_sub(a, im, t);
            break;
        }

        case OP_INC:
        case OP_DEC: {
            ValType t; int dst;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &dst)) {
                vm_fault(self, "OP_INC/OP_DEC: bad operand"); return;
            }
            int64_t a = type_normalize(self->reg[dst], t);
            int64_t one;
            if (t == TYPE_F32) one = f32_to_raw(1.0f);
            else if (t == TYPE_F64) one = f64_to_raw(1.0);
            else one = 1;
            self->reg[dst] = (op == OP_INC) ? type_add(a, one, t) : type_sub(a, one, t);
            break;
        }

        case OP_NOT: {
            ValType t; int dst;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &dst)) {
                vm_fault(self, "NOT: bad operand"); return;
            }
            if (is_float_type(t)) { vm_fault(self, "NOT: not supported for float type"); return; }
            int64_t a = type_normalize(self->reg[dst], t);
            self->reg[dst] = type_normalize(~a, t);
            break;
        }

        // COMPARATION
        case OP_CMP: {
            int a, b; ValType t;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &a) ||
                !fetch_reg(prog, self, &b)) {
                vm_fault(self, "OP_CMP: bad operand"); return;
            }
            do_compare(self, self->reg[a], self->reg[b], t);
            break;
        }

        case OP_CMPI: {
            int a; ValType t; int64_t imm;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &a) ||
                !fetch_imm(prog, self, t, &imm)) {
                vm_fault(self, "OP_CMPI: bad operand"); return;
            }
            do_compare(self, self->reg[a], imm, t);
            break;
        }

        // JUMPS
        case OP_JMP: {
            uint32_t addr;
            if (!fetch_u32(prog, self, &addr)) { vm_fault(self, "JMP: bad operand"); return; }
            self->pc = addr;
            break;
        }

        case OP_JZ:
        case OP_JNZ:
        case OP_JG:
        case OP_JL:
        case OP_JGE:
        case OP_JLE: {
            uint32_t addr;
            if (!fetch_u32(prog, self, &addr)) { vm_fault(self, "conditional jump (imm): bad operand"); return; }
            int take = 0;
            switch (op) {
                case OP_JZ:  take = self->flag_zero; break;
                case OP_JNZ: take = !self->flag_zero; break;
                case OP_JG:  take = (!self->flag_zero && !self->flag_neg); break;
                case OP_JL:  take = self->flag_neg; break;
                case OP_JGE: take = (!self->flag_neg); break;
                case OP_JLE: take = (self->flag_zero || self->flag_neg); break;
                default: break;
            }
            if (take) self->pc = addr;
            break;
        }

        // JUMPS (REGISTER)
        case OP_JMPR: {
            int raddr;
            if (!fetch_reg(prog, self, &raddr)) {
                vm_fault(self, "OP_JMPR: bad operand"); return;
            }
            self->pc = (uint32_t)self->reg[raddr];
            break;
        }

        case OP_JZR:
        case OP_JNZR:
        case OP_JGR:
        case OP_JLR:
        case OP_JGER:
        case OP_JLER: {
            int raddr;
            if (!fetch_reg(prog, self, &raddr)) {
                vm_fault(self, "conditional jump (reg): bad operand"); return;
            }
            int take = 0;
            switch (op) {
                case OP_JZR:  take = self->flag_zero; break;
                case OP_JNZR: take = !self->flag_zero; break;
                case OP_JGR:  take = (!self->flag_zero && !self->flag_neg); break;
                case OP_JLR:  take = self->flag_neg; break;
                case OP_JGER: take = (!self->flag_neg); break;
                case OP_JLER: take = (self->flag_zero || self->flag_neg); break;
                default: break;
            }
            if (take) self->pc = (uint32_t)self->reg[raddr];
            break;
        }

        // CALL FUNCTIONS
        case OP_CALLR: {
            int raddr;
            if (!fetch_reg(prog, self, &raddr)) {
                vm_fault(self, "OP_CALLR: bad operand"); return;
            }
            if (self->call_sp >= CALL_STACK_MAX) { vm_fault(self, "OP_CALLR: call-stack overflow"); return; }
            self->call_stack[self->call_sp++] = self->pc;
            self->pc = (uint32_t)self->reg[raddr];
            break;
        }
        
        case OP_CALL: {
            uint32_t addr;
            if (!fetch_u32(prog, self, &addr)) { vm_fault(self, "OP_CALL: bad operand"); return; }
            if (self->call_sp >= CALL_STACK_MAX) { vm_fault(self, "OP_CALL: call-stack overflow"); return; }
            self->call_stack[self->call_sp++] = self->pc;
            self->pc = addr;
            break;
        }

        case OP_RET: {
            if (self->call_sp <= 0) { vm_fault(self, "OP_RET: call-stack underflow"); return; }
            self->pc = self->call_stack[--self->call_sp];
            break;
        }

        // DATA STACK
        case OP_PUSH: {
            int r;
            if (!fetch_reg(prog, self, &r)) { vm_fault(self, "OP_PUSH: bad operand"); return; }
            if (self->data_sp >= DATA_STACK_MAX) { vm_fault(self, "OP_PUSH: stack overflow"); return; }
            self->data_stack[self->data_sp++] = self->reg[r];
            break;
        }

        case OP_POP: {
            int r;
            if (!fetch_reg(prog, self, &r)) { vm_fault(self, "OP_POP: bad operand"); return; }
            if (self->data_sp <= 0) { vm_fault(self, "OP_POP: stack underflow"); return; }
            self->reg[r] = self->data_stack[--self->data_sp];
            break;
        }

        // MEMORY ACCESS
        case OP_LOAD: {
            int dst; ValType t; uint32_t addr;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &dst) ||
                !fetch_u32(prog, self, &addr)) {
                vm_fault(self, "OP_LOAD: bad operand"); return;
            }
            int sz = type_size(t);
            if (!ram_bounds_ok(self, addr, (uint32_t)sz)) { vm_fault(self, "OP_LOAD: address out of RAM"); return; }
            uint64_t raw = read_sized_le(&self->ram[addr], sz);
            self->reg[dst] = type_normalize((int64_t)raw, t);
            break;
        }

        case OP_STORE: {
            ValType t; uint32_t addr; int src;
            if (!fetch_type(prog, self, &t) || !fetch_u32(prog, self, &addr) ||
                !fetch_reg(prog, self, &src)) {
                vm_fault(self, "OP_STORE: bad operand"); return;
            }
            int sz = type_size(t);
            if (!ram_bounds_ok(self, addr, (uint32_t)sz)) { vm_fault(self, "OP_STORE: address out of RAM"); return; }
            int64_t v = type_normalize(self->reg[src], t);
            write_sized_le(&self->ram[addr], (uint64_t)v, sz);
            break;
        }

        case OP_LOADR: {
            int dst, addr_reg; ValType t;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &dst) ||
                !fetch_reg(prog, self, &addr_reg)) {
                vm_fault(self, "OP_LOADR: bad operand"); return;
            }
            uint32_t addr = (uint32_t)self->reg[addr_reg];
            int sz = type_size(t);
            if (!ram_bounds_ok(self, addr, (uint32_t)sz)) { vm_fault(self, "OP_LOADR: address out of RAM"); return; }
            uint64_t raw = read_sized_le(&self->ram[addr], sz);
            self->reg[dst] = type_normalize((int64_t)raw, t);
            break;
        }

        case OP_STORER: {
            int addr_reg, src; ValType t;
            if (!fetch_type(prog, self, &t) || !fetch_reg(prog, self, &addr_reg) ||
                !fetch_reg(prog, self, &src)) {
                vm_fault(self, "OP_STORER: bad operand"); return;
            }
            uint32_t addr = (uint32_t)self->reg[addr_reg];
            int sz = type_size(t);
            if (!ram_bounds_ok(self, addr, (uint32_t)sz)) { vm_fault(self, "OP_STORER: address out of RAM"); return; }
            int64_t v = type_normalize(self->reg[src], t);
            write_sized_le(&self->ram[addr], (uint64_t)v, sz);
            break;
        }

        // MEMORY (BLOCK)
        case OP_MEMCPY: {
            int rdst, rsrc, rlen;
            if (!fetch_reg(prog, self, &rdst) || !fetch_reg(prog, self, &rsrc) ||
                !fetch_reg(prog, self, &rlen)) {
                vm_fault(self, "OP_MEMCPY: bad operand"); return;
            }
            uint32_t dst = (uint32_t)self->reg[rdst];
            uint32_t src = (uint32_t)self->reg[rsrc];
            uint32_t len = (uint32_t)self->reg[rlen];
            if (!ram_bounds_ok(self, dst, len) || !ram_bounds_ok(self, src, len)) {
                vm_fault(self, "OP_MEMCPY: address out of RAM"); return;
            }
            // memmove (not memcpy) - the source and destination regions may
            // overlap (e.g. shifting a buffer in RAM by a few bytes), and
            // memmove is safe for that case, unlike memcpy.
            if (len > 0) memmove(&self->ram[dst], &self->ram[src], len);
            break;
        }

        case OP_MEMSET: {
            int raddr, rval, rlen;
            if (!fetch_reg(prog, self, &raddr) || !fetch_reg(prog, self, &rval) ||
                !fetch_reg(prog, self, &rlen)) {
                vm_fault(self, "OP_MEMSET: bad operand"); return;
            }
            uint32_t addr = (uint32_t)self->reg[raddr];
            uint8_t  val  = (uint8_t)self->reg[rval];
            uint32_t len  = (uint32_t)self->reg[rlen];
            if (!ram_bounds_ok(self, addr, len)) {
                vm_fault(self, "OP_MEMSET: address out of RAM"); return;
            }
            if (len > 0) memset(&self->ram[addr], val, len);
            break;
        }

        // SOUND
        case OP_BEEP: {
            rt_bell(self->rt);
            break;
        }

        case OP_SOUND: {
            int raddr, rlen;
            if (!fetch_reg(prog, self, &raddr) || !fetch_reg(prog, self, &rlen)) {
                vm_fault(self, "OP_SOUND: bad operand"); return;
            }
            uint32_t addr = (uint32_t)self->reg[raddr];
            uint32_t len  = (uint32_t)self->reg[rlen];
            if (len == 0) break;
            if (!ram_bounds_ok(self, addr, len)) {
                vm_fault(self, "OP_SOUND: address out of RAM"); return;
            }
            rt_sound(self->rt, &self->ram[addr], len);
            break;
        }

        // GFX
        case OP_GFX_FRAME: {
            int rx, ry, rw, rh, raddr;
            if (!fetch_reg(prog, self, &rx) || !fetch_reg(prog, self, &ry) ||
                !fetch_reg(prog, self, &rw) || !fetch_reg(prog, self, &rh) ||
                !fetch_reg(prog, self, &raddr)) {
                vm_fault(self, "OP_GFX_FRAME: bad operand"); return;
            }
            uint16_t x = (uint16_t)self->reg[rx];
            uint16_t y = (uint16_t)self->reg[ry];
            uint16_t w = (uint16_t)self->reg[rw];
            uint16_t h = (uint16_t)self->reg[rh];
            uint32_t addr = (uint32_t)self->reg[raddr];
            uint64_t need = (uint64_t)w * (uint64_t)h * 4ull;

            if (!ram_bounds_ok(self, addr, need)) {
                vm_fault(self, "OP_GFX_FRAME: buffer out of RAM"); return;
            }
            rt_frame(self->rt, &self->ram[addr], x, y, h, w);
            break;
        }

        case OP_GFX_PIX: {
            int rx, ry, rc;
            if (!fetch_reg(prog, self, &rx) || !fetch_reg(prog, self, &ry) ||
                !fetch_reg(prog, self, &rc)) {
                vm_fault(self, "OP_GFX_PIX: bad operand"); return;
            }
            uint16_t x = (uint16_t)self->reg[rx];
            uint16_t y = (uint16_t)self->reg[ry];
            uint32_t c = (uint32_t)self->reg[rc];
            uint8_t r = (uint8_t)(c & 0xFF);
            uint8_t g = (uint8_t)((c >> 8) & 0xFF);
            uint8_t b = (uint8_t)((c >> 16) & 0xFF);
            uint8_t a = (uint8_t)((c >> 24) & 0xFF);
            rt_pix(self->rt, x, y, r, g, b, a);
            break;
        }

        case OP_GFX_RAW: {
            int raddr, rlen, roff;
            if (!fetch_reg(prog, self, &raddr) || !fetch_reg(prog, self, &rlen) ||
                !fetch_reg(prog, self, &roff)) {
                vm_fault(self, "OP_GFX_RAW: bad operand"); return;
            }
            uint32_t addr = (uint32_t)self->reg[raddr];
            uint32_t len  = (uint32_t)self->reg[rlen];
            uint32_t off  = (uint32_t)self->reg[roff];

            if (!ram_bounds_ok(self, addr, len)) {
                vm_fault(self, "OP_GFX_RAW: buffer out of RAM"); return;
            }
            uint32_t fb_size = (uint32_t)self->screen_w * (uint32_t)self->screen_h * 4u;
            if ((uint64_t)off + (uint64_t)len > (uint64_t)fb_size) {
                vm_fault(self, "OP_GFX_RAW: write out of screen bounds"); return;
            }
            rt_raw(self->rt, &self->ram[addr], len, off);
            break;
        }

        case OP_GFX_CLEAR: {
            rt_clear(self->rt);
            break;
        }

        case OP_GFX_CLEARC: {
            int rc;
            if (!fetch_reg(prog, self, &rc)) {
                vm_fault(self, "OP_GFX_CLEARC: bad operand"); return;
            }
            uint32_t c = (uint32_t)self->reg[rc];
            uint8_t r = (uint8_t)(c & 0xFF);
            uint8_t g = (uint8_t)((c >> 8) & 0xFF);
            uint8_t b = (uint8_t)((c >> 16) & 0xFF);
            uint8_t a = (uint8_t)((c >> 24) & 0xFF);
            rt_clear_color(self->rt, r, g, b, a);
            break;
        }

        case OP_GFX_GETRES: {
            int rh, rw;
            if (!fetch_reg(prog, self, &rh) || !fetch_reg(prog, self, &rw)) {
                vm_fault(self, "OP_GFX_GETRES: bad operand"); return;
            }
            self->reg[rh] = (int64_t)self->screen_h;
            self->reg[rw] = (int64_t)self->screen_w;
            break;
        }

        case OP_VSYNC: {
            uint32_t period_us;
            if (!fetch_u32(prog, self, &period_us)) {
                vm_fault(self, "OP_VSYNC: bad operand"); return;
            }
            (void)period_us;
            self->tick_count++;
            self->vsync_yield = 1;
            break;
        }

        // TEXT
        case OP_TXT_CHAR: {
            int rx, ry, rcp, rfg, rbg;
            if (!fetch_reg(prog, self, &rx) || !fetch_reg(prog, self, &ry) ||
                !fetch_reg(prog, self, &rcp) || !fetch_reg(prog, self, &rfg) ||
                !fetch_reg(prog, self, &rbg)) {
                vm_fault(self, "OP_TXT_CHAR: bad operand"); return;
            }
            uint16_t x = (uint16_t)self->reg[rx];
            uint16_t y = (uint16_t)self->reg[ry];
            uint16_t cp = (uint16_t)self->reg[rcp];
            uint32_t fgc = (uint32_t)self->reg[rfg];
            uint32_t bgc = (uint32_t)self->reg[rbg];
            rt_color fg = { (uint8_t)(fgc & 0xFF), (uint8_t)((fgc>>8)&0xFF), (uint8_t)((fgc>>16)&0xFF), (uint8_t)((fgc>>24)&0xFF) };
            rt_color bg = { (uint8_t)(bgc & 0xFF), (uint8_t)((bgc>>8)&0xFF), (uint8_t)((bgc>>16)&0xFF), (uint8_t)((bgc>>24)&0xFF) };
            rt_char(self->rt, x, y, cp, fg, bg);
            break;
        }

        case OP_TXT_FRAME: {
            int rx, ry, rw, rh, raddr;
            if (!fetch_reg(prog, self, &rx) || !fetch_reg(prog, self, &ry) ||
                !fetch_reg(prog, self, &rw) || !fetch_reg(prog, self, &rh) ||
                !fetch_reg(prog, self, &raddr)) {
                vm_fault(self, "OP_TXT_FRAME: bad operand"); return;
            }
            uint16_t x = (uint16_t)self->reg[rx];
            uint16_t y = (uint16_t)self->reg[ry];
            uint16_t w = (uint16_t)self->reg[rw];
            uint16_t h = (uint16_t)self->reg[rh];
            uint32_t addr = (uint32_t)self->reg[raddr];
            uint64_t need = (uint64_t)w * (uint64_t)h * 10ull; // cp(2)+fg(4)+bg(4) na bunku

            if (!ram_bounds_ok(self, addr, need)) {
                vm_fault(self, "OP_TXT_FRAME: buffer out of RAM"); return;
            }
            rt_txtframe(self->rt, &self->ram[addr], x, y, h, w);
            break;
        }

        case OP_TXT_GETRES: {
            int rrows, rcols;
            if (!fetch_reg(prog, self, &rrows) || !fetch_reg(prog, self, &rcols)) {
                vm_fault(self, "OP_TXT_GETRES: bad operand"); return;
            }
            self->reg[rrows] = (int64_t)self->term_h;
            self->reg[rcols] = (int64_t)self->term_w;
            break;
        }

        case OP_TXT_CLEAR: {
            rt_txtclear(self->rt);
            break;
        }

        case OP_TXT_CURSET: {
            int rx, ry;
            if (!fetch_reg(prog, self, &rx) || !fetch_reg(prog, self, &ry)) {
                vm_fault(self, "OP_CUR_SET: bad operand"); return;
            }
            rt_curset(self->rt, (uint16_t)self->reg[rx], (uint16_t)self->reg[ry]);
            break;
        }

        case OP_TXT_CURGET: {
            int rx, ry;
            if (!fetch_reg(prog, self, &rx) || !fetch_reg(prog, self, &ry)) {
                vm_fault(self, "OP_CUR_GET: bad operand"); return;
            }
            uint16_t x, y;
            rt_curget(self->rt, &x, &y);
            self->reg[rx] = (int64_t)x;
            self->reg[ry] = (int64_t)y;
            break;
        }

        case OP_TXT_CURENA: {
            int ren;
            if (!fetch_reg(prog, self, &ren)) {
                vm_fault(self, "OP_CUR_ENA: bad operand"); return;
            }
            rt_curena(self->rt, (uint8_t)self->reg[ren]);//
            break;
        }

        // INPUT
        case OP_IN_MOUSE: {
            int rx, ry, rb;
            if (!fetch_reg(prog, self, &rx) || !fetch_reg(prog, self, &ry) ||
                !fetch_reg(prog, self, &rb)) {
                vm_fault(self, "OP_IN_MOUSE: bad operand"); return;
            }
            uint16_t x, y; uint8_t b;
            rt_mouse(self->rt, &x, &y, &b);
            self->reg[rx] = (int64_t)x;
            self->reg[ry] = (int64_t)y;
            self->reg[rb] = (int64_t)b;
            break;
        }

        case OP_IN_KEY: {
            int rcount, raddr;
            if (!fetch_reg(prog, self, &rcount) || !fetch_reg(prog, self, &raddr)) {
                vm_fault(self, "OP_IN_KEY: bad operand"); return;
            }
            uint32_t addr = (uint32_t)self->reg[raddr];
            if (!ram_bounds_ok(self, addr, 8)) {
                vm_fault(self, "OP_IN_KEY: buffer out of RAM"); return;
            }
            int count; int keys[4] = {0,0,0,0};
            rt_keyboard(self->rt, &count, keys, 4);
            self->reg[rcount] = (int64_t)count;
            for (uint32_t i = 0; i < 4; i++) {
                uint16_t k = (uint16_t)keys[i];
                self->ram[addr + i*2]     = (uint8_t)(k & 0xFF);
                self->ram[addr + i*2 + 1] = (uint8_t)(k >> 8);
            }
            break;
        }

        case OP_IN_POLL: {
            int rmx, rmy, rbtn, rkc, raddr;
            if (!fetch_reg(prog, self, &rmx) || !fetch_reg(prog, self, &rmy) ||
                !fetch_reg(prog, self, &rbtn) || !fetch_reg(prog, self, &rkc) ||
                !fetch_reg(prog, self, &raddr)) {
                vm_fault(self, "OP_IN_POLL: bad operand"); return;
            }
            uint32_t addr = (uint32_t)self->reg[raddr];
            if (!ram_bounds_ok(self, addr, 8)) {
                vm_fault(self, "OP_IN_POLL: buffer out of RAM"); return;
            }
            uint16_t mx, my; uint8_t mb; int kc; uint16_t keys[4] = {0,0,0,0};
            rt_poll(self->rt, &mx, &my, &mb, &kc, keys);
            self->reg[rmx]  = (int64_t)mx;
            self->reg[rmy]  = (int64_t)my;
            self->reg[rbtn] = (int64_t)mb;
            self->reg[rkc]  = (int64_t)kc;
            for (uint32_t i = 0; i < 4; i++) {
                self->ram[addr + i*2]     = (uint8_t)(keys[i] & 0xFF);
                self->ram[addr + i*2 + 1] = (uint8_t)(keys[i] >> 8);
            }
            break;
        }

        case OP_IN_TXT_MOUSE: {
            int rcol, rrow, rbtn;
            if (!fetch_reg(prog, self, &rcol) || !fetch_reg(prog, self, &rrow) ||
                !fetch_reg(prog, self, &rbtn)) {
                vm_fault(self, "OP_IN_TXT_MOUSE: bad operand"); return;
            }
            uint16_t col, row; uint8_t b;
            rt_txt_mouse(self->rt, &col, &row, &b);
            self->reg[rcol] = (int64_t)col;
            self->reg[rrow] = (int64_t)row;
            self->reg[rbtn] = (int64_t)b;
            break;
        }

        case OP_IN_TXT_POLL: {
            int rcol, rrow, rbtn, rkc, raddr;
            if (!fetch_reg(prog, self, &rcol) || !fetch_reg(prog, self, &rrow) ||
                !fetch_reg(prog, self, &rbtn) || !fetch_reg(prog, self, &rkc) ||
                !fetch_reg(prog, self, &raddr)) {
                vm_fault(self, "OP_IN_TXT_POLL: bad operand"); return;
            }
            uint32_t addr = (uint32_t)self->reg[raddr];
            if (!ram_bounds_ok(self, addr, 8)) {
                vm_fault(self, "OP_IN_TXT_POLL: buffer out of RAM"); return;
            }
            uint16_t col, row; uint8_t mb; int kc; uint16_t keys[4] = {0,0,0,0};
            rt_txt_poll(self->rt, &col, &row, &mb, &kc, keys);
            self->reg[rcol] = (int64_t)col;
            self->reg[rrow] = (int64_t)row;
            self->reg[rbtn] = (int64_t)mb;
            self->reg[rkc]  = (int64_t)kc;
            for (uint32_t i = 0; i < 4; i++) {
                self->ram[addr + i*2]     = (uint8_t)(keys[i] & 0xFF);
                self->ram[addr + i*2 + 1] = (uint8_t)(keys[i] >> 8);
            }
            break;
        }

        // DISK
        case OP_SVFS_MKDIR: {
            int rname;
            if (!fetch_reg(prog, self, &rname)) {
                vm_fault(self, "OP_SVFS_MKDIR: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_MKDIR: name not NUL-terminated within RAM"); return; }
            svfs_mkdir(name);
            break;
        }

        case OP_SVFS_RMDIR: {
            int rname, rrec;
            if (!fetch_reg(prog, self, &rname) || !fetch_reg(prog, self, &rrec)) {
                vm_fault(self, "OP_SVFS_RMDIR: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_RMDIR: name not NUL-terminated within RAM"); return; }
            int recursive = ((uint64_t)self->reg[rrec] & 0xFFu) != 0;
            svfs_rmdir(name, recursive);
            break;
        }

        case OP_SVFS_CREATE: {
            int rname;
            if (!fetch_reg(prog, self, &rname)) {
                vm_fault(self, "OP_SVFS_CREATE: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_CREATE: name not NUL-terminated within RAM"); return; }
            svfs_create(name);
            break;
        }

        case OP_SVFS_RM: {
            int rname;
            if (!fetch_reg(prog, self, &rname)) {
                vm_fault(self, "OP_SVFS_RM: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_RM: name not NUL-terminated within RAM"); return; }
            svfs_rm(name);
            break;
        }

        case OP_SVFS_OPEN: {
            int rname;
            if (!fetch_reg(prog, self, &rname)) {
                vm_fault(self, "OP_SVFS_OPEN: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_OPEN: name not NUL-terminated within RAM"); return; }
            svfs_open(name);
            break;
        }

        case OP_SVFS_CLOSE: {
            int rname;
            if (!fetch_reg(prog, self, &rname)) {
                vm_fault(self, "OP_SVFS_CLOSE: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_CLOSE: name not NUL-terminated within RAM"); return; }
            svfs_close(name);
            break;
        }

        // Text (NUL-terminated) write variant. svfs_write() itself is void,
        // but is nothing more than svfs_write_bytes(name, content,
        // strlen(content)) (see svfs.c) - calling write_bytes directly here
        // therefore changes no behavior, it just additionally yields a
        // status for the VM program that the void wrapper would otherwise
        // discard.
        case OP_SVFS_WRITE: {
            int rname, rdata, rstatus;
            if (!fetch_reg(prog, self, &rname) || !fetch_reg(prog, self, &rdata) ||
                !fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_SVFS_WRITE: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_WRITE: name not NUL-terminated within RAM"); return; }

            uint32_t data_addr = (uint32_t)self->reg[rdata];
            size_t data_len;
            if (!vm_ram_cstr_len(self, data_addr, (uint32_t)self->ram_size, &data_len)) {
                vm_fault(self, "OP_SVFS_WRITE: content not NUL-terminated within RAM"); return;
            }
            self->reg[rstatus] = svfs_write_bytes(name, &self->ram[data_addr], data_len);
            break;
        }

        // Text read variant: reads the file into RAM and ALWAYS appends a
        // trailing '\0' (1 byte of buf_size is reserved for it) - unlike
        // OP_SVFS_READ_BYTES, which copies exactly buf_size bytes with no
        // terminator.
        case OP_SVFS_READ: {
            int rname, rbuf, rbufsize, rdstlen, rstatus;
            if (!fetch_reg(prog, self, &rname) || !fetch_reg(prog, self, &rbuf) ||
                !fetch_reg(prog, self, &rbufsize) || !fetch_reg(prog, self, &rdstlen) ||
                !fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_SVFS_READ: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_READ: name not NUL-terminated within RAM"); return; }

            uint32_t buf_addr = (uint32_t)self->reg[rbuf];
            uint32_t buf_size = (uint32_t)self->reg[rbufsize];
            if (!ram_bounds_ok(self, buf_addr, buf_size)) {
                vm_fault(self, "OP_SVFS_READ: buffer out of RAM"); return;
            }
            uint32_t text_cap = (buf_size > 0) ? buf_size - 1 : 0; // 1 byte reserved for '\0'

            size_t out_len = 0;
            int ok = svfs_read_bytes(name, &self->ram[buf_addr], text_cap, &out_len);
            if (ok) {
                if (buf_size > 0) self->ram[buf_addr + out_len] = 0; // out_len <= text_cap = buf_size-1
            } else {
                out_len = 0;
            }
            self->reg[rdstlen]  = (int64_t)out_len;
            self->reg[rstatus]  = ok;
            break;
        }

        // Binary-safe write variant ("saving a raw block from RAM as a
        // file"): writes exactly `len` bytes, including any embedded zero
        // bytes - see svfs_write_bytes().
        case OP_SVFS_WRITE_BYTES: {
            int rname, rdata, rlen, rstatus;
            if (!fetch_reg(prog, self, &rname) || !fetch_reg(prog, self, &rdata) ||
                !fetch_reg(prog, self, &rlen) || !fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_SVFS_WRITE_BYTES: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_WRITE_BYTES: name not NUL-terminated within RAM"); return; }

            uint32_t data_addr = (uint32_t)self->reg[rdata];
            uint32_t len       = (uint32_t)self->reg[rlen];
            if (!ram_bounds_ok(self, data_addr, len)) {
                vm_fault(self, "OP_SVFS_WRITE_BYTES: buffer out of RAM"); return;
            }
            self->reg[rstatus] = svfs_write_bytes(name, &self->ram[data_addr], len);
            break;
        }

        // Binary-safe read variant ("loading a file into a raw block"):
        // copies up to buf_size bytes exactly as stored, WITHOUT adding a
        // terminator - see svfs_read_bytes().
        case OP_SVFS_READ_BYTES: {
            int rname, rbuf, rbufsize, rdstlen, rstatus;
            if (!fetch_reg(prog, self, &rname) || !fetch_reg(prog, self, &rbuf) ||
                !fetch_reg(prog, self, &rbufsize) || !fetch_reg(prog, self, &rdstlen) ||
                !fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_SVFS_READ_BYTES: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_READ_BYTES: name not NUL-terminated within RAM"); return; }

            uint32_t buf_addr = (uint32_t)self->reg[rbuf];
            uint32_t buf_size = (uint32_t)self->reg[rbufsize];
            if (!ram_bounds_ok(self, buf_addr, buf_size)) {
                vm_fault(self, "OP_SVFS_READ_BYTES: buffer out of RAM"); return;
            }
            size_t out_len = 0;
            int ok = svfs_read_bytes(name, &self->ram[buf_addr], buf_size, &out_len);
            if (!ok) out_len = 0;
            self->reg[rdstlen] = (int64_t)out_len;
            self->reg[rstatus] = ok;
            break;
        }

        // Changes the current directory. The name may also be "." / ".." /
        // "/" - all handled the same way as svfs_cd(). status_dst gets 1/0.
        case OP_SVFS_CD: {
            int rname, rstatus;
            if (!fetch_reg(prog, self, &rname) || !fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_SVFS_CD: bad operand"); return;
            }
            const char *name = vm_ram_cstr(self, (uint32_t)self->reg[rname], SVFS_VM_NAME_SCAN_MAX);
            if (!name) { vm_fault(self, "OP_SVFS_CD: name not NUL-terminated within RAM"); return; }
            self->reg[rstatus] = svfs_cd(name);
            break;
        }

        // Reads the contents of the current directory and serializes it
        // into RAM as an array of fixed-size records - see the comment at
        // OP_SVFS_DIR in vm.h for the exact per-record format.
        case OP_SVFS_DIR: {
            int rbuf, rmax, rcount, rstatus;
            if (!fetch_reg(prog, self, &rbuf) || !fetch_reg(prog, self, &rmax) ||
                !fetch_reg(prog, self, &rcount) || !fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_SVFS_DIR: bad operand"); return;
            }
            uint32_t buf_addr    = (uint32_t)self->reg[rbuf];
            uint32_t max_entries = (uint32_t)self->reg[rmax];

            // Host-side scratch buffer for svfs_dir() - comfortably above
            // SVFS's own cap on entries per directory (a directory is a
            // single fixed block, see svfs_internal.h), so all of them
            // always fit.
            SvfsDirEntry tmp[SVFS_VM_DIR_HOST_CAP];
            int found = svfs_dir(tmp, SVFS_VM_DIR_HOST_CAP);
            if (found < 0) {
                // No filesystem attached/initialized.
                self->reg[rcount]  = 0;
                self->reg[rstatus] = 0;
                break;
            }

            uint32_t n = (uint32_t)found;
            if (n > max_entries) n = max_entries;

            uint64_t needed = (uint64_t)n * SVFS_VM_DIR_RECORD_SIZE;
            if (!ram_bounds_ok(self, buf_addr, needed)) {
                vm_fault(self, "OP_SVFS_DIR: buffer out of RAM"); return;
            }

            for (uint32_t i = 0; i < n; i++) {
                uint8_t *rec = &self->ram[buf_addr + (uint64_t)i * SVFS_VM_DIR_RECORD_SIZE];
                memset(rec, 0, SVFS_VM_DIR_RECORD_SIZE);

                // [0..43]: NUL-terminated "name.ext" (or just "name").
                if (tmp[i].ext[0] != '\0') {
                    snprintf((char *)rec, 44, "%s.%s", tmp[i].name, tmp[i].ext);
                } else {
                    snprintf((char *)rec, 44, "%s", tmp[i].name);
                }
                // [44]: 0 = file, 1 = directory, 2 = hidden file.
                rec[44] = tmp[i].is_dir ? 1u : (tmp[i].is_hidden ? 2u : 0u);
                // [48..51] / [52..55] / [56..59]: size / ts_created / ts_modified.
                write_u32_le(rec + 48, tmp[i].size);
                write_u32_le(rec + 52, tmp[i].ts_created);
                write_u32_le(rec + 56, tmp[i].ts_modified);
            }

            self->reg[rcount]  = (int64_t)n;
            self->reg[rstatus] = 1;
            break;
        }
        // Just a connection check - does not read/write RAM, does not call
        // any svfs_*() function, only looks at vm->disk_path.
        case OP_SVFS_ATTACHED: {
            int rstatus;
            if (!fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_SVFS_ATTACHED: bad operand"); return;
            }
            self->reg[rstatus] = (self->disk_path != NULL) ? 1 : 0;
            break;
        }

        // NETWORK
        case OP_NET_ATTACHED: {
            int rstatus;
            if (!fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_NET_ATTACHED: bad operand"); return;
            }
            self->reg[rstatus] = (self->net != NULL) ? 1 : 0;
            break;
        }

        case OP_NET_SEND: {
            int raddr, rlen, rstatus;
            if (!fetch_reg(prog, self, &raddr) || !fetch_reg(prog, self, &rlen) ||
                !fetch_reg(prog, self, &rstatus)) {//
                vm_fault(self, "OP_NET_SEND: bad operand"); return;
            }
            VmNetChannel *ch = vm_net(self);
            if (!ch) { self->reg[rstatus] = 0; break; }

            uint32_t addr = (uint32_t)self->reg[raddr];
            uint32_t len  = (uint32_t)self->reg[rlen];
            if (!ram_bounds_ok(self, addr, len)) {
                vm_fault(self, "OP_NET_SEND: buffer out of RAM"); return;
            }

            if (vmnet_send(ch, &self->ram[addr], len) == 0) {
                self->reg[rstatus] = 1;
            } else if (errno == EMSGSIZE) {
                self->reg[rstatus] = 2;
            } else {
                vm_net_detach(self);
                self->reg[rstatus] = 3;
            }
            break;
        }

        case OP_NET_RECV: {
            int raddr, rbufsize, rdstlen, rstatus;
            if (!fetch_reg(prog, self, &raddr) || !fetch_reg(prog, self, &rbufsize) ||
                !fetch_reg(prog, self, &rdstlen) || !fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_NET_RECV: bad operand"); return;
            }
            VmNetChannel *ch = vm_net(self);
            if (!ch) { self->reg[rdstlen] = 0; self->reg[rstatus] = 0; break; }

            uint32_t addr     = (uint32_t)self->reg[raddr];
            uint32_t buf_size = (uint32_t)self->reg[rbufsize];
            if (!ram_bounds_ok(self, addr, buf_size)) {
                vm_fault(self, "OP_NET_RECV: buffer out of RAM"); return;
            }

            // Never call the (otherwise blocking) vmnet_recv() unless a 0ms
            // poll first confirms a packet is actually waiting - this is
            // what keeps this opcode from ever stalling the VM.
            int ready = vmnet_poll_readable(ch, 0);
            if (ready <= 0) {
                self->reg[rdstlen] = 0;
                self->reg[rstatus] = 0;
                break;
            }

            ssize_t n = vmnet_recv(ch, &self->ram[addr], buf_size);
            if (n > 0) {
                self->reg[rdstlen] = (int64_t)n;
                self->reg[rstatus] = 1;
            } else if (n == 0) {
                vm_net_detach(self);
                self->reg[rdstlen] = 0;
                self->reg[rstatus] = 2;
            } else if (errno == EMSGSIZE) {
                self->reg[rdstlen] = 0;
                self->reg[rstatus] = 3;
            } else {
                vm_net_detach(self);
                self->reg[rdstlen] = 0;
                self->reg[rstatus] = 2;
            }
            break;
        }

        case OP_NET_POLL: {
            int rstatus;
            if (!fetch_reg(prog, self, &rstatus)) {
                vm_fault(self, "OP_NET_POLL: bad operand"); return;
            }
            VmNetChannel *ch = vm_net(self);
            self->reg[rstatus] = (ch && vmnet_poll_readable(ch, 0) > 0) ? 1 : 0;
            break;
        }

        case OP_NET_MAXPACKET: {
            int rdst;
            if (!fetch_reg(prog, self, &rdst)) {
                vm_fault(self, "OP_NET_MAXPACKET: bad operand"); return;
            }
            VmNetChannel *ch = vm_net(self);
            self->reg[rdst] = (int64_t)(ch ? vmnet_channel_max_packet(ch) : 0);
            break;
        }

        default:
            fprintf(stderr, "VM: Unknown opcode (%d)\n", op);
            return;
    }


}

static void vm_do_destroy(VM *self) {
    if (self->net) {
        vm_net_detach(self);
        printf("VM: network card detached\n");
    }

    if (self->disk_path) {
        svfs_save(self->disk_path);
        svfs_shutdown();
        printf("VM: virtual disk detached (saved to %s)\n", self->disk_path);
        free(self->disk_path);
        self->disk_path = NULL;
    }

    if (self->ram) {
        free(self->ram);
        self->ram = NULL;
    }
    if (self->image_name) {
        free(self->image_name);
        self->image_name = NULL;
    }
    if (self->prog.code) {
        free(self->prog.code);
        self->prog.code = NULL;
    }
    if (self->prog.data) {
        free(self->prog.data);
        self->prog.data = NULL;
    }
    if (self->boot_prog.code) {
        free(self->boot_prog.code);
        self->boot_prog.code = NULL;
    }
    if (self->boot_prog.data) {
        free(self->boot_prog.data);
        self->boot_prog.data = NULL;
    }
    self->running = 0;
    printf("VM: destroyed\n");
}

void vm_init(VM *vm) {
    memset(vm, 0, sizeof(*vm));

    vm->create  = vm_do_create;
    vm->step    = vm_do_step;
    vm->destroy = vm_do_destroy;
    vm->reset   = vm_do_reset;
}
