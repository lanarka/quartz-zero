"""
A pure-Python emulator of QuartzVM for the debugger's purposes.

It tries to mirror the behavior of src/vm.c (vm_do_step) as faithfully as
possible for everything related to program flow, registers, stacks, and
memory. Instructions related to sound/graphics/text mode/input/disk (SVFS)
are NOT actually emulated - the VM just "skips" them (consumes the operands
and, where relevant, zeroes the destination registers) and writes a note to
the log that it was an I/O instruction. This matches the design goal: what
matters is program flow, the stack, and memory - not the surrounding
hardware.
"""

import math
import random
import struct
import time

from . import isa
from .disasm import decode_one, format_instr

MASK64 = (1 << 64) - 1
NUM_REGS = isa.REGISTER_COUNT
CALL_STACK_MAX = 8192
DATA_STACK_MAX = 65535


# type helpers - mirror the functions in vm.c (type_normalize, type_add, ...)

def f32_from_bits(raw):
    # Reinterpret the low 32 bits of raw as an IEEE-754 float (no numeric
    # conversion - this is a bit-pattern reinterpretation, like a C cast
    # through a union/pointer).
    bits = raw & 0xFFFFFFFF
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def f32_to_raw(f):
    # Inverse of f32_from_bits: get the raw 32-bit pattern of a Python float
    # as if it were stored as an IEEE-754 single precision value.
    return struct.unpack("<I", struct.pack("<f", f))[0]


def f64_from_bits(raw):
    bits = raw & MASK64
    return struct.unpack("<d", struct.pack("<Q", bits))[0]


def f64_to_raw(d):
    return struct.unpack("<Q", struct.pack("<d", d))[0]


def type_normalize(v, t):
    """Reduce an arbitrary Python int to the value it would hold in a
    register of type t: mask to the right width and, for signed integer
    types, reinterpret the top bit as the sign (two's complement)."""
    u = v & MASK64
    if t == "i8":
        v8 = u & 0xFF
        return v8 - 0x100 if v8 >= 0x80 else v8
    if t == "u8":
        return u & 0xFF
    if t == "i16":
        v16 = u & 0xFFFF
        return v16 - 0x10000 if v16 >= 0x8000 else v16
    if t == "u16":
        return u & 0xFFFF
    if t == "i32":
        v32 = u & 0xFFFFFFFF
        return v32 - 0x100000000 if v32 >= 0x80000000 else v32
    if t == "u32":
        return u & 0xFFFFFFFF
    if t == "i64":
        return u - (1 << 64) if u >= (1 << 63) else u
    if t == "u64":
        return u
    if t == "f32":
        # Floats are stored/passed around as their raw bit pattern, not as
        # a Python float, so normalizing just means masking to 32 bits.
        return u & 0xFFFFFFFF
    if t == "f64":
        return u
    return u


def is_float_type(t):
    return t in isa.TYPE_IS_FLOAT


def is_unsigned_type(t):
    return t in isa.TYPE_IS_UNSIGNED


def safe_fdiv(a, b):
    # IEEE-754-style division that never raises: 0/0 -> NaN, x/0 -> +-Inf
    # following the sign rule, matching what the VM's float division does.
    if b != 0.0:
        return a / b
    if a == 0.0:
        return math.nan
    sign = math.copysign(1.0, a) * math.copysign(1.0, b)
    return math.inf if sign > 0 else -math.inf


def safe_fmod(a, b):
    if b != 0.0:
        return math.fmod(a, b)
    return math.nan


def c_div(a, b):
    # Integer division truncated toward zero, i.e. C semantics, as opposed
    # to Python's // which truncates toward negative infinity.
    q = abs(a) // abs(b)
    if (a < 0) != (b < 0):
        q = -q
    return q


def c_mod(a, b):
    # Remainder consistent with c_div (same sign as the dividend, unlike
    # Python's % which takes the sign of the divisor).
    return a - c_div(a, b) * b


def type_add(a, b, t):
    if t == "f32":
        return f32_to_raw(f32_from_bits(a) + f32_from_bits(b))
    if t == "f64":
        return f64_to_raw(f64_from_bits(a) + f64_from_bits(b))
    return type_normalize(a + b, t)


def type_sub(a, b, t):
    if t == "f32":
        return f32_to_raw(f32_from_bits(a) - f32_from_bits(b))
    if t == "f64":
        return f64_to_raw(f64_from_bits(a) - f64_from_bits(b))
    return type_normalize(a - b, t)


