"""
Decoding of QuartzVM bytecode.

The single function decode_one() is used both for the static disassembly
listing (build_listing) and for real execution in the VM (emulator.py) - so
the display and the execution can never drift out of sync with each other.
"""

from . import isa


class Instr:
    """A single decoded instruction.

    Uses __slots__ since many thousands of these can exist for a large
    program (the whole static listing is built up front), so keeping the
    per-instance memory overhead low is worthwhile.
    """

    __slots__ = (
        "addr", "size", "opcode", "mnemonic", "type_name",
        "operands", "valid", "error", "raw",
    )

    def __init__(self, addr, size, opcode, mnemonic, type_name, operands, valid, error, raw):
        self.addr = addr
        self.size = size
        self.opcode = opcode
        self.mnemonic = mnemonic
        self.type_name = type_name
        self.operands = operands  # list of {"kind":..., "value": int}
        self.valid = valid
        self.error = error
        self.raw = raw

    @property
    def category(self):
        # None for invalid/unknown instructions, since there's no mnemonic
        # to look up a category for.
        if self.mnemonic is None:
            return None
        return isa.INSTRUCTIONS[self.mnemonic].category

    @property
    def next_addr(self):
        # Address immediately after this instruction - i.e. where the
        # (linear, non-branching) next instruction would start.
        return self.addr + self.size


def decode_one(code, addr):
    """Decode a single instruction at address addr. Never raises - on any
    error it returns an Instr with valid=False and a description in .error
    (the same principle as vm_fault in the C VM)."""
    n = len(code)
    if addr >= n:
        return None

    opcode = code[addr]
    entry = isa.OPCODE_TO_NAME.get(opcode)
    if entry is None:
        return Instr(addr, 1, opcode, None, None, [], False,
                      f"unknown opcode 0x{opcode:02X}", bytes(code[addr:addr + 1]))

    name, spec = entry
    i = addr + 1
    type_name = None

    # Typed instructions (add/sub/load/store/...) carry one extra byte right
    # after the opcode that selects the operand type (i8..u64, f32/f64).
    if spec.typed:
        if i >= n:
            return Instr(addr, i - addr, opcode, name, None, [], False,
                          "missing type byte (end of code)", bytes(code[addr:i]))
        tcode = code[i]
        type_name = isa.CODE_TO_TYPE_NAME.get(tcode)
        i += 1
        if type_name is None:
            return Instr(addr, i - addr, opcode, name, None, [], False,
                          f"invalid type byte 0x{tcode:02X}", bytes(code[addr:i]))

    # Walk the operand list declared for this mnemonic in isa.py, reading
    # each one as a little-endian unsigned integer of the appropriate size.
    # (Signed/float interpretation happens later, in format_operand/the
    # emulator - here we just extract the raw bits.)
    operands = []
    for kind in spec.operands:
        if kind == isa.IMMT:
            size = isa.immt_encoded_size(type_name)
        else:
            size = isa.OPERAND_SIZE[kind]
        if i + size > n:
            return Instr(addr, i - addr, opcode, name, type_name, operands, False,
                          "code ended unexpectedly inside the operands",
                          bytes(code[addr:i]))
        raw_bytes = bytes(code[i:i + size])
        value = int.from_bytes(raw_bytes, "little", signed=False)
        i += size
        operands.append({"kind": kind, "size": size, "value": value})

    return Instr(addr, i - addr, opcode, name, type_name, operands, True, None,
                 bytes(code[addr:i]))


def format_operand(op, type_name, labels):
    """Render a single decoded operand as text for the disassembly view,
    e.g. "#3" for a register, a label name (or hex address) for a jump
    target, or the numeric value for an immediate."""
    kind = op["kind"]
    value = op["value"]
    if kind == isa.REG:
        return f"#{value}"
    if kind == isa.ADDR:
        # Prefer a label name over a raw address when the .map file has one.
        names = labels.get(value)
        if names:
            return names[0]
        return f"0x{value:06x}"
    if kind == isa.FLOAT:
        import struct
        f = struct.unpack("<f", struct.pack("<I", value & 0xFFFFFFFF))[0]
        return repr(f)
    if kind == isa.IMMT:
        if type_name in isa.TYPE_IS_FLOAT:
            import struct
            if isa.TYPE_SIZE[type_name] == 4:
                f = struct.unpack("<f", struct.pack("<I", value & 0xFFFFFFFF))[0]
            else:
                f = struct.unpack("<d", struct.pack("<Q", value & 0xFFFFFFFFFFFFFFFF))[0]
            return repr(f)
        size = isa.TYPE_SIZE[type_name]
        if type_name in isa.TYPE_IS_SIGNED:
            bits = size * 8
            if size == 8:
                # A 64-bit immediate was read directly as a 64-bit value in
                # the real instruction encoding (no sign-extension needed).
                sval = value if value < (1 << 63) else value - (1 << 64)
            else:
                # A 32-bit immediate was always read as a signed 32-bit
                # value and sign-extended, regardless of the target type.
                sval = value if value < (1 << 31) else value - (1 << 32)
            return str(sval)
        return str(value)
    # IMM8/IMM16/IMM32 (untyped, e.g. OP_VIDEO)
    return str(value)


def format_instr(instr, labels):
    """Render a full decoded instruction as one line of assembly-like text,
    e.g. "add i32 #1 #2" or an error line for an invalid/undecodable one."""
    if not instr.valid:
        if instr.mnemonic:
            return f"{instr.mnemonic}    ; {instr.error}"
        return f".byte 0x{instr.opcode:02X}    ; {instr.error}"

    parts = [instr.mnemonic]
    if instr.type_name:
        parts[0] = instr.mnemonic  # mnemonic stays unsuffixed, the type is shown separately
        parts.append(instr.type_name)
    for op in instr.operands:
        parts.append(format_operand(op, instr.type_name, labels))
    return " ".join(parts)


def build_listing(code):
    """Linearly walk the whole code segment starting at address 0 and build
    a list of Instr plus an addr->index map (for jumps/PC lookups). If it
    hits an invalid instruction, it continues 1 byte further on (so a
    single bad instruction doesn't stop the entire listing)."""
    listing = []
    addr_index = {}
    addr = 0
    n = len(code)
    while addr < n:
        instr = decode_one(code, addr)
        if instr is None:
            break
        addr_index[addr] = len(listing)
        listing.append(instr)
        if instr.valid:
            addr = instr.next_addr
        else:
            addr += 1
    return listing, addr_index
