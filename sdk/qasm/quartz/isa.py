"""
Quartz - The Computer Assembler
================================

quartz/isa.py - the assembler's model of the Quartz VM's instruction set.

This is the single source of truth qasm.py and qdisasm.py both consult to
know how to encode/decode each instruction: which mnemonic maps to which
opcode byte, how many operands it takes and of what kind (register,
immediate, address, ...), and whether it's one of the "typed" instructions
that carry an extra leading type byte (i8/u8/.../f64) selecting which
integer/float width the operands are interpreted as. It intentionally
contains no parsing or encoding logic itself - see qasm.py's analyze()/
encode() for that - only the static instruction table and the small helpers
needed to compute operand sizes from it.
"""

from collections import namedtuple

REG = "reg"
IMM8 = "imm8"
IMM16 = "imm16"
IMM32 = "imm32"
IMMT = "immt"    # An immediate whose width depends on the instruction's type byte (1/2/4/8 bytes)
FLOAT = "float"  # Always a 32-bit IEEE-754 float, regardless of any type byte
ADDR = "addr"    # A 32-bit code address; may be written as a label name in source

OPERAND_SIZE = {REG: 1, IMM8: 1, IMM16: 2, IMM32: 4, FLOAT: 4, ADDR: 4}

INT_RANGE = {
    IMM8: (-(1 << 7), (1 << 8) - 1),
    IMM16: (-(1 << 15), (1 << 16) - 1),
    IMM32: (-(1 << 31), (1 << 32) - 1),
    ADDR: (0, (1 << 32) - 1),
}

REGISTER_COUNT = 16

# Type-byte suffix names, in the exact order their numeric type codes are
# assigned (index == encoded byte value) - keep this order in sync with the
# ValType enum in the VM's vm.h.
TYPE_SUFFIXES = (
    "i8",
    "i16",
    "i32",
    "u8",
    "u16",
    "u32",
    "i64",
    "u64",
    "f32",
    "f64"
)

TYPE_NAME_TO_CODE = {name: code for code, name in enumerate(TYPE_SUFFIXES)}
CODE_TO_TYPE_NAME = {code: name for code, name in enumerate(TYPE_SUFFIXES)}
TYPE_BYTE_SIZE = 1
TYPE_SIZE = {
    "i8": 1, "u8": 1, "i16": 2, "u16": 2, "i32": 4, 
    "u32": 4, "f32": 4, "i64": 8, "u64": 8, "f64": 8,
}
TYPE_IS_FLOAT = {"f32", "f64"}
TYPE_IS_UNSIGNED = {"u8", "u16", "u32", "u64"}


def int_range_for_type(tname):
    """Inclusive (min, max) representable range for a given type name,
    treating it as signed if its VM encoding is signed and unsigned
    otherwise (matches the VM's own two's-complement semantics)."""
    bits = TYPE_SIZE[tname] * 8
    return (-(1 << (bits - 1)), (1 << bits) - 1)


def immt_encoded_size(tname):
    """Byte width of an IMMT operand for a given type: 8 bytes for the two
    64-bit types (i64/u64/f64), 4 bytes for everything else - narrower
    integer/float immediates are still encoded in a 4-byte slot."""
    return 8 if TYPE_SIZE[tname] == 8 else 4


def operand_encoded_size(kind, vtype):
    """Encoded byte size of a single operand of the given `kind`. IMMT is
    the only kind whose size depends on the instruction's type byte
    (`vtype`, a numeric type code); every other kind has a fixed size from
    OPERAND_SIZE."""
    if kind == IMMT:
        tname = CODE_TO_TYPE_NAME[vtype]
        return immt_encoded_size(tname)
    return OPERAND_SIZE[kind]


BaseInstr = namedtuple("BaseInstr", ["opcode", "operands", "typed", "float_ok"])


def _T(opcode, operands, float_ok=True):
    """Defines a "typed" instruction: it expects a leading type-name operand
    in source (e.g. `add i32 #0 #1`) which gets encoded as an extra type
    byte. Set float_ok=False for instructions that are only meaningful for
    integer types (bitwise ops, shifts, etc.) so the analyzer can reject
    e.g. `and f32 ...`."""
    return BaseInstr(opcode, operands, True, float_ok)


def _U(opcode, operands):
    """Defines an "untyped" instruction: fixed operand kinds, no leading
    type byte at all."""
    return BaseInstr(opcode, operands, False, False)


