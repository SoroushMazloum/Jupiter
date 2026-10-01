# JupiterOS v0.10-v0.12

This release builds on v0.9.3 and implements three milestones:

## v0.10 - Userspace C toolchain

- `include/jupiter.h` provides a tiny userspace API.
- `user/crt0.S` provides C program startup and calls `main(argc, argv)`.
- `user/hello.c` is a real C userspace program.
- `tools/make_jexe.py` wraps a flat i386 binary as JEXE v1.
- `make user` builds `disk/HELLO.JXE` and `disk/LOOP.JXE`.

## v0.11 - argc/argv

The kernel builds the initial userspace stack with:

- `argc`
- `argv[]`
- NUL-terminated argument strings

Example:

```text
jupiter> exec HELLO.JXE one two
```

The C program receives those arguments through `main(int argc, char **argv)`.

## v0.12 - Process management

- Up to four concurrent user processes.
- Separate user code/stack pages and page tables per process.
- Per-process address-space switching with CR3.
- Process states: READY, RUNNING, ZOMBIE, FREE.
- `ps` shows process state and ticks.
- `kill PID` terminates a process.
- `wait PID` waits for a zombie and reaps its process slot.
- `j_getpid()` returns the current process ID.

## Build

```bash
make
```

For only userspace programs:

```bash
make user
```

To reset the filesystem:

```bash
make format
```

To run:

```bash
make run
```

The host build requires NASM, a 32-bit-capable GCC/binutils toolchain, and QEMU.

## v0.12.1 userspace linker fix

The userspace linker places the program virtual address at `0x00400000` while keeping the JEXE file flat and compact. This is required because the C compiler emits absolute addresses for string literals and other data.

Without this, a C userspace program could start successfully but crash when its first `j_print()` accessed a string at an unmapped low address.

## v0.12.2 terminal output synchronization

v0.12.2 fixes garbled console output when a Ring 3 program prints while the
preemptive PIT scheduler is running. VGA terminal output is now protected by
saving/restoring the interrupt flag so a timer interrupt cannot switch tasks
in the middle of a logical output operation. User `write` syscalls and
process start/exit messages are emitted atomically, preventing characters
from the shell prompt and userspace output from interleaving.

## v0.12.3 foreground exec and terminal output fix

`exec PROGRAM [args...]` now runs in the foreground by default. The shell does not print a new prompt until the program exits, preventing the shell prompt and command-line redraw from overwriting userspace output.

To intentionally run a userspace program in the background, append `&`, for example:

    exec LOOP.JXE &

Background processes can still be inspected with `ps`, terminated with `kill`, and waited on with `wait`.
