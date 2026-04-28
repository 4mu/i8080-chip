#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>


typedef struct {
    uint8_t memory[65536];

    // Accumulator + general purpose regs
    uint8_t a;             // Accumulator
    uint8_t b, c;          // Counter pair
    uint8_t d, e;          // Pointer pair
    uint8_t h, l;          // Memaddr pair

    // Special regs
    uint16_t sp;           // Stack ptr
    uint16_t pc;           // Prog count

    // Flags reg
    uint8_t f;

    int halted;
    int cycles;
    int interrupts_enabled;
    int interrupt_pending;   // vector to deliver, -1 if none
} i8080;


// Flag masks
#define FLAG_Z  (1 << 6)
#define FLAG_S  (1 << 7)
#define FLAG_P  (1 << 2)
#define FLAG_CY (1 << 0)
#define FLAG_AC (1 << 4)


// Read write
static inline uint8_t read_byte(i8080 *cpu, uint16_t addr) {
    return cpu->memory[addr];
}
static inline void write_byte(i8080 *cpu, uint16_t addr, uint8_t value) {
    cpu->memory[addr] = value;
}


// Reg pair helpers
static inline uint16_t get_bc(i8080 *cpu) {
    return (cpu->b << 8) | cpu->c;
}
static inline void set_bc(i8080 *cpu, uint16_t v) {
    cpu->b = v >> 8;
    cpu->c = v & 0xFF;
}
static inline uint16_t get_de(i8080 *cpu) {
    return (cpu->d << 8) | cpu->e;
}
static inline void set_de(i8080 *cpu, uint16_t v) {
    cpu->d = v >> 8;
    cpu->e = v & 0xFF;
}
static inline uint16_t get_hl(i8080 *cpu) {
    return (cpu->h << 8) | cpu->l;
}
static inline void set_hl(i8080 *cpu, uint16_t v) {
    cpu->h = v >> 8;
    cpu->l = v & 0xFF;
}


// IO

typedef uint8_t (*io_read_handler)(uint8_t port);
typedef void    (*io_write_handler)(uint8_t port, uint8_t value);

static io_read_handler  io_read_table[256];
static io_write_handler io_write_table[256];

static uint8_t io_read_unhandled(uint8_t port) {
    (void)port;
    return 0;
}
static void io_write_unhandled(uint8_t port, uint8_t value) {
    (void)port;
    (void)value;
}

void io_register_read(uint8_t port, io_read_handler fn) {
    io_read_table[port] = fn;
}
void io_register_write(uint8_t port, io_write_handler fn) {
    io_write_table[port] = fn;
}

static inline uint8_t io_in(uint8_t port) {
    return io_read_table[port](port);
}
static inline void io_out(uint8_t port, uint8_t value) {
    io_write_table[port](port, value);
}

// Initialise IO tables
static void init_io_tables(void) {
    for (int i = 0; i < 256; i++) {
        io_read_table[i]  = io_read_unhandled;
        io_write_table[i] = io_write_unhandled;
    }
}


// Opcode handlers
typedef void (*opcode_handler)(i8080 *cpu, uint8_t op);
static opcode_handler opcode_table[256];

// Raise an interrupt from outside (platform/hardware layer)
void raise_interrupt(i8080 *cpu, uint8_t vector) {
    cpu->interrupt_pending = vector;
}

static void op_unimplemented(i8080 *cpu, uint8_t op) {
    (void)op;
    cpu->cycles += 4;
}

