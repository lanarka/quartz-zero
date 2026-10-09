"""Syntax highlighting for the disassembled code display."""

from rich.highlighter import RegexHighlighter
from rich.theme import Theme

from . import isa

# Build one alternation regex fragment per instruction category by pulling
# the mnemonic list straight from isa.INSTRUCTIONS, so the highlighter stays
# in sync automatically whenever the instruction set changes.
_FLOW = "|".join(sorted(
    n for n, spec in isa.INSTRUCTIONS.items() if spec.category == isa.CAT_FLOW))
_STACK = "|".join(sorted(
    n for n, spec in isa.INSTRUCTIONS.items() if spec.category == isa.CAT_STACK))
_MEM = "|".join(sorted(
    n for n, spec in isa.INSTRUCTIONS.items() if spec.category == isa.CAT_MEM))
_ALU = "|".join(sorted(
    n for n, spec in isa.INSTRUCTIONS.items() if spec.category == isa.CAT_ALU))
_CORE = "|".join(sorted(
    n for n, spec in isa.INSTRUCTIONS.items() if spec.category == isa.CAT_CORE))
_IO = "|".join(sorted(
    n for n, spec in isa.INSTRUCTIONS.items() if spec.category == isa.CAT_IO))
_TYPES = "|".join(isa.TYPE_SUFFIXES)


class AsmHighlighter(RegexHighlighter):
    """Applies one style per token kind (mnemonic category, type suffix,
    register, address, label, number, comment) to a line of disassembly
    text. Rules are tried in order, so more specific patterns (comments)
    come first."""

    base_style = "asm."
    highlights = [
        r"(?P<comment>;.*$)",
        rf"(?P<flow>\b({_FLOW})\b)",
        rf"(?P<stack>\b({_STACK})\b)",
        rf"(?P<mem>\b({_MEM})\b)",
        rf"(?P<alu>\b({_ALU})\b)",
        rf"(?P<core>\b({_CORE})\b)",
        rf"(?P<io>\b({_IO})\b)",
        rf"(?P<type>\b({_TYPES})\b)",
        r"(?P<reg>#\d+)",
        r"(?P<hex>\b0x[0-9A-Fa-f]+\b)",
        r"(?P<label>\b[A-Za-z_][A-Za-z0-9_]*(?=:))",
        r"(?P<num>-?\b\d+\.\d+\b|-?\b\d+\b)",
    ]


# Color scheme mapping each highlight group above to a rich style. I/O
# instructions are deliberately dim since the debugger doesn't actually
# execute them (see emulator.py's _skip_io).
ASM_THEME = Theme({
    "asm.comment": "dim italic green",
    "asm.flow": "bold magenta",
    "asm.stack": "bold yellow",
    "asm.mem": "bold cyan",
    "asm.alu": "bold white",
    "asm.core": "bold blue",
    "asm.io": "dim white",
    "asm.type": "green",
    "asm.reg": "bright_cyan",
    "asm.hex": "bright_blue",
    "asm.label": "bold bright_yellow",
    "asm.num": "bright_blue",
})
