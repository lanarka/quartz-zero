"""
Quartz - The Computer Assembler
================================

qasm.py is the main entry point of the Quartz assembler: it turns a .quartz
source file (plus any files it %includes) into a Quartz .bin image (or a raw
code blob with --raw) that the Quartz VM (see vm.c/vm.h in the emulator
project) can execute directly.

Pipeline overview (see assemble_file() for the orchestration):

  1. preprocess()   - Reads the main source file and everything it
                       transitively %includes, strips comments
                       (strip_comments()), and handles the file-level
                       directives %include / %runtime / %entrypoint / %data.
                       Produces a single flat, ordered list of source lines
                       ("merged") tagged with which physical file/line they
                       came from, so later error messages can still point at
                       the right file.
  2. parse_source() - Parses each merged line individually with a small Lark
                       LALR grammar (_GRAMMAR) into a flat stream of
                       LabelDef/InstrDef "events", in source order. Kept
                       line-by-line (rather than parsing the whole file as
                       one tree) so a syntax error on one line doesn't
                       prevent reporting errors on other lines.
  3. analyze()      - Walks the event stream, resolves each instruction
                       against the ISA (quartz/isa.py) to check operand
                       counts/kinds/ranges, tracks label addresses as it
                       computes each instruction's encoded size, and
                       (optionally) performs lightweight type-tracking of
                       registers to warn about a value being read back with
                       a different type than it was last written with
                       (--cast-warnings). Label references are resolved in a
                       second pass once every label's address is known
                       (pending_addr_refs).
  4. encode()       - Turns the fully-resolved instruction list into the
                       actual little-endian byte stream.
  5. write_bin()/write_raw() - Wraps the code (plus an optional data segment
                       from %data, see quartz/data.py) in the Quartz .bin
                       header, or writes the raw bytes as-is.

Multi-file programs: each source file gets a short "file_id" (its basename
without extension). Labels defined in a file other than the main one are
automatically qualified as "file_id@label" internally, so the same label
name can be reused across independently-written include files without
clashing - see _qualify_label_ref().
"""

import argparse
import os
import re
import sys
from lark import Lark
from lark.exceptions import UnexpectedCharacters, UnexpectedToken, UnexpectedInput
from quartz import isa
from quartz import tools
from quartz.errors import AsmError, AsmWarning, print_errors
from quartz.data import add_data

# Lark grammar for a single line of Quartz assembly. Deliberately line-scoped
# (see parse_source(), which calls _parser.parse() once per line) rather than
# describing a whole program, so that one malformed line can be reported as a
# single syntax error without derailing the parse of every other line.
_GRAMMAR = r"""
start: label* instruction?

label: NAME ":"
instruction: NAME operand*
operand: REGISTER -> register
       | FLOAT    -> float_
       | HEXINT   -> hexint
       | BININT   -> binint
       | CHARLIT  -> charlit
       | INT      -> int_
       | IDENT    -> ident

NAME: /[A-Za-z_][A-Za-z0-9_]*/
IDENT: /[A-Za-z_][A-Za-z0-9_]*(@[A-Za-z_][A-Za-z0-9_]*)?/
REGISTER: /#[0-9]+/
FLOAT.3: /-?[0-9]+\.[0-9]+/
HEXINT.3: /0x[0-9a-fA-F]+/
BININT.3: /0b[01]+/
CHARLIT.3: /'(\\[nrt0\\']|[^'\\\n])'/
INT.1: /-?[0-9]+/

%ignore /[ \t]+/
"""

_parser = Lark(_GRAMMAR, parser='lalr')


# Strips /* ... */ and // ... comments from raw source text while preserving
# line/column positions exactly (comments are blanked out with spaces, not
# removed), so that line numbers and column offsets reported for later
# stages (parsing, analysis) still line up with the original file. Returns
# the cleaned text plus a list of AsmError for unterminated block comments.
def strip_comments(text):
    out = []
    errors = []
    i, n = 0, len(text)
    line, col = 1, 1

    while i < n:
        two = text[i:i + 2]
        if two == '/*':
            start_line, start_col = line, col
            out.append('  ')
            i += 2
            col += 2
            closed = False
            while i < n:
                if text[i:i + 2] == '*/':
                    out.append('  ')
                    i += 2
                    col += 2
                    closed = True
                    break
                elif text[i] == '\n':
                    out.append('\n')
                    i += 1
                    line += 1
                    col = 1
                else:
                    out.append(' ')
                    i += 1
                    col += 1
            if not closed:
                errors.append(AsmError(start_line, start_col, "Unterminated block comment", length=2))
        elif two == '//':
            while i < n and text[i] != '\n':
                out.append(' ')
                i += 1
                col += 1
        elif text[i] == '\n':
            out.append('\n')
            i += 1
            line += 1
            col = 1
        else:
            out.append(text[i])
            i += 1
            col += 1

    return ''.join(out), errors

