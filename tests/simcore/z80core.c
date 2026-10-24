/* The simulator's Z80 core in C: see z80core.h.
 *
 * Every branch below mirrors a branch of tests/z80sim.py in the same
 * order (Z80.step, step_ed, step_cb, step_prefix, alu, _logic, ...).
 * Where the model differs from silicon (approximated flags, XOR A
 * leaving F alone, unsupported encodings) this file copies the model.
 * A failed callback or access sets c->err and the step returns at once
 * (CK), as the Python core stops at the raise.
 */

#include <stdio.h>
#include <string.h>

#include "z80core.h"

#define CK do { if (c->err) return; } while (0)
#define CKV(v) do { if (c->err) return (v); } while (0)

static void core_exit(struct core *c, const char *msg)
{
	if (c->err) return;
	c->err = CORE_EXIT;
	snprintf(c->msg, sizeof(c->msg), "%s", msg);
}

/* ---- flags ---------------------------------------------------------- */

static void sf(struct core *c, int bit, int on)
{
	c->f = on ? (c->f | bit) : (c->f & ~bit);
}

#define CF ((c->f >> 0) & 1)
#define ZF ((c->f >> 6) & 1)
#define SNF ((c->f >> 7) & 1)
#define PV ((c->f >> 2) & 1)
#define SET_CF(v) sf(c, 0x01, (v) != 0)
#define SET_ZF(v) sf(c, 0x40, (v) != 0)
#define SET_SNF(v) sf(c, 0x80, (v) != 0)
#define SET_PV(v) sf(c, 0x04, (v) != 0)

static int popcount8(int v)
{
	int n = 0;

	v &= 0xFF;
	while (v) {
		n += v & 1;
		v >>= 1;
	}
	return n;
}

/* ---- memory --------------------------------------------------------- */

static int decoder_bank(struct core *c, int addr, int latch)
{
	int a13 = (addr >> 13) & 1, a14 = (addr >> 14) & 1;
	int a15 = (addr >> 15) & 1;
	int m0 = (latch >> 3) & 1, m1 = (latch >> 4) & 1;
	int q0 = latch & 1, q1 = (latch >> 1) & 1, q2 = (latch >> 2) & 1;
	int p0 = (latch >> 5) & 1, p1 = (latch >> 6) & 1;
	int p2 = (latch >> 7) & 1;
	int a16, a17, a18;
	int common_8k, os_body, window, force_bank0, force_b7;

	if (c->revision == 12) {
		common_8k = a15 & a14 & a13;
		os_body = (a13 & (1 - a14)) | (a14 & (1 - a15)) |
			  (a15 & (1 - a13));
		force_bank0 = m1 & common_8k;
		force_b7 = m1 & m0 & os_body;
		a16 = (q0 & (1 - force_bank0)) | force_b7;
		a17 = (q1 & (1 - force_bank0)) | force_b7;
		a18 = (q2 & (1 - force_bank0)) | force_b7;
		return (a18 << 2) | (a17 << 1) | a16;
	}
	if (m1 && !m0 && a15 && !a14 && (p0 | p1 | p2)) {
		a16 = 1 - p0;
		a17 = 1 - p1;
		a18 = 1 - p2;
		return (a18 << 2) | (a17 << 1) | a16;
	}
	common_8k = a15 & a14 & a13;
	os_body = (a13 & (1 - a14)) | (a14 & (1 - a15)) | (a15 & (1 - a13));
	window = m1 & m0 & (((1 - a15) & a14 & a13) |
			    (a15 & (1 - (a14 & a13))));
	force_bank0 = m1 & common_8k;
	force_b7 = m1 & m0 & os_body & (1 - window);
	a16 = (q0 & (1 - force_bank0) & (1 - window)) | force_b7 |
	      ((1 - p0) & window);
	a17 = (q1 & (1 - force_bank0) & (1 - window)) | force_b7 |
	      ((1 - p1) & window);
	a18 = (q2 & (1 - force_bank0) & (1 - window)) | force_b7 |
	      ((1 - p2) & window);
	return (a18 << 2) | (a17 << 1) | a16;
}

/* Z80.bank_of: 0 and the bank, or nonzero after a SystemExit. */
int core_bank_of(struct core *c, int addr, int *bank)
{
	int mode = c->latch & 0x18;

	if (addr >= 0xE000) {
		*bank = 0;
		return 0;
	}
	if (mode == 0x10 || mode == 0x18) {
		*bank = decoder_bank(c, addr, c->latch);
		return 0;
	}
	{
		char msg[64];

		snprintf(msg, sizeof(msg), "sim: bank_of unsupported mode %02Xh",
			 mode);
		core_exit(c, msg);
	}
	return 1;
}

static uint8_t *sram_cell(struct core *c, int bank, int addr)
{
	if (c->bank[bank] == NULL || addr >= c->bank_len[bank]) {
		if (!c->err) {
			c->err = CORE_INDEXERR;
			c->err_arg = addr;
		}
		return NULL;
	}
	return &c->bank[bank][addr];
}

int core_rd(struct core *c, int addr)
{
	int mode = c->latch & 0x18;
	int bank;
	uint8_t *cell;

	addr &= 0xFFFF;
	if (mode == 0x00) {
		int page = (c->latch >> 5) & 0x07;

		if (c->rom[page] == NULL) {
			if (!c->err) {
				c->err = CORE_KEYERR;
				c->err_arg = page;
			}
			return 0;
		}
		if (addr >= c->rom_len[page]) {
			if (!c->err) {
				c->err = CORE_INDEXERR;
				c->err_arg = addr;
			}
			return 0;
		}
		return c->rom[page][addr];
	}
	if (mode == 0x08) {
		cell = sram_cell(c, c->latch & 0x07, addr);
		return cell ? *cell : 0;
	}
	if (core_bank_of(c, addr, &bank)) return 0;
	cell = sram_cell(c, bank, addr);
	return cell ? *cell : 0;
}

