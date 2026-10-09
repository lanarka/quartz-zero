## Instruction reference

The table below lists every VM opcode, grouped into the same sections (and
in the same order) as the `switch (op)` inside `vm_do_step()` in `vm.c`.

Argument notation used below:

- `REG` — one register index, encoded as a single byte (`0`–`15`)
- `TYPE` — one type tag byte (see the types table above)
- `IMM` — an immediate value, 4 bytes normally, 8 bytes if `TYPE` is
  `I64`/`U64`/`F64`
- `ADDR32` — an absolute code address, 4 bytes (used by jumps/calls) or a
  RAM address, 4 bytes (used by `load`/`store`)
- `u8`/`u16`/`u32`/`f32` (lowercase, immediate) — a fixed-width immediate
  that isn't a typed `IMM` (e.g. `OP_VIDEO`'s operands)
- `(reg)` next to an operand name — that operand is a register holding the
  actual value/address used by the instruction (register-indirect)
- `(none)` — the instruction takes no operands

### ESSENTIALS

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_NOP` | `nop` | `(none)` | Does nothing. |
| `OP_HALT` | `halt` | `(none)` | Stops the VM (window closes automatically). |
| `OP_RAND` | `rnd` | `REG IMM32` | `dst`, `max`. Writes a random value in `[0, max)` into `dst` (treats `max=0` as `1`). |
| `OP_RAND_SEED` | `rndseed` | `seed(reg)` | Seeds the pseudo-random number generator used by `rnd` with `reg[seed]` (low 32 bits, via `srand()`). Without an explicit seed, the VM uses whatever default seed the runtime started with. |
| `OP_WAIT` | `wait` | `REG` | Sleeps for `reg[R]` microseconds (hybrid sleep, not a CPU-burning busy loop; capped at 5 seconds). |
| `OP_WAITI` | `waiti` | `IMM32` | Sleeps for a fixed number of microseconds (same cap as `wait`). |
| `OP_TIME` | `time` | `dst(reg)` | Stores the current wall-clock time, in milliseconds since the Unix epoch, into `dst`. Resolution is milliseconds and depends on the system clock. |
| `OP_TICK` | `tick` | `dst(reg)` | Stores the number of frames rendered so far into `dst`. Incremented by 1 on every `vsync`; reset to 0 on a full VM reset (`OP_RESET` / `Ctrl+Alt+R`), but **not** by `OP_VMX`. |
| `OP_RAMSIZE` | `ramsize` | `dst(reg)` | Stores the total size of the VM's RAM, in bytes, into `dst`. |
| `OP_VMX` | `vmx` | `code_addr(reg) code_len(reg) entrypoint(reg)` | "Warm restart": replaces the running code with new code copied from RAM at `code_addr`/`code_len` (raw machine code, same format as a `.bin`'s code segment without the file header). RAM is **not** touched — it stays exactly as the previous run left it. Registers, flags, and both stacks are zeroed, and `pc` jumps to `entrypoint`. The program's original data segment is not reloaded. Faults if `code_len == 0`, the code range doesn't fit in RAM, or `entrypoint >= code_len`. |
| `OP_RESET` | `reset` | `(none)` | "Cold" restart, identical to `Ctrl+Alt+R`: zeroes registers/flags/stacks/RAM, reloads the program's data segment, resets `pc` to the original entry point, and clears both display layers. Unlike `OP_VMX`, RAM is **not** preserved. |
| `OP_VIDEO` | `video` | `video_mode(u8) fps(u16) scale(f32) cursor(u8)` (immediates) | Changes the active video mode, target FPS, window scale, and OS mouse cursor visibility at runtime (same knobs as `--videomode`/`--fps`/`--scale`/`--cursor`). `video_mode` must be `1`/`2`/`3`, otherwise the VM faults and none of the four values change. `fps` goes straight to `SetTargetFPS()` (`0` = uncapped); `scale` is clamped to `[RT_MIN_SCALE, RT_MAX_SCALE]`; `cursor` uses `0`=hidden / nonzero=visible. **Switching video modes reallocates the framebuffer and text layer — old content is not preserved.** |
| `OP_VIDEO_INFO` | `videoinf` | `dst_mode(reg) dst_fps(reg) dst_scale(reg) dst_cursor(reg)` | Reads back the current video mode, FPS, window scale, and cursor visibility (whatever `OP_VIDEO`, the startup CLI options, or `Ctrl+Alt+PgUp`/`PgDn`/`S` last set). `dst_scale` receives the **raw bit pattern** of the `f32` value (read it with a `TYPE_F32` operation, not as a plain integer). `dst_cursor` receives a plain `0`/`1`. |

### DEBUG

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_PRINT` | `print` | `TYPE REG` | Debug-prints a register's value (interpreted as `TYPE`) to stdout. |