static void op_0x00(i8080 *cpu, uint8_t op) {  // NOP
    (void)op;
    cpu->cycles += 4;
}
static void op_0x01(i8080 *cpu, uint8_t op) {  // LXI B, d16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);

    uint16_t value = (hi << 8) | lo;
    set_bc(cpu, value);

    cpu->cycles += 10;
}
static void op_0x02(i8080 *cpu, uint8_t op) {  // STAX B
    (void)op;
    uint16_t addr = get_bc(cpu);
    write_byte(cpu, addr, cpu->a);

    cpu->cycles += 7;
}
static void op_0x03(i8080 *cpu, uint8_t op) {  // INX B
    (void)op;
    uint16_t value = get_bc(cpu);
    set_bc(cpu, value + 1);

    cpu->cycles += 5;
}
static void op_0x04(i8080 *cpu, uint8_t op) {  // INR B
    (void)op;
    uint8_t result = cpu->b + 1;

    // AC: check carry from bit 3
    cpu->f &= ~FLAG_AC;
    if (((cpu->b & 0x0F) + 1) & 0x10)
        cpu->f |= FLAG_AC;

    cpu->b = result;

    // S flag
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z flag
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P flag (parity)
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x05(i8080 *cpu, uint8_t op) {  // DCR B
    (void)op;
    uint8_t result = cpu->b - 1;

    // AC: borrow from bit 4 (lower nibble underflow)
    cpu->f &= ~FLAG_AC;
    if ((cpu->b & 0x0F) == 0x00)
        cpu->f |= FLAG_AC;

    cpu->b = result;

    // S flag
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z flag
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P flag
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x06(i8080 *cpu, uint8_t op) {  // MVI B, d8
    (void)op;
    uint8_t value = read_byte(cpu, cpu->pc++);
    cpu->b = value;

    cpu->cycles += 7;
}
static void op_0x07(i8080 *cpu, uint8_t op) {  // RLC
    (void)op;
    uint8_t old_a = cpu->a;

    uint8_t msb = (old_a & 0x80) >> 7;

    cpu->a = (old_a << 1) | msb;

    // Carry = old bit 7
    if (msb)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->cycles += 4;
}
static void op_0x09(i8080 *cpu, uint8_t op) {  // DAD B
    (void)op;
    uint32_t hl = get_hl(cpu);
    uint32_t bc = get_bc(cpu);

    uint32_t result = hl + bc;

    set_hl(cpu, (uint16_t)result);

    // CY flag only
    if (result & 0x10000)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->cycles += 10;
}
static void op_0x0a(i8080 *cpu, uint8_t op) {  // LDAX B
    (void)op;
    uint16_t addr = get_bc(cpu);
    cpu->a = read_byte(cpu, addr);

    cpu->cycles += 7;
}
static void op_0x0b(i8080 *cpu, uint8_t op) {  // DCX B
    (void)op;
    uint16_t value = get_bc(cpu);
    set_bc(cpu, value - 1);

    cpu->cycles += 5;
}
static void op_0x0c(i8080 *cpu, uint8_t op) {  // INR C
    (void)op;
    uint8_t result = cpu->c + 1;

    // AC: carry from bit 3
    cpu->f &= ~FLAG_AC;
    if (((cpu->c & 0x0F) + 1) & 0x10)
        cpu->f |= FLAG_AC;

    cpu->c = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x0d(i8080 *cpu, uint8_t op) {  // DCR C
    (void)op;
    uint8_t result = cpu->c - 1;

    // AC: borrow from bit 4
    cpu->f &= ~FLAG_AC;
    if ((cpu->c & 0x0F) == 0x00)
        cpu->f |= FLAG_AC;

    cpu->c = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x0e(i8080 *cpu, uint8_t op) {  // MVI C, d8
    (void)op;
    uint8_t value = read_byte(cpu, cpu->pc++);
    cpu->c = value;

    cpu->cycles += 7;
}
static void op_0x0f(i8080 *cpu, uint8_t op) {  // RRC
    (void)op;
    uint8_t old_a = cpu->a;

    uint8_t lsb = old_a & 0x01;

    cpu->a = (old_a >> 1) | (lsb << 7);

    // Carry = old bit 0
    if (lsb)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->cycles += 4;
}

static void op_0x11(i8080 *cpu, uint8_t op) {  // LXI D, d16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);

    uint16_t value = (hi << 8) | lo;
    set_de(cpu, value);

    cpu->cycles += 10;
}
static void op_0x12(i8080 *cpu, uint8_t op) {  // STAX D
    (void)op;
    uint16_t addr = get_de(cpu);
    write_byte(cpu, addr, cpu->a);

    cpu->cycles += 7;
}
static void op_0x13(i8080 *cpu, uint8_t op) {  // INX D
    (void)op;
    uint16_t value = get_de(cpu);
    set_de(cpu, value + 1);

    cpu->cycles += 5;
}
static void op_0x14(i8080 *cpu, uint8_t op) {  // INR D
    (void)op;
    uint8_t result = cpu->d + 1;

    // AC: carry from bit 3
    cpu->f &= ~FLAG_AC;
    if (((cpu->d & 0x0F) + 1) & 0x10)
        cpu->f |= FLAG_AC;

    cpu->d = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x15(i8080 *cpu, uint8_t op) {  // DCR D
    (void)op;
    uint8_t result = cpu->d - 1;

    // AC: borrow from lower nibble
    cpu->f &= ~FLAG_AC;
    if ((cpu->d & 0x0F) == 0x00)
        cpu->f |= FLAG_AC;

    cpu->d = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x16(i8080 *cpu, uint8_t op) {  // MVI D, d8
    (void)op;
    uint8_t value = read_byte(cpu, cpu->pc++);
    cpu->d = value;

    cpu->cycles += 7;
}
static void op_0x17(i8080 *cpu, uint8_t op) {  // RAL
    (void)op;
    uint8_t old_a = cpu->a;

    uint8_t old_cy = (cpu->f & FLAG_CY) ? 1 : 0;
    uint8_t new_cy = (old_a & 0x80) >> 7;

    cpu->a = (old_a << 1) | old_cy;

    if (new_cy)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->cycles += 4;
}
static void op_0x19(i8080 *cpu, uint8_t op) {  // DAD D
    (void)op;
    uint32_t hl = get_hl(cpu);
    uint32_t de = get_de(cpu);

    uint32_t result = hl + de;

    set_hl(cpu, (uint16_t)result);

    // Carry flag only
    if (result & 0x10000)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->cycles += 10;
}
static void op_0x1a(i8080 *cpu, uint8_t op) {  // LDAX D
    (void)op;
    uint16_t addr = get_de(cpu);
    cpu->a = read_byte(cpu, addr);

    cpu->cycles += 7;
}
static void op_0x1b(i8080 *cpu, uint8_t op) {  // DCX
    (void)op;
    uint16_t value = get_de(cpu);
    set_de(cpu, value - 1);

    cpu->cycles += 5;
}
static void op_0x1c(i8080 *cpu, uint8_t op) {  // INR E
    (void)op;
    uint8_t result = cpu->e + 1;

    // AC: carry from bit 3
    cpu->f &= ~FLAG_AC;
    if (((cpu->e & 0x0F) + 1) & 0x10)
        cpu->f |= FLAG_AC;

    cpu->e = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x1d(i8080 *cpu, uint8_t op) {  // DCR
    (void)op;
    uint8_t result = cpu->e - 1;

    // AC: borrow from lower nibble
    cpu->f &= ~FLAG_AC;
    if ((cpu->e & 0x0F) == 0x00)
        cpu->f |= FLAG_AC;

    cpu->e = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x1e(i8080 *cpu, uint8_t op) {  // MVI E, d8
    (void)op;
    uint8_t value = read_byte(cpu, cpu->pc++);
    cpu->e = value;

    cpu->cycles += 7;
}
static void op_0x1f(i8080 *cpu, uint8_t op) {  // RAR
    (void)op;
    uint8_t old_a = cpu->a;

    uint8_t old_cy = (cpu->f & FLAG_CY) ? 1 : 0;
    uint8_t new_cy = old_a & 0x01;

    cpu->a = (old_a >> 1) | (old_cy << 7);

    if (new_cy)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->cycles += 4;
}

static void op_0x21(i8080 *cpu, uint8_t op) {  // LXI H, d16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);

    uint16_t value = (hi << 8) | lo;
    set_hl(cpu, value);

    cpu->cycles += 10;
}
static void op_0x22(i8080 *cpu, uint8_t op) {  // SHLD addr
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);

    uint16_t addr = (hi << 8) | lo;

    write_byte(cpu, addr, cpu->l);
    write_byte(cpu, addr + 1, cpu->h);

    cpu->cycles += 16;
}
static void op_0x23(i8080 *cpu, uint8_t op) {  // INX H
    (void)op;
    uint16_t value = get_hl(cpu);
    set_hl(cpu, value + 1);

    cpu->cycles += 5;
}
static void op_0x24(i8080 *cpu, uint8_t op) {  // INR H
    (void)op;
    uint8_t result = cpu->h + 1;

    // AC: carry from bit 3
    cpu->f &= ~FLAG_AC;
    if (((cpu->h & 0x0F) + 1) & 0x10)
        cpu->f |= FLAG_AC;

    cpu->h = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x25(i8080 *cpu, uint8_t op) {  // DCR H
    (void)op;
    uint8_t result = cpu->h - 1;

    // AC: borrow from lower nibble
    cpu->f &= ~FLAG_AC;
    if ((cpu->h & 0x0F) == 0x00)
        cpu->f |= FLAG_AC;

    cpu->h = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x26(i8080 *cpu, uint8_t op) {  // MVI H, d8
    (void)op;
    uint8_t value = read_byte(cpu, cpu->pc++);
    cpu->h = value;

    cpu->cycles += 7;
}
static void op_0x27(i8080 *cpu, uint8_t op) {  // DAA
    (void)op;
    uint8_t correction = 0;
    uint8_t old_a = cpu->a;
    uint8_t cy = (cpu->f & FLAG_CY) ? 1 : 0;

    // lower nibble
    if ((cpu->a & 0x0F) > 9 || (cpu->f & FLAG_AC))
        correction |= 0x06;

    // upper nibble
    if ((cpu->a >> 4) > 9 || cy || ((cpu->a + correction) > 0x9F))
        correction |= 0x60;

    uint16_t result = cpu->a + correction;

    // set AC (based on lower nibble adjust)
    if (((old_a & 0x0F) + (correction & 0x0F)) & 0x10)
        cpu->f |= FLAG_AC;
    else
        cpu->f &= ~FLAG_AC;

    // set CY
    if (result & 0x100)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->a = (uint8_t)result;

    // S
    if (cpu->a & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (cpu->a == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(cpu->a);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    cpu->cycles += 4;
}
static void op_0x29(i8080 *cpu, uint8_t op) {  // DAD H
    (void)op;
    uint32_t hl = get_hl(cpu);

    uint32_t result = hl + hl;

    set_hl(cpu, (uint16_t)result);

    // Carry flag only
    if (result & 0x10000)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->cycles += 10;
}
static void op_0x2a(i8080 *cpu, uint8_t op) {  // LHLD addr
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);

    uint16_t addr = (hi << 8) | lo;

    cpu->l = read_byte(cpu, addr);
    cpu->h = read_byte(cpu, addr + 1);

    cpu->cycles += 16;
}
static void op_0x2b(i8080 *cpu, uint8_t op) {  // DCX H
    (void)op;
    uint16_t value = get_hl(cpu);
    set_hl(cpu, value - 1);

    cpu->cycles += 5;
}
static void op_0x2c(i8080 *cpu, uint8_t op) {  // INR L
    (void)op;
    uint8_t result = cpu->l + 1;

    // AC: carry from bit 3
    cpu->f &= ~FLAG_AC;
    if (((cpu->l & 0x0F) + 1) & 0x10)
        cpu->f |= FLAG_AC;

    cpu->l = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x2d(i8080 *cpu, uint8_t op) {  // DCR L
    (void)op;
    uint8_t result = cpu->l - 1;

    // AC: borrow from lower nibble
    cpu->f &= ~FLAG_AC;
    if ((cpu->l & 0x0F) == 0x00)
        cpu->f |= FLAG_AC;

    cpu->l = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity = __builtin_parity(result);
    if (parity == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x2e(i8080 *cpu, uint8_t op) {  // MVI L, d8
    (void)op;
    uint8_t value = read_byte(cpu, cpu->pc++);
    cpu->l = value;

    cpu->cycles += 7;
}
static void op_0x2f(i8080 *cpu, uint8_t op) {  // CMA
    (void)op;
    cpu->a = ~cpu->a;

    cpu->cycles += 4;
}

static void op_0x31(i8080 *cpu, uint8_t op) {  // LXI SP, d16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    cpu->sp = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0x32(i8080 *cpu, uint8_t op) {  // STA addr
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    uint16_t addr = (uint16_t)((hi << 8) | lo);
    write_byte(cpu, addr, cpu->a);
    cpu->cycles += 13;
}
static void op_0x33(i8080 *cpu, uint8_t op) {  // INX SP
    (void)op;
    cpu->sp++;
    cpu->cycles += 5;
}
static void op_0x34(i8080 *cpu, uint8_t op) {  // INR M
    (void)op;
    uint16_t addr = get_hl(cpu);
    uint8_t val = read_byte(cpu, addr);
    uint8_t result = val + 1;

    // AC: carry from bit 3
    cpu->f &= ~FLAG_AC;
    if (((val & 0x0F) + 1) & 0x10)
        cpu->f |= FLAG_AC;

    write_byte(cpu, addr, result);

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity34 = __builtin_parity(result);
    if (parity34 == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 10;
}
static void op_0x35(i8080 *cpu, uint8_t op) {  // DCR M
    (void)op;
    uint16_t addr = get_hl(cpu);
    uint8_t val = read_byte(cpu, addr);
    uint8_t result = val - 1;

    // AC: borrow from lower nibble
    cpu->f &= ~FLAG_AC;
    if ((val & 0x0F) == 0x00)
        cpu->f |= FLAG_AC;

    write_byte(cpu, addr, result);

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity35 = __builtin_parity(result);
    if (parity35 == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 10;
}
static void op_0x36(i8080 *cpu, uint8_t op) {  // MVI M, d8
    (void)op;
    uint8_t value = read_byte(cpu, cpu->pc++);
    write_byte(cpu, get_hl(cpu), value);
    cpu->cycles += 10;
}
static void op_0x37(i8080 *cpu, uint8_t op) {  // STC
    (void)op;
    cpu->f |= FLAG_CY;
    cpu->cycles += 4;
}
static void op_0x39(i8080 *cpu, uint8_t op) {  // DAD SP
    (void)op;
    uint32_t hl = get_hl(cpu);
    uint32_t sp = cpu->sp;

    uint32_t result = hl + sp;

    set_hl(cpu, (uint16_t)result);

    // CY flag only
    if (result & 0x10000)
        cpu->f |= FLAG_CY;
    else
        cpu->f &= ~FLAG_CY;

    cpu->cycles += 10;
}
static void op_0x3a(i8080 *cpu, uint8_t op) {  // LDA addr
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    uint16_t addr = (uint16_t)((hi << 8) | lo);
    cpu->a = read_byte(cpu, addr);
    cpu->cycles += 13;
}
static void op_0x3b(i8080 *cpu, uint8_t op) {  // DCX SP
    (void)op;
    cpu->sp--;
    cpu->cycles += 5;
}
static void op_0x3c(i8080 *cpu, uint8_t op) {  // INR A
    (void)op;
    uint8_t result = cpu->a + 1;

    // AC: carry from bit 3
    cpu->f &= ~FLAG_AC;
    if (((cpu->a & 0x0F) + 1) & 0x10)
        cpu->f |= FLAG_AC;

    cpu->a = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity3c = __builtin_parity(result);
    if (parity3c == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x3d(i8080 *cpu, uint8_t op) {  // DCR A
    (void)op;
    uint8_t result = cpu->a - 1;

    // AC: borrow from lower nibble
    cpu->f &= ~FLAG_AC;
    if ((cpu->a & 0x0F) == 0x00)
        cpu->f |= FLAG_AC;

    cpu->a = result;

    // S
    if (result & 0x80) cpu->f |= FLAG_S;
    else cpu->f &= ~FLAG_S;

    // Z
    if (result == 0) cpu->f |= FLAG_Z;
    else cpu->f &= ~FLAG_Z;

    // P
    uint8_t parity3d = __builtin_parity(result);
    if (parity3d == 0) cpu->f |= FLAG_P;
    else cpu->f &= ~FLAG_P;

    // CY unchanged

    cpu->cycles += 5;
}
static void op_0x3e(i8080 *cpu, uint8_t op) {  // MVI A, d8
    (void)op;
    uint8_t value = read_byte(cpu, cpu->pc++);
    cpu->a = value;
    cpu->cycles += 7;
}
static void op_0x3f(i8080 *cpu, uint8_t op) {  // CMC
    (void)op;
    // Complement carry flag
    cpu->f ^= FLAG_CY;
    cpu->cycles += 4;
}

// MOV r1, r2  (0x40-0x7F, except 0x76 which is HLT)
// Opcode bits 5-3 = dst reg, bits 2-0 = src reg
// Encoding: B=0, C=1, D=2, E=3, H=4, L=5, M=6, A=7
static uint8_t mov_get_reg(i8080 *cpu, uint8_t reg) {
    switch (reg) {
        case 0: return cpu->b;
        case 1: return cpu->c;
        case 2: return cpu->d;
        case 3: return cpu->e;
        case 4: return cpu->h;
        case 5: return cpu->l;
        case 6: return read_byte(cpu, get_hl(cpu));
        case 7: return cpu->a;
        default: return 0;
    }
}
static void mov_set_reg(i8080 *cpu, uint8_t reg, uint8_t val) {
    switch (reg) {
        case 0: cpu->b = val; break;
        case 1: cpu->c = val; break;
        case 2: cpu->d = val; break;
        case 3: cpu->e = val; break;
        case 4: cpu->h = val; break;
        case 5: cpu->l = val; break;
        case 6: write_byte(cpu, get_hl(cpu), val); break;
        case 7: cpu->a = val; break;
    }
}
static void op_mov(i8080 *cpu, uint8_t op) {
    uint8_t dst = (op >> 3) & 0x07;
    uint8_t src = op & 0x07;
    uint8_t val = mov_get_reg(cpu, src);
    mov_set_reg(cpu, dst, val);
    // M src or dst = 7 cycles, reg to reg = 5 cycles
    cpu->cycles += (src == 6 || dst == 6) ? 7 : 5;
}

static void op_0x76(i8080 *cpu, uint8_t op) {  // HLT
    (void)op;
    cpu->halted = 1;
    cpu->cycles += 7;
}

// ALU helpers (0x80-0xBF)

static inline void set_szp_flags(i8080 *cpu, uint8_t result) {
    if (result & 0x80) cpu->f |= FLAG_S;  else cpu->f &= ~FLAG_S;
    if (result == 0)   cpu->f |= FLAG_Z;  else cpu->f &= ~FLAG_Z;
    if (__builtin_parity(result) == 0) cpu->f |= FLAG_P; else cpu->f &= ~FLAG_P;
}
static void op_add(i8080 *cpu, uint8_t op) {   // ADD r  0x80-0x87
    uint8_t src = mov_get_reg(cpu, op & 0x07);
    uint16_t result = (uint16_t)cpu->a + src;

    cpu->f &= ~FLAG_AC;
    if (((cpu->a & 0x0F) + (src & 0x0F)) & 0x10) cpu->f |= FLAG_AC;

    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;

    cpu->a = (uint8_t)result;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += ((op & 0x07) == 6) ? 7 : 4;
}
static void op_adc(i8080 *cpu, uint8_t op) {   // ADC r  0x88-0x8F
    uint8_t src = mov_get_reg(cpu, op & 0x07);
    uint8_t cy  = (cpu->f & FLAG_CY) ? 1 : 0;
    uint16_t result = (uint16_t)cpu->a + src + cy;

    cpu->f &= ~FLAG_AC;
    if (((cpu->a & 0x0F) + (src & 0x0F) + cy) & 0x10) cpu->f |= FLAG_AC;

    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;

    cpu->a = (uint8_t)result;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += ((op & 0x07) == 6) ? 7 : 4;
}
static void op_sub(i8080 *cpu, uint8_t op) {   // SUB r  0x90-0x97
    uint8_t src = mov_get_reg(cpu, op & 0x07);
    uint16_t result = (uint16_t)cpu->a - src;

    cpu->f &= ~FLAG_AC;
    if ((cpu->a & 0x0F) < (src & 0x0F)) cpu->f |= FLAG_AC;

    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;

    cpu->a = (uint8_t)result;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += ((op & 0x07) == 6) ? 7 : 4;
}
static void op_sbb(i8080 *cpu, uint8_t op) {   // SBB r  0x98-0x9F
    uint8_t src = mov_get_reg(cpu, op & 0x07);
    uint8_t cy  = (cpu->f & FLAG_CY) ? 1 : 0;
    uint16_t result = (uint16_t)cpu->a - src - cy;

    cpu->f &= ~FLAG_AC;
    if ((cpu->a & 0x0F) < ((src & 0x0F) + cy)) cpu->f |= FLAG_AC;

    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;

    cpu->a = (uint8_t)result;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += ((op & 0x07) == 6) ? 7 : 4;
}
static void op_ana(i8080 *cpu, uint8_t op) {   // ANA r  0xA0-0xA7
    uint8_t src = mov_get_reg(cpu, op & 0x07);

    // AC = OR of bit 3 of both operands (i8080 quirk)
    if ((cpu->a | src) & 0x08) cpu->f |= FLAG_AC; else cpu->f &= ~FLAG_AC;

    cpu->a &= src;
    cpu->f &= ~FLAG_CY;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += ((op & 0x07) == 6) ? 7 : 4;
}
static void op_xra(i8080 *cpu, uint8_t op) {   // XRA r  0xA8-0xAF
    uint8_t src = mov_get_reg(cpu, op & 0x07);
    cpu->a ^= src;
    cpu->f &= ~(FLAG_CY | FLAG_AC);
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += ((op & 0x07) == 6) ? 7 : 4;
}
static void op_ora(i8080 *cpu, uint8_t op) {   // ORA r  0xB0-0xB7
    uint8_t src = mov_get_reg(cpu, op & 0x07);
    cpu->a |= src;
    cpu->f &= ~(FLAG_CY | FLAG_AC);
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += ((op & 0x07) == 6) ? 7 : 4;
}
static void op_cmp(i8080 *cpu, uint8_t op) {   // CMP r  0xB8-0xBF
    uint8_t src = mov_get_reg(cpu, op & 0x07);
    uint16_t result = (uint16_t)cpu->a - src;

    cpu->f &= ~FLAG_AC;
    if ((cpu->a & 0x0F) < (src & 0x0F)) cpu->f |= FLAG_AC;

    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;

    // A is NOT written - flags only
    set_szp_flags(cpu, (uint8_t)result);
    cpu->cycles += ((op & 0x07) == 6) ? 7 : 4;
}

// Stack helpers

static inline void stack_push(i8080 *cpu, uint16_t val) {
    cpu->sp--;
    write_byte(cpu, cpu->sp, val >> 8);
    cpu->sp--;
    write_byte(cpu, cpu->sp, val & 0xFF);
}
static inline uint16_t stack_pop(i8080 *cpu) {
    uint8_t lo = read_byte(cpu, cpu->sp++);
    uint8_t hi = read_byte(cpu, cpu->sp++);
    return (uint16_t)((hi << 8) | lo);
}

// Conditions returns

static void op_0xC0(i8080 *cpu, uint8_t op) {  // RNZ — Return if Not Zero
    (void)op;
    if (!(cpu->f & FLAG_Z)) {
        cpu->pc = stack_pop(cpu);
        cpu->cycles += 11;
    } else {
        cpu->cycles += 5;
    }
}
static void op_0xC8(i8080 *cpu, uint8_t op) {  // RZ — Return if Zero
    (void)op;
    if (cpu->f & FLAG_Z) {
        cpu->pc = stack_pop(cpu);
        cpu->cycles += 11;
    } else {
        cpu->cycles += 5;
    }
}
static void op_0xC9(i8080 *cpu, uint8_t op) {  // RET — Unconditional return
    (void)op;
    cpu->pc = stack_pop(cpu);
    cpu->cycles += 10;
}
static void op_0xD0(i8080 *cpu, uint8_t op) {  // RNC — Return if No Carry
    (void)op;
    if (!(cpu->f & FLAG_CY)) {
        cpu->pc = stack_pop(cpu);
        cpu->cycles += 11;
    } else {
        cpu->cycles += 5;
    }
}
static void op_0xD8(i8080 *cpu, uint8_t op) {  // RC — Return if Carry
    (void)op;
    if (cpu->f & FLAG_CY) {
        cpu->pc = stack_pop(cpu);
        cpu->cycles += 11;
    } else {
        cpu->cycles += 5;
    }
}
static void op_0xE0(i8080 *cpu, uint8_t op) {  // RPO — Return if Parity Odd
    (void)op;
    if (!(cpu->f & FLAG_P)) {
        cpu->pc = stack_pop(cpu);
        cpu->cycles += 11;
    } else {
        cpu->cycles += 5;
    }
}
static void op_0xE8(i8080 *cpu, uint8_t op) {  // RPE — Return if Parity Even
    (void)op;
    if (cpu->f & FLAG_P) {
        cpu->pc = stack_pop(cpu);
        cpu->cycles += 11;
    } else {
        cpu->cycles += 5;
    }
}
static void op_0xF0(i8080 *cpu, uint8_t op) {  // RP — Return if Positive (S=0)
    (void)op;
    if (!(cpu->f & FLAG_S)) {
        cpu->pc = stack_pop(cpu);
        cpu->cycles += 11;
    } else {
        cpu->cycles += 5;
    }
}
static void op_0xF8(i8080 *cpu, uint8_t op) {  // RM — Return if Minus (S=1)
    (void)op;
    if (cpu->f & FLAG_S) {
        cpu->pc = stack_pop(cpu);
        cpu->cycles += 11;
    } else {
        cpu->cycles += 5;
    }
}

// POP

static void op_0xC1(i8080 *cpu, uint8_t op) {  // POP B
    (void)op;
    set_bc(cpu, stack_pop(cpu));
    cpu->cycles += 10;
}
static void op_0xD1(i8080 *cpu, uint8_t op) {  // POP D
    (void)op;
    set_de(cpu, stack_pop(cpu));
    cpu->cycles += 10;
}
static void op_0xE1(i8080 *cpu, uint8_t op) {  // POP H
    (void)op;
    set_hl(cpu, stack_pop(cpu));
    cpu->cycles += 10;
}
static void op_0xF1(i8080 *cpu, uint8_t op) {  // POP PSW
    (void)op;
    uint16_t val = stack_pop(cpu);
    cpu->a = val >> 8;
    // Restore flags: bit1 always 1, bits 3 and 5 always 0 on 8080
    cpu->f = (val & 0xFF) | 0x02;
    cpu->f &= ~(0x08 | 0x20);
    cpu->cycles += 10;
}

// Conditions jumps

static void op_0xC2(i8080 *cpu, uint8_t op) {  // JNZ a16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (!(cpu->f & FLAG_Z))
        cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xC3(i8080 *cpu, uint8_t op) {  // JMP a16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xCA(i8080 *cpu, uint8_t op) {  // JZ a16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (cpu->f & FLAG_Z)
        cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xD2(i8080 *cpu, uint8_t op) {  // JNC a16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (!(cpu->f & FLAG_CY))
        cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xDA(i8080 *cpu, uint8_t op) {  // JC a16
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (cpu->f & FLAG_CY)
        cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xE2(i8080 *cpu, uint8_t op) {  // JPO a16 — Jump if Parity Odd
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (!(cpu->f & FLAG_P))
        cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xEA(i8080 *cpu, uint8_t op) {  // JPE a16 — Jump if Parity Even
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (cpu->f & FLAG_P)
        cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xF2(i8080 *cpu, uint8_t op) {  // JP a16 — Jump if Positive (S=0)
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (!(cpu->f & FLAG_S))
        cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xFA(i8080 *cpu, uint8_t op) {  // JM a16 — Jump if Minus (S=1)
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (cpu->f & FLAG_S)
        cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 10;
}
static void op_0xE9(i8080 *cpu, uint8_t op) {  // PCHL — Jump to HL
    (void)op;
    cpu->pc = get_hl(cpu);
    cpu->cycles += 5;
}

// Conditions calls

static void op_0xC4(i8080 *cpu, uint8_t op) {  // CNZ a16 — Call if Not Zero
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (!(cpu->f & FLAG_Z)) {
        stack_push(cpu, cpu->pc);
        cpu->pc = (uint16_t)((hi << 8) | lo);
        cpu->cycles += 17;
    } else {
        cpu->cycles += 11;
    }
}
static void op_0xCC(i8080 *cpu, uint8_t op) {  // CZ a16 — Call if Zero
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (cpu->f & FLAG_Z) {
        stack_push(cpu, cpu->pc);
        cpu->pc = (uint16_t)((hi << 8) | lo);
        cpu->cycles += 17;
    } else {
        cpu->cycles += 11;
    }
}
static void op_0xCD(i8080 *cpu, uint8_t op) {  // CALL a16 — Unconditional call
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    stack_push(cpu, cpu->pc);
    cpu->pc = (uint16_t)((hi << 8) | lo);
    cpu->cycles += 17;
}
static void op_0xD4(i8080 *cpu, uint8_t op) {  // CNC a16 — Call if No Carry
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (!(cpu->f & FLAG_CY)) {
        stack_push(cpu, cpu->pc);
        cpu->pc = (uint16_t)((hi << 8) | lo);
        cpu->cycles += 17;
    } else {
        cpu->cycles += 11;
    }
}
static void op_0xDC(i8080 *cpu, uint8_t op) {  // CC a16 — Call if Carry
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (cpu->f & FLAG_CY) {
        stack_push(cpu, cpu->pc);
        cpu->pc = (uint16_t)((hi << 8) | lo);
        cpu->cycles += 17;
    } else {
        cpu->cycles += 11;
    }
}
static void op_0xE4(i8080 *cpu, uint8_t op) {  // CPO a16 — Call if Parity Odd
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (!(cpu->f & FLAG_P)) {
        stack_push(cpu, cpu->pc);
        cpu->pc = (uint16_t)((hi << 8) | lo);
        cpu->cycles += 17;
    } else {
        cpu->cycles += 11;
    }
}
static void op_0xEC(i8080 *cpu, uint8_t op) {  // CPE a16 — Call if Parity Even
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (cpu->f & FLAG_P) {
        stack_push(cpu, cpu->pc);
        cpu->pc = (uint16_t)((hi << 8) | lo);
        cpu->cycles += 17;
    } else {
        cpu->cycles += 11;
    }
}
static void op_0xF4(i8080 *cpu, uint8_t op) {  // CP a16 — Call if Positive (S=0)
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (!(cpu->f & FLAG_S)) {
        stack_push(cpu, cpu->pc);
        cpu->pc = (uint16_t)((hi << 8) | lo);
        cpu->cycles += 17;
    } else {
        cpu->cycles += 11;
    }
}
static void op_0xFC(i8080 *cpu, uint8_t op) {  // CM a16 — Call if Minus (S=1)
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->pc++);
    uint8_t hi = read_byte(cpu, cpu->pc++);
    if (cpu->f & FLAG_S) {
        stack_push(cpu, cpu->pc);
        cpu->pc = (uint16_t)((hi << 8) | lo);
        cpu->cycles += 17;
    } else {
        cpu->cycles += 11;
    }
}

// PUSH

static void op_0xC5(i8080 *cpu, uint8_t op) {  // PUSH B
    (void)op;
    stack_push(cpu, get_bc(cpu));
    cpu->cycles += 11;
}
static void op_0xD5(i8080 *cpu, uint8_t op) {  // PUSH D
    (void)op;
    stack_push(cpu, get_de(cpu));
    cpu->cycles += 11;
}
static void op_0xE5(i8080 *cpu, uint8_t op) {  // PUSH H
    (void)op;
    stack_push(cpu, get_hl(cpu));
    cpu->cycles += 11;
}
static void op_0xF5(i8080 *cpu, uint8_t op) {  // PUSH PSW
    (void)op;
    // bit1 always 1, bits 3 and 5 always 0 on 8080
    uint8_t flags = (cpu->f | 0x02) & ~(0x08 | 0x20);
    stack_push(cpu, (uint16_t)((cpu->a << 8) | flags));
    cpu->cycles += 11;
}

// Immediate ALU

static void op_0xC6(i8080 *cpu, uint8_t op) {  // ADI d8
    (void)op;
    uint8_t imm = read_byte(cpu, cpu->pc++);
    uint16_t result = (uint16_t)cpu->a + imm;
    cpu->f &= ~FLAG_AC;
    if (((cpu->a & 0x0F) + (imm & 0x0F)) & 0x10) cpu->f |= FLAG_AC;
    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;
    cpu->a = (uint8_t)result;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += 7;
}
static void op_0xCE(i8080 *cpu, uint8_t op) {  // ACI d8
    (void)op;
    uint8_t imm = read_byte(cpu, cpu->pc++);
    uint8_t cy  = (cpu->f & FLAG_CY) ? 1 : 0;
    uint16_t result = (uint16_t)cpu->a + imm + cy;
    cpu->f &= ~FLAG_AC;
    if (((cpu->a & 0x0F) + (imm & 0x0F) + cy) & 0x10) cpu->f |= FLAG_AC;
    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;
    cpu->a = (uint8_t)result;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += 7;
}
static void op_0xD6(i8080 *cpu, uint8_t op) {  // SUI d8
    (void)op;
    uint8_t imm = read_byte(cpu, cpu->pc++);
    uint16_t result = (uint16_t)cpu->a - imm;
    cpu->f &= ~FLAG_AC;
    if ((cpu->a & 0x0F) < (imm & 0x0F)) cpu->f |= FLAG_AC;
    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;
    cpu->a = (uint8_t)result;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += 7;
}
static void op_0xDE(i8080 *cpu, uint8_t op) {  // SBI d8
    (void)op;
    uint8_t imm = read_byte(cpu, cpu->pc++);
    uint8_t cy  = (cpu->f & FLAG_CY) ? 1 : 0;
    uint16_t result = (uint16_t)cpu->a - imm - cy;
    cpu->f &= ~FLAG_AC;
    if ((cpu->a & 0x0F) < ((imm & 0x0F) + cy)) cpu->f |= FLAG_AC;
    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;
    cpu->a = (uint8_t)result;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += 7;
}
static void op_0xE6(i8080 *cpu, uint8_t op) {  // ANI d8
    (void)op;
    uint8_t imm = read_byte(cpu, cpu->pc++);
    // AC = OR of bit 3 of both operands (i8080 quirk)
    if ((cpu->a | imm) & 0x08) cpu->f |= FLAG_AC; else cpu->f &= ~FLAG_AC;
    cpu->a &= imm;
    cpu->f &= ~FLAG_CY;
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += 7;
}
static void op_0xEE(i8080 *cpu, uint8_t op) {  // XRI d8
    (void)op;
    uint8_t imm = read_byte(cpu, cpu->pc++);
    cpu->a ^= imm;
    cpu->f &= ~(FLAG_CY | FLAG_AC);
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += 7;
}
static void op_0xF6(i8080 *cpu, uint8_t op) {  // ORI d8
    (void)op;
    uint8_t imm = read_byte(cpu, cpu->pc++);
    cpu->a |= imm;
    cpu->f &= ~(FLAG_CY | FLAG_AC);
    set_szp_flags(cpu, cpu->a);
    cpu->cycles += 7;
}
static void op_0xFE(i8080 *cpu, uint8_t op) {  // CPI d8 — Compare immediate
    (void)op;
    uint8_t imm = read_byte(cpu, cpu->pc++);
    uint16_t result = (uint16_t)cpu->a - imm;
    cpu->f &= ~FLAG_AC;
    if ((cpu->a & 0x0F) < (imm & 0x0F)) cpu->f |= FLAG_AC;
    cpu->f &= ~FLAG_CY;
    if (result & 0x100) cpu->f |= FLAG_CY;
    // A is NOT written — flags only
    set_szp_flags(cpu, (uint8_t)result);
    cpu->cycles += 7;
}

// RST (restart vectors)

static void op_rst(i8080 *cpu, uint8_t op) {   // RST n  (0xC7/CF/D7/DF/E7/EF/F7/FF)
    uint8_t vec = op & 0x38;                    // bits 5-3 give the vector address
    stack_push(cpu, cpu->pc);
    cpu->pc = vec;
    cpu->cycles += 11;
}

// IO

static void op_0xD3(i8080 *cpu, uint8_t op) {  // OUT d8
    (void)op;
    uint8_t port = read_byte(cpu, cpu->pc++);
    io_out(port, cpu->a);
    cpu->cycles += 10;
}
static void op_0xDB(i8080 *cpu, uint8_t op) {  // IN d8
    (void)op;
    uint8_t port = read_byte(cpu, cpu->pc++);
    cpu->a = io_in(port);
    cpu->cycles += 10;
}

// Interrupt control

static void op_0xF3(i8080 *cpu, uint8_t op) {  // DI — Disable Interrupts
    (void)op;
    cpu->interrupts_enabled = 0;
    cpu->cycles += 4;
}
static void op_0xFB(i8080 *cpu, uint8_t op) {  // EI — Enable Interrupts
    (void)op;
    cpu->interrupts_enabled = 1;
    cpu->cycles += 4;
}

// Micsc

static void op_0xE3(i8080 *cpu, uint8_t op) {  // XTHL — Exchange HL with (SP)
    (void)op;
    uint8_t lo = read_byte(cpu, cpu->sp);
    uint8_t hi = read_byte(cpu, cpu->sp + 1);
    write_byte(cpu, cpu->sp,     cpu->l);
    write_byte(cpu, cpu->sp + 1, cpu->h);
    cpu->l = lo;
    cpu->h = hi;
    cpu->cycles += 18;
}
static void op_0xEB(i8080 *cpu, uint8_t op) {  // XCHG — Exchange DE and HL
    (void)op;
    uint8_t tmp;
    tmp = cpu->h; cpu->h = cpu->d; cpu->d = tmp;
    tmp = cpu->l; cpu->l = cpu->e; cpu->e = tmp;
    cpu->cycles += 5;
}
static void op_0xF9(i8080 *cpu, uint8_t op) {  // SPHL — Move HL to SP
    (void)op;
    cpu->sp = get_hl(cpu);
    cpu->cycles += 5;
}


// Initialise opcode table
static void init_opcode_table(void) {
    // Default everything to unimplemented
    for (int i = 0; i < 256; i++) {
        opcode_table[i] = op_unimplemented;
    }

    // Assign implemented opcodes
    opcode_table[0x00] = op_0x00;
    opcode_table[0x01] = op_0x01;
    opcode_table[0x02] = op_0x02;
    opcode_table[0x03] = op_0x03;
    opcode_table[0x04] = op_0x04;
    opcode_table[0x05] = op_0x05;
    opcode_table[0x06] = op_0x06;
    opcode_table[0x07] = op_0x07;
    opcode_table[0x09] = op_0x09;
    opcode_table[0x0a] = op_0x0a;
    opcode_table[0x0b] = op_0x0b;
    opcode_table[0x0c] = op_0x0c;
    opcode_table[0x0d] = op_0x0d;
    opcode_table[0x0e] = op_0x0e;
    opcode_table[0x0f] = op_0x0f;

    opcode_table[0x11] = op_0x11;
    opcode_table[0x12] = op_0x12;
    opcode_table[0x13] = op_0x13;
    opcode_table[0x14] = op_0x14;
    opcode_table[0x15] = op_0x15;
    opcode_table[0x16] = op_0x16;
    opcode_table[0x17] = op_0x17;
    opcode_table[0x19] = op_0x19;
    opcode_table[0x1a] = op_0x1a;
    opcode_table[0x1b] = op_0x1b;
    opcode_table[0x1c] = op_0x1c;
    opcode_table[0x1d] = op_0x1d;
    opcode_table[0x1e] = op_0x1e;
    opcode_table[0x1f] = op_0x1f;

    opcode_table[0x21] = op_0x21;
    opcode_table[0x22] = op_0x22;
    opcode_table[0x23] = op_0x23;
    opcode_table[0x24] = op_0x24;
    opcode_table[0x25] = op_0x25;
    opcode_table[0x26] = op_0x26;
    opcode_table[0x27] = op_0x27;
    opcode_table[0x29] = op_0x29;
    opcode_table[0x2a] = op_0x2a;
    opcode_table[0x2b] = op_0x2b;
    opcode_table[0x2c] = op_0x2c;
    opcode_table[0x2d] = op_0x2d;
    opcode_table[0x2e] = op_0x2e;
    opcode_table[0x2f] = op_0x2f;

    opcode_table[0x31] = op_0x31;
    opcode_table[0x32] = op_0x32;
    opcode_table[0x33] = op_0x33;
    opcode_table[0x34] = op_0x34;
    opcode_table[0x35] = op_0x35;
    opcode_table[0x36] = op_0x36;
    opcode_table[0x37] = op_0x37;
    opcode_table[0x39] = op_0x39;
    opcode_table[0x3a] = op_0x3a;
    opcode_table[0x3b] = op_0x3b;
    opcode_table[0x3c] = op_0x3c;
    opcode_table[0x3d] = op_0x3d;
    opcode_table[0x3e] = op_0x3e;
    opcode_table[0x3f] = op_0x3f;

    // MOV r1, r2 (0x40-0x7F, 0x76 is HLT done below)
    for (int i = 0x40; i <= 0x7F; i++)
        opcode_table[i] = op_mov;

    opcode_table[0x76] = op_0x76;

    // ALU group (0x80-0xBF)
    for (int i = 0x80; i <= 0x87; i++) opcode_table[i] = op_add;
    for (int i = 0x88; i <= 0x8F; i++) opcode_table[i] = op_adc;
    for (int i = 0x90; i <= 0x97; i++) opcode_table[i] = op_sub;
    for (int i = 0x98; i <= 0x9F; i++) opcode_table[i] = op_sbb;
    for (int i = 0xA0; i <= 0xA7; i++) opcode_table[i] = op_ana;
    for (int i = 0xA8; i <= 0xAF; i++) opcode_table[i] = op_xra;
    for (int i = 0xB0; i <= 0xB7; i++) opcode_table[i] = op_ora;
    for (int i = 0xB8; i <= 0xBF; i++) opcode_table[i] = op_cmp;

    // 0xC0–0xFF
    // Conditions returns
    opcode_table[0xC0] = op_0xC0;
    opcode_table[0xC8] = op_0xC8;
    opcode_table[0xC9] = op_0xC9;
    opcode_table[0xD0] = op_0xD0;
    opcode_table[0xD8] = op_0xD8;
    opcode_table[0xE0] = op_0xE0;
    opcode_table[0xE8] = op_0xE8;
    opcode_table[0xF0] = op_0xF0;
    opcode_table[0xF8] = op_0xF8;

    // POP
    opcode_table[0xC1] = op_0xC1;
    opcode_table[0xD1] = op_0xD1;
    opcode_table[0xE1] = op_0xE1;
    opcode_table[0xF1] = op_0xF1;

    // Conditions jumps
    opcode_table[0xC2] = op_0xC2;
    opcode_table[0xC3] = op_0xC3;
    opcode_table[0xCA] = op_0xCA;
    opcode_table[0xD2] = op_0xD2;
    opcode_table[0xDA] = op_0xDA;
    opcode_table[0xE2] = op_0xE2;
    opcode_table[0xE9] = op_0xE9;
    opcode_table[0xEA] = op_0xEA;
    opcode_table[0xF2] = op_0xF2;
    opcode_table[0xFA] = op_0xFA;

    // Conditios calls
    opcode_table[0xC4] = op_0xC4;
    opcode_table[0xCC] = op_0xCC;
    opcode_table[0xCD] = op_0xCD;
    opcode_table[0xD4] = op_0xD4;
    opcode_table[0xDC] = op_0xDC;
    opcode_table[0xE4] = op_0xE4;
    opcode_table[0xEC] = op_0xEC;
    opcode_table[0xF4] = op_0xF4;
    opcode_table[0xFC] = op_0xFC;

    // PUSH
    opcode_table[0xC5] = op_0xC5;
    opcode_table[0xD5] = op_0xD5;
    opcode_table[0xE5] = op_0xE5;
    opcode_table[0xF5] = op_0xF5;

    // Immediate ALU
    opcode_table[0xC6] = op_0xC6;
    opcode_table[0xCE] = op_0xCE;
    opcode_table[0xD6] = op_0xD6;
    opcode_table[0xDE] = op_0xDE;
    opcode_table[0xE6] = op_0xE6;
    opcode_table[0xEE] = op_0xEE;
    opcode_table[0xF6] = op_0xF6;
    opcode_table[0xFE] = op_0xFE;

    // RST vectors
    opcode_table[0xC7] = op_rst;
    opcode_table[0xCF] = op_rst;
    opcode_table[0xD7] = op_rst;
    opcode_table[0xDF] = op_rst;
    opcode_table[0xE7] = op_rst;
    opcode_table[0xEF] = op_rst;
    opcode_table[0xF7] = op_rst;
    opcode_table[0xFF] = op_rst;

    // I/O
    opcode_table[0xD3] = op_0xD3;
    opcode_table[0xDB] = op_0xDB;

    // Interrupt control
    opcode_table[0xF3] = op_0xF3;
    opcode_table[0xFB] = op_0xFB;

    // Misc
    opcode_table[0xE3] = op_0xE3;
    opcode_table[0xEB] = op_0xEB;
    opcode_table[0xF9] = op_0xF9;
}


// Execute
void execute(i8080 *cpu, uint8_t opcode) {
    opcode_table[opcode](cpu, opcode);
}


// FDE cycle
void emulate(i8080 *cpu) {
    while (!cpu->halted) {

        // Deliver pending interrupt
        if (cpu->interrupts_enabled && cpu->interrupt_pending >= 0) {
            cpu->interrupts_enabled = 0;
            cpu->halted = 0;
            stack_push(cpu, cpu->pc);
            cpu->pc = (uint16_t)cpu->interrupt_pending;
            cpu->interrupt_pending = -1;
            cpu->cycles += 11;
            continue;
        }

        uint16_t pc_before = cpu->pc;

        uint8_t opcode = read_byte(cpu, cpu->pc);
        cpu->pc++;

        execute(cpu, opcode);

        (void)pc_before;
    }
}


// Initialise CPU
void init_cpu(i8080 *cpu) {
    memset(cpu, 0, sizeof(*cpu));
    cpu->sp = 0xFFFF;
    cpu->pc = 0x0000;
    cpu->interrupt_pending = -1;
}


// Program loader
void load_program(i8080 *cpu, const char *filename, uint16_t offset) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("fopen");
        exit(1);
    }

    size_t max = 65536 - offset;
    size_t n = fread(&cpu->memory[offset], 1, max, f);

    if (n == 0) {
        fprintf(stderr, "Warning: program loaded 0 bytes\n");
    }

    fclose(f);
}


// Main
int main(int argc, char **argv) {
    i8080 cpu;
    init_cpu(&cpu);

    // Initialize opcode table
    init_opcode_table();

    // Initialize IO tables
    init_io_tables();

    if (argc > 1) {
        load_program(&cpu, argv[1], 0x0000);
    } else {
        cpu.memory[0x0000] = 0x00;
        cpu.memory[0x0001] = 0x00;
        cpu.memory[0x0002] = 0x76;
    }

    emulate(&cpu);

    printf("Done. PC=0x%04X Cycles=%d\n", cpu.pc, cpu.cycles);
    return 0;
}