static int rd(struct core *c, int addr)
{
	return core_rd(c, addr);
}

static int rd16(struct core *c, int addr)
{
	int lo = rd(c, addr);

	return lo | (rd(c, (addr + 1) & 0xFFFF) << 8);
}

static void wr(struct core *c, int addr, int v)
{
	int mode = c->latch & 0x18;
	int bank;
	uint8_t *cell;

	addr &= 0xFFFF;
	if (mode == 0x00 || mode == 0x08) {
		cell = sram_cell(c, c->latch & 0x07, addr);
		if (cell) *cell = (uint8_t)(v & 0xFF);
		return;
	}
	if (core_bank_of(c, addr, &bank)) return;
	cell = sram_cell(c, bank, addr);
	if (cell) *cell = (uint8_t)(v & 0xFF);
}

static void wr16(struct core *c, int addr, int v)
{
	wr(c, addr, v & 0xFF);
	CK;
	wr(c, (addr + 1) & 0xFFFF, (v >> 8) & 0xFF);
}

static void push16(struct core *c, int v)
{
	c->sp = (c->sp - 2) & 0xFFFF;
	wr16(c, c->sp, v);
}

static int pop16(struct core *c)
{
	int v = rd16(c, c->sp);

	c->sp = (c->sp + 2) & 0xFFFF;
	return v;
}

static int fetch(struct core *c)
{
	int op = rd(c, c->pc);

	c->pc = (c->pc + 1) & 0xFFFF;
	return op;
}

static int fetch16(struct core *c)
{
	int lo = fetch(c);
	int hi = fetch(c);

	return lo | (hi << 8);
}

static int signed8(int v)
{
	return (v & 0x80) ? v - 256 : v;
}

static void set_rr(struct core *c, int code, int v)
{
	if (code == 0) { c->b = (v >> 8) & 0xFF; c->c = v & 0xFF; }
	else if (code == 1) { c->d = (v >> 8) & 0xFF; c->e = v & 0xFF; }
	else if (code == 2) { c->h = (v >> 8) & 0xFF; c->l = v & 0xFF; }
	else c->sp = v;
}

static int get_rr(struct core *c, int code)
{
	if (code == 0) return (c->b << 8) | c->c;
	if (code == 1) return (c->d << 8) | c->e;
	if (code == 2) return (c->h << 8) | c->l;
	return c->sp;
}

#define HL ((c->h << 8) | c->l)

static void set_hl(struct core *c, int v)
{
	c->h = (v >> 8) & 0xFF;
	c->l = v & 0xFF;
}

/* The register file by r code (0-7, 6 is (HL) and handled by callers). */
static int32_t *reg8(struct core *c, int code)
{
	switch (code) {
	case 0: return &c->b;
	case 1: return &c->c;
	case 2: return &c->d;
	case 3: return &c->e;
	case 4: return &c->h;
	case 5: return &c->l;
	default: return &c->a;
	}
}

static int read_r(struct core *c, int code)
{
	if (code == 6) return rd(c, HL);
	return *reg8(c, code);
}

static void write_r(struct core *c, int code, int value)
{
	value &= 0xFF;
	if (code == 6) wr(c, HL, value);
	else *reg8(c, code) = value;
}

static int parity(int value)
{
	return (popcount8(value) % 2) ? 0 : 1;
}

/* ---- I/O ------------------------------------------------------------ */

static int io_in(struct core *c, int port)
{
	int v = c->io_in(port & 0xFF);

	if (v < 0) {
		if (!c->err) c->err = CORE_PYERR;
		return 0;
	}
	return v;
}

static void io_out(struct core *c, int port, int value)
{
	if ((port & 0xFF) == c->stop_out_port) c->out_hit = 1;
	if (c->io_out(port & 0xFF, value & 0xFF) < 0 && !c->err)
		c->err = CORE_PYERR;
}

/* ---- the clock and the CTC ------------------------------------------ */

static const int64_t ctc_trigger_hz[4] = {1843200, 3686400, 7372800,
					  7372800};
#define CTC_CLOCK_HZ 10000000

static void assert_int(struct core *c, int vector)
{
	c->int_vector = vector & 0xFF;
	c->int_pending = 1;
}

static void ctc_advance(struct core *c, int64_t tstates)
{
	struct core_ctc *ctc = c->ctc;
	int i;

	for (i = 0; i < 4; ++i) {
		struct core_ctc_channel *ch = &ctc->ch[i];
		int64_t divisor;

		if (!ch->running) continue;
		if (ch->counter_mode) {
			ch->remaining += tstates * ctc_trigger_hz[i];
			divisor = CTC_CLOCK_HZ;
		} else {
			ch->remaining += tstates;
			divisor = ch->prescaler;
		}
		while (ch->remaining >= divisor) {
			ch->remaining -= divisor;
			ch->down -= 1;
			if (ch->down == 0) {
				ch->down = ch->constant;
				ctc->expiries[i] += 1;
				if (ch->interrupt_enabled) ch->requested = 1;
			}
		}
	}
	if (!(c->iff1 && !c->int_pending && c->im == 2 && !c->ei_delay))
		return;
	for (i = 0; i < 4; ++i) {
		if (ctc->ch[i].requested) {
			ctc->ch[i].requested = 0;
			ctc->delivered[i] += 1;
			assert_int(c, (ctc->vector_base & 0xF8) |
				      ((i << 1) & 0x07));
			return;
		}
	}
}

void core_clock(struct core *c, int64_t tstates)
{
	c->tstates += tstates;
	if (c->ctc != NULL) ctc_advance(c, tstates);
	if (c->py_clock && c->clock(tstates) < 0 && !c->err)
		c->err = CORE_PYERR;
}

