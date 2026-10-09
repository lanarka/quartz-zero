"""
Description of the QuartzVM instruction set, for the debugger's purposes.

This module is deliberately independent from the SDK assembler (qasm) - it
does not import lark or anything else, just a plain description of the
opcodes exactly as vm.c reads them.
"""

from collections import namedtuple

REG   = "reg"     # 1 byte - register number (0..15)
IMM8  = "imm8"    # 1 byte literal
IMM16 = "imm16"   # 2 bytes literal (LE)
IMM32 = "imm32"   # 4 bytes literal (LE)
IMMT  = "immt"    # literal whose size is determined by the instruction's type byte (4 or 8 bytes)
FLOAT = "float"   # 4 bytes f32 literal (e.g. OP_VIDEO scale)
ADDR  = "addr"    # 4 bytes absolute code address (LE)

# Encoded size in bytes for each operand kind (IMMT is handled separately
# via immt_encoded_size() since its size depends on the type suffix).
OPERAND_SIZE = {REG: 1, IMM8: 1, IMM16: 2, IMM32: 4, FLOAT: 4, ADDR: 4}

# --- type suffixes (type byte following the opcode for "typed" instructions) ---
TYPE_SUFFIXES = (
    "i8", "i16", "i32", "u8", "u16", "u32", "i64", "u64", "f32", "f64",
)
TYPE_NAME_TO_CODE = {name: code for code, name in enumerate(TYPE_SUFFIXES)}
CODE_TO_TYPE_NAME = {code: name for code, name in enumerate(TYPE_SUFFIXES)}
TYPE_SIZE = {
    "i8": 1, "u8": 1, "i16": 2, "u16": 2, "i32": 4,
    "u32": 4, "f32": 4, "i64": 8, "u64": 8, "f64": 8,
}
TYPE_IS_FLOAT = {"f32", "f64"}
TYPE_IS_UNSIGNED = {"u8", "u16", "u32", "u64"}
TYPE_IS_SIGNED = {"i8", "i16", "i32", "i64"}


def immt_encoded_size(tname):
    # An IMMT operand is encoded as 8 bytes only for 64-bit types; every
    # other type (including all float/int widths below 64 bits) is still
    # read as a 4-byte immediate on the wire, matching vm.c's fetch_imm().
    return 8 if TYPE_SIZE[tname] == 8 else 4


REGISTER_COUNT = 16

CAT_FLOW = "flow"        # jumps, call/ret
CAT_STACK = "stack"      # push/pop
CAT_MEM = "mem"          # load/store/memcpy/memset
CAT_ALU = "alu"          # arithmetic, comparisons, mov
CAT_CORE = "core"        # nop/halt/rand/time/ramsize/reset/print...
CAT_IO = "io"            # sound/graphics/text/input/disk - "not debuggable"

BaseInstr = namedtuple("BaseInstr", ["opcode", "operands", "typed", "float_ok", "category"])


def _T(opcode, operands, category, float_ok=True):
    # Shorthand for a "typed" instruction (one that carries a type byte
    # right after the opcode, e.g. add/sub/load/store).
    return BaseInstr(opcode, operands, True, float_ok, category)


def _U(opcode, operands, category):
    # Shorthand for an "untyped" instruction (no type byte, e.g. jumps,
    # call/ret, push/pop, and all the I/O instructions).
    return BaseInstr(opcode, operands, False, False, category)


