// =============================================================================
// Quartz - The Computer
// -----------------------------------------------------------------------------
// vm.h - public interface of the Quartz virtual machine.
//
// Defines the binary image format (BIN_MAGIC*/BIN_VERSION/RUNTIME_VERSION),
// the opcode set (see the enum below - individual opcodes are documented at
// their implementation site in vm.c and are intentionally not repeated here),
// the operand value types (ValType), and the VM struct itself which bundles
// all machine state (registers, RAM, stacks, program counter, flags) together
// with a small vtable of lifecycle function pointers (create/step/destroy/
// reset) that vm_init() wires up to the implementations in vm.c.
//
// The typical lifecycle, driven by quartz.c, is:
//   vm_init(&vm)                       -> wire up function pointers
//   vm.create(&vm, ram_size, path, rt) -> allocate RAM, load the program image
//   vm_attach_disk(&vm, disk_path)     -> optional: mount an SVFS disk image
//   vm.step(&vm)                       -> execute exactly one instruction
//   vm.reset(&vm, prog)                -> "cold" reset back to the boot image
//   vm.destroy(&vm)                    -> free everything, detach the disk
// =============================================================================

#ifndef VM_H
#define VM_H

#include <stdint.h>

#define NUM_REGS         16
#define CALL_STACK_MAX   8192
#define DATA_STACK_MAX   65535

#define RUNTIME_VERSION  0xFF
#define BIN_VERSION      0x04

#define BIN_MAGIC0       0x7F
#define BIN_MAGIC1       0x51
#define BIN_MAGIC2       0x00
#define BIN_MAGIC3       0x7F

typedef enum {
    TYPE_I8  = 0,
    TYPE_I16 = 1,
    TYPE_I32 = 2,
    TYPE_U8  = 3,
    TYPE_U16 = 4,
    TYPE_U32 = 5,
    TYPE_I64 = 6,
    TYPE_U64 = 7,
    TYPE_F32 = 8,
    TYPE_F64 = 9,
    TYPE_COUNT
} ValType;

enum {
    // ESSENTIALS
    OP_NOP,
    OP_HALT,
    OP_RAND,
    OP_RAND_SEED,
    OP_WAIT,
    OP_WAITI,
    OP_TIME,
    OP_TICK,
    OP_RAMSIZE,
    OP_VMX,
    OP_RESET,
    OP_VIDEO,
    OP_VIDEO_INFO,

    // DEBUG
    OP_PRINT,

    // DATA MOVE
    OP_MOV,
    OP_MOVI,

    // ARITHMETIC (REGISTER, REGISTER)
    OP_ADD,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_MOD,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_SHL,
    OP_SHR,

    // ARITHMETIC (REGISTER, IMMEDIATE)
    OP_ADDI,
    OP_SUBI,
    OP_INC,
    OP_DEC,
    OP_NOT,

    // COMPARATION
    OP_CMP,
    OP_CMPI,

    // JUMPS
    OP_JMP,
    OP_JZ,
    OP_JNZ,
    OP_JG,
    OP_JL,
    OP_JGE,
    OP_JLE,
    
    // JUMPS (REGISTER)
    OP_JMPR,
    OP_JZR, 
    OP_JNZR, 
    OP_JGR, 
    OP_JLR, 
    OP_JGER, 
    OP_JLER,

    // CALL FUNCTIONS
    OP_CALLR,
    OP_CALL,
    OP_RET,

    // DATA STACK
    OP_PUSH,
    OP_POP,

    // MEMORY ACCESS
    OP_LOAD,
    OP_STORE,
    OP_LOADR,
    OP_STORER,

    // MEMORY (BLOCK)
    OP_MEMCPY,
    OP_MEMSET,

    // SOUND
    OP_BEEP,
    OP_SOUND,

    // GFX
    OP_GFX_FRAME,
    OP_GFX_PIX,
    OP_GFX_RAW,
    OP_GFX_CLEAR,
    OP_GFX_CLEARC,
    OP_GFX_GETRES,
    OP_VSYNC,

    // TEXT
    OP_TXT_CHAR,
    OP_TXT_FRAME,
    OP_TXT_GETRES,
    OP_TXT_CLEAR,
    OP_TXT_CURSET,
    OP_TXT_CURGET,
    OP_TXT_CURENA,

    // INPUT
    OP_IN_MOUSE,
    OP_IN_KEY,
    OP_IN_POLL,
    OP_IN_TXT_MOUSE,
    OP_IN_TXT_POLL,

    // DISK
    OP_SVFS_MKDIR,
    OP_SVFS_RMDIR,
    OP_SVFS_CREATE,
    OP_SVFS_RM,
    OP_SVFS_OPEN,
    OP_SVFS_CLOSE,
    OP_SVFS_WRITE,
    OP_SVFS_READ,
    OP_SVFS_WRITE_BYTES,
    OP_SVFS_READ_BYTES,
    OP_SVFS_CD,
    OP_SVFS_DIR,
    OP_SVFS_ATTACHED,

    // NETWORK
    OP_NET_ATTACHED,
    OP_NET_SEND,
    OP_NET_RECV,
    OP_NET_POLL,
    OP_NET_MAXPACKET
};

// A loaded program image: executable code plus an optional initial data
// segment that gets copied into VM RAM at data_addr on (re)load. Populated
// either from a .bin file on disk (see load_program() in vm.c) or, for
// OP_VMX, directly from a code blob sitting in VM RAM.
typedef struct {
    uint8_t  *code;
    uint32_t  code_len;
    uint32_t  entry;      // PC value to start execution at
    uint8_t  *data;
    uint32_t  data_len;
    uint32_t  data_addr;  // destination offset in VM RAM for `data`
} Program;


typedef struct VM VM;

struct VM {
    uint8_t  *ram;
    int       ram_size;
    char     *image_name;
    void     *rt;
    int       running;
    char     *disk_path;
    void     *net;         // NULL if no network card is attached (see vm_attach_net()).
                           // Otherwise points to a heap-allocated VmNetChannel (vmnet.h)
                           // from the src-net "sietovka" library. Kept as an opaque
                           // void* here for the same reason `rt` is - vm.h stays free
                           // of vmnet.h's types, only vm.c ever casts and dereferences it.
    uint16_t  screen_h;
    uint16_t  screen_w;
    uint16_t  term_h;
    uint16_t  term_w;
    int       vsync_yield;
    Program   prog;
    Program   boot_prog;   // The original program loaded from the file given on the command
                           // line at vm_create() time. OP_VMX never modifies this copy, so it
                           // serves as the source for a "cold" reset (OP_RESET, Ctrl+Alt+R) -
                           // unlike self->prog, which can be overwritten by a program loaded
                           // from RAM at runtime.
    uint32_t  pc;
    int64_t   reg[NUM_REGS];
    uint32_t  call_stack[CALL_STACK_MAX];
    int       call_sp;
    int64_t   data_stack[DATA_STACK_MAX];
    int       data_sp;
    int       flag_zero;
    int       flag_neg;
    uint64_t  tick_count;

    void (*create)(VM *self, int ram_size, const char *image_name, void *rt_handle);
    void (*step)(VM *self);
    void (*destroy)(VM *self);
    void (*reset)(VM *vm, const Program *prog);
};

void vm_init(VM *vm);
int vm_attach_disk(VM *vm, const char *disk_path);
int vm_attach_net(VM *vm, const char *sock_path);

#endif