_INCLUDE_RE = re.compile(r'^%include\s+"([^"]*)"\s*$')
_RUNTIME_RE = re.compile(r'^%runtime\s+(\S+)\s*$')
_ENTRYPOINT_RE = re.compile(r'^%entrypoint\s+(\S+)\s*$')
_DATA_RE = re.compile(r'^%data\s+"([^"]*)"\s*$')
_INT_RE = re.compile(r'^-?[0-9]+$')
_HEXINT_RE = re.compile(r'^0[xX][0-9a-fA-F]+$')
_LABEL_REF_RE = re.compile(r'^[A-Za-z_][A-Za-z0-9_]*(@[A-Za-z_][A-Za-z0-9_]*)?$')

DEFAULT_runtime_version = 0xFF
DEFAULT_ENTRYPOINT = 0
_ADDR_RANGE = (0, (1 << 32) - 1)


# Qualifies a bare label reference with the file it needs to resolve
# against, so that "label" typed inside an %include'd file unambiguously
# means "that file's own label" rather than accidentally matching a
# same-named label in the main file (or another include). A reference that
# is already qualified ("file@label") or that lives in the main file itself
# is left untouched.
def _qualify_label_ref(text, file_id, main_id):
    if '@' in text:
        return text
    if file_id != main_id:
        return f"{file_id}@{text}"
    return text


class SourceFile:
    """Bookkeeping for one physical source file: its short file_id, its path
    on disk, and its original (un-preprocessed) lines - kept around so error
    messages can quote/point at the real source later (see quartz/errors.py:
    preview())."""
    def __init__(self, file_id, path, raw_lines):
        self.file_id = file_id
        self.path = path
        self.raw_lines = raw_lines


def file_id_for(path):
    return os.path.splitext(os.path.basename(path))[0]


