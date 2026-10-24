/* The simulator's Z80 core in C.
 *
 * A transliteration of tests/z80sim.py's Z80.step with the same
 * semantics: the same opcode chain and order, the same flag results
 * (including the model's approximations), block instructions as one
 * step costed up front, the same T-state tables (handed in by Python),
 * IM2 interrupts, HALT and the EI delay, the MEM_DECODER revisions 12
 * and 13, the latch port and the CTC.  Every other port, and the V9958
 * frame clock, call back into Python.  tests/simcore/core.py is the
 * ctypes side; struct layouts here and there must match.
 */

#ifndef Z80CORE_H
#define Z80CORE_H

#include <stdint.h>

/* An error ends the current step at the point it happened. */
enum {
	CORE_OK = 0,
	CORE_PYERR = 1,     /* a Python callback raised; Python re-raises */
	CORE_EXIT = 2,      /* SystemExit(msg) */
	CORE_KEYERR = 3,    /* KeyError(arg): a missing ROM page */
	CORE_INDEXERR = 4   /* IndexError: an access past a bank's end */
};

typedef int (*core_io_in_fn)(int port);             /* 0..255, <0 error */
typedef int (*core_io_out_fn)(int port, int value); /* 0, <0 error */
typedef int (*core_clock_fn)(int64_t tstates);      /* 0, <0 error */
typedef int (*core_accept_fn)(void);                /* 0, <0 error */

struct core_ctc_channel {
	int32_t control_follows;
	int32_t counter_mode;
	int32_t prescaler;
	int32_t interrupt_enabled;
	int32_t constant;
	int32_t down;
	int32_t running;
	int32_t requested;
	int64_t remaining;
};

struct core_ctc {
	int32_t vector_base;
	int32_t pad;
	struct core_ctc_channel ch[4];
	int64_t delivered[4];
	int64_t expiries[4];
};

struct core {
	/* registers */
	int32_t a, f, b, c, d, e, h, l;
	int32_t alt_a, alt_f, alt_b, alt_c, alt_d, alt_e, alt_h, alt_l;
	int32_t pc, sp, ix, iy, i, im;
	int32_t iff1, iff2, halted, int_pending, int_vector, ei_delay;
	int32_t latch, revision;
	int64_t cycles, tstates;
	/* memory: SRAM banks 0-7 and ROM pages 0-7 (NULL: absent) */
	uint8_t *bank[8];
	int64_t bank_len[8];
	uint8_t *rom[8];
	int64_t rom_len[8];
	/* T-state tables */
	int32_t one_t[256];
	int32_t ed_t[256];
	int32_t dd_t[256];
	int32_t ed_block[256];
	int32_t interrupt_tstates;
	/* attached models */
	struct core_ctc *ctc;       /* NULL: no CTC */
	int32_t py_clock;           /* nonzero: call `clock` every _clock */
	int32_t pad0;
	core_io_in_fn io_in;
	core_io_out_fn io_out;
	core_clock_fn clock;
	/* the pending error */
	int32_t err;
	int32_t err_arg;
	char msg[96];
	/* run stops after a step: an OUT to this port (< 0: none) */
	int32_t stop_out_port;
	int32_t out_hit;
	/* nonzero: an instance overrides _accept_interrupt; call `accept`
	 * (which reaches core_accept) instead of accepting directly */
	int32_t py_accept;
	int32_t pad1;
	core_accept_fn accept;
};

int core_step(struct core *c);
/* Z80._accept_interrupt: take the pending interrupt now. */
int core_accept(struct core *c);
void core_clock(struct core *c, int64_t tstates);
int core_bank_of(struct core *c, int addr, int *bank);
int core_rd(struct core *c, int addr);

/* Run up to `limit` iterations of: [stop check] [delay skip] step.
 * `stops` is a 65536-bit map (NULL: none), skipped on the first
 * iteration when `check_first` is zero.  `delay_addr` < 0 disables the
 * serial-connect skip.  After a step, the run also stops when the step
 * wrote c->stop_out_port, or when `halt` is set and the CPU is halted.
 * Returns 0 (limit), 1 (a PC stop, before the iteration), 2 (an OUT
 * stop) or 3 (a halt stop), both after it, or -1 (an error, c->err
 * set); *done is the iterations completed. */
int core_run(struct core *c, int64_t limit, const uint8_t *stops,
	     int check_first, int delay_addr, int delay_latch, int halt,
	     int64_t *done);

#endif