/* opcode_cost: the T-states of the instruction at PC. */
static int64_t opcode_cost(struct core *c)
{
	int op = rd(c, c->pc);

	if (op == 0xED) {
		int second = rd(c, (c->pc + 1) & 0xFFFF);

		if (c->ed_block[second]) {
			int64_t count = (c->b << 8) | c->c;

			if (count == 0) count = 0x10000;
			if (second == 0xB2 || second == 0xB3 || second == 0xBA ||
			    second == 0xBB) {
				count = c->b ? c->b : 0x100;
			} else if (second == 0xB1 || second == 0xB9) {
				int step = second == 0xB1 ? 1 : 0xFFFF;
				int hl = HL;
				int64_t n;

				for (n = 1; n <= count; ++n) {
					if (rd(c, hl) == c->a) {
						count = n;
						break;
					}
					hl = (hl + step) & 0xFFFF;
				}
			}
			return 21 * count - 5;
		}
		return c->ed_t[second];
	}
	if (op == 0xCB) {
		int second = rd(c, (c->pc + 1) & 0xFFFF);

		return (second & 7) == 6 ? 12 : 8;
	}
	if (op == 0xDD || op == 0xFD) {
		int second = rd(c, (c->pc + 1) & 0xFFFF);

		if (second == 0xCB) return 23;
		return c->dd_t[second];
	}
	return c->one_t[op];
}

/* ---- interrupts ----------------------------------------------------- */

static void accept_pending(struct core *c)
{
	c->int_pending = 0;
	c->iff1 = 0;
	c->iff2 = 0;
	core_clock(c, c->interrupt_tstates);
	CK;
	push16(c, c->pc);
	CK;
	c->pc = rd16(c, ((c->i << 8) | c->int_vector) & 0xFFFF);
	c->halted = 0;
}

int core_accept(struct core *c)
{
	c->err = CORE_OK;
	accept_pending(c);
	return c->err;
}

/* An acceptance: the instance's override when one is installed. */
static void take_interrupt(struct core *c)
{
	if (c->py_accept) {
		if (c->accept() < 0 && !c->err) c->err = CORE_PYERR;
		return;
	}
	accept_pending(c);
}

static int cond(struct core *c, int cc)
{
	switch (cc) {
	case 0: return !ZF;
	case 1: return ZF;
	case 2: return !CF;
	case 3: return CF;
	case 4: return !PV;
	case 5: return PV;
	case 6: return !SNF;
	default: return SNF;
	}
}

/* ---- arithmetic ----------------------------------------------------- */

#define L_NONE 0
#define L_SUB 1
#define L_ADD 2

static int logic(struct core *c, int raw, int kind, int operand, int left)
{
	int res = raw & 0xFF;

	SET_ZF(res == 0);
	SET_SNF((res & 0x80) != 0);
	if (kind == L_SUB) {
		sf(c, 0x10, ((left ^ operand ^ res) & 0x10) != 0);
		sf(c, 0x02, 1);
		SET_CF(raw < 0);
		SET_PV(((left ^ operand) & (left ^ res) & 0x80) != 0);
	} else if (kind == L_ADD) {
		sf(c, 0x10, ((left ^ operand ^ res) & 0x10) != 0);
		sf(c, 0x02, 0);
		SET_CF(raw > 0xFF);
		SET_PV(((left ^ res) & (operand ^ res) & 0x80) != 0);
	} else {
		sf(c, 0x10, 0);
		sf(c, 0x02, 0);
		SET_CF(0);
		SET_PV((popcount8(res) & 1) == 0);
	}
	return res;
}

static void sub8(struct core *c, int n, int carry)
{
	int left = c->a;

	c->a = logic(c, left - n - carry, L_SUB, n, left);
}

static void add8(struct core *c, int n, int carry)
{
	int left = c->a;

	c->a = logic(c, left + n + carry, L_ADD, n, left);
}

static void inc_flags(struct core *c, int old, int res)
{
	SET_ZF(res == 0);
	SET_SNF((res & 0x80) != 0);
	sf(c, 0x10, (old & 0x0F) == 0x0F);
	sf(c, 0x02, 0);
	SET_PV(res == 0x80);
}

static void dec_flags(struct core *c, int old, int res)
{
	SET_ZF(res == 0);
	SET_SNF((res & 0x80) != 0);
	sf(c, 0x10, (old & 0x0F) == 0x00);
	sf(c, 0x02, 1);
	SET_PV(res == 0x7F);
}

static int add16(struct core *c, int hl, int rr)
{
	int res = hl + rr;

	sf(c, 1, (res & 0x10000) != 0);
	return res & 0xFFFF;
}

static void alu(struct core *c, int op)
{
	int r = op & 7;
	int v;
	int grp = op & 0xF8;

	if (r == 6) {
		v = rd(c, HL);
		CK;
	} else {
		v = *reg8(c, r);
	}
	if (grp == 0x80) add8(c, v, 0);
	else if (grp == 0x88) add8(c, v, CF ? 1 : 0);
	else if (grp == 0x90) sub8(c, v, 0);
	else if (grp == 0x98) sub8(c, v, CF ? 1 : 0);
	else if (grp == 0xA0) c->a = logic(c, c->a & v, L_NONE, 0, 0);
	else if (grp == 0xA8) c->a = logic(c, c->a ^ v, L_NONE, 0, 0);
	else if (grp == 0xB0) c->a = logic(c, c->a | v, L_NONE, 0, 0);
	else if (grp == 0xB8) logic(c, c->a - v, L_SUB, v, c->a);
	else {
		char msg[64];

		snprintf(msg, sizeof(msg), "sim: unsupported alu op %02X", op);
		core_exit(c, msg);
	}
}