# Reads main_path and, recursively, every file it %includes, concatenating
# them into a single ordered stream of (file_id, lineno, line_text) tuples
# ("merged") while also collecting the file-scoped %runtime / %entrypoint /
# %data directives (each of which may only be defined once across the whole
# program - a repeat definition produces a warning and is ignored, tracked
# via the *_state dicts below). Returns everything assemble_file() needs to
# proceed to parsing: merged lines, a map of file_id -> SourceFile (for
# later error reporting), the resolved runtime version, entrypoint
# reference, data reference, and any errors/warnings collected so far.
def preprocess(main_path):
    main_id = file_id_for(main_path)
    files = {}
    merged = []
    errors = []
    warnings = []
    runtime_state = {'value': None, 'file_id': None, 'line': None}
    entrypoint_state = {'ref': None, 'file_id': None, 'line': None}
    data_state = {'value': None, 'file_id': None, 'line': None}
    include_stack = []

    def expand(path, parent_ctx):
        # Recursively reads `path` and inlines it into `merged`. parent_ctx
        # is (file_id, lineno) of the %include statement that triggered this
        # expansion (or None for the initial call on main_path), used only
        # to attribute "file not found"/circular-include errors to the
        # right including line.
        abspath = os.path.abspath(path)
        if abspath in include_stack:
            chain = ' -> '.join(include_stack + [abspath])
            if parent_ctx:
                pfid, pline = parent_ctx
                errors.append(AsmError(pline, 1, f"Circular %include detected: {chain}",
                                        file_id=pfid))
            else:
                errors.append(AsmError(1, 1, f"Circular %include detected: {chain}"))
            return

        if not os.path.isfile(path):
            if parent_ctx:
                pfid, pline = parent_ctx
                errors.append(AsmError(pline, 1, f"Cannot open included file '{path}'",
                                        file_id=pfid))
            else:
                #errors.append(AsmError(1, 1, f"Cannot open source file '{path}'"))
                print(f"Cannot open source file '{path}'")
                sys.exit(1)
            return

        fid = file_id_for(path)
        try:
            with open(path, encoding='utf-8') as f:
                raw_text = f.read()
        except OSError as e:
            if parent_ctx:
                pfid, pline = parent_ctx
                errors.append(AsmError(pline, 1, f"Cannot open included file '{path}' ({e})",
                                        file_id=pfid))
            else:
                errors.append(AsmError(1, 1, f"Cannot open source file '{path}' ({e})"))
            return

        raw_lines = raw_text.split('\n')
        clean_text, comment_errors = strip_comments(raw_text)
        for ce in comment_errors:
            ce.file_id = fid
        errors.extend(comment_errors)
        clean_lines = clean_text.split('\n')

        files[fid] = SourceFile(fid, path, raw_lines)
        include_stack.append(abspath)

        for lineno, line_text in enumerate(clean_lines, start=1):
            stripped = line_text.strip()
            if not stripped or not stripped.startswith('%'):
                # Ordinary code line (or blank): pass it straight through to
                # the merged stream to be parsed later.
                merged.append((fid, lineno, line_text))
                continue

            m_inc = _INCLUDE_RE.match(stripped)
            if m_inc:
                target = m_inc.group(1)
                target_path = os.path.join(os.path.dirname(os.path.abspath(path)), target)
                expand(target_path, (fid, lineno))
                continue

            m_runtime = _RUNTIME_RE.match(stripped)
            if m_runtime:
                raw_val = m_runtime.group(1)
                if not _INT_RE.match(raw_val):
                    errors.append(AsmError(lineno, 1,
                                            f"Invalid %runtime value '{raw_val}' (expected an integer 0..255)",
                                            length=len(stripped), file_id=fid))
                    continue
                value = int(raw_val)
                if not (0 <= value <= 255):
                    errors.append(AsmError(lineno, 1,
                                            f"%runtime value out of range (0..255): {value}",
                                            length=len(stripped), file_id=fid))
                    continue
                if runtime_state['value'] is None:
                    runtime_state['value'] = value
                    runtime_state['file_id'] = fid
                    runtime_state['line'] = lineno
                else:
                    first_path = files[runtime_state['file_id']].path
                    warnings.append(AsmWarning(
                        lineno, 1,
                        f"'%runtime' is already defined (first defined at file "
                        f"'{first_path}', line {runtime_state['line']}) - ignoring",
                        length=len(stripped), file_id=fid))
                continue

            m_entry = _ENTRYPOINT_RE.match(stripped)
            if m_entry:
                raw_val = m_entry.group(1)
                col = m_entry.start(1) + 1
                if _HEXINT_RE.match(raw_val) or _INT_RE.match(raw_val):
                    value = int(raw_val, 16) if raw_val.lower().startswith('0x') else int(raw_val)
                    if not (_ADDR_RANGE[0] <= value <= _ADDR_RANGE[1]):
                        errors.append(AsmError(
                            lineno, 1,
                            f"%entrypoint address out of range (0..0x{_ADDR_RANGE[1]:x}): {raw_val}",
                            length=len(stripped), file_id=fid))
                        continue
                    ref = ('addr', value, fid, lineno, col, raw_val)
                elif _LABEL_REF_RE.match(raw_val):
                    qname = _qualify_label_ref(raw_val, fid, main_id)
                    ref = ('label', qname, fid, lineno, col, raw_val)
                else:
                    errors.append(AsmError(
                        lineno, 1,
                        f"Invalid %entrypoint value '{raw_val}' "
                        f"(expected a label name, or an address in decimal or hex '0x...')",
                        length=len(stripped), file_id=fid))
                    continue
                if entrypoint_state['ref'] is None:
                    entrypoint_state['ref'] = ref
                    entrypoint_state['file_id'] = fid
                    entrypoint_state['line'] = lineno
                else:
                    first_path = files[entrypoint_state['file_id']].path
                    warnings.append(AsmWarning(
                        lineno, 1,
                        f"'%entrypoint' is already defined (first defined at file "
                        f"'{first_path}', line {entrypoint_state['line']}) - ignoring",
                        length=len(stripped), file_id=fid))
                continue

            m_data = _DATA_RE.match(stripped)
            if m_data:
                raw_val = m_data.group(1)
                if data_state['value'] is None:
                    data_state['value'] = raw_val
                    data_state['file_id'] = fid
                    data_state['line'] = lineno
                else:
                    first_path = files[data_state['file_id']].path
                    warnings.append(AsmWarning(
                        lineno, 1,
                        f"'%data' is already defined (first defined at file "
                        f"'{first_path}', line {data_state['line']}) - ignoring",
                        length=len(stripped), file_id=fid))
                continue

            directive_name = stripped.split()[0] if stripped.split() else stripped
            errors.append(AsmError(lineno, 1, f"Unknown directive '{directive_name}'",
                                    length=len(directive_name), file_id=fid))

        include_stack.pop()

    expand(main_path, None)
    runtime_version = runtime_state['value'] if runtime_state['value'] is not None else DEFAULT_runtime_version
    entrypoint_ref = entrypoint_state['ref']
    data_ref = data_state['value']
    return merged, files, runtime_version, entrypoint_ref, data_ref, errors, warnings


