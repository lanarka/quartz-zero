# NETHELP.md - Quartz VM network card

This document describes the 5 new instructions that make up the VM's
"network card" (`OP_NET_ATTACHED`, `OP_NET_SEND`, `OP_NET_RECV`,
`OP_NET_POLL`, `OP_NET_MAXPACKET`). It is the reference for what each
instruction does; the source (`src/vm.c`, `src/vm.h`) intentionally keeps
only short implementation notes and points here for the rest.

## Overview

The network card is a **bidirectional, packet-oriented** link between this
VM process and a separate HOST process, over a UNIX domain socket (see
`src-net/vmnet.h`, and `src/qhost.c` / `src-net/host.c` for a HOST
implementation). It is entirely **optional**:

- If the VM was started **without** `--net <socket_path>`, no network card
  exists. Every `OP_NET_*` instruction below still executes normally - it
  just reports "not attached" (see each instruction's status codes) instead
  of faulting. A program can safely use these instructions unconditionally
  and simply check `OP_NET_ATTACHED` (or any status register) to see
  whether a network card is actually present.
- If `--net <socket_path>` **was** given, the VM connects to that UNIX
  socket once, at startup, before it starts executing any code. A HOST
  process must already be listening on that socket path (it does not need
  to have called `accept()` yet - the connection will simply sit in the
  kernel's backlog until it does).
- If the HOST process disappears (closes the connection, or the socket
  errors) while the VM is running, the card **auto-detaches**: the current
  `OP_NET_SEND`/`OP_NET_RECV` call reports that as a status code, and every
  `OP_NET_*` instruction from then on behaves exactly as if `--net` had
  never been given (i.e. `OP_NET_ATTACHED` will report 0 again). There is
  no automatic reconnect.

The card only ever exchanges whole, discrete **packets** - not a raw
continuous byte stream. Each `OP_NET_SEND` call sends exactly one packet;
each `OP_NET_RECV` call receives at most one packet. It is entirely up to
the running program to decide what bytes go in a packet and how to
interpret them (e.g. its own header/framing, a text protocol, etc.) - the
VM does not impose a packet format.

## Never blocks the VM

None of these instructions can stall the VM's execution loop waiting on the
network. `OP_NET_RECV` (the only one of the five that could conceivably
block) is always non-blocking: it checks first whether a packet has
actually arrived and only copies it out if so; if nothing has arrived yet,
it returns immediately reporting "nothing waiting". A program that wants to
wait for a reply has to poll for it across multiple steps/frames itself
(see the example below) - exactly like it already has to do for keyboard/
mouse input via `OP_IN_POLL`.

## Instruction reference

Registers are given as `rX` (an operand naming *which register number* to
use) vs. `reg[rX]` (*the value currently stored* in that register).

---

### `OP_NET_ATTACHED rstatus`

Reports whether a network card is currently attached.

- `reg[rstatus] = 1` - a network card is attached.
- `reg[rstatus] = 0` - no network card (either `--net` was never given, or
  it was but the HOST has since disconnected).

Does not touch RAM or the socket; just a plain state check. Safe to call
at any time, as often as you like.

---

### `OP_NET_SEND raddr, rlen, rstatus`

Sends `reg[rlen]` bytes, read from VM RAM starting at address `reg[raddr]`,
to the HOST as a single packet.

Status codes written to `reg[rstatus]`:

| Value | Meaning |
|-------|---------|
| `0` | No network card attached. Nothing was sent. |
| `1` | Sent successfully. |
| `2` | `reg[rlen]` exceeds the channel's max packet size (see `OP_NET_MAXPACKET`). Nothing was sent, and the connection itself is untouched - this is a program-side sizing mistake, not a network failure. |
| `3` | Send failed for another reason (most likely: the HOST is gone). The card is auto-detached - a following `OP_NET_ATTACHED` will report `0`. |

---

### `OP_NET_RECV raddr, rbufsize, rdstlen, rstatus`

Non-blocking receive of **at most one** packet into VM RAM, written
starting at address `reg[raddr]`, with room for up to `reg[rbufsize]`
bytes. The actual number of bytes written is reported in `reg[rdstlen]`.

Status codes written to `reg[rstatus]`:

| Value | Meaning |
|-------|---------|
| `0` | No network card attached, **or** no packet is waiting right now. `reg[rdstlen] = 0`. Call again later (e.g. next frame/tick) to keep checking. |
| `1` | A packet was received. `reg[rdstlen]` holds its size (`<= reg[rbufsize]`), and that many bytes at `reg[raddr]` are now valid. |
| `2` | The HOST closed the connection, or the socket errored. The card is auto-detached - a following `OP_NET_ATTACHED` will report `0`. `reg[rdstlen] = 0`. |
| `3` | A packet **was** waiting, but it's larger than `reg[rbufsize]`. It has already been discarded (consumed off the socket, but not copied anywhere) - the connection itself is left up and unaffected. Either grow your receive buffer or check `OP_NET_MAXPACKET` up front to size it correctly. `reg[rdstlen] = 0`. |

Call this once per step/frame from a polling loop if you want to react to
incoming packets as they arrive (see example below).

---

### `OP_NET_POLL rstatus`

A cheap, read-only check for whether a packet is currently waiting to be
received - without actually receiving it.

- `reg[rstatus] = 1` - at least one packet is waiting; a following
  `OP_NET_RECV` should succeed immediately.
- `reg[rstatus] = 0` - no network card attached, or nothing waiting right
  now.

This is purely a convenience: `OP_NET_RECV` already performs the same
non-blocking check internally before it ever tries to receive, so calling
`OP_NET_POLL` first is optional. It's useful when a program wants to know
*whether* data is waiting before deciding *which* buffer to receive it
into, without any risk of blocking either way.

---

### `OP_NET_MAXPACKET rdst`

Reports the maximum packet size the channel was set up with (a property of
the connection, fixed for its whole lifetime).

- `reg[rdst]` = max packet size in bytes, if attached.
- `reg[rdst] = 0` if no network card is attached.

Use this to size send/receive buffers correctly up front instead of
guessing - packets larger than this cannot be sent (`OP_NET_SEND` status
`2`) and incoming packets larger than your own buffer get dropped
(`OP_NET_RECV` status `3`), so as long as your buffers are at least this
big you'll never hit either case for size reasons.

---

## Typical usage pattern

```
; --- one-time setup ---
OP_NET_MAXPACKET  r_maxpkt      ; find out how big buffers need to be
                                ; (skip this and just use a big-enough
                                ; buffer if you don't care)

; --- send something ---
; ... write your packet bytes into RAM at MSG_ADDR, MSG_LEN bytes ...
OP_NET_SEND       r_addr(MSG_ADDR), r_len(MSG_LEN), r_status
; check r_status: 1 = sent, 0 = not attached, 2/3 = problem

; --- poll for incoming packets, once per frame/tick ---
loop:
  OP_NET_RECV     r_addr(RECV_BUF), r_bufsize(RECV_CAP), r_dstlen, r_status
  ; r_status == 1: a packet of r_dstlen bytes just arrived at RECV_BUF -
  ;                go handle it
  ; r_status == 0: nothing right now - just continue with the rest of
  ;                the frame, and check again next frame
  ; r_status == 2: HOST disconnected - card is now detached
  ; r_status == 3: an oversized packet was dropped - buffer is too small
  ...
```

## Not attached / no `--net` given

If the VM was started without `--net`, all five instructions still run
without faulting - they just behave as documented above for "no network
card attached": `OP_NET_ATTACHED` and `OP_NET_POLL` report `0`,
`OP_NET_SEND` reports status `0` (and sends nothing), `OP_NET_RECV` reports
status `0` with `reg[rdstlen] = 0` (and reads nothing), and
`OP_NET_MAXPACKET` reports `0`. This mirrors exactly how the existing
`OP_SVFS_*` disk instructions behave without `--disk`.