/* The CB rotate/shift group: the new value, CF set. */
static int rot(struct core *c, int grp, int value)
{
	int cbit;

	if (grp == 0) {
		cbit = (value >> 7) & 1;
		value = ((value << 1) | cbit) & 0xFF;
	} else if (grp == 1) {
		cbit = value & 1;
		value = ((value >> 1) | (cbit << 7)) & 0xFF;
	} else if (grp == 2) {
		cbit = (value >> 7) & 1;
		value = ((value << 1) | CF) & 0xFF;
	} else if (grp == 3) {
		cbit = value & 1;
		value = ((value >> 1) | (CF << 7)) & 0xFF;
	} else if (grp == 4) {
		cbit = (value >> 7) & 1;
		value = (value << 1) & 0xFF;
	} else if (grp == 5) {
		cbit = value & 1;
		value = (value >> 1) | (value & 0x80);
	} else {
		cbit = value & 1;
		value = value >> 1;
	}
	SET_CF(cbit);
	return value;
}

/* ---- ED ------------------------------------------------------------- */

static void step_ed(struct core *c)
{
	int op = fetch(c);
	int bc, de, value;

	CK;
	if (op == 0xB0) {
		bc = (c->b << 8) | c->c;
		while (bc) {
			value = rd(c, HL);
			CK;
			wr(c, (c->d << 8) | c->e, value);
			CK;
			set_hl(c, (get_rr(c, 2) + 1) & 0xFFFF);
			de = (c->d << 8) | c->e;
			c->d = ((de + 1) >> 8) & 0xFF;
			c->e = (de + 1) & 0xFF;
			bc -= 1;
			c->b = (bc >> 8) & 0xFF;
			c->c = bc & 0xFF;
			c->cycles += 1;
		}
		SET_ZF(0);
	} else if (op == 0xB8) {
		bc = (c->b << 8) | c->c;
		while (bc) {
			value = rd(c, HL);
			CK;
			wr(c, (c->d << 8) | c->e, value);
			CK;
			set_hl(c, (get_rr(c, 2) - 1) & 0xFFFF);
			de = (((c->d << 8) | c->e) - 1) & 0xFFFF;
			c->d = de >> 8;
			c->e = de & 0xFF;
			bc -= 1;
			c->b = (bc >> 8) & 0xFF;
			c->c = bc & 0xFF;
			c->cycles += 1;
		}
		sf(c, 0x10, 0);
		sf(c, 0x02, 0);
		sf(c, 0x04, 0);
	} else if (op == 0xB1) {
		int found = 0;

		bc = (c->b << 8) | c->c;
		for (;;) {
			value = rd(c, HL);
			CK;
			set_hl(c, (get_rr(c, 2) + 1) & 0xFFFF);
			bc = (bc - 1) & 0xFFFF;
			c->cycles += 1;
			if (value == c->a) {
				found = 1;
				break;
			}
			if (bc == 0) break;
		}
		c->b = bc >> 8;
		c->c = bc & 0xFF;
		sf(c, 0x40, found);
		sf(c, 0x04, bc != 0);
		sf(c, 0x02, 1);
	} else if (op == 0xA0) {
		value = rd(c, HL);
		CK;
		wr(c, (c->d << 8) | c->e, value);
		CK;
		set_hl(c, (get_rr(c, 2) + 1) & 0xFFFF);
		de = (((c->d << 8) | c->e) + 1) & 0xFFFF;
		c->d = de >> 8;
		c->e = de & 0xFF;
		bc = (((c->b << 8) | c->c) - 1) & 0xFFFF;
		c->b = bc >> 8;
		c->c = bc & 0xFF;
		sf(c, 0x10, 0);
		sf(c, 0x02, 0);
		sf(c, 0x04, bc != 0);
	} else if (op == 0x7B) {
		int nn = fetch16(c);

		CK;
		c->sp = rd16(c, nn);
	} else if (op == 0x73) {
		int nn = fetch16(c);

		CK;
		wr16(c, nn, c->sp);
	} else if (op == 0x5B) {
		int nn = fetch16(c);

		CK;
		set_rr(c, 1, rd16(c, nn));
	} else if (op == 0x4B) {
		int nn = fetch16(c);

		CK;
		set_rr(c, 0, rd16(c, nn));
	} else if (op == 0x53) {
		int nn = fetch16(c);

		CK;
		wr16(c, nn, (c->d << 8) | c->e);
	} else if (op == 0x43) {
		int nn = fetch16(c);

		CK;
		wr16(c, nn, (c->b << 8) | c->c);
	} else if (op == 0x47) {
		c->i = c->a;
	} else if (op == 0x57) {
		c->a = c->i;
		sf(c, 0x04, c->iff2);
	} else if (op == 0x5F) {
		c->a = (int32_t)(c->cycles & 0x7F);
		sf(c, 0x04, c->iff2);
	} else if (op == 0x46 || op == 0x56 || op == 0x5E) {
		c->im = op == 0x46 ? 0 : op == 0x56 ? 1 : 2;
	} else if (op == 0x4D || op == 0x45) {
		c->pc = pop16(c);
		CK;
		c->iff1 = c->iff2;
	} else if (op == 0x42 || op == 0x52 || op == 0x62 || op == 0x72 ||
		   op == 0x4A || op == 0x5A || op == 0x6A || op == 0x7A) {
		int rr = (op >> 4) & 3;
		int carry = CF ? 1 : 0;
		int res;

		if (op & 0x08) {
			res = HL + get_rr(c, rr) + carry;
			SET_CF(res > 0xFFFF);
		} else {
			res = HL - get_rr(c, rr) - carry;
			SET_CF(res < 0);
		}
		set_hl(c, res & 0xFFFF);
		SET_ZF(HL == 0);
		SET_SNF((HL & 0x8000) != 0);
	} else if (op == 0xA2) {
		value = io_in(c, c->c);
		CK;
		wr(c, HL, value);
		CK;
		set_hl(c, (HL + 1) & 0xFFFF);
		c->b = (c->b - 1) & 0xFF;
		SET_ZF(c->b == 0);
		SET_SNF((value & 0x80) != 0);
		sf(c, 0x02, 1);
		SET_PV(parity(value));
	} else if (op == 0xA3) {
		value = rd(c, HL);
		CK;
		io_out(c, c->c, value);
		CK;
		set_hl(c, (HL + 1) & 0xFFFF);
		c->b = (c->b - 1) & 0xFF;
		SET_ZF(c->b == 0);
		SET_SNF((value & 0x80) != 0);
		sf(c, 0x02, 1);
		SET_PV(parity(value));
	} else if (op == 0xB3) {
		for (;;) {
			value = rd(c, HL);
			CK;
			io_out(c, c->c, value);
			CK;
			set_hl(c, (HL + 1) & 0xFFFF);
			c->b = (c->b - 1) & 0xFF;
			c->cycles += 1;
			if (c->b == 0) break;
		}
		SET_ZF(1);
		SET_SNF(0);
		sf(c, 0x02, 1);
	} else if ((op & 0xC7) == 0x40) {
		int code = (op >> 3) & 7;

		value = io_in(c, c->c);
		CK;
		if (code != 6) {
			write_r(c, code, value);
			CK;
		}
		SET_ZF(value == 0);
		SET_SNF((value & 0x80) != 0);
		SET_PV(parity(value));
	} else if ((op & 0xC7) == 0x41) {
		int code = (op >> 3) & 7;

		if (op == 0x71) {
			value = 0;
		} else {
			value = read_r(c, code);
			CK;
		}
		io_out(c, c->c, value);
	} else if (op == 0x44) {
		int old = c->a;

		c->a = 0;
		sub8(c, old, 0);
	} else {
		char msg[64];

		snprintf(msg, sizeof(msg), "sim: unsupported ED %02X at %04X",
			 op, (c->pc - 2) & 0xFFFF);
		core_exit(c, msg);
	}
}

