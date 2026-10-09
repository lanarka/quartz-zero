"""
Quartz - The Computer Assembler
================================

qdisasm.py - disassembler: turns a Quartz .bin image back into readable
Quartz assembly text, printed to stdout.

This is the mirror image of qasm.py's encode()/write_bin(): read_binary()
parses the .bin header and pulls out the raw code bytes, disassemble()
walks that byte stream instruction by instruction using the same
opcode/operand table as the assembler (quartz/isa.py, via OPCODE_TO_BASE),
and load_map() optionally loads a .map file (as produced by qasm.py's
--map option) to print label names instead of raw hex addresses wherever an
address happens to match a known label. Output is not guaranteed to
re-assemble byte-for-byte identical to any original source (labels/comments
beyond what the .map file provides are lost), but is intended to be a
faithful, readable reconstruction of the instruction stream for inspection
and debugging.
"""

import sys
import os
from quartz import isa
from quartz import tools

OPERAND_SIZE = isa.OPERAND_SIZE


def sanitize_label(name):
    # Labels loaded from a .map file may be file-qualified as "file@label"
    # (see qasm.py's _qualify_label_ref()); '@' isn't valid in the assembly
    # syntax this tool emits, so it's replaced with a double underscore to
    # produce a printable, still-unique identifier.
    return name.replace('@', '__')


def load_map(path):
    # Loads a .map file (format: "0x<hex addr>:<label name>" per line, as
    # written by qasm.py's write_map()) into an addr -> [label names] dict.
    # A single address can have more than one label pointing at it, hence
    # the list. Returns an empty dict (silently) if no map file is given or
    # found - the disassembler still works without one, just showing raw
    # addresses instead of names.
    labels = {}
    if not path or not os.path.isfile(path):
        return labels
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            addr_str, _, name = line.partition(':')
            addr = int(addr_str, 16)
            labels.setdefault(addr, []).append(sanitize_label(name))
    return labels


def format_operand(kind, value, label_names, signed=False, size=None):
    # Renders one decoded operand value as assembly source text: registers
    # as "#N", addresses as a label name if one is known at that address
    # (falling back to hex), floats via repr() for round-trip-friendly
    # output, and plain integers reinterpreted as signed when the caller
    # says the type byte indicates a signed type.
    if kind == isa.REG:
        return f"#{value}"
    if kind == isa.ADDR:
        names = label_names.get(value)
        return names[0] if names else f"0x{value:06x}"
    if kind == isa.FLOAT:
        return f"{value:.10f}"
    if isinstance(value, float):
        return f"{value:.10f}"
    if signed and size is not None and value >= (1 << (size * 8 - 1)):
        value -= (1 << (size * 8))
    return str(value)


def disassemble(code, label_names):
    # Linear sweep over the raw code bytes, one instruction at a time:
    # opcode byte -> look up its mnemonic/operand spec in isa.py -> (if
    # typed) read the type byte -> read and decode each operand according
    # to its kind and, for IMMT, the just-read type. Any decoding failure
    # (unknown opcode, truncated instruction) is reported as an inline
    # comment rather than aborting, so a corrupt or partially-written binary
    # can still be disassembled as far as possible; each result tuple's
    # third element (`valid`) tells the caller whether to print the line as
    # real code or as a "// ..." comment.
    lines = []
    i, n = 0, len(code)
    while i < n:
        addr = i
        opcode = code[i]
        base = isa.OPCODE_TO_BASE.get(opcode)

        if base is None:
            lines.append((addr, f".byte 0x{opcode:02X}    // unknown opcode", False))
            i += 1
            continue

        base_name, spec = base
        i += 1

        name = base_name
        type_token = None
        if spec.typed:
            if i >= n:
                lines.append((addr, f"{base_name}    // file ended unexpectedly (missing type byte)", False))
                break
            tcode = code[i]
            i += 1
            tname = isa.CODE_TO_TYPE_NAME.get(tcode)
            if tname is None:
                lines.append((addr, f"{base_name}    // invalid type byte 0x{tcode:02X}", False))
                continue
            type_token = tname
            is_signed = tname.startswith('i')
        else:
            is_signed = False
            tname = None

        types = spec.operands
        operands = [type_token] if type_token is not None else []
        try:
            for kind in types:
                if kind == isa.IMMT:
                    size = isa.immt_encoded_size(tname)
                else:
                    size = OPERAND_SIZE[kind]
                raw = code[i:i + size]
                if len(raw) < size:
                    raise IndexError
                i += size
                if kind == isa.FLOAT:
                    val = tools.decode_float32(tools.from_bytes_le(raw))
                    operands.append(format_operand(kind, val, label_names))
                elif kind == isa.IMMT and tname in isa.TYPE_IS_FLOAT:
                    bits = tools.from_bytes_le(raw)
                    val = tools.decode_float32(bits) if size == 4 else tools.decode_float64(bits)
                    operands.append(format_operand(kind, val, label_names))
                else:
                    val = tools.from_bytes_le(raw)
                    operands.append(format_operand(kind, val, label_names, signed=is_signed, size=size))
        except IndexError:
            lines.append((addr, f"{name}    // file ended unexpectedly inside operands", False))
            break

        text = name if not operands else f"{name} " + " ".join(operands)
        lines.append((addr, text, True))
    return lines


def read_binary(path):
    # Parses the fixed 22-byte .bin header written by qasm.py's write_bin()
    # (magic/version, runtime version, entrypoint, code/data sizes and
    # addresses) and returns just the code bytes plus the runtime version
    # and entrypoint - the data segment itself is intentionally not
    # exposed here (see __unused__* below), since this tool only
    # reconstructs the instruction stream, not the data blobs.
    with open(path, 'rb') as f:
        data = f.read()
    magic = data[0:5]
    if magic != bytes([0x7F, 0x51, 0x00, 0x7F, 0x04]):
        print("// Warning: file does not start with the expected qasm .img header "
              "(magic/version mismatch), continuing anyway.", file=sys.stderr)

    if len(data) < 14:
        print("// Warning: file is shorter than the expected .img header (14 bytes), "
              "runtime version / entrypoint / code cannot be read reliably.", file=sys.stderr)
        runtime_version = 0xFF
        entry_point = 0
        code = []
        return code, runtime_version, entry_point

    runtime_version = data[5]
    entry_point = tools.from_bytes_le(data[6:10])
    code_size = tools.from_bytes_le(data[10:14])
    
    __unused__data_size = tools.from_bytes_le(data[14:18])
    __unused__data_addr = tools.from_bytes_le(data[18:22])
    
    code = list(data[22:22 + code_size])
    return code, runtime_version, entry_point


def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <program.img> [program.map]")
        sys.exit(1)

    bin_path = sys.argv[1]
    map_path = sys.argv[2] if len(sys.argv) >= 3 else os.path.splitext(bin_path)[0] + '.map'

    code, runtime_version, entry_point = read_binary(bin_path)
    label_names = load_map(map_path)

    if label_names:
        print(f"// using labels from {map_path}")
    else:
        print(f"// no .map file found ({map_path}), addresses shown as hex comments only")

    print(f"%runtime {runtime_version}")

    entry_names = label_names.get(entry_point)
    if entry_names:
        print(f"%entrypoint {entry_names[0]}")
    else:
        print(f"%entrypoint 0x{entry_point:06x}")
    print()

    for addr, text, valid in disassemble(code, label_names):
        for name in label_names.get(addr, []):
            print(f"{name}:")
        line = f"/*0x{addr:06x}*/\t{text}"
        print(line if valid else f"// {line}")


if __name__ == '__main__':
    main()