# The full instruction table: mnemonic -> (opcode, operand kinds, typed?,
# float allowed?, category). This is the single source of truth used by
# the decoder, the emulator, and the syntax highlighter.
INSTRUCTIONS = {
    "nop":          _U(0,  [], CAT_CORE),
    "halt":         _U(1,  [], CAT_CORE),
    "rnd":          _U(2,  [REG, IMM32], CAT_CORE),
    "rndseed":      _U(3,  [REG], CAT_CORE),
    "wait":         _U(4,  [REG], CAT_CORE),
    "waiti":        _U(5,  [IMM32], CAT_CORE),
    "time":         _U(6,  [REG], CAT_CORE),
    "tick":         _U(7,  [REG], CAT_CORE),
    "ramsize":      _U(8,  [REG], CAT_CORE),
    "vmx":          _U(9,  [REG, REG, REG], CAT_CORE),
    "reset":        _U(10, [], CAT_CORE),
    "video":        _U(11, [IMM8, IMM16, FLOAT, IMM8], CAT_IO),
    "videoinf":     _U(12, [REG, REG, REG, REG], CAT_IO),

    "print":        _T(13, [REG], CAT_CORE),

    "mov":          _U(14, [REG, REG], CAT_ALU),
    "movi":         _T(15, [REG, IMMT], CAT_ALU),

    "add":          _T(16, [REG, REG], CAT_ALU),
    "sub":          _T(17, [REG, REG], CAT_ALU),
    "mul":          _T(18, [REG, REG], CAT_ALU),
    "div":          _T(19, [REG, REG], CAT_ALU),
    "mod":          _T(20, [REG, REG], CAT_ALU),
    "and":          _T(21, [REG, REG], CAT_ALU, float_ok=False),
    "or":           _T(22, [REG, REG], CAT_ALU, float_ok=False),
    "xor":          _T(23, [REG, REG], CAT_ALU, float_ok=False),
    "shl":          _T(24, [REG, REG], CAT_ALU, float_ok=False),
    "shr":          _T(25, [REG, REG], CAT_ALU, float_ok=False),

    "addi":         _T(26, [REG, IMMT], CAT_ALU),
    "subi":         _T(27, [REG, IMMT], CAT_ALU),
    "inc":          _T(28, [REG], CAT_ALU),
    "dec":          _T(29, [REG], CAT_ALU),
    "not":          _T(30, [REG], CAT_ALU, float_ok=False),

    "cmp":          _T(31, [REG, REG], CAT_ALU),
    "cmpi":         _T(32, [REG, IMMT], CAT_ALU),

    "jmp":          _U(33, [ADDR], CAT_FLOW),
    "jz":           _U(34, [ADDR], CAT_FLOW),
    "jnz":          _U(35, [ADDR], CAT_FLOW),
    "jg":           _U(36, [ADDR], CAT_FLOW),
    "jl":           _U(37, [ADDR], CAT_FLOW),
    "jge":          _U(38, [ADDR], CAT_FLOW),
    "jle":          _U(39, [ADDR], CAT_FLOW),

    "jmpr":         _U(40, [REG], CAT_FLOW),
    "jzr":          _U(41, [REG], CAT_FLOW),
    "jnzr":         _U(42, [REG], CAT_FLOW),
    "jgr":          _U(43, [REG], CAT_FLOW),
    "jlr":          _U(44, [REG], CAT_FLOW),
    "jger":         _U(45, [REG], CAT_FLOW),
    "jler":         _U(46, [REG], CAT_FLOW),

    "callr":        _U(47, [REG], CAT_FLOW),
    "call":         _U(48, [ADDR], CAT_FLOW),
    "ret":          _U(49, [], CAT_FLOW),

    "push":         _U(50, [REG], CAT_STACK),
    "pop":          _U(51, [REG], CAT_STACK),

    "load":         _T(52, [REG, ADDR], CAT_MEM),
    "store":        _T(53, [ADDR, REG], CAT_MEM),
    "loadr":        _T(54, [REG, REG], CAT_MEM),
    "storer":       _T(55, [REG, REG], CAT_MEM),

    "memcpy":       _U(56, [REG, REG, REG], CAT_MEM),
    "memset":       _U(57, [REG, REG, REG], CAT_MEM),

    "beep":         _U(58, [], CAT_IO),
    "sound":        _U(59, [REG, REG], CAT_IO),

    "xframe":       _U(60, [REG, REG, REG, REG, REG], CAT_IO),
    "xpix":         _U(61, [REG, REG, REG], CAT_IO),
    "xraw":         _U(62, [REG, REG, REG], CAT_IO),
    "xclear":       _U(63, [], CAT_IO),
    "xclearc":      _U(64, [REG], CAT_IO),
    "xgetres":      _U(65, [REG, REG], CAT_IO),
    "xvsync":       _U(66, [IMM32], CAT_IO),

    "xtchar":       _U(67, [REG, REG, REG, REG, REG], CAT_IO),
    "xtframe":      _U(68, [REG, REG, REG, REG, REG], CAT_IO),
    "xtgetres":     _U(69, [REG, REG], CAT_IO),
    "xtclear":      _U(70, [], CAT_IO),
    "xtcurset":     _U(71, [REG, REG], CAT_IO),
    "xtcurget":     _U(72, [REG, REG], CAT_IO),
    "xtcurena":     _U(73, [REG], CAT_IO),

    "ximouse":      _U(74, [REG, REG, REG], CAT_IO),
    "xikey":        _U(75, [REG, REG], CAT_IO),
    "xipoll":       _U(76, [REG, REG, REG, REG, REG], CAT_IO),
    "xitmouse":     _U(77, [REG, REG, REG], CAT_IO),
    "xitpoll":      _U(78, [REG, REG, REG, REG, REG], CAT_IO),

    "fsmkdir":      _U(79, [REG], CAT_IO),
    "fsrmdir":      _U(80, [REG, REG], CAT_IO),
    "fscreate":     _U(81, [REG], CAT_IO),
    "fsrm":         _U(82, [REG], CAT_IO),
    "fsopen":       _U(83, [REG], CAT_IO),
    "fsclose":      _U(84, [REG], CAT_IO),
    "fswrite":      _U(85, [REG, REG, REG], CAT_IO),
    "fsread":       _U(86, [REG, REG, REG, REG, REG], CAT_IO),
    "fswriteb":     _U(87, [REG, REG, REG, REG], CAT_IO),
    "fsreadb":      _U(88, [REG, REG, REG, REG, REG], CAT_IO),
    "fscd":         _U(89, [REG, REG], CAT_IO),
    "fsdir":        _U(90, [REG, REG, REG, REG], CAT_IO),
    "fsatt":        _U(91, [REG], CAT_IO),

    "netatt":       _U(92,  [REG], CAT_IO),
    "netsend":      _U(93,  [REG, REG, REG], CAT_IO),
    "netrecv":      _U(94,  [REG, REG, REG, REG], CAT_IO),
    "netpoll":      _U(95,  [REG], CAT_IO),
    "netmaxpacket": _U(96,  [REG], CAT_IO),    
}

# Reverse lookup used by the decoder: raw opcode byte -> (mnemonic, spec).
OPCODE_TO_NAME = {b.opcode: (name, b) for name, b in INSTRUCTIONS.items()}

# Instructions the VM actually executes for real (mnemonic names) - everything
# except CAT_IO, which the emulator only skips (see emulator.py _skip_io).
DEBUGGABLE_CATEGORIES = {CAT_FLOW, CAT_STACK, CAT_MEM, CAT_ALU, CAT_CORE}