# The instruction set itself: mnemonic -> BaseInstr(opcode, operand kinds,
# typed?, float_ok?). This table must be kept in sync with the VM's own
# opcode numbering in vm.h/vm.c - the numeric opcode values here are not
# arbitrary, they are exactly what the VM's vm_do_step() switch dispatches
# on.
_BASE_INSTRUCTIONS = {
    "nop":          _U(0,  []),
    "halt":         _U(1,  []),
    "rnd":          _U(2,  [REG, IMM32]),
    "rndseed":      _U(3,  [REG]),
    "wait":         _U(4,  [REG]),
    "waiti":        _U(5,  [IMM32]),
    "time":         _U(6,  [REG]),
    "tick":         _U(7,  [REG]),
    "ramsize":      _U(8,  [REG]),
    "vmx":          _U(9,  [REG, REG, REG]),
    "reset":        _U(10, []),
    "video":        _U(11, [IMM8, IMM16, FLOAT, IMM8]),
    "videoinf":     _U(12, [REG, REG, REG, REG]),

    "print":        _T(13, [REG]),

    "mov":          _U(14, [REG, REG]),
    "movi":         _T(15, [REG, IMMT]),

    "add":          _T(16, [REG, REG]),
    "sub":          _T(17, [REG, REG]),
    "mul":          _T(18, [REG, REG]),
    "div":          _T(19, [REG, REG]),
    "mod":          _T(20, [REG, REG]),
    "and":          _T(21, [REG, REG], float_ok=False),
    "or":           _T(22, [REG, REG], float_ok=False),
    "xor":          _T(23, [REG, REG], float_ok=False),
    "shl":          _T(24, [REG, REG], float_ok=False),
    "shr":          _T(25, [REG, REG], float_ok=False),

    "addi":         _T(26, [REG, IMMT]),
    "subi":         _T(27, [REG, IMMT]),
    "inc":          _T(28, [REG]),
    "dec":          _T(29, [REG]),
    "not":          _T(30, [REG], float_ok=False),

    "cmp":          _T(31, [REG, REG]),
    "cmpi":         _T(32, [REG, IMMT]),

    "jmp":          _U(33, [ADDR]),
    "jz":           _U(34, [ADDR]),
    "jnz":          _U(35, [ADDR]),
    "jg":           _U(36, [ADDR]),
    "jl":           _U(37, [ADDR]),
    "jge":          _U(38, [ADDR]),
    "jle":          _U(39, [ADDR]),

    "jmpr":         _U(40, [REG]),
    "jzr":          _U(41, [REG]),
    "jnzr":         _U(42, [REG]),
    "jgr":          _U(43, [REG]),
    "jlr":          _U(44, [REG]),
    "jger":         _U(45, [REG]),
    "jler":         _U(46, [REG]),

    "callr":        _U(47, [REG]),
    "call":         _U(48, [ADDR]),
    "ret":          _U(49, []),

    "push":         _U(50, [REG]),
    "pop":          _U(51, [REG]),

    "load":         _T(52, [REG, ADDR]),
    "store":        _T(53, [ADDR, REG]),
    "loadr":        _T(54, [REG, REG]),
    "storer":       _T(55, [REG, REG]),

    "memcpy":       _U(56, [REG, REG, REG]),
    "memset":       _U(57, [REG, REG, REG]),
    
    "beep":         _U(58, []),
    "sound":        _U(59, [REG, REG]),

    "xframe":       _U(60,  [REG, REG, REG, REG, REG]),
    "xpix":         _U(61,  [REG, REG, REG]),
    "xraw":         _U(62,  [REG, REG, REG]),
    "xclear":       _U(63,  []),
    "xclearc":      _U(64,  [REG]),
    "xgetres":      _U(65,  [REG, REG]),
    "xvsync":       _U(66,  [IMM32]),

    "xtchar":       _U(67,  [REG, REG, REG, REG, REG]),
    "xtframe":      _U(68,  [REG, REG, REG, REG, REG]),
    "xtgetres":     _U(69,  [REG, REG]),
    "xtclear":      _U(70,  []),
    "xtcurset":     _U(71,  [REG, REG]),
    "xtcurget":     _U(72,  [REG, REG]),
    "xtcurena":     _U(73,  [REG]),

    "ximouse":      _U(74,  [REG, REG, REG]),
    "xikey":        _U(75,  [REG, REG]),
    "xipoll":       _U(76,  [REG, REG, REG, REG, REG]),
    "xitmouse":     _U(77,  [REG, REG, REG]),
    "xitpoll":      _U(78,  [REG, REG, REG, REG, REG]),

    "fsmkdir":      _U(79,  [REG]),
    "fsrmdir":      _U(80,  [REG, REG]),
    "fscreate":     _U(81,  [REG]),
    "fsrm":         _U(82,  [REG]),
    "fsopen":       _U(83,  [REG]),
    "fsclose":      _U(84,  [REG]),
    "fswrite":      _U(85,  [REG, REG, REG]),
    "fsread":       _U(86,  [REG, REG, REG, REG, REG]),
    "fswriteb":     _U(87,  [REG, REG, REG, REG]),
    "fsreadb":      _U(88,  [REG, REG, REG, REG, REG]),
    "fscd":         _U(89,  [REG, REG]),
    "fsdir":        _U(90,  [REG, REG, REG, REG]),
    "fsatt":        _U(91,  [REG]),

    "netatt":       _U(92,  [REG]),
    "netsend":      _U(93,  [REG, REG, REG]),
    "netrecv":      _U(94,  [REG, REG, REG, REG]),
    "netpoll":      _U(95,  [REG]),
    "netmaxpacket": _U(96,  [REG]),

}

# Reverse lookup used by the disassembler (qdisasm.py) to go from a raw
# opcode byte back to its mnemonic + BaseInstr spec.
OPCODE_TO_BASE = {b.opcode: (name, b) for name, b in _BASE_INSTRUCTIONS.items()}
INSTRUCTIONS = _BASE_INSTRUCTIONS