class LabelDef:
    """A `label:` line, recorded at parse time. Its address is only assigned
    later, in analyze(), once we know the size of every instruction that
    precedes it."""
    def __init__(self, name, line, col, file_id):
        self.name = name
        self.line = line
        self.col = col
        self.file_id = file_id


class InstrDef:
    """One parsed instruction line: mnemonic plus its raw, not-yet-validated
    operand tokens (each a (kind, text, line, col) tuple, as produced by
    parse_source()). Operand validation/encoding happens later in
    analyze()."""
    def __init__(self, mnemonic, line, col, operands, file_id):
        self.mnemonic = mnemonic
        self.line = line
        self.col = col
        self.operands = operands
        self.file_id = file_id


# Parses each preprocessed source line independently with the Lark grammar
# and flattens the resulting parse trees into a single ordered list of
# LabelDef/InstrDef "events" (labels and instructions can share a line, e.g.
# "loop: add i32 #0 #1", hence a line can produce more than one event).
# Line-by-line parsing means a syntax error on one line is reported and
# skipped without blocking parsing of the rest of the file.
def parse_source(merged_lines):
    events = []
    errors = []

    for file_id, lineno, line_text in merged_lines:
        if not line_text.strip():
            continue
        try:
            tree = _parser.parse(line_text)
        except UnexpectedCharacters as e:
            errors.append(AsmError(lineno, e.column, f"Illegal character '{e.char}'",
                                    file_id=file_id))
            continue
        except UnexpectedToken as e:
            tok = e.token
            errors.append(AsmError(lineno, tok.column,
                                    f"Unexpected token '{tok}'",
                                    length=len(str(tok)), file_id=file_id))
            continue
        except UnexpectedInput as e:
            errors.append(AsmError(lineno, getattr(e, 'column', 1),
                                    "Syntax error", file_id=file_id))
            continue

        for child in tree.children:
            if child.data == 'label':
                name_tok = child.children[0]
                events.append(LabelDef(str(name_tok), lineno, name_tok.column, file_id))
            elif child.data == 'instruction':
                name_tok = child.children[0]
                operands = []
                for opnode in child.children[1:]:
                    val_tok = opnode.children[0]
                    operands.append((opnode.data, str(val_tok), lineno, val_tok.column))
                events.append(InstrDef(str(name_tok), lineno, name_tok.column, operands, file_id))

    return events, errors

_TYPE_KINDS = {
    isa.REG: {'register'},
    isa.IMM8: {'int_', 'hexint', 'binint', 'charlit'},
    isa.IMM16: {'int_', 'hexint', 'binint'},
    isa.IMM32: {'int_', 'hexint', 'binint'},
    isa.IMMT: {'int_', 'hexint', 'binint', 'charlit', 'float_'},
    isa.FLOAT: {'float_', 'int_'},
    isa.ADDR: {'ident', 'int_', 'hexint', 'binint'},
}

_CHAR_ESCAPES = {'n': '\n', 'r': '\r', 't': '\t', '0': '\0', '\\': '\\', "'": "'"}

