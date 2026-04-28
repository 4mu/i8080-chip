# i8080 — Intel 8080 CPU Emulator

A cycle-accurate emulator of the Intel 8080 processor written in C. Implements the full documented instruction set, a pluggable I/O handler system, and software interrupt delivery.

## Features

- Complete documented 8080 instruction set (NOP through RST vectors)
- Accurate flag behaviour: Sign, Zero, Parity, Carry, and Auxiliary Carry
- Correct PSW push/pop quirks (bit 1 always set, bits 3 and 5 always clear)
- Dispatch-table-based execution for fast opcode lookup
- Pluggable I/O port handlers via `io_register_read` / `io_register_write`
- Software interrupt delivery via `raise_interrupt`
- Cycle counting for timing-sensitive applications
- Binary program loader with configurable load address

## Building

```sh
cc -O2 -o i8080 i8080.c
```

No external dependencies. A C99-compatible compiler is required. GCC and Clang are both supported; the parity flag computation uses `__builtin_parity`, so MSVC is not currently supported without a small shim.

## Usage

Run with a binary ROM image:

```sh
./i8080 program.bin
```

If no file is provided, the emulator runs a minimal built-in stub (two NOPs followed by HLT).

On exit, the final PC and total cycle count are printed:

```
Done. PC=0x0000 Cycles=11
```

## API

The emulator is designed to be embedded. The key functions are:

```c
void init_cpu(i8080 *cpu);
void load_program(i8080 *cpu, const char *filename, uint16_t offset);
void emulate(i8080 *cpu);
```

### I/O Ports

Register handlers for specific I/O ports before calling `emulate`:

```c
uint8_t my_read(uint8_t port) { return 0x42; }
void    my_write(uint8_t port, uint8_t value) { /* ... */ }

io_register_read(0x01, my_read);
io_register_write(0x01, my_write);
```

Unregistered ports return `0x00` on read and silently discard writes.

### Interrupts

Interrupts can be raised from outside the emulation loop (e.g. from a timer or platform layer):

```c
raise_interrupt(&cpu, 0x38);   // deliver RST 7
```

The interrupt is held pending until the CPU executes an `EI` instruction and the emulation loop picks it up. Interrupts are automatically disabled on delivery, matching 8080 hardware behaviour.

## CPU State

The `i8080` struct exposes the full processor state and can be inspected or modified at any point:

```c
typedef struct {
    uint8_t  memory[65536];
    uint8_t  a, b, c, d, e, h, l;  // registers
    uint16_t sp, pc;                // stack pointer, program counter
    uint8_t  f;                     // flags register
    int      halted;
    int      cycles;
    int      interrupts_enabled;
    int      interrupt_pending;
} i8080;
```

## Known Limitations

- Undocumented opcodes (0x08, 0x10, 0x18, 0x20, 0x28, 0x30, 0x38) halt the CPU rather than acting as NOP aliases.
- The I/O and opcode dispatch tables are global statics, so only one emulated CPU instance is supported per process.
- The cycle counter is a signed `int` and will overflow for long-running programs.
- `__builtin_parity` is a GCC/Clang extension; a portable fallback is needed for other compilers.

## License

MIT