### DATA MOVE

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_MOV` | `mov` | `REG REG` | `dst`, `src`. Raw 64-bit register copy. |
| `OP_MOVI` | `movi` | `TYPE REG IMM` | `dst`, immediate. Loads an immediate value (normalized to `TYPE`) into `dst`. |

### ARITHMETIC (REGISTER, REGISTER)

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_ADD` | `add` | `TYPE REG REG` | `dst`, `src`. `dst = dst + src` (as `TYPE`). |
| `OP_SUB` | `sub` | `TYPE REG REG` | `dst = dst - src`. |
| `OP_MUL` | `mul` | `TYPE REG REG` | `dst = dst * src`. |
| `OP_DIV` | `div` | `TYPE REG REG` | `dst = dst / src`. Faults on integer division by zero. |
| `OP_MOD` | `mod` | `TYPE REG REG` | `dst = dst % src` (or `fmod` for float types). Faults on integer division by zero. |
| `OP_AND` | `and` | `TYPE REG REG` | `dst = dst & src`. Integer types only. |
| `OP_OR` | `or` | `TYPE REG REG` | `dst = dst \| src`. Integer types only. |
| `OP_XOR` | `xor` | `TYPE REG REG` | `dst = dst ^ src`. Integer types only. |
| `OP_SHL` | `shl` | `TYPE REG REG` | `dst = dst << (src & 63)`. Integer types only. |
| `OP_SHR` | `shr` | `TYPE REG REG` | `dst = dst >> (src & 63)`. Logical shift for unsigned types, arithmetic for signed. |

### ARITHMETIC (REGISTER, IMMEDIATE)

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_ADDI` | `addi` | `TYPE REG IMM` | `dst = dst + imm`. |
| `OP_SUBI` | `subi` | `TYPE REG IMM` | `dst = dst - imm`. |
| `OP_INC` | `inc` | `TYPE REG` | `dst = dst + 1` (`1.0` for float types). |
| `OP_DEC` | `dec` | `TYPE REG` | `dst = dst - 1`. |
| `OP_NOT` | `not` | `TYPE REG` | `dst = ~dst` (bitwise). Integer types only. |

### COMPARATION

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_CMP` | `cmp` | `TYPE REG REG` | Compares `reg[a]` vs `reg[b]` as `TYPE`; sets the zero/negative flags used by conditional jumps. |
| `OP_CMPI` | `cmpi` | `TYPE REG IMM` | Compares `reg[a]` vs an immediate as `TYPE`. |

### JUMPS

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_JMP` | `jmp` | `ADDR32` | Unconditional jump. |
| `OP_JZ` | `jz` | `ADDR32` | Jump if equal (zero flag set). |
| `OP_JNZ` | `jnz` | `ADDR32` | Jump if not equal. |
| `OP_JG` | `jg` | `ADDR32` | Jump if greater. |
| `OP_JL` | `jl` | `ADDR32` | Jump if less. |
| `OP_JGE` | `jge` | `ADDR32` | Jump if greater or equal. |
| `OP_JLE` | `jle` | `ADDR32` | Jump if less or equal. |

### JUMPS (REGISTER)

Unlike `jmp`/`jz`/`jnz`/.../`call`, which take their target address as a
`u32` immediate embedded directly in the code stream, the instructions
below read their target address at runtime from a register. This is useful
for computed jumps, jump tables, or calling a function pointer stored in
RAM. None of these validate that the target address is within `code_len` —
behavior matches the immediate-address family: an invalid address simply
faults naturally on the next instruction fetch (out-of-bounds PC).

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_JMPR` | `jmpr` | `addr(reg)` | Unconditional jump: `pc = addr`. |
| `OP_JZR` | `jzr` | `addr(reg)` | Jump to `addr` if the zero flag is set (register-address equivalent of `jz`). |
| `OP_JNZR` | `jnzr` | `addr(reg)` | Jump to `addr` if the zero flag is **not** set (equivalent of `jnz`). |
| `OP_JGR` | `jgr` | `addr(reg)` | Jump to `addr` if greater-than (equivalent of `jg`). |
| `OP_JLR` | `jlr` | `addr(reg)` | Jump to `addr` if less-than (equivalent of `jl`). |
| `OP_JGER` | `jger` | `addr(reg)` | Jump to `addr` if greater-or-equal (equivalent of `jge`). |
| `OP_JLER` | `jler` | `addr(reg)` | Jump to `addr` if less-or-equal (equivalent of `jle`). |