# Role of each REG operand for the (optional, --cast-warnings) "cast" type
# check, which tracks the type each register was last written with and
# warns when it's later read back with a different one:
#   'r'    - the register is read with the current instruction's type
#            (checked against the type it was last written with)
#   'w'    - the register is (only) written with the current instruction's
#            type
#   'rw'   - the register is first read (checked), then written with the
#            same type (e.g. add: reg #0 = reg #0 <op> reg #1)
#   'addr' - the register holds an address (loadr/storer) - the type check
#            is skipped and the tracked type is left unchanged
#   'u'    - the register is written WITHOUT any type information
#            (mov/rnd/pop) - its tracked type then becomes "unknown"
#   None   - the register is read without any type check (push), tracked
#            type is left unchanged
#
_REG_ROLES = {
    'print': ['r'],
    'mov': ['u', None],
    'movi': ['w'],
    'add': ['rw', 'r'], 'sub': ['rw', 'r'], 'mul': ['rw', 'r'], 'div': ['rw', 'r'],
    'mod': ['rw', 'r'], 'and': ['rw', 'r'], 'or': ['rw', 'r'], 'xor': ['rw', 'r'],
    'shl': ['rw', 'r'], 'shr': ['rw', 'r'],
    'addi': ['rw'], 'subi': ['rw'],
    'inc': ['rw'], 'dec': ['rw'], 'not': ['rw'],
    'cmp': ['r', 'r'], 'cmpi': ['r'],
    'rnd': ['u'],
    'push': [None],
    'pop': ['u'],
    'load': ['w'],
    'store': ['r'],
    'loadr': ['w', 'addr'],
    'storer': ['addr', 'r'],
}


def _parse_char_literal(text):
    inner = text[1:-1]
    if inner.startswith('\\'):
        ch = _CHAR_ESCAPES[inner[1]]
    else:
        ch = inner
    return ord(ch)


def _parse_int_literal(kind, text):
    if kind == 'hexint':
        return int(text, 16)
    if kind == 'binint':
        return int(text, 2)
    if kind == 'charlit':
        return _parse_char_literal(text)
    return int(text)