/* ---- CB ------------------------------------------------------------- */

static void step_cb(struct core *c)
{
	int op = fetch(c);
	int r, mem, value, bit;

	CK;
	r = op & 7;
	mem = r == 6;
	if (mem) {
		value = rd(c, HL);
		CK;
	} else {
		value = *reg8(c, r);
	}
	if (op < 0x40) {
		value = rot(c, (op >> 3) & 7, value);
		if (mem) {
			wr(c, HL, value);
			CK;
		} else {
			*reg8(c, r) = value;
		}
		SET_ZF(value == 0);
		SET_SNF((value & 0x80) != 0);
		SET_PV((popcount8(value) & 1) == 0);
	} else if (op < 0x80) {
		bit = (op >> 3) & 7;
		SET_ZF(!((value >> bit) & 1));
	} else if (op < 0xC0) {
		bit = (op >> 3) & 7;
		value = value & ~(1 << bit) & 0xFF;
		if (mem) wr(c, HL, value);
		else *reg8(c, r) = value;
	} else {
		bit = (op >> 3) & 7;
		value = value | (1 << bit);
		if (mem) wr(c, HL, value);
		else *reg8(c, r) = value;
	}
}

/* ---- DD / FD -------------------------------------------------------- */

static void set_index(struct core *c, int op, int v)
{
	if (op == 0xDD) c->ix = v;
	else c->iy = v;
}

