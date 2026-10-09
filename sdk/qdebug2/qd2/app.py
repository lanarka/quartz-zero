"""
QDebugger 2 - a textual GUI for debugging compiled QuartzVM binaries.

Controls:
    space / s   step (one instruction)
    c           animated run (start/stop), also stops on a breakpoint/halt
    f           fast run until halt/breakpoint/fault (no animation)
    r           reset the VM to the start
    b           toggle a breakpoint on the current (PC) instruction
    [   ]       manually scroll the code panel (paging); the next step
                re-centers on PC again
    up / down   move the memory window by 1 row
    pageup/dn   move the memory window by a full page
    home        memory window to address 0x0
    :           enter an address for the memory panel to jump to (hex)
    q           quit
"""

import bisect

from rich.text import Text
from textual.app import App
from textual.containers import Container, Horizontal, Vertical
from textual.reactive import reactive
from textual.widgets import DataTable, Footer, Input, Static

from . import isa
from .disasm import build_listing, format_instr
from .emulator import Emulator
from .highlighter import ASM_THEME, AsmHighlighter

TITLE = "QDebugger II"
VERSION = "1.0"

MEM_BYTES_PER_ROW = 16
CODE_LINES_FALLBACK = 24
MEM_ROWS_FALLBACK = 12
MAX_STACK_ROWS = 14
LOG_MAX_LINES = 300


class Banner(Static):
    """Single-line title bar shown at the top of the screen."""

    def __init__(self, subtitle=""):
        super().__init__()
        self.subtitle = subtitle

    def on_mount(self):
        text = Text()
        text.append(f" {TITLE} ", style="bold black on cyan")
        text.append(f" v{VERSION}  ", style="cyan")
        text.append(self.subtitle, style="yellow")
        self.update(text)


class AddressPrompt(Input):
    """Small input box used to jump the memory panel to a given address."""