def analyze(events, main_id, cast_warnings=False, extra_used_labels=None):
    """Semantic pass over the event stream produced by parse_source().

    Single forward pass over `events` that, for every InstrDef:
      - looks up its mnemonic in the ISA (isa.INSTRUCTIONS),
      - validates/decodes its (optional) leading type operand for typed
        instructions,
      - checks the remaining operand count/kinds/ranges against what the
        ISA expects,
      - assigns it its address (`offset`, running byte count) and computes
        how many bytes it will encode to, advancing `offset` for the next
        instruction,
      - optionally (--cast-warnings) tracks each register's last-written
        type to flag reads with a mismatched type (see _REG_ROLES above).

    Label addresses (`labels`) are known immediately, since a LabelDef simply
    records the current `offset`. Address-typed operands that reference a
    label (`ident` operands, e.g. `jmp loop`), however, may point forward to
    a label not yet seen - these are deferred into `pending_addr_refs` and
    resolved in a second pass at the end, once every label's final address
    is known.

    Returns (encoded, labels, errors, warnings), where `encoded` is a list of
    dicts ready for encode() to turn into bytes.
    """
    errors = []
    labels = {}
    label_positions = {}
    encoded = []
    pending_addr_refs = []
    used_labels = set(extra_used_labels or ())
    reg_last_type = {}
    offset = 0

    for ev in events:
        if isinstance(ev, LabelDef):
            qname = ev.name if ev.file_id == main_id else f"{ev.file_id}@{ev.name}"
            if qname in labels:
                errors.append(AsmError(ev.line, ev.col,
                                        f"Label '{ev.name}' already defined",
                                        length=len(ev.name), file_id=ev.file_id))
            else:
                labels[qname] = offset
                label_positions[qname] = (ev.file_id, ev.line, ev.col, len(ev.name))
            continue

        # InstrDef
        spec = isa.INSTRUCTIONS.get(ev.mnemonic)
        if spec is None:
            errors.append(AsmError(ev.line, ev.col,
                                    f"Unknown instruction '{ev.mnemonic}'",
                                    length=len(ev.mnemonic), file_id=ev.file_id))
            continue
        vtype = None
        rest_operands = ev.operands
        if spec.typed:
            if not ev.operands:
                errors.append(AsmError(
                    ev.line, ev.col,
                    f"Instruction '{ev.mnemonic}' expects a type (e.g. i32) as its first operand",
                    length=len(ev.mnemonic), file_id=ev.file_id))
                continue

            tkind, ttext, tline, tcol = ev.operands[0]
            if tkind != 'ident':
                errors.append(AsmError(
                    tline, tcol,
                    f"Expected a type name (e.g. i32) after '{ev.mnemonic}', got '{ttext}'",
                    length=len(ttext), file_id=ev.file_id))
                continue
            if ttext not in isa.TYPE_NAME_TO_CODE:
                errors.append(AsmError(
                    tline, tcol,
                    f"Unknown type '{ttext}' (expected one of: {', '.join(isa.TYPE_SUFFIXES)})",
                    length=len(ttext), file_id=ev.file_id))
                continue
            if ttext in isa.TYPE_IS_FLOAT and not spec.float_ok:
                errors.append(AsmError(
                    tline, tcol,
                    f"Instruction '{ev.mnemonic}' does not support float type '{ttext}'",
                    length=len(ttext), file_id=ev.file_id))
                continue

            vtype = isa.TYPE_NAME_TO_CODE[ttext]
            rest_operands = ev.operands[1:]

        expected = spec.operands
        if len(rest_operands) != len(expected):
            errors.append(AsmError(
                ev.line, ev.col,
                f"Instruction '{ev.mnemonic}' expects {len(expected)} operand(s), "
                f"got {len(rest_operands)}",
                length=len(ev.mnemonic), file_id=ev.file_id))
            continue

        entry = {'opcode': spec.opcode, 'types': expected, 'values': [None] * len(expected),
                  'vtype': vtype, 'address': offset}
        ok = True
        reg_roles = _REG_ROLES.get(ev.mnemonic)
        for idx, (exp_type, (kind, text, oline, ocol)) in enumerate(zip(expected, rest_operands)):
            if kind not in _TYPE_KINDS[exp_type]:
                errors.append(AsmError(oline, ocol, f"Illegal operand '{text}'",
                                        length=len(text), file_id=ev.file_id))
                ok = False
                break

            if exp_type == isa.REG:
                value = int(text[1:])
                if not (0 <= value < isa.REGISTER_COUNT):
                    errors.append(AsmError(oline, ocol, f"Illegal operand '{text}'",
                                            length=len(text), file_id=ev.file_id))
                    ok = False
                    break
                entry['values'][idx] = value
                role = reg_roles[idx] if reg_roles and idx < len(reg_roles) else None
                if role in ('r', 'rw') and vtype is not None:
                    prev = reg_last_type.get(value)
                    if cast_warnings and prev is not None and prev != vtype:
                        errors_or_warn_msg = (
                            f"Register #{value} was last written as "
                            f"'{isa.CODE_TO_TYPE_NAME[prev]}' but is read here as "
                            f"'{isa.CODE_TO_TYPE_NAME[vtype]}'")
                        entry.setdefault('_cast_warnings', []).append(
                            (oline, ocol, errors_or_warn_msg, len(text)))
                if role in ('w', 'rw') and vtype is not None:
                    reg_last_type[value] = vtype
                elif role == 'u':
                    reg_last_type[value] = None

            elif exp_type == isa.FLOAT:
                value = float(text)
                entry['values'][idx] = value

            elif exp_type == isa.IMMT:
                tname = isa.CODE_TO_TYPE_NAME[entry['vtype']]
                if tname in isa.TYPE_IS_FLOAT:
                    if kind not in ('float_', 'int_'):
                        errors.append(AsmError(oline, ocol,
                                                f"Illegal operand '{text}' (expected a number for {tname})",
                                                length=len(text), file_id=ev.file_id))
                        ok = False
                        break
                    entry['values'][idx] = float(text)
                else:
                    if kind not in ('int_', 'hexint', 'binint', 'charlit'):
                        errors.append(AsmError(oline, ocol,
                                                f"Illegal operand '{text}' (expected an integer for {tname})",
                                                length=len(text), file_id=ev.file_id))
                        ok = False
                        break
                    value = _parse_int_literal(kind, text)
                    lo, hi = isa.int_range_for_type(tname)
                    if not (lo <= value <= hi):
                        errors.append(AsmError(oline, ocol, f"Illegal operand '{text}'",
                                                length=len(text), file_id=ev.file_id))
                        ok = False
                        break
                    if tname in isa.TYPE_IS_UNSIGNED and value < 0:
                        entry.setdefault('_cast_warnings', []).append(
                            (oline, ocol,
                             f"Negative value '{value}' used for unsigned type '{tname}'",
                             len(text)))
                    entry['values'][idx] = value

            elif exp_type == isa.ADDR and kind == 'ident':
                qref = _qualify_label_ref(text, ev.file_id, main_id)
                pending_addr_refs.append((entry, idx, qref, oline, ocol, ev.file_id, text))
                used_labels.add(qref)
                entry['values'][idx] = 0

            else:
                value = _parse_int_literal(kind, text)
                lo, hi = isa.INT_RANGE[exp_type]
                if not (lo <= value <= hi):
                    errors.append(AsmError(oline, ocol, f"Illegal operand '{text}'",
                                            length=len(text), file_id=ev.file_id))
                    ok = False
                    break
                entry['values'][idx] = value

        if not ok:
            continue

        encoded.append(entry)
        type_byte = isa.TYPE_BYTE_SIZE if vtype is not None else 0
        offset += 1 + type_byte + sum(isa.operand_encoded_size(t, vtype) for t in expected)

    warnings = []
    for entry in encoded:
        for (line, col, msg, length) in entry.pop('_cast_warnings', []):
            warnings.append(AsmWarning(line, col, msg, length=length))

    # Second pass: every label's address is now known, so forward (and
    # backward) label references collected during the main loop above can
    # finally be resolved into concrete addresses.
    for entry, idx, qref, line, col, file_id, raw_text in pending_addr_refs:
        if qref in labels:
            entry['values'][idx] = labels[qref]
        else:
            errors.append(AsmError(line, col, f"Undefined label '{raw_text}'",
                                    length=len(raw_text), file_id=file_id))

    for qname, (file_id, line, col, name_len) in label_positions.items():
        if qname not in used_labels:
            warnings.append(AsmWarning(line, col, f"Label '{qname}' is defined but never used",
                                        length=name_len, file_id=file_id))

    return encoded, labels, errors, warnings