### CALL FUNCTIONS

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_CALLR` | `callr` | `addr(reg)` | Register-addressed call: pushes the return address (the instruction right after `callr`) onto the call stack, then sets `pc = addr`. Faults with a call-stack overflow if the stack is already full. Returns normally via `ret`. |
| `OP_CALL` | `call` | `ADDR32` | Pushes the return address onto the call stack, then jumps to the immediate address. |
| `OP_RET` | `ret` | `(none)` | Pops an address off the call stack and jumps to it. |

### DATA STACK

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_PUSH` | `push` | `REG` | Pushes `reg[R]` onto the data stack. |
| `OP_POP` | `pop` | `REG` | Pops the data stack into `reg[R]`. |

### MEMORY ACCESS

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_LOAD` | `load` | `TYPE REG ADDR32` | `dst`, address. `reg[dst] = RAM[addr]`, read as `TYPE`. |
| `OP_STORE` | `store` | `TYPE ADDR32 REG` | address, `src`. `RAM[addr] = reg[src]`, written as `TYPE`. |
| `OP_LOADR` | `loadr` | `TYPE REG REG` | `dst`, `addr_reg`. Register-indirect load: `reg[dst] = RAM[reg[addr_reg]]`. |
| `OP_STORER` | `storer` | `TYPE REG REG` | `addr_reg`, `src`. Register-indirect store: `RAM[reg[addr_reg]] = reg[src]`. |

### MEMORY (BLOCK)

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_MEMCPY` | `memcpy` | `dst_addr(reg) src_addr(reg) len(reg)` | Copies `len` bytes from RAM `src_addr` to RAM `dst_addr`. Both `[dst_addr, dst_addr+len)` and `[src_addr, src_addr+len)` must lie entirely within RAM. Implemented with `memmove`, so overlapping ranges are safe. `len == 0` is a no-op. |
| `OP_MEMSET` | `memset` | `addr(reg) value(reg) len(reg)` | Fills `len` bytes at RAM `addr` with the low byte of `value`. `[addr, addr+len)` must lie entirely within RAM. `len == 0` is a no-op. |

### SOUND

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_BEEP` | `beep` | `(none)` | Plays the fixed, built-in bell sound. |
| `OP_SOUND` | `sound` | `addr(reg) len(reg)` | Plays a WAV sound loaded from VM RAM: `len` bytes starting at `addr`, handed to the runtime's audio backend and played once. Unlike `beep`, this lets a program play arbitrary sound data it generated or loaded itself. `len == 0` is a no-op; faults if the range doesn't fit in RAM. |

### GFX

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_GFX_FRAME` | `xframe` | `REG REG REG REG REG` | `x, y, w, h, addr`. Blits a `w×h` RGBA8888 rectangle from `RAM[addr]` onto the pixel framebuffer at `(x,y)`. |
| `OP_GFX_PIX` | `xpix` | `REG REG REG` | `x, y, color`. Sets one pixel; `color` is a packed u32 (byte0=R, byte1=G, byte2=B, byte3=A). |
| `OP_GFX_RAW` | `xraw` | `REG REG REG` | `addr, len, offset`. Copies `len` raw bytes from `RAM[addr]` directly into the pixel framebuffer at byte `offset`. |
| `OP_GFX_CLEAR` | `xclear` | `(none)` | Clears the pixel framebuffer to opaque black (fixed, for binary compatibility). |
| `OP_GFX_CLEARC` | `xclearc` | `color(reg)` | Clears the pixel framebuffer to a specific RGBA color (same packed-`u32` layout as `gfx_pix`), including alpha. |
| `OP_GFX_GETRES` | `xgetres` | `REG REG` | `dst_h, dst_w`. Stores the screen resolution into the given registers. |
| `OP_VSYNC` | `vsync` | `IMM32` | Yields the rest of this frame's instruction budget back to the main loop, so the frame gets drawn now instead of after the full budget runs out. The operand is read (for binary format compatibility) but currently unused — pacing is tied to the render loop's actual FPS. |