def type_mul(a, b, t):
    if t == "f32":
        return f32_to_raw(f32_from_bits(a) * f32_from_bits(b))
    if t == "f64":
        return f64_to_raw(f64_from_bits(a) * f64_from_bits(b))
    return type_normalize(a * b, t)


def type_div(a, b, t):
    if t == "f32":
        return f32_to_raw(safe_fdiv(f32_from_bits(a), f32_from_bits(b)))
    if t == "f64":
        return f64_to_raw(safe_fdiv(f64_from_bits(a), f64_from_bits(b)))
    if is_unsigned_type(t):
        return type_normalize((a & MASK64) // (b & MASK64), t)
    return type_normalize(c_div(a, b), t)


def type_mod(a, b, t):
    if t == "f32":
        return f32_to_raw(safe_fmod(f32_from_bits(a), f32_from_bits(b)))
    if t == "f64":
        return f64_to_raw(safe_fmod(f64_from_bits(a), f64_from_bits(b)))
    if is_unsigned_type(t):
        return type_normalize((a & MASK64) % (b & MASK64), t)
    return type_normalize(c_mod(a, b), t)


def type_shl(a, b, t):
    # Shift amount is masked to 0..63 (C-style undefined-behavior-avoidance
    # for shifts by >= the register width), same as vm.c.
    shift = b & 63
    return type_normalize(((a & MASK64) << shift) & MASK64, t)


def type_shr(a, b, t):
    shift = b & 63
    if is_unsigned_type(t):
        return type_normalize((a & MASK64) >> shift, t)
    # Signed shift-right: Python's >> on a (possibly negative) int already
    # performs an arithmetic shift, matching the C VM's signed >>.
    return type_normalize(a >> shift, t)


def do_compare(raw_a, raw_b, t):
    """Compute the (zero, negative) flag pair for a cmp/cmpi instruction:
    zero == (a == b), negative == (a < b), evaluated with the correct
    signedness/float semantics for type t."""
    na = type_normalize(raw_a, t)
    nb = type_normalize(raw_b, t)
    if t == "f32":
        fa, fb = f32_from_bits(na), f32_from_bits(nb)
        return fa == fb, fa < fb
    if t == "f64":
        fa, fb = f64_from_bits(na), f64_from_bits(nb)
        return fa == fb, fa < fb
    return na == nb, na < nb


def fmt_typed_value(raw, t):
    """Format a raw register value the same way OP_PRINT does in vm.c."""
    v = type_normalize(raw, t)
    if t == "f32":
        return f"{f32_from_bits(v):.9g}"
    if t == "f64":
        return f"{f64_from_bits(v):.17g}"
    if is_unsigned_type(t):
        return str(v)
    return str(v)


class VMFault(Exception):
    """Raised internally by _execute() to signal a runtime fault (division
    by zero, out-of-bounds memory access, stack overflow/underflow, ...).
    Caught by step(), which turns it into a halt with a logged reason -
    mirroring how vm_fault() stops the real VM."""

    def __init__(self, message):
        super().__init__(message)
        self.message = message


class Emulator:
    """State and execution of QuartzVM. After each call to step(), the
    caller (the TUI) reads .events (the list of new log lines since the
    last call) and the current register/stack/memory state directly from
    the instance attributes."""

    def __init__(self, image, ram_size=1024 * 1024):
        self.image = image
        self.ram_size = ram_size
        self.ram = bytearray(ram_size)
        self.code = image.code
        self.reg = [0] * NUM_REGS
        self.pc = image.entry
        self.call_stack = []
        self.data_stack = []
        self.flag_zero = False
        self.flag_neg = False
        self.tick_count = 0
        self.running = True
        self.halt_reason = None
        self.step_count = 0
        self.last_instr = None
        self.last_instr_addr = None
        self.events = []
        # Fixed seed by default so runs are reproducible until the program
        # explicitly reseeds via `rndseed`.
        self._rand = random.Random(0)
        self.reset()

    def reset(self):
        """Reset the VM to its initial state: zero registers/RAM, reload
        the data segment, clear both stacks and flags, and point PC back
        at the image's entry point. Used both at startup and by the user
        pressing 'r', as well as by the VM's own `reset` instruction."""
        self.reg = [0] * NUM_REGS
        self.ram = bytearray(self.ram_size)
        self.pc = self.image.entry
        self.call_stack = []
        self.data_stack = []
        self.flag_zero = False
        self.flag_neg = False
        self.tick_count = 0
        self.running = True
        self.halt_reason = None
        self.step_count = 0
        self.last_instr = None
        self.last_instr_addr = None
        self.events = []
        if self.image.data:
            end = self.image.data_addr + len(self.image.data)
            if end <= self.ram_size:
                self.ram[self.image.data_addr:end] = self.image.data
        self._log(f"VM reset, entry=0x{self.image.entry:06x}, RAM={self.ram_size}B")

    def _log(self, msg):
        self.events.append(msg)

    def _fault(self, msg):
        # Stop the VM and record why, mirroring vm_fault() in the C VM.
        self.running = False
        self.halt_reason = msg
        self._log(f"[FAULT] {msg}")

    def ram_bounds_ok(self, addr, size):
        # A zero-length access is always "in bounds" (nothing to check),
        # matching memcpy/memset with length 0 being a harmless no-op.
        if size == 0:
            return True
        if addr < 0 or addr >= self.ram_size:
            return False
        return addr + size <= self.ram_size

    def read_mem(self, addr, size):
        return int.from_bytes(self.ram[addr:addr + size], "little", signed=False)

    def write_mem(self, addr, value, size):
        v = value & ((1 << (size * 8)) - 1)
        self.ram[addr:addr + size] = v.to_bytes(size, "little")

    def step(self):
        """Execute a single instruction. Returns the decoded Instr (or
        None if the VM is no longer running)."""
        self.events = []
        if not self.running:
            return None

        instr = decode_one(self.code, self.pc)
        if instr is None:
            self._fault("PC out of code range (unexpected end of code)")
            return None
        if not instr.valid:
            self._fault(instr.error or "invalid instruction")
            self.last_instr = instr
            self.last_instr_addr = instr.addr
            return instr

        # Advance PC before executing, so that a jump/call/ret instruction
        # can simply overwrite self.pc without needing to know it was
        # already moved forward - exactly like the fetch-then-execute
        # order in vm_do_step().
        start_addr = self.pc
        self.pc = instr.next_addr
        self.last_instr = instr
        self.last_instr_addr = start_addr

        try:
            self._execute(instr)
        except VMFault as e:
            self._fault(e.message)

        self.step_count += 1
        return instr

    def _operand_values(self, instr):
        return [op["value"] for op in instr.operands]

    def _skip_io(self, instr, why, zero_regs=()):
        """Shared handling for "non-debuggable" I/O instructions - just log
        it and optionally zero the output registers, so the program can
        keep running its normal flow (e.g. status/error code = 0)."""
        vals = self._operand_values(instr)
        for idx in zero_regs:
            self.reg[vals[idx]] = 0
        self._log(f"[SKIP] {instr.mnemonic} ({why} - not emulated)")

    def _execute(self, instr):
        m = instr.mnemonic
        t = instr.type_name
        vals = self._operand_values(instr)

        # ESSENTIALS
        if m == "nop":
            return
        if m == "halt":
            self.running = False
            self.halt_reason = "HALT"
            self._log("[HALT] the program ended with the halt instruction")
            return
        if m == "rnd":
            dst, mx = vals
            self.reg[dst] = self._rand.randrange(mx) if mx else 0
            return
        if m == "rndseed":
            self._rand = random.Random(self.reg[vals[0]] & MASK64)
            return
        if m in ("wait", "waiti"):
            self._log(f"[SKIP] {m} (waiting - not performed in the debugger)")
            return
        if m == "time":
            self.reg[vals[0]] = int(time.time() * 1000)
            return
        if m == "tick":
            self.reg[vals[0]] = self.tick_count
            return
        if m == "ramsize":
            self.reg[vals[0]] = self.ram_size
            return
        if m == "vmx":
            self._log("[INFO] vmx ignored (dynamic code loading is not "
                       "supported in the debugger)")
            return
        if m == "reset":
            self.reset()
            self._log("[INFO] the program performed its own reset (reset instruction)")
            return
        if m in ("video", "videoinf"):
            zero = (0, 1, 2, 3) if m == "videoinf" else ()
            self._skip_io(instr, "video mode", zero)
            return

        # DEBUG
        if m == "print":
            r = vals[0]
            txt = fmt_typed_value(self.reg[r], t)
            self._log(f"[PRINT] R{r} ({t}) = {txt}")
            return

        # DATA MOVE
        if m == "mov":
            dst, src = vals
            self.reg[dst] = self.reg[src]
            return
        if m == "movi":
            dst, imm = vals
            self.reg[dst] = type_normalize(self._imm_to_i64(imm, instr.operands[1]["size"]), t)
            return

        # ARITHMETIC (REG, REG)
        if m in ("add", "sub", "mul", "div", "mod", "and", "or", "xor", "shl", "shr"):
            dst, src = vals
            if is_float_type(t) and m in ("and", "or", "xor", "shl", "shr"):
                raise VMFault(f"{m}: bitwise operation is not supported for float type")
            a = type_normalize(self.reg[dst], t)
            b = type_normalize(self.reg[src], t)
            if m == "add":
                self.reg[dst] = type_add(a, b, t)
            elif m == "sub":
                self.reg[dst] = type_sub(a, b, t)
            elif m == "mul":
                self.reg[dst] = type_mul(a, b, t)
            elif m == "div":
                if not is_float_type(t) and b == 0:
                    raise VMFault("div: division by zero")
                self.reg[dst] = type_div(a, b, t)
            elif m == "mod":
                if not is_float_type(t) and b == 0:
                    raise VMFault("mod: division by zero")
                self.reg[dst] = type_mod(a, b, t)
            elif m == "and":
                self.reg[dst] = type_normalize(a & b, t)
            elif m == "or":
                self.reg[dst] = type_normalize(a | b, t)
            elif m == "xor":
                self.reg[dst] = type_normalize(a ^ b, t)
            elif m == "shl":
                self.reg[dst] = type_shl(a, b, t)
            elif m == "shr":
                self.reg[dst] = type_shr(a, b, t)
            return

        # ARITHMETIC (REG, IMM)
        if m in ("addi", "subi"):
            dst = vals[0]
            imm = self._imm_to_i64(vals[1], instr.operands[1]["size"])
            a = type_normalize(self.reg[dst], t)
            im = type_normalize(imm, t)
            self.reg[dst] = type_add(a, im, t) if m == "addi" else type_sub(a, im, t)
            return
        if m in ("inc", "dec"):
            dst = vals[0]
            a = type_normalize(self.reg[dst], t)
            one = f32_to_raw(1.0) if t == "f32" else (f64_to_raw(1.0) if t == "f64" else 1)
            self.reg[dst] = type_add(a, one, t) if m == "inc" else type_sub(a, one, t)
            return
        if m == "not":
            if is_float_type(t):
                raise VMFault("not: not supported for float type")
            dst = vals[0]
            a = type_normalize(self.reg[dst], t)
            self.reg[dst] = type_normalize(~a, t)
            return

        # COMPARE
        if m == "cmp":
            a, b = vals
            self.flag_zero, self.flag_neg = do_compare(self.reg[a], self.reg[b], t)
            return
        if m == "cmpi":
            a = vals[0]
            imm = self._imm_to_i64(vals[1], instr.operands[1]["size"])
            self.flag_zero, self.flag_neg = do_compare(self.reg[a], imm, t)
            return

        # JUMPS
        if m == "jmp":
            self.pc = vals[0]
            return
        if m in ("jz", "jnz", "jg", "jl", "jge", "jle"):
            take = self._cond_take(m)
            if take:
                self.pc = vals[0]
            return
        if m == "jmpr":
            self.pc = self.reg[vals[0]] & 0xFFFFFFFF
            return
        if m in ("jzr", "jnzr", "jgr", "jlr", "jger", "jler"):
            # Register-indirect jumps share the same condition logic as
            # their address-immediate counterparts; strip the trailing "r"
            # to reuse _cond_take.
            take = self._cond_take(m[:-1])
            if take:
                self.pc = self.reg[vals[0]] & 0xFFFFFFFF
            return

        # CALL / RET
        if m == "callr":
            if len(self.call_stack) >= CALL_STACK_MAX:
                raise VMFault("callr: call stack overflow")
            self.call_stack.append(self.pc)
            self.pc = self.reg[vals[0]] & 0xFFFFFFFF
            return
        if m == "call":
            if len(self.call_stack) >= CALL_STACK_MAX:
                raise VMFault("call: call stack overflow")
            self.call_stack.append(self.pc)
            self.pc = vals[0]
            return
        if m == "ret":
            if not self.call_stack:
                raise VMFault("ret: call stack underflow")
            self.pc = self.call_stack.pop()
            return

        # DATA STACK
        if m == "push":
            if len(self.data_stack) >= DATA_STACK_MAX:
                raise VMFault("push: data stack overflow")
            self.data_stack.append(self.reg[vals[0]])
            return
        if m == "pop":
            if not self.data_stack:
                raise VMFault("pop: data stack underflow")
            self.reg[vals[0]] = self.data_stack.pop()
            return

        # MEMORY
        if m in ("load", "loadr"):
            size = isa.TYPE_SIZE[t]
            if m == "load":
                dst, addr = vals
            else:
                dst, areg = vals
                addr = self.reg[areg] & 0xFFFFFFFF
            if not self.ram_bounds_ok(addr, size):
                raise VMFault(f"{m}: address out of RAM range (0x{addr:x})")
            raw = self.read_mem(addr, size)
            self.reg[dst] = type_normalize(raw, t)
            return
        if m in ("store", "storer"):
            size = isa.TYPE_SIZE[t]
            if m == "store":
                addr, src = vals
            else:
                areg, src = vals
                addr = self.reg[areg] & 0xFFFFFFFF
            if not self.ram_bounds_ok(addr, size):
                raise VMFault(f"{m}: address out of RAM range (0x{addr:x})")
            v = type_normalize(self.reg[src], t)
            self.write_mem(addr, v, size)
            return
        if m == "memcpy":
            rdst, rsrc, rlen = vals
            dst = self.reg[rdst] & 0xFFFFFFFF
            src = self.reg[rsrc] & 0xFFFFFFFF
            length = self.reg[rlen] & 0xFFFFFFFF
            if not self.ram_bounds_ok(dst, length) or not self.ram_bounds_ok(src, length):
                raise VMFault("memcpy: address out of RAM range")
            if length:
                self.ram[dst:dst + length] = self.ram[src:src + length]
            return
        if m == "memset":
            raddr, rval, rlen = vals
            addr = self.reg[raddr] & 0xFFFFFFFF
            value = self.reg[rval] & 0xFF
            length = self.reg[rlen] & 0xFFFFFFFF
            if not self.ram_bounds_ok(addr, length):
                raise VMFault("memset: address out of RAM range")
            if length:
                self.ram[addr:addr + length] = bytes([value]) * length
            return

        # I/O (not debuggable)
        if m == "beep":
            self._skip_io(instr, "sound")
            return
        if m == "sound":
            self._skip_io(instr, "sound")
            return
        if m.startswith("x") and not m.startswith("xt") and not m.startswith("xi"):
            zero = {"xgetres": (0, 1)}.get(m, ())
            self._skip_io(instr, "graphics", zero)
            return
        if m.startswith("xt"):
            zero = {"xtgetres": (0, 1), "xtcurget": (0, 1)}.get(m, ())
            self._skip_io(instr, "text mode", zero)
            return
        if m.startswith("xi"):
            zero = {
                "ximouse": (0, 1, 2), "xikey": (0,),
                "xipoll": (0, 1, 2, 3), "xitmouse": (0, 1, 2), "xitpoll": (0, 1, 2, 3),
            }.get(m, ())
            self._skip_io(instr, "input", zero)
            return
        if m.startswith("fs"):
            zero = {
                "fsrmdir": (1,), "fswrite": (2,), "fsread": (3, 4),
                "fswriteb": (3,), "fsreadb": (3, 4), "fscd": (1,), "fsdir": (2, 3),
            }.get(m, ())
            self._skip_io(instr, "disk (SVFS)", zero)
            return

        raise VMFault(f"unimplemented instruction '{m}'")

    def _imm_to_i64(self, raw_unsigned, size):
        """Mirrors fetch_imm() from vm.c: an 8-byte value is taken as-is,
        a 4-byte value is always first interpreted as a signed 32-bit
        value and then sign-extended to 64 bits (regardless of the target
        type)."""
        if size == 8:
            return raw_unsigned
        v = raw_unsigned & 0xFFFFFFFF
        return v - (1 << 32) if v >= (1 << 31) else v

    def _cond_take(self, m):
        # Evaluate a conditional jump's condition from the Z/N flags set by
        # the most recent cmp/cmpi. Mirrors the flag logic in vm.c exactly:
        # jg/jl/jge/jle are defined purely in terms of Z (equal) and N (a<b).
        if m == "jz":
            return self.flag_zero
        if m == "jnz":
            return not self.flag_zero
        if m == "jg":
            return (not self.flag_zero) and (not self.flag_neg)
        if m == "jl":
            return self.flag_neg
        if m == "jge":
            return not self.flag_neg
        if m == "jle":
            return self.flag_zero or self.flag_neg
        return False