def encode(encoded_instrs):
    """Serializes the fully-resolved instruction list from analyze() into
    the final little-endian machine code byte stream: opcode byte, then (for
    typed instructions) the type byte, then each operand in the width/format
    isa.py defines for it (floats and IMMT-typed immediates get special
    encoding via quartz/tools.py's IEEE-754 helpers)."""
    code = []
    for entry in encoded_instrs:
        code.append(entry['opcode'])
        if entry['vtype'] is not None:
            code.append(entry['vtype'])
        for t, v in zip(entry['types'], entry['values']):
            if t == isa.FLOAT:
                code.extend(tools.to_bytes_le(tools.encode_float32(v), 4))
            elif t == isa.IMMT:
                tname = isa.CODE_TO_TYPE_NAME[entry['vtype']]
                size = isa.immt_encoded_size(tname)
                if tname in isa.TYPE_IS_FLOAT:
                    bits = tools.encode_float32(v) if size == 4 else tools.encode_float64(v)
                    code.extend(tools.to_bytes_le(bits, size))
                else:
                    code.extend(tools.to_bytes_le(v, size))
            else:
                code.extend(tools.to_bytes_le(v, isa.OPERAND_SIZE[t]))
    return code


def write_data_map(datamap_path, addrs):
    """Writes a human-readable "<addr>:<name>" listing of every named blob
    inside the %data segment, mirroring write_map()'s format for code
    labels. Purely a debugging aid; the VM never reads this file."""
    with open(datamap_path, 'w') as f:
        for name, addr in addrs:
            f.write(f"0x{addr:06x}:{name}\n")


def write_bin(output_path, code, runtime_version=DEFAULT_runtime_version, entry_point=DEFAULT_ENTRYPOINT, data_ref=None):
    """Writes the Quartz .bin image: a fixed 22-byte header (magic number,
    binary format version, target runtime version, entrypoint, code size,
    data size, data address - see the matching header parsed by
    load_program() in the VM's vm.c) followed by the code and, if %data was
    used, the processed data segment."""
    header = [
        0x7F, 0x51, 0x00, 0x7F,  # Magick
        0x04,                    # Binary version
        runtime_version & 0xFF,  # Runtime version
    ]
    entry_point_bytes = tools.to_bytes_le(entry_point, 4)
    code_size = tools.to_bytes_le(len(code), 4)

    if not data_ref:    
        data_size = tools.to_bytes_le(0, 4)
        data_addr = tools.to_bytes_le(0, 4)
        data = []
    else:
        _data_size, _data_addr, data, addrs = add_data(data_ref)
        data_size = tools.to_bytes_le(_data_size, 4)
        data_addr = tools.to_bytes_le(_data_addr, 4)
        write_data_map(output_path+'.data-map', addrs)

    done = header + entry_point_bytes + code_size + data_size + data_addr + code + data
    with open(output_path, 'wb') as f:
        f.write(bytes(done))