static void step_prefix(struct core *c, int op)
{
	int op2 = fetch(c);
	int reg, off, addr, v;

	CK;
	reg = op == 0xDD ? c->ix : c->iy;
	if (op2 == 0xCB) {
		int op3, bit;

		off = signed8(fetch(c));
		CK;
		addr = (reg + off) & 0xFFFF;
		op3 = fetch(c);
		CK;
		v = rd(c, addr);
		CK;
		if (op3 < 0x40) {
			int r;

			v = rot(c, (op3 >> 3) & 7, v);
			wr(c, addr, v);
			CK;
			r = op3 & 7;
			if (r != 6) *reg8(c, r) = v;
			SET_ZF(v == 0);
			SET_SNF((v & 0x80) != 0);
			SET_PV((popcount8(v) & 1) == 0);
		} else if (op3 < 0x80) {
			bit = (op3 >> 3) & 7;
			SET_ZF(!((v >> bit) & 1));
		} else if (op3 < 0xC0) {
			bit = (op3 >> 3) & 7;
			wr(c, addr, v & ~(1 << bit) & 0xFF);
		} else {
			bit = (op3 >> 3) & 7;
			wr(c, addr, v | (1 << bit));
		}
	} else if (op2 == 0x21) {
		v = fetch16(c);
		CK;
		set_index(c, op, v);
	} else if (op2 == 0x2A) {
		int nn = fetch16(c);

		CK;
		v = rd16(c, nn);
		CK;
		set_index(c, op, v);
	} else if (op2 == 0x22) {
		int nn = fetch16(c);

		CK;
		wr16(c, nn, reg);
	} else if (op2 == 0x23) {
		if (op == 0xDD) c->ix = (c->ix + 1) & 0xFFFF;
		else c->iy = (c->iy + 1) & 0xFFFF;
	} else if (op2 == 0x2B) {
		if (op == 0xDD) c->ix = (c->ix - 1) & 0xFFFF;
		else c->iy = (c->iy - 1) & 0xFFFF;
	} else if (op2 == 0xE5) {
		push16(c, op == 0xDD ? c->ix : c->iy);
	} else if (op2 == 0xE1) {
		v = pop16(c);
		CK;
		set_index(c, op, v);
	} else if (op2 == 0xE9) {
		c->pc = reg;
	} else if (op2 == 0xF9) {
		c->sp = reg;
	} else if (op2 == 0x09 || op2 == 0x19 || op2 == 0x29 || op2 == 0x39) {
		int rr = (op2 >> 4) & 3;
		int res = reg + (rr == 2 ? reg : get_rr(c, rr));

		set_index(c, op, res & 0xFFFF);
		sf(c, 1, (res & 0x10000) != 0);
	} else if (op2 == 0x36) {
		int n;

		off = signed8(fetch(c));
		CK;
		addr = (reg + off) & 0xFFFF;
		n = fetch(c);
		CK;
		wr(c, addr, n);
	} else if (op2 == 0x46 || op2 == 0x4E || op2 == 0x56 || op2 == 0x5E ||
		   op2 == 0x66 || op2 == 0x6E || op2 == 0x7E) {
		off = signed8(fetch(c));
		CK;
		v = rd(c, (reg + off) & 0xFFFF);
		CK;
		*reg8(c, (op2 >> 3) & 7) = v;
	} else if ((op2 >= 0x70 && op2 <= 0x75) || op2 == 0x77) {
		off = signed8(fetch(c));
		CK;
		wr(c, (reg + off) & 0xFFFF, *reg8(c, op2 & 7));
	} else if (op2 == 0x86 || op2 == 0x8E || op2 == 0x96 || op2 == 0x9E ||
		   op2 == 0xA6 || op2 == 0xAE || op2 == 0xB6 || op2 == 0xBE) {
		int grp = op2 & 0xF8;

		off = signed8(fetch(c));
		CK;
		v = rd(c, (reg + off) & 0xFFFF);
		CK;
		if (grp == 0x80) add8(c, v, 0);
		else if (grp == 0x88) add8(c, v, CF ? 1 : 0);
		else if (grp == 0x90) sub8(c, v, 0);
		else if (grp == 0x98) sub8(c, v, CF ? 1 : 0);
		else if (grp == 0xA0) c->a = logic(c, c->a & v, L_NONE, 0, 0);
		else if (grp == 0xA8) c->a = logic(c, c->a ^ v, L_NONE, 0, 0);
		else if (grp == 0xB0) c->a = logic(c, c->a | v, L_NONE, 0, 0);
		else logic(c, c->a - v, L_SUB, 0, 0);
	} else if (op2 == 0x34 || op2 == 0x35) {
		int old;

		off = signed8(fetch(c));
		CK;
		addr = (reg + off) & 0xFFFF;
		old = rd(c, addr);
		CK;
		v = op2 == 0x34 ? (old + 1) & 0xFF : (old - 1) & 0xFF;
		wr(c, addr, v);
		CK;
		if (op2 == 0x34) inc_flags(c, old, v);
		else dec_flags(c, old, v);
	} else {
		char msg[64];

		snprintf(msg, sizeof(msg), "sim: unsupported %02X %02X", op, op2);
		core_exit(c, msg);
	}
}

/* ---- the main step -------------------------------------------------- */

static int is_ld_reg(int op)
{
	if (op >= 0x40 && op <= 0x6F) return (op & 7) <= 5 || (op & 7) == 7;
	if (op >= 0x78 && op <= 0x7D) return 1;
	return 0;
}

static void ld_reg(struct core *c, int op)
{
	if (op >= 0x40 && op <= 0x6F) {
		int src = op & 7;

		if (src == 6) return;
		*reg8(c, (op >> 3) & 7) = *reg8(c, src);
		return;
	}
	c->a = *reg8(c, op & 7);
}