### TEXT

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_TXT_CHAR` | `xtchar` | `REG REG REG REG REG` | `x, y, cp, fg, bg`. Draws one character (codepoint `cp`) at text-grid cell `(x,y)`; `fg`/`bg` are packed u32 RGBA colors. |
| `OP_TXT_FRAME` | `xtframe` | `REG REG REG REG REG` | `x, y, w, h, addr`. Fills a `w×h` block of the text grid from `RAM[addr]`; each cell is 10 bytes (`u16` codepoint + `u32` fg + `u32` bg). |
| `OP_TXT_GETRES` | `xtgetres` | `REG REG` | `dst_rows, dst_cols`. Stores the text-grid size into the given registers. |
| `OP_TXT_CLEAR` | `xtclear` | `(none)` | Clears every cell of the text grid (character, foreground, and background all reset), independently of the pixel framebuffer. |
| `OP_TXT_CURSET` | `xtcurset` | `REG REG` | `x, y`. Moves the text-mode cursor. |
| `OP_TXT_CURGET` | `xtcurget` | `REG REG` | `dst_x, dst_y`. Reads the current cursor position into the given registers. |
| `OP_TXT_CURENA` | `xtcurena` | `REG` | `enabled`. Sets cursor visibility: `0`=hidden, `1`=block, `2`=thin bar. |

### INPUT

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_IN_MOUSE` | `ximouse` | `REG REG REG` | `dst_x, dst_y, dst_btn`. Reads the current mouse position (pixel coordinates) and button bitmask (bit0=left, bit1=right). |
| `OP_IN_KEY` | `xikey` | `REG REG` | `dst_count, addr`. Reads up to 4 currently-held key codes into `RAM[addr..addr+7]` as four little-endian `u16` values (2 bytes per key, so codes above 255 — e.g. arrow keys — aren't truncated); `dst_count` receives how many are set. |
| `OP_IN_POLL` | `xipoll` | `REG REG REG REG REG` | `dst_mx, dst_my, dst_btn, dst_kc, addr`. Combined mouse (pixel coords) + keyboard poll in a single call; keys written to `RAM[addr..addr+7]` the same way as `in_key`. |
| `OP_IN_TXT_MOUSE` | `xitmouse` | `REG REG REG` | `dst_col, dst_row, dst_btn`. Reads the current mouse position as a text-grid `(col, row)` cell instead of pixels, plus the button bitmask — the text-mode equivalent of `in_mouse`. |
| `OP_IN_TXT_POLL` | `xitpoll` | `REG REG REG REG REG` | `dst_col, dst_row, dst_btn, dst_kc, addr`. Combined mouse (text-grid coords) + keyboard poll — the text-mode equivalent of `xipoll`; keys written the same way as `xikey`. |

### DISK

These instructions are bound to an optionally-attached virtual disk
(`--disk <image path>`, see `main.c` and `vm->disk_path`). The disk image
must already be formatted — the `svfs` CLI tool formats it (in
`disk-src/`); the VM itself only ever reads/writes an already-formatted
image and never calls `svfs_format()`.

Every instruction here that needs a file/directory name reads it as a
NUL-terminated string directly out of VM RAM: the operand is a **register
that holds the address** of that string, not the name itself. The name is
always a flat entry of the current SVFS directory (no `/` path splitting is
done by the VM, same as `svfs.h`).

All of these instructions work fine with no disk attached — the SVFS
library itself prints an error to stderr and does nothing — so the VM does
not need to separately check "is a disk attached?" before executing them.

| OP_CODE | Mnemonic | Arguments | Description |
|---|---|---|---|
| `OP_SVFS_MKDIR` | `fsmkdir` | `name_addr(reg)` | Creates a subdirectory with the given name in the current directory. |
| `OP_SVFS_RMDIR` | `fsrmdir` | `name_addr(reg) recursive(reg)` | Deletes a subdirectory. If the low byte of `recursive` is nonzero, deletes a non-empty directory too (including its contents); otherwise a non-empty directory is refused. |
| `OP_SVFS_CREATE` | `fscreate` | `name_addr(reg)` | Creates a new, empty file in the current directory. |
| `OP_SVFS_RM` | `fsrm` | `name_addr(reg)` | Deletes a file from the current directory. |
| `OP_SVFS_OPEN` | `fsopen` | `name_addr(reg)` | Opens a file (tracked in SVFS's open-file table; not persisted across program runs). |
| `OP_SVFS_CLOSE` | `fsclose` | `name_addr(reg)` | Closes a previously-opened file. |
| `OP_SVFS_WRITE` | `fswrite` | `name_addr(reg) data_addr(reg) status_dst(reg)` | Text (NUL-terminated) write: writes the NUL-terminated string at `data_addr` to the file (an embedded `'\0'` shortens the write). The file must already exist (`svfs_create`). `status_dst` gets `1` (success) / `0` (failure). |
| `OP_SVFS_READ` | `fsread` | `name_addr(reg) buf_addr(reg) buf_size(reg) dst_len(reg) status_dst(reg)` | Text read: reads the file's contents into RAM at `buf_addr` and always appends a terminating `'\0'` (1 byte of `buf_size` is reserved for it, so text is truncated to at most `buf_size-1` bytes). `dst_len` gets the number of text bytes copied (excluding the terminator, `0` on failure), `status_dst` gets `1`/`0`. |
| `OP_SVFS_WRITE_BYTES` | `fswriteb` | `name_addr(reg) data_addr(reg) len(reg) status_dst(reg)` | Binary-safe write: writes exactly `len` bytes from RAM at `data_addr`, including any embedded zero bytes. Use for saving a frame/block from RAM as a file. `status_dst` gets `1`/`0`. |
| `OP_SVFS_READ_BYTES` | `fsreadb` | `name_addr(reg) buf_addr(reg) buf_size(reg) dst_len(reg) status_dst(reg)` | Binary-safe read: reads up to `buf_size` bytes of the file exactly as stored (**no** terminator appended) into RAM at `buf_addr` — i.e. "load a file into a frame". `dst_len` gets the bytes actually copied (`0` on failure), `status_dst` gets `1`/`0`. |
| `OP_SVFS_CD` | `fscd` | `name_addr(reg) status_dst(reg)` | Changes SVFS's current directory. Accepts a flat subdirectory name, `"."` (no change), `".."` (parent directory), or `"/"` (root). `status_dst` gets `1` (success) / `0` (failure, e.g. nonexistent directory or a name that belongs to a file) — on failure the current directory is left unchanged. |
| `OP_SVFS_DIR` | `fsdir` | `buf_addr(reg) max_entries(reg) dst_count(reg) status_dst(reg)` | Reads the current directory's contents and writes them into RAM at `buf_addr` as an array of fixed-size 64-byte entries (format below), so a VM program can read them with plain loads. `max_entries` caps how many entries are written (checked to fit in RAM). `dst_count` gets the entries actually written (can be `0` for an empty directory, even on success), `status_dst` gets `1` (success) / `0` (no attached/initialized filesystem). |
| `OP_SVFS_ATTACHED` | `fsattached` | `dst(reg)` | Disk attached info (1: Attached, 0: Not attached) |

**`OP_SVFS_DIR` entry format** (64 bytes per entry, all multi-byte values
little-endian):

| Offset | Field | Description |
|---|---|---|
| `0..43`  | name | NUL-terminated `"name.ext"` string (or just `"name"` if it has no extension), already in the exact form usable as `name_addr` for `svfs_cd`/`svfs_open`/... on this entry. |
| `44`     | attribute | `0` = file, `1` = directory, `2` = hidden file. |
| `45..47` | reserved | Always `0`. |
| `48..51` | size | File size in bytes (`uint32`, `0` for directories). |
| `52..55` | created | Creation time (`uint32`, Unix timestamp). |
| `56..59` | modified | Last-modified time (`uint32`, Unix timestamp). |
| `60..63` | reserved | Always `0`. |
