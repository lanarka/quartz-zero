"""
Quartz - The Computer Assembler
================================

quartz/errors.py - error/warning representation and pretty-printing.

AsmError is the single type used throughout the assembler for both errors
and warnings (AsmWarning is just a convenience constructor that sets
severity="warning"), carrying enough position info (file_id/line/col/length)
to render a caret pointing at the exact offending span of source, as done by
print_errors()/preview() below - used by qasm.py's assemble_file() to report
everything collected during preprocessing, parsing, and analysis.
"""

import colorama

CONTEXT_LINES = 3


class AsmError(Exception):
    def __init__(self, line, col, message, length=1, severity="error", file_id=None):
        super().__init__(message)
        self.line = line
        self.col = col
        self.length = max(1, length)
        self.message = message
        self.severity = severity
        self.file_id = file_id

    def sort_key(self):
        # Used to sort collected errors/warnings for display: grouped by
        # file, then top-to-bottom, left-to-right within a line.
        return (self.file_id or "", self.line, self.col)


def AsmWarning(line, col, message, length=1, file_id=None):
    return AsmError(line, col, message, length=length, severity="warning", file_id=file_id)


def _pointer_line(raw_line, col, length, warn=False):
    prefix_src = raw_line[:col - 1]
    prefix = "".join(ch if ch.isspace() else " " for ch in prefix_src)
    color = colorama.Style.BRIGHT
    color += colorama.Fore.YELLOW if warn else colorama.Fore.RED
    return prefix + color + "^" + "-" * (length - 1) + colorama.Style.RESET_ALL 


def format_error(filename, raw_lines, err):
    lines = []
    label = "Error" if err.severity == "error" else "Warning"
    if err.severity == "error":
        lines.append(colorama.Fore.RED)
    else:
        lines.append(colorama.Fore.YELLOW)
    lines.append(f"{label}: {err.message},\nat file {filename}: Line {err.line}, col {err.col}:")
    lines.append(colorama.Style.RESET_ALL)
    return "\n".join(lines)


def preview(raw_lines, err):
    start = max(1, err.line - (CONTEXT_LINES - 1))
    entries = []
    for lineno in range(start, err.line + 1):
        if 1 <= lineno <= len(raw_lines):
            entries.append((lineno, raw_lines[lineno - 1]))

    error_raw_line = raw_lines[err.line - 1] if 1 <= err.line <= len(raw_lines) else ""
    pointer = _pointer_line(
        error_raw_line, err.col, err.length,
        False if err.severity == "error" else True
    )

    done = ""
    for lineno, line in entries:
        ln = f"{colorama.Fore.BLACK}{lineno:5d}{colorama.Style.RESET_ALL}"
        done += f"{ln} {colorama.Fore.GREEN}|{colorama.Style.RESET_ALL} {line}\n"
    done += f"      {colorama.Fore.GREEN}|{colorama.Style.RESET_ALL} {pointer} \n"
    return done


def print_errors(files, default_file_id, errors):
    colorama.init(autoreset=True)
    errors = sorted(errors, key=AsmError.sort_key)
    for err in errors:
        fid = err.file_id or default_file_id
        filename, raw_lines = files.get(fid, (fid or "?", []))
        print(format_error(filename, raw_lines, err))
        print(preview(raw_lines, err))
        print()
    return len(errors)