static void step_op(struct core *c, int op)
{
	int v, off, nn, addr, old;

	if (op == 0x00) {
	} else if (op == 0x07) {
		v = (c->a >> 7) & 1;
		c->a = ((c->a << 1) | v) & 0xFF;
		SET_CF(v);
	} else if (op == 0x0F) {
		v = c->a & 1;
		c->a = ((c->a >> 1) | (v << 7)) & 0xFF;
		SET_CF(v);
	} else if (op == 0x17) {
		v = (c->a >> 7) & 1;
		c->a = ((c->a << 1) | (CF ? 1 : 0)) & 0xFF;
		SET_CF(v);
	} else if (op == 0x1F) {
		v = c->a & 1;
		c->a = ((c->a >> 1) | ((CF ? 1 : 0) << 7)) & 0xFF;
		SET_CF(v);
	} else if (op == 0x76) {
		c->halted = 1;
	} else if (op == 0xF3) {
		c->iff1 = c->iff2 = 0;
	} else if (op == 0xFB) {
		c->iff1 = c->iff2 = 1;
		c->ei_delay = 2;
	} else if (op == 0x08) {
		int t;

		t = c->a; c->a = c->alt_a; c->alt_a = t;
		t = c->f; c->f = c->alt_f; c->alt_f = t;
	} else if (op == 0x10) {
		off = signed8(fetch(c));
		CK;
		c->b = (c->b - 1) & 0xFF;
		if (c->b) c->pc = (c->pc + off) & 0xFFFF;
	} else if (op == 0xD9) {
		int t;

		t = c->b; c->b = c->alt_b; c->alt_b = t;
		t = c->c; c->c = c->alt_c; c->alt_c = t;
		t = c->d; c->d = c->alt_d; c->alt_d = t;
		t = c->e; c->e = c->alt_e; c->alt_e = t;
		t = c->h; c->h = c->alt_h; c->alt_h = t;
		t = c->l; c->l = c->alt_l; c->alt_l = t;
	} else if (op == 0xF9) {
		c->sp = HL;
	} else if (op == 0xC9) {
		c->pc = pop16(c);
	} else if (op == 0xC3) {
		c->pc = fetch16(c);
	} else if (op == 0xE9) {
		c->pc = HL;
	} else if (op == 0x18 || op == 0x28 || op == 0x20 || op == 0x38 ||
		   op == 0x30) {
		off = signed8(fetch(c));
		CK;
		if (op == 0x18 || (op == 0x28 && ZF) || (op == 0x20 && !ZF) ||
		    (op == 0x38 && CF) || (op == 0x30 && !CF))
			c->pc = (c->pc + off) & 0xFFFF;
	} else if (op == 0xC2 || op == 0xCA || op == 0xD2 || op == 0xDA ||
		   op == 0xE2 || op == 0xEA || op == 0xF2 || op == 0xFA) {
		int take;

		nn = fetch16(c);
		CK;
		switch (op) {
		case 0xC2: take = !ZF; break;
		case 0xCA: take = ZF; break;
		case 0xD2: take = !CF; break;
		case 0xDA: take = CF; break;
		case 0xE2: take = !PV; break;
		case 0xEA: take = PV; break;
		case 0xF2: take = !SNF; break;
		default: take = SNF; break;
		}
		if (take) c->pc = nn;
	} else if (op == 0xC4 || op == 0xCC || op == 0xD4 || op == 0xDC ||
		   op == 0xE4 || op == 0xEC || op == 0xF4 || op == 0xFC) {
		nn = fetch16(c);
		CK;
		if (cond(c, (op >> 3) & 7)) {
			push16(c, c->pc);
			CK;
			c->pc = nn;
		}
	} else if (op == 0xC0 || op == 0xC8 || op == 0xD0 || op == 0xD8 ||
		   op == 0xE0 || op == 0xE8 || op == 0xF0 || op == 0xF8) {
		if (cond(c, (op >> 3) & 7)) c->pc = pop16(c);
	} else if (op == 0xCD) {
		nn = fetch16(c);
		CK;
		push16(c, c->pc);
		CK;
		c->pc = nn;
	} else if (op == 0xC7 || (op >= 0xC7 && (op & 0xC7) == 0xC7)) {
		push16(c, c->pc);
		CK;
		c->pc = op & 0x38;
	} else if (op == 0xC5 || op == 0xD5 || op == 0xE5 || op == 0xF5) {
		v = op == 0xC5 ? (c->b << 8) | c->c :
		    op == 0xD5 ? (c->d << 8) | c->e :
		    op == 0xE5 ? (c->h << 8) | c->l : (c->a << 8) | c->f;
		push16(c, v);
	} else if (op == 0xC1 || op == 0xD1 || op == 0xE1 || op == 0xF1) {
		v = pop16(c);
		CK;
		if (op == 0xC1) { c->b = v >> 8; c->c = v & 0xFF; }
		else if (op == 0xD1) { c->d = v >> 8; c->e = v & 0xFF; }
		else if (op == 0xE1) { c->h = v >> 8; c->l = v & 0xFF; }
		else { c->a = v >> 8; c->f = v & 0xFF; }
	} else if (op == 0xDD || op == 0xFD) {
		step_prefix(c, op);
	} else if (op == 0xCB) {
		step_cb(c);
	} else if (op == 0xD3 || op == 0xDB) {
		int port = fetch(c);

		CK;
		if (op == 0xD3) {
			io_out(c, port, c->a);
		} else {
			v = io_in(c, port);
			CK;
			c->a = v;
		}
	} else if (op == 0x01 || op == 0x11 || op == 0x21 || op == 0x31) {
		nn = fetch16(c);
		CK;
		set_rr(c, (op >> 4) & 3, nn);
	} else if (op == 0x09) {
		set_hl(c, add16(c, get_rr(c, 2), get_rr(c, 0)));
	} else if (op == 0x19) {
		set_hl(c, add16(c, get_rr(c, 2), get_rr(c, 1)));
	} else if (op == 0x29) {
		set_hl(c, add16(c, get_rr(c, 2), get_rr(c, 2)));
	} else if (op == 0x22) {
		nn = fetch16(c);
		CK;
		wr16(c, nn, HL);
	} else if (op == 0x2A) {
		nn = fetch16(c);
		CK;
		v = rd16(c, nn);
		CK;
		set_hl(c, v);
	} else if (op == 0x32) {
		nn = fetch16(c);
		CK;
		wr(c, nn, c->a);
	} else if (op == 0x3A) {
		nn = fetch16(c);
		CK;
		v = rd(c, nn);
		CK;
		c->a = v;
	} else if (op == 0x36) {
		addr = HL;
		v = fetch(c);
		CK;
		wr(c, addr, v);
	} else if (op == 0x34 || op == 0x35) {
		addr = HL;
		old = rd(c, addr);
		CK;
		v = op == 0x34 ? (old + 1) & 0xFF : (old - 1) & 0xFF;
		wr(c, addr, v);
		CK;
		if (op == 0x34) inc_flags(c, old, v);
		else dec_flags(c, old, v);
	} else if (op == 0x39) {
		int res = HL + c->sp;

		set_hl(c, res);
		sf(c, 1, (res & 0x10000) != 0);
	} else if (is_ld_reg(op)) {
		ld_reg(c, op);
	} else if (op == 0x46 || op == 0x4E || op == 0x56 || op == 0x5E ||
		   op == 0x66 || op == 0x6E) {
		v = rd(c, HL);
		CK;
		*reg8(c, (op >> 3) & 7) = v;
	} else if ((op >= 0x70 && op <= 0x75) || op == 0x77) {
		wr(c, HL, *reg8(c, op & 7));
	} else if (op == 0x7E) {
		v = rd(c, HL);
		CK;
		c->a = v;
	} else if (op == 0x02) {
		wr(c, (c->b << 8) | c->c, c->a);
	} else if (op == 0x0A) {
		v = rd(c, (c->b << 8) | c->c);
		CK;
		c->a = v;
	} else if (op == 0x12) {
		wr(c, (c->d << 8) | c->e, c->a);
	} else if (op == 0x1A) {
		v = rd(c, (c->d << 8) | c->e);
		CK;
		c->a = v;
	} else if (op == 0x3E) {
		v = fetch(c);
		CK;
		c->a = v;
	} else if (op == 0x06 || op == 0x0E || op == 0x16 || op == 0x1E ||
		   op == 0x26 || op == 0x2E) {
		v = fetch(c);
		CK;
		*reg8(c, (op >> 3) & 7) = v;
	} else if (op == 0x2F) {
		c->a ^= 0xFF;
	} else if (op == 0x37) {
		SET_CF(1);
	} else if (op == 0x3F) {
		SET_CF(CF ^ 1);
	} else if (op == 0xAF) {
		c->a = 0;
	} else if (op == 0xA7) {
		logic(c, c->a & c->a, L_NONE, 0, 0);
	} else if (op == 0xB7) {
		logic(c, c->a | c->a, L_NONE, 0, 0);
	} else if (op >= 0x80 && op <= 0xBF) {
		alu(c, op);
	} else if (op == 0xE6 || op == 0xEE || op == 0xF6) {
		v = fetch(c);
		CK;
		if (op == 0xE6) c->a = logic(c, c->a & v, L_NONE, 0, 0);
		else if (op == 0xEE) c->a = logic(c, c->a ^ v, L_NONE, 0, 0);
		else c->a = logic(c, c->a | v, L_NONE, 0, 0);
	} else if (op == 0xFE) {
		v = fetch(c);
		CK;
		logic(c, c->a - v, L_SUB, v, c->a);
	} else if (op == 0xD6 || op == 0xDE || op == 0xC6 || op == 0xCE) {
		v = fetch(c);
		CK;
		if (op == 0xD6) sub8(c, v, 0);
		else if (op == 0xDE) sub8(c, v, CF ? 1 : 0);
		else if (op == 0xC6) add8(c, v, 0);
		else add8(c, v, CF ? 1 : 0);
	} else if (op == 0xEB) {
		int t;

		t = c->d; c->d = c->h; c->h = t;
		t = c->e; c->e = c->l; c->l = t;
	} else if (op == 0xE3) {
		int hl = HL;

		v = rd16(c, c->sp);
		CK;
		set_hl(c, v);
		wr16(c, c->sp, hl);
	} else if (op == 0x0B) {
		set_rr(c, 0, (get_rr(c, 0) - 1) & 0xFFFF);
	} else if (op == 0x1B) {
		set_rr(c, 1, (get_rr(c, 1) - 1) & 0xFFFF);
	} else if (op == 0x2B) {
		set_hl(c, (get_rr(c, 2) - 1) & 0xFFFF);
	} else if (op == 0x04 || op == 0x0C || op == 0x14 || op == 0x1C ||
		   op == 0x24 || op == 0x2C || op == 0x3C) {
		int32_t *r = reg8(c, (op >> 3) & 7);

		old = *r;
		v = (old + 1) & 0xFF;
		*r = v;
		inc_flags(c, old, v);
	} else if (op == 0x05 || op == 0x0D || op == 0x15 || op == 0x1D ||
		   op == 0x25 || op == 0x2D || op == 0x3D) {
		int32_t *r = reg8(c, (op >> 3) & 7);

		old = *r;
		v = (old - 1) & 0xFF;
		*r = v;
		dec_flags(c, old, v);
	} else if (op == 0x3B) {
		c->sp = (c->sp - 1) & 0xFFFF;
	} else if (op == 0x03) {
		set_rr(c, 0, (get_rr(c, 0) + 1) & 0xFFFF);
	} else if (op == 0x13) {
		set_rr(c, 1, (get_rr(c, 1) + 1) & 0xFFFF);
	} else if (op == 0x23) {
		set_hl(c, (get_rr(c, 2) + 1) & 0xFFFF);
	} else if (op == 0x33) {
		c->sp = (c->sp + 1) & 0xFFFF;
	} else if (op == 0xED) {
		step_ed(c);
	} else {
		char msg[64];

		snprintf(msg, sizeof(msg), "sim: unsupported opcode %02X at %04X",
			 op, (c->pc - 1) & 0xFFFF);
		core_exit(c, msg);
	}
}