class DebuggerApp(App):

    # Textual CSS: layout is a banner on top, a three-column main row
    # (code / registers+stacks / memory), a log panel, and a hidden
    # address-entry prompt docked at the bottom (shown only via ':').
    CSS = """
    Screen {
        background: #05050c;
        color: #e6e6e6;
    }
    #banner {
        height: 1;
        background: #101026;
        padding: 0 1;
    }
    #main_row {
        height: 1fr;
    }
    .panel {
        border: round #3a3a5c;
        border-title-color: #7fdfff;
        border-title-style: bold;
        background: #0b0b18;
        padding: 0 1;
    }
    #code_col {
        width: 2fr;
    }
    #code_body {
        height: 1fr;
        overflow: hidden;
    }
    #mid_col {
        width: 38;
    }
    #regs_table {
        height: 18;
    }
    #flags_panel {
        height: 5;
    }
    #callstack_table {
        height: 1fr;
    }
    #datastack_table {
        height: 1fr;
    }
    #mem_col {
        width: 44;
    }
    #mem_body {
        height: 1fr;
        overflow: hidden;
    }
    #log_panel {
        height: 9;
        border: round #3a3a5c;
        background: #0b0b18;
    }
    #addr_prompt {
        dock: bottom;
        height: 3;
        border: round yellow;
        display: none;
    }
    DataTable {
        background: #0b0b18;
    }
    """

    # Maps each key to an action method (action_<name>) and a short label
    # shown in the footer.
    BINDINGS = [
        ("space", "step", "Step"),
        ("s", "step", "Step"),
        ("c", "toggle_run", "Run"),
        ("f", "fast_run", "Fast"),
        ("r", "reset", "Reset"),
        ("b", "toggle_breakpoint", "BrkPt"),
        ("bracketleft", "code_page(-1)", "Code-"),
        ("bracketright", "code_page(1)", "Code+"),
        ("up", "mem_scroll(-1)", "Mem.up"),
        ("down", "mem_scroll(1)", "Mem.dn"),
        ("pageup", "mem_page(-1)", "MemPgUp"),
        ("pagedown", "mem_page(1)", "MemPgDn"),
        ("home", "mem_home", "Mem.0"),
        ("colon", "goto_prompt", "Go to addr."),
        ("q", "quit", "Quit"),
    ]

    # True while the animated ('c') run is active; drives the timer that
    # steps the VM ~25 times per second.
    running_continuous = reactive(False)

    def __init__(self, image, labels, bin_path, ram_size):
        super().__init__()
        self.image = image
        self.labels = labels
        self.bin_path = bin_path
        self.vm = Emulator(image, ram_size=ram_size)

        # Static disassembly of the whole code segment, built once up
        # front. addr_index maps an address to its position in `listing`
        # for instructions that were decoded on a boundary; addresses that
        # fall inside an instruction (or past the end) are resolved via
        # bisect in _pc_index().
        self.listing, self.addr_index = build_listing(image.code)
        self._sorted_addrs = sorted(self.addr_index)
        self.breakpoints = set()

        # Code panel scroll state: while code_follow is True the view
        # auto-centers on PC every refresh; manual paging ([ and ])
        # disables following until the next step/run re-enables it.
        self.code_window_start = 0
        self.code_follow = True
        self.code_visible_lines = CODE_LINES_FALLBACK

        # Memory panel scroll state (independent of the code panel).
        self.mem_addr = 0
        self.mem_visible_rows = MEM_ROWS_FALLBACK

        # Snapshot of register values before the last step, used to
        # highlight which registers changed in refresh_regs().
        self.prev_regs = list(self.vm.reg)
        self._continuous_timer = None

    def compose(self):
        # Declarative widget tree for Textual; on_mount() below fills in
        # the dynamic content and grabs references for later refreshes.
        yield Banner("space=step  c=run  f=fast  r=reset  b=breakpoint  q=quit")
        with Horizontal(id="main_row"):
            with Vertical(id="code_col", classes="panel") as col:
                col.border_title = f"Code - {self.bin_path}"
                yield Container(Static(id="code_text"), id="code_body")
            with Vertical(id="mid_col"):
                dt = DataTable(id="regs_table", classes="panel")
                dt.border_title = "Registers"
                yield dt
                st = Static(id="flags_panel", classes="panel")
                st.border_title = "VM state"
                yield st
                cs = DataTable(id="callstack_table", classes="panel")
                cs.border_title = "Call stack"
                yield cs
                ds = DataTable(id="datastack_table", classes="panel")
                ds.border_title = "Data stack"
                yield ds
            with Vertical(id="mem_col", classes="panel") as mc:
                mc.border_title = "Memory (RAM)"
                yield Container(Static(id="mem_text"), id="mem_body")
        yield Static(id="log_panel")
        yield AddressPrompt(placeholder="hex address, e.g. 1000", id="addr_prompt")
        yield Footer()

    def on_mount(self):
        # Register the assembly color theme and the highlighter instance
        # used every time the code panel is redrawn.
        self.console.push_theme(ASM_THEME)
        self._hl = AsmHighlighter()

        self._code_text = self.query_one("#code_text", Static)
        self._mem_text = self.query_one("#mem_text", Static)
        self._flags = self.query_one("#flags_panel", Static)
        self._log = self.query_one("#log_panel", Static)
        self._log_lines = []

        # Registers table: one row per register, updated in place (not
        # rebuilt) on every refresh so the cursor/scroll position is
        # never disturbed.
        self._regs_table = self.query_one("#regs_table", DataTable)
        self._regs_table.cursor_type = "none"
        self._regs_table.zebra_stripes = True
        self._regs_table.can_focus = False
        self._regs_table.add_column("R", key="r", width=4)
        self._regs_table.add_column("Hex", key="hex", width=19)
        self._regs_table.add_column("Dec", key="dec", width=12)
        for i in range(isa.REGISTER_COUNT):
            self._regs_table.add_row(f"r{i}", "", "", key=str(i))

        self._cs_table = self.query_one("#callstack_table", DataTable)
        self._cs_table.cursor_type = "none"
        self._cs_table.zebra_stripes = True
        self._cs_table.can_focus = False
        self._cs_table.add_columns("#", "Return to")

        self._ds_table = self.query_one("#datastack_table", DataTable)
        self._ds_table.cursor_type = "none"
        self._ds_table.zebra_stripes = True
        self._ds_table.can_focus = False
        self._ds_table.add_columns("#", "Value")

        self._addr_prompt = self.query_one("#addr_prompt", AddressPrompt)
        self._addr_prompt.can_focus = False

        # Show whatever log lines the VM emitted during __init__/reset()
        # (e.g. the initial "VM reset, entry=..." line) before the first
        # real render.
        for e in self.vm.events:
            self._push_log(e)
        self.refresh_all()
        self.set_focus(None)

    def on_resize(self, event):
        # Recompute how many code lines / memory rows actually fit in
        # their panels whenever the terminal is resized, so paging and
        # centering stay accurate instead of using the fallback constants.
        if not hasattr(self, "_hl"):
            return
        try:
            body_h = self.query_one("#code_body").size.height
            if body_h:
                self.code_visible_lines = max(4, body_h)
        except Exception:
            pass
        try:
            mem_h = self.query_one("#mem_body").size.height
            if mem_h:
                self.mem_visible_rows = max(3, mem_h)
        except Exception:
            pass
        self.refresh_all()

    # actions
    def action_step(self):
        self._do_step()

    def _do_step(self):
        # Shared single-step logic used by the 'space'/'s' key binding.
        if not self.vm.running:
            self.bell()
            return
        self.prev_regs = list(self.vm.reg)
        self.vm.step()
        for e in self.vm.events:
            self._push_log(e)
        self.code_follow = True
        self.refresh_all()

    def action_reset(self):
        self.vm.reset()
        for e in self.vm.events:
            self._push_log(e)
        self.prev_regs = list(self.vm.reg)
        self.code_follow = True
        self.running_continuous = False
        self.refresh_all()

    def action_toggle_breakpoint(self):
        addr = self.vm.pc
        if addr in self.breakpoints:
            self.breakpoints.discard(addr)
            self._push_log(f"[BRK] breakpoint cleared at 0x{addr:06x}")
        else:
            self.breakpoints.add(addr)
            self._push_log(f"[BRK] breakpoint set at 0x{addr:06x}")
        self.refresh_all()

    def action_toggle_run(self):
        # 'c' starts/stops the animated run. Running is driven by a
        # recurring Textual timer (_continuous_tick) rather than a busy
        # loop, so the UI stays responsive to key presses while it runs.
        if self.running_continuous:
            self.running_continuous = False
            self._stop_timer()
            self._push_log("[RUN] run stopped")
        else:
            if not self.vm.running:
                self.bell()
                return
            self.running_continuous = True
            self._push_log("[RUN] animated run started (c = stop)")
            self._continuous_timer = self.set_interval(1 / 25, self._continuous_tick)
        self.refresh_all()

    def _stop_timer(self):
        if self._continuous_timer is not None:
            self._continuous_timer.stop()
            self._continuous_timer = None

    def _continuous_tick(self):
        # Called ~25 times/second while running_continuous is True: single
        # steps the VM and stops itself on halt or a hit breakpoint.
        if not self.vm.running:
            self.running_continuous = False
            self._stop_timer()
            self.refresh_all()
            return
        self.prev_regs = list(self.vm.reg)
        self.vm.step()
        for e in self.vm.events:
            self._push_log(e)
        self.code_follow = True
        if not self.vm.running or self.vm.pc in self.breakpoints:
            self.running_continuous = False
            self._stop_timer()
            if self.vm.running:
                self._push_log(f"[BRK] stopped at breakpoint 0x{self.vm.pc:06x}")
        self.refresh_all()

    def action_fast_run(self):
        # 'f' runs as many steps as possible synchronously (no animation,
        # no per-step redraw) up to a safety budget, then redraws once at
        # the end. Much faster than the animated run for skipping over
        # long loops, at the cost of not seeing intermediate states.
        if not self.vm.running:
            self.bell()
            return
        self.prev_regs = list(self.vm.reg)
        budget = 500000
        first = True
        while self.vm.running and budget > 0:
            # Don't immediately re-trigger the breakpoint we might already
            # be sitting on when fast-run was started; only stop for one
            # hit on a *subsequent* instruction.
            if not first and self.vm.pc in self.breakpoints:
                self._push_log(f"[BRK] stopped at breakpoint 0x{self.vm.pc:06x}")
                break
            self.vm.step()
            first = False
            budget -= 1
        for e in self.vm.events:
            self._push_log(e)
        if budget == 0:
            self._push_log("[INFO] fast-run step limit reached (500000 steps)")
        self.code_follow = True
        self.refresh_all()

    def action_code_page(self, direction):
        # Manual scroll of the code panel with '[' / ']'; disables
        # follow-PC mode until the next step/run re-centers it.
        self.code_follow = False
        step = max(1, self.code_visible_lines - 2)
        self.code_window_start = max(
            0, min(len(self.listing) - 1, self.code_window_start + direction * step))
        self.refresh_code()

    def action_mem_scroll(self, direction):
        self._move_mem(direction * MEM_BYTES_PER_ROW)

    def action_mem_page(self, direction):
        self._move_mem(direction * MEM_BYTES_PER_ROW * self.mem_visible_rows)

    def action_mem_home(self):
        self.mem_addr = 0
        self.refresh_mem()

    def _move_mem(self, delta):
        new_addr = self.mem_addr + delta
        max_addr = max(0, self.vm.ram_size - MEM_BYTES_PER_ROW)
        self.mem_addr = max(0, min(max_addr, new_addr))
        self.refresh_mem()

    def action_goto_prompt(self):
        # Reveal and focus the hidden address-entry input (bound to ':').
        self._addr_prompt.can_focus = True
        self._addr_prompt.styles.display = "block"
        self._addr_prompt.value = ""
        self._addr_prompt.focus()

    def on_key(self, event):
        # Escape closes the address prompt without jumping anywhere.
        if event.key == "escape" and self._addr_prompt.styles.display != "none":
            self._addr_prompt.styles.display = "none"
            self._addr_prompt.value = ""
            self._addr_prompt.can_focus = False
            self.set_focus(None)
            event.stop()

    def on_input_submitted(self, event):
        # Enter in the address prompt: parse the typed hex address and
        # jump the memory panel to it (rounded down to a full row).
        if event.input.id != "addr_prompt":
            return
        text = event.value.strip().lower().removeprefix("0x")
        self._addr_prompt.styles.display = "none"
        self._addr_prompt.value = ""
        self._addr_prompt.can_focus = False
        self.set_focus(None)
        if not text:
            return
        try:
            addr = int(text, 16)
        except ValueError:
            self.bell()
            return
        max_addr = max(0, self.vm.ram_size - MEM_BYTES_PER_ROW)
        self.mem_addr = max(0, min(max_addr, addr - (addr % MEM_BYTES_PER_ROW)))
        self.refresh_mem()

    # rendering
    def refresh_all(self):
        self.refresh_code()
        self.refresh_regs()
        self.refresh_flags()
        self.refresh_stacks()
        self.refresh_mem()

    def _pc_index(self):
        """Find where the current PC sits in `listing`.

        Returns (index, exact): exact is True when PC lands precisely on a
        decoded instruction boundary. If PC falls inside a previously
        undecodable region (or past the last instruction), exact is False
        and index points at the nearest instruction at or before PC, found
        via binary search over the sorted known addresses.
        """
        pc = self.vm.pc
        idx = self.addr_index.get(pc)
        if idx is not None:
            return idx, True
        pos = bisect.bisect_right(self._sorted_addrs, pc) - 1
        if pos < 0:
            return 0, False
        return self.addr_index[self._sorted_addrs[pos]], False

    def refresh_code(self):
        if not self.listing:
            self._code_text.update("(empty code segment)")
            return

        pc_idx, exact = self._pc_index()
        if self.code_follow:
            # Center the PC instruction vertically in the visible window.
            half = self.code_visible_lines // 2
            self.code_window_start = max(0, pc_idx - half)

        start = min(self.code_window_start, max(0, len(self.listing) - 1))
        end = min(len(self.listing), start + self.code_visible_lines)

        lines = []
        for i in range(start, end):
            instr = self.listing[i]
            addr = instr.addr
            is_pc = (i == pc_idx)
            # "->" marks PC landing exactly on this instruction; "~>" marks
            # the nearest-instruction fallback when PC is off-boundary.
            marker = "->" if is_pc and exact else ("~>" if is_pc else "  ")
            bp = "*" if addr in self.breakpoints else " "
            prefix = Text(f"{marker}{bp} 0x{addr:06x}  ", style="bold red" if is_pc else "grey50")

            label_names = self.labels.get(addr)
            label_line = None
            if label_names:
                label_line = Text(f"{label_names[0]}:", style="bold bright_yellow")

            text_str = format_instr(instr, self.labels)
            body = self._hl(text_str)
            if not instr.valid:
                body.stylize("bold red")
            elif instr.category == isa.CAT_IO:
                # Visually mark I/O instructions as "not really executed".
                body.stylize("dim")

            if is_pc:
                line = Text()
                line.append_text(prefix)
                line.append_text(body)
                line.stylize("on #202045")
            else:
                line = Text.assemble(prefix, body)

            if label_line is not None:
                lines.append(label_line)
            lines.append(line)

        content = Text("\n").join(lines)
        if not exact:
            note = Text(f"\n(PC 0x{self.vm.pc:06x} outside the statically disassembled code)",
                        style="italic yellow")
            content = content + note
        self._code_text.update(content)

    def refresh_regs(self):
        for i in range(isa.REGISTER_COUNT):
            val = self.vm.reg[i]
            uval = val & ((1 << 64) - 1)
            changed = val != self.prev_regs[i]
            style = "bold bright_green" if changed else ""
            hex_cell = Text(f"0x{uval:016x}"[:18], style=style)
            dec_cell = Text(str(val), style=style)
            self._regs_table.update_cell(str(i), "hex", hex_cell)
            self._regs_table.update_cell(str(i), "dec", dec_cell)

    def refresh_flags(self):
        vm = self.vm
        state = "HALT" if not vm.running else ("RUN" if self.running_continuous else "STOP")
        color = "red" if not vm.running else ("green" if self.running_continuous else "yellow")
        text = Text()
        text.append(f"PC: 0x{vm.pc:06x}\n", style="bold cyan")
        text.append(f"Z:{int(vm.flag_zero)} N:{int(vm.flag_neg)}   ", style="white")
        text.append(f"steps: {vm.step_count}\n", style="grey70")
        text.append(f"state: {state}", style=f"bold {color}")
        if vm.halt_reason and not vm.running:
            text.append(f"\n{vm.halt_reason}", style="red")
        self._flags.update(text)

    def refresh_stacks(self):
        # Both stacks are shown top-of-stack first (most recently
        # pushed/called at the top), capped to MAX_STACK_ROWS with a
        # "+N more" row when there's overflow, to keep the panel bounded
        # regardless of how deep the program's stacks get.
        self._cs_table.clear()
        cs = self.vm.call_stack
        shown = list(reversed(cs))[:MAX_STACK_ROWS]
        for idx, addr in enumerate(shown):
            depth = len(cs) - idx
            self._cs_table.add_row(str(depth), f"0x{addr:06x}")
        if len(cs) > MAX_STACK_ROWS:
            self._cs_table.add_row("...", f"(+{len(cs) - MAX_STACK_ROWS})")

        self._ds_table.clear()
        ds = self.vm.data_stack
        shown = list(reversed(ds))[:MAX_STACK_ROWS]
        for idx, val in enumerate(shown):
            pos = len(ds) - 1 - idx
            self._ds_table.add_row(str(pos), str(val))
        if len(ds) > MAX_STACK_ROWS:
            self._ds_table.add_row("...", f"(+{len(ds) - MAX_STACK_ROWS})")

    def refresh_mem(self):
        # Classic hex-dump view: address, hex bytes, then the printable
        # ASCII representation (non-printable bytes shown as '.').
        vm = self.vm
        rows = []
        addr = self.mem_addr
        for r in range(self.mem_visible_rows):
            row_addr = addr + r * MEM_BYTES_PER_ROW
            if row_addr >= vm.ram_size:
                break
            chunk = vm.ram[row_addr:row_addr + MEM_BYTES_PER_ROW]
            hex_part = " ".join(f"{b:02x}" for b in chunk)
            ascii_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
            line = Text()
            line.append(f"0x{row_addr:06x}: ", style="bold cyan")
            line.append(hex_part, style="white")
            line.append("  " + ascii_part, style="grey58")
            rows.append(line)
        content = Text("\n").join(rows) if rows else Text("(out of RAM range)")
        self._mem_text.update(content)

    def _push_log(self, msg):
        # Keep an unbounded-looking but actually capped log history
        # (LOG_MAX_LINES), while only rendering the last few lines in the
        # small on-screen log panel. Color-codes lines by their [TAG]
        # prefix so faults/prints/halts stand out at a glance.
        self._log_lines.append(msg)
        if len(self._log_lines) > LOG_MAX_LINES:
            self._log_lines = self._log_lines[-LOG_MAX_LINES:]
        text = Text()
        tail = self._log_lines[-8:]
        for i, line in enumerate(tail):
            style = "white"
            if line.startswith("[FAULT]"):
                style = "bold red"
            elif line.startswith("[PRINT]"):
                style = "bold green"
            elif line.startswith("[HALT]"):
                style = "bold red"
            elif line.startswith("[SKIP]"):
                style = "dim yellow"
            elif line.startswith("[BRK]"):
                style = "bold magenta"
            elif line.startswith("[RUN]"):
                style = "cyan"
            text.append(line, style=style)
            if i != len(tail) - 1:
                text.append("\n")
        self._log.update(text)


def run_app(image, labels, bin_path, ram_size):
    app = DebuggerApp(image, labels, bin_path, ram_size)
    app.run()