def write_raw(output_path, code):
    """Writes just the raw code bytes with no header at all (--raw) - useful
    for e.g. embedding the code as a blob to be loaded at runtime via the
    VM's OP_VMX, which expects bare code, not a full .bin image."""
    with open(output_path, 'wb') as f:
        f.write(bytes(code))


def write_map(map_path, labels):
    """Writes a human-readable "<addr>:<label>" listing for every label in
    the program, consumed by qdisasm.py to show label names instead of raw
    addresses when disassembling a .bin file back to assembly."""
    with open(map_path, 'w') as f:
        for name, addr in labels.items():
            f.write(f"0x{addr:06x}:{name}\n")


def assemble_file(src_path, out_path, map_path=None, cast_warnings=False, raw=False):
    """Top-level driver: runs the full preprocess -> parse -> analyze ->
    encode -> write pipeline for one source file, printing any collected
    errors/warnings along the way. Errors from preprocessing are reported
    immediately (together with any syntax errors) and abort before running
    analyze(), since a badly preprocessed file makes semantic analysis
    meaningless. Returns True on success (output written), False otherwise."""
    main_id = file_id_for(src_path)
    merged, files, runtime_version, entrypoint_ref, data_ref, pre_errors, pre_warnings = preprocess(src_path)
    files_map = {fid: (sf.path, sf.raw_lines) for fid, sf in files.items()}

    events, syntax_errors = parse_source(merged)

    if pre_errors:
        all_errors = pre_errors + syntax_errors
        print_errors(files_map, main_id, all_errors)
        n = len(all_errors)
        print(f"{n} error{'s' if n != 1 else ''}, no output written.", file=sys.stderr)
        return False

    extra_used = {entrypoint_ref[1]} if entrypoint_ref is not None and entrypoint_ref[0] == 'label' else None
    encoded, labels, semantic_errors, warnings = analyze(
        events, main_id, cast_warnings=cast_warnings, extra_used_labels=extra_used)

    all_errors = syntax_errors + semantic_errors
    entry_point = DEFAULT_ENTRYPOINT
    # Resolve %entrypoint now that every label's final address is known:
    # either it was already a raw address, or it referenced a label that
    # must exist in `labels` by this point.
    if entrypoint_ref is not None:
        kind, val, ep_file_id, ep_line, ep_col, raw_text = entrypoint_ref
        if kind == 'addr':
            entry_point = val
        elif val in labels:
            entry_point = labels[val]
        else:
            all_errors.append(AsmError(
                ep_line, ep_col, f"Undefined label '{raw_text}' in %entrypoint",
                length=len(raw_text), file_id=ep_file_id))

    all_warnings = pre_warnings + warnings

    if all_errors:
        print_errors(files_map, main_id, all_errors)
        n = len(all_errors)
        print(f"{n} error{'s' if n != 1 else ''}, no output written.", file=sys.stderr)
        return False

    if all_warnings:
        print_errors(files_map, main_id, all_warnings)

    code = encode(encoded)
    if raw:
        write_raw(out_path, code)
    else:
        write_bin(out_path, code, runtime_version, entry_point, data_ref=data_ref)

    if map_path:
        write_map(map_path, labels)
    print(f"OK: {out_path} ({len(code)} bytes, runtime version {runtime_version}, "
          f"entrypoint 0x{entry_point:06x})" + (f", {map_path}" if map_path else ""))
    return True


def main():
    ap = argparse.ArgumentParser(description="quartz assembler")
    ap.add_argument('source', help="Input .quartz file")
    ap.add_argument('output', help="Output .bin file")
    ap.add_argument('--map', nargs='?', const='', default=None,
                     help="Generate .map file (optional with custom name)")
    ap.add_argument('--cast-warnings', action='store_true',
                     help="enable warnings when a register is read back with a "
                          "different type than the one it was last written with")
    ap.add_argument("--raw", action="store_true", help="Generate raw code format")
    
    args = ap.parse_args()
    if args.map is None:
        map_path = None
    elif args.map == '':
        map_path = os.path.splitext(args.output)[0] + '.map'
    else:
        map_path = args.map

    ok = assemble_file(args.source, args.output, map_path, 
            cast_warnings=args.cast_warnings, raw=args.raw)
    sys.exit(0 if ok else 1)

if __name__ == '__main__':
    main()