int core_step(struct core *c)
{
	int op;
	int64_t cost;

	c->err = CORE_OK;
	if (c->halted) {
		if (c->int_pending && c->iff1) take_interrupt(c);
		else core_clock(c, 4);
		return c->err;
	}
	if (c->int_pending && c->iff1 && !c->ei_delay && c->im == 2) {
		take_interrupt(c);
		return c->err;
	}
	c->cycles += 1;
	cost = opcode_cost(c);
	CKV(c->err);
	core_clock(c, cost);
	CKV(c->err);
	op = fetch(c);
	CKV(c->err);
	step_op(c, op);
	CKV(c->err);
	if (c->ei_delay && op != 0xFB) c->ei_delay -= 1;
	return c->err;
}

int core_run(struct core *c, int64_t limit, const uint8_t *stops,
	     int check_first, int delay_addr, int delay_latch, int halt,
	     int64_t *done)
{
	int64_t n;

	for (n = 0; n < limit; ++n) {
		if (stops != NULL && (n > 0 || check_first) &&
		    (stops[c->pc >> 3] & (1u << (c->pc & 7)))) {
			*done = n;
			return 1;
		}
		if (delay_addr >= 0 && c->latch == delay_latch &&
		    c->pc == delay_addr) {
			c->err = CORE_OK;
			c->pc = pop16(c);
			if (c->err) {
				*done = n;
				return -1;
			}
		}
		c->out_hit = 0;
		if (core_step(c) != CORE_OK) {
			*done = n;
			return -1;
		}
		if (c->out_hit) {
			*done = n + 1;
			return 2;
		}
		if (halt && c->halted) {
			*done = n + 1;
			return 3;
		}
	}
	*done = n;
	return 0;
}
