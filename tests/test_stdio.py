#!/usr/bin/env python3
"""libkos console-buffer and stream unit cases, in the host Z80.

The SDK's console buffering and its stream layer are *executed*, not
inspected.  Each probe compiles the real `user/sdk/src` members with
SDCC exactly as the SDK rule does, links them against the SDK calls they
make - stubbed by the probe: a small record-and-answer console driver, a
small file provider, a clock - and runs the linked bytes in the host
Z80.  The probe reports one line per case (`<label>+` pass, `<label>-`
fail) and the tests assert the labels and the recorded traffic:

* the console buffer: a line flushes at '\\n', a full buffer flushes
  with it, a write longer than the free space flushes first and then
  goes out directly, every console read flushes before it reads, and a
  flush error is returned by the call that flushed while `putchar` keeps
  returning its byte;
* the stream pool: the mode string grammar, the fifth `fopen` returning
  NULL, and the pool freeing a slot at `fclose`;
* the stream calls: write and read back, `fgets`, `fseek` inside and
  outside the buffer, `ftell` accounting for buffered bytes, `rewind`,
  a write after a seek, `fflush(NULL)` and append mode;
* `stream_plan`: the two documented fixtures and an infeasible consumer.

Run with: `python3 -m unittest tests.test_stdio`.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import ihx  # noqa: E402
from z80sim import Z80  # noqa: E402

SDCC = "sdcc"
SDASZ80 = "sdasz80"
SDLDZ80 = "sdldz80"

SDK_SRC = os.path.join(ROOT, "user", "sdk", "src")

# The SDK's own compile line, plus its private source directory: the
# probe compiles the real members, `kos_internal.h` included.
CFLAGS = ["-mz80", "-c", "--nostdinc", "--Werror",
          "-I", os.path.join(ROOT, "abi"),
          "-I", os.path.join(ROOT, "user", "sdk", "include"),
          "-I", SDK_SRC]

ENTRY_ASM = """\
	.module sdk_probe_entry

	.globl _start
	.globl _probe_main

	.area _CODE

_start::
	ld	sp, #0xE000
	call	_probe_main
	halt
"""


def _run(argv, cwd=None):
    """Run a tool; a failure is loud."""
    result = subprocess.run(argv, cwd=cwd or ROOT, capture_output=True,
                            text=True)
    if result.returncode != 0:
        raise AssertionError("%s failed (%d):\n%s\n%s"
                             % (" ".join(argv), result.returncode,
                                result.stdout, result.stderr))
    return result


def _source(relative):
    """One SDK source's text."""
    with open(os.path.join(SDK_SRC, relative)) as handle:
        return handle.read()


# The entries that pair with the arithmetic members: the SDK's own
# `__divuint`/`__modulong` wrappers are assembly, not C.
ARITH_ASM = [_source(os.path.join("klibc", "sdcc_arith16.asm")),
             _source(os.path.join("klibc", "sdcc_arith_entries.asm")),
             _source(os.path.join("klibc", "sdcc_arith_signed_entries.asm"))]
# The 16-bit multiply `fread` and `fwrite` use for a group of items.
MUL_ASM = [_source(os.path.join("klibc", "mul.asm"))]
# The klibc string routines the stream probe links, in assembly.
STRING_ASM = [_source(os.path.join("klibc", "strlen.asm")),
              _source(os.path.join("klibc", "memcpy.asm"))]


class SdkProbe:
    """One probe program: SDK members + the probe's own sources + an
    entry, linked and run in the host Z80."""

    def __init__(self, tmp, sources, probe_c, probe_asm=None):
        self.tmp = tmp
        self.sources = list(sources)
        self.probe_c = probe_c
        self.probe_asm = probe_asm

    def path(self, name):
        return os.path.join(self.tmp, name)

    def write(self, name, text):
        with open(self.path(name), "w") as handle:
            handle.write(text)

    def build(self):
        self.write("probe.c", self.probe_c)
        self.write("entry.asm", ENTRY_ASM)
        objects = []
        for index, source in enumerate(self.sources):
            name = "sdk%d.c" % index
            shutil.copyfile(os.path.join(SDK_SRC, source),
                            self.path(name))
            _run([SDCC] + CFLAGS + ["-o", name + ".rel", name],
                 cwd=self.tmp)
            objects.append(self.path(name + ".rel"))
        for index, source in enumerate(self.probe_asm or []):
            name = "probe%d.asm" % index
            self.write(name, source)
            _run([SDASZ80, "-o", name + ".rel", name], cwd=self.tmp)
            objects.append(self.path(name + ".rel"))
        _run([SDCC] + CFLAGS + ["-o", "probe.rel", "probe.c"], cwd=self.tmp)
        objects.append(self.path("probe.rel"))
        _run([SDASZ80, "-o", "entry.rel", "entry.asm"], cwd=self.tmp)
        objects.append(self.path("entry.rel"))
        self.write("probe.lk", "\n".join([
            "-n", "-m", "-w",
            "-i " + self.path("probe.ihx"),
            "-b _CODE = 0x015D",
        ] + objects + ["-e", ""]))
        _run([SDLDZ80, "-f", "probe.lk"], cwd=self.tmp)
        with open(self.path("probe.map")) as handle:
            self.syms = ihx.parse_map_symbols(handle.read())
        return self

    def run(self, limit=2000000):
        with open(self.path("probe.ihx")) as handle:
            image = ihx.parse_ihx(handle.read())
        mem = bytearray(0x10000)
        for address, byte in image.items():
            mem[address] = byte
        # Mode 11 maps 0x0000-0x1FFF to bank 0 and the rest of the RAM
        # window to bank 7, so the linked image (code from 0x015D, data
        # after it) is handed to both: every address lands in the bank the
        # machine reads it from.
        flat = bytes(mem)
        z = Z80(flat, flat)
        z.latch = 0x18    # mode 11
        z.sp = 0xE000
        z.pc = self.syms["_start"]
        for _ in range(limit):
            z.step()
            if z.rd(z.pc) == 0x76:    # halt
                break
        self.machine = z
        return self

    def ram(self, name):
        address = self.syms[name]
        return self.machine._sram(self.machine.bank_of(address))

    def byte(self, name):
        return self.ram(name)[self.syms[name]]

    def word(self, name):
        base = self.syms[name]
        bank = self.ram(name)
        return bank[base] | (bank[base + 1] << 8)

    def long(self, name):
        bank = self.ram(name)
        base = self.syms[name]
        return (bank[base] | (bank[base + 1] << 8) |
                (bank[base + 2] << 16) | (bank[base + 3] << 24))

    def text(self, name, length_name):
        bank = self.ram(name)
        base = self.syms[name]
        return bytes(bank[base:base + self.word(length_name)])


class ProbeTestBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="kosstdio-")
        self.addCleanup(shutil.rmtree, self.tmp, ignore_errors=True)

    def labels(self, probe):
        """The probe's case log: {label: passed}."""
        bank = probe.ram("_probe_log")
        base = probe.syms["_probe_log"]
        length = probe.word("_probe_log_len")
        text = bytes(bank[base:base + length]).decode("ascii")
        result = {}
        for line in text.splitlines():
            label, mark = line[:-1], line[-1]
            result[label] = mark == "+"
        return result

    def assert_cases(self, probe, *labels):
        logged = self.labels(probe)
        for label in labels:
            self.assertIn(label, logged, "case %s did not run" % label)
            self.assertTrue(logged[label], "case %s failed" % label)


CONSOLE_PROBE = r"""
#include "kos.h"
#include "kos/protocol.h"

/* The probe's console driver: every request is recorded, writes append
 * their bytes to the transcript, reads answer from a small queue. */
struct probe_op {
	unsigned char op;
	unsigned int len;
};

static struct probe_op ops[64];
static unsigned int op_count;
static char probe_text[1024];
static unsigned int text_len;
static unsigned char read_queue[16];
static unsigned int read_queued;
static unsigned char fail_write;
static char long_text[300];

/* getchar() waits through idle_input(), but this probe exercises
 * buffering only; the real wait is covered by the shell and timer
 * suites. */
void idle_input(void)
{
}

void idle_input_ready(void)
{
}

char probe_log[512];
unsigned int probe_log_len;

static void check(const char *label, int ok)
{
	while (*label != '\0') {
		probe_log[probe_log_len++] = *label++;
	}
	probe_log[probe_log_len++] = ok ? '+' : '-';
	probe_log[probe_log_len++] = '\n';
}

static void reset_traffic(void)
{
	op_count = 0u;
	text_len = 0u;
	fail_write = 0u;
}

int call(unsigned char handle, struct kos_ipc_request *req)
{
	unsigned int i;

	(void)handle;
	if (req->frame.op == KCONSOLE_OP_WRITE) {
		ops[op_count].op = 'W';
		ops[op_count].len = req->length;
		++op_count;
		if (fail_write) {
			fail_write = 0u;
			return -KOS_EIO;
		}
		for (i = 0u; i < req->length; ++i) {
			probe_text[text_len++] = ((const char *)req->buffer)[i];
		}
		return (int)req->length;
	}
	if (req->frame.op == KCONSOLE_OP_READ) {
		ops[op_count].op = 'R';
		ops[op_count].len = req->length;
		++op_count;
		if (read_queued == 0u) {
			return 0;
		}
		((char *)req->buffer)[0] = (char)read_queue[0];
		for (i = 1u; i < read_queued; ++i) {
			read_queue[i - 1u] = read_queue[i];
		}
		--read_queued;
		return 1;
	}
	if (req->frame.op == KCONSOLE_OP_POLL) {
		ops[op_count].op = 'P';
		ops[op_count].len = 0u;
		++op_count;
		return 0;
	}
	return -KOS_EINVAL;
}

static void queue_read(char byte)
{
	read_queue[read_queued++] = (unsigned char)byte;
}

void probe_main(void)
{
	char line[16];
	int result;

	/* A '\n' flushes the line and nothing else does. */
	reset_traffic();
	(void)putchar('a');
	(void)putchar('b');
	(void)putchar('\n');
	check("line-flush", op_count == 1u && ops[0].op == 'W' &&
	      ops[0].len == 3u && text_len == 3u && probe_text[0] == 'a' &&
	      probe_text[1] == 'b' && probe_text[2] == '\n');

	/* The buffer flushes when it fills (128 bytes). */
	reset_traffic();
	{
		unsigned int i;

		for (i = 0u; i < 128u; ++i) {
			(void)putchar('x');
		}
	}
	check("buffer-full", op_count == 1u && ops[0].op == 'W' &&
	      ops[0].len == 128u && text_len == 128u);

	/* A short write waits for the flush and then goes out whole. */
	reset_traffic();
	result = console_write("hello", 5u);
	check("short-write-buffered", result == 5 && op_count == 0u);
	result = console_flush();
	check("flush-writes-it", result == 0 && op_count == 1u &&
	      ops[0].len == 5u && text_len == 5u &&
	      probe_text[0] == 'h' && probe_text[4] == 'o');

	/* A write longer than the free space flushes first, then goes out
	 * directly. */
	{
		unsigned int i;

		for (i = 0u; i < sizeof(long_text); ++i) {
			long_text[i] = 'L';
		}
	}
	reset_traffic();
	(void)putchar('a');
	result = console_write(long_text, 200u);
	check("long-write-flushes-first", result == 200 &&
	      op_count == 2u && ops[0].len == 1u && ops[1].len == 200u);

	/* A read flushes the output before it asks for input. */
	reset_traffic();
	queue_read('z');
	(void)putchar('P');
	result = console_read(line, 1u);
	check("read-flushes-first", result == 1 && line[0] == 'z' &&
	      op_count == 2u && ops[0].op == 'W' && ops[0].len == 1u &&
	      ops[1].op == 'R');

	/* The poll operation flushes too. */
	reset_traffic();
	(void)putchar('Q');
	(void)console_poll();
	check("poll-flushes-first", op_count == 2u && ops[0].op == 'W' &&
	      ops[1].op == 'P');

	/* getchar flushes before it reads, and echoes reach the wire in
	 * the order they were typed. */
	reset_traffic();
	queue_read('h');
	(void)putchar('R');
	result = getchar();
	check("getchar-flushes-first", result == 'h' && op_count == 2u &&
	      ops[0].op == 'W' && ops[1].op == 'R');

	/* The line editor's echo precedes the read that follows it, and
	 * the line ends with CRLF on the wire. */
	reset_traffic();
	queue_read('h');
	queue_read('i');
	queue_read('\r');
	result = console_readline(line, sizeof(line));
	check("readline-edits", result == 2 && line[0] == 'h' &&
	      line[1] == 'i' && line[2] == '\0');
	check("readline-order", op_count == 6u &&
	      ops[0].op == 'R' && ops[1].op == 'W' && ops[1].len == 1u &&
	      ops[2].op == 'R' && ops[3].op == 'W' && ops[3].len == 1u &&
	      ops[4].op == 'R' && ops[5].op == 'W' && ops[5].len == 2u &&
	      text_len == 4u && probe_text[0] == 'h' &&
	      probe_text[1] == 'i' && probe_text[2] == '\r' &&
	      probe_text[3] == '\n');

	/* A flush error is returned by the call that flushed; putchar
	 * still returns its byte. */
	reset_traffic();
	(void)putchar('a');
	fail_write = 1u;
	result = console_flush();
	check("flush-error-returned", result == -KOS_EIO && op_count == 1u);

	reset_traffic();
	{
		unsigned int i;

		for (i = 0u; i < 127u; ++i) {
			(void)putchar('y');
		}
	}
	fail_write = 1u;
	result = putchar('x');
	check("putchar-keeps-byte", result == 'x' && op_count == 1u);

	/* A write that meets the failed flush reports that error. */
	reset_traffic();
	(void)putchar('a');
	fail_write = 1u;
	result = console_write(long_text, 200u);
	check("write-reports-flush-error", result == -KOS_EIO);
}
"""

STREAM_PROBE = r"""
#include "kos.h"

/* The probe's file provider: one 2 KiB file, a cursor and the call
 * counters the test reads back. */
#define FILE_CAPACITY 6000u

static unsigned char file_data[FILE_CAPACITY];
static unsigned int file_size;
static unsigned int file_pos;
static unsigned int file_writes;
static unsigned char file_open_fails;
static unsigned char file_write_fails;

/* The backend request trace and the fault knobs the bulk cases drive:
 * a cap on the bytes one read or write call accepts, a read error at a
 * file offset, and a budget of bytes the file will take before every
 * write fails.  The trace holds the requested length of each call. */
static unsigned int tr_n;
static char tr_kind[24];
static unsigned int tr_len[24];
static unsigned int read_cap;
static unsigned int write_cap;
static unsigned int read_fail_at;
static unsigned char budget_on;
static unsigned int write_budget;

static void tr_add(char kind, unsigned int length)
{
	if (tr_n < 24u) {
		tr_kind[tr_n] = kind;
		tr_len[tr_n] = length;
	}
	++tr_n;
}

char probe_log[2048];
unsigned int probe_log_len;
unsigned char pattern[1000];
static unsigned char big[5200];
static unsigned char wsrc[5200];

/* The stream layer's exit hook: the real one lives in the console
 * client, which this probe does not link. */
void (*kos_stream_flush)(void);

int console_flush(void)
{
	return 0;
}

int open(const char *path, unsigned char flags)
{
	if (file_open_fails || path[0] != 'f' || path[1] != '\0') {
		return -KOS_ENOENT;
	}
	file_pos = 0u;
	if ((flags & KFS_O_TRUNC) != 0u) {
		file_size = 0u;
	}
	return 1;
}

int close(unsigned int handle)
{
	(void)handle;
	return 0;
}

int read(unsigned int handle, void *buffer, unsigned int length)
{
	unsigned int i;
	unsigned int available = file_size > file_pos ?
				 file_size - file_pos : 0u;

	(void)handle;
	tr_add('R', length);
	if (read_fail_at != 0u && file_pos >= read_fail_at) {
		return -KOS_EIO;
	}
	if (read_cap != 0u && length > read_cap) {
		length = read_cap;
	}
	if (length > available) {
		length = available;
	}
	for (i = 0u; i < length; ++i) {
		((unsigned char *)buffer)[i] = file_data[file_pos + i];
	}
	file_pos += length;
	return (int)length;
}

int write(unsigned int handle, const void *buffer, unsigned int length)
{
	unsigned int i;

	(void)handle;
	++file_writes;
	tr_add('W', length);
	if (file_write_fails) {
		return -KOS_EIO;
	}
	if (write_cap != 0u && length > write_cap) {
		length = write_cap;
	}
	if (budget_on) {
		if (write_budget == 0u) {
			return -KOS_EIO;
		}
		if (length > write_budget) {
			length = write_budget;
		}
		write_budget -= length;
	}
	for (i = 0u; i < length; ++i) {
		if (file_pos + i < FILE_CAPACITY) {
			file_data[file_pos + i] =
				((const unsigned char *)buffer)[i];
		}
	}
	file_pos += length;
	if (file_pos > file_size) {
		file_size = file_pos;
	}
	return (int)length;
}

long seek(unsigned int handle, long offset, unsigned char whence)
{
	long target;

	(void)handle;
	if (whence == KFS_SEEK_END) {
		target = (long)file_size + offset;
	} else if (whence == KFS_SEEK_CUR) {
		target = (long)file_pos + offset;
	} else {
		target = offset;
	}
	if (target < 0L || target > (long)FILE_CAPACITY) {
		return -KOS_EINVAL;
	}
	file_pos = (unsigned int)target;
	return target;
}

int sync(unsigned int handle)
{
	(void)handle;
	return 0;
}

/* `kos_stream_refill` records a refill's clock cost only when
 * `fmeasure` opted in, which these cases never do; the symbol is still
 * linked. */
int timer_now(int handle, uint32_t *now)
{
	(void)handle;
	*now = 0u;
	return -KOS_ENOENT;
}

static void check(const char *label, int ok)
{
	while (*label != '\0') {
		probe_log[probe_log_len++] = *label++;
	}
	probe_log[probe_log_len++] = ok ? '+' : '-';
	probe_log[probe_log_len++] = '\n';
}

static void fill_file(unsigned int size)
{
	unsigned int i;

	for (i = 0u; i < size; ++i) {
		file_data[i] = (unsigned char)(i * 7u + 1u);
	}
	file_size = size;
	file_pos = 0u;
}

static int file_matches(unsigned int at, const unsigned char *bytes,
			unsigned int length)
{
	unsigned int i;

	for (i = 0u; i < length; ++i) {
		if (file_data[at + i] != bytes[i]) {
			return 0;
		}
	}
	return 1;
}

static void reset_io(void)
{
	tr_n = 0u;
	read_cap = 0u;
	write_cap = 0u;
	read_fail_at = 0u;
	budget_on = 0u;
	write_budget = 0u;
}

/* The trace is `n` calls of these lengths (each of `kind`). */
static int trace_is(char kind, unsigned int n, unsigned int a,
		    unsigned int b, unsigned int c, unsigned int d)
{
	unsigned int want[4];
	unsigned int i;

	want[0] = a;
	want[1] = b;
	want[2] = c;
	want[3] = d;
	if (tr_n != n) {
		return 0;
	}
	for (i = 0u; i < n; ++i) {
		if (tr_kind[i] != kind || tr_len[i] != want[i]) {
			return 0;
		}
	}
	return 1;
}

static FILE *open_read(unsigned int size)
{
	fill_file(size);
	reset_io();
	return fopen("f", "r");
}

static FILE *open_write(void)
{
	fill_file(0u);
	reset_io();
	return fopen("f", "w");
}

/* One read of `size` x `count` from a fresh stream over `file` bytes:
 * the items, the position and the bytes must be the first `bytes` of
 * the file; the trace is left for the caller. */
static int read_run(const char *label, unsigned int file, unsigned int size,
		    unsigned int count, unsigned int bytes)
{
	FILE *f = open_read(file);
	unsigned int items = (unsigned int)fread(big, size, count, f);
	int ok = items == count && ftell(f) == (long)bytes &&
		 file_matches(0u, big, bytes) && ferror(f) == 0 &&
		 feof(f) == 0;

	(void)fclose(f);
	check(label, ok);
	return ok;
}

/* The same through fwrite: the file holds the bytes once closed. */
static int write_run(const char *label, unsigned int size,
		     unsigned int count, unsigned int bytes)
{
	FILE *f = open_write();
	unsigned int items = (unsigned int)fwrite(wsrc, size, count, f);
	int ok = items == count && ftell(f) == (long)bytes &&
		 ferror(f) == 0;

	ok = ok && fclose(f) == 0 && file_size == bytes &&
	     file_matches(0u, wsrc, bytes);
	check(label, ok);
	return ok;
}

static void bulk_cases(void)
{
	FILE *f;
	unsigned int i;
	unsigned int items;

	for (i = 0u; i < 5200u; ++i) {
		wsrc[i] = (unsigned char)(i * 5u + 3u);
	}

	/* The same 1536 bytes as 1536x1, 512x3, 3x512, 16x96: the items,
	 * the bytes and the backend trace (one direct request) agree. */
	read_run("rd-s1", 3000u, 1u, 1536u, 1536u);
	check("rd-s1-trace", trace_is('R', 1u, 1536u, 0u, 0u, 0u));
	read_run("rd-s3", 3000u, 3u, 512u, 1536u);
	check("rd-s3-trace", trace_is('R', 1u, 1536u, 0u, 0u, 0u));
	read_run("rd-s512", 3000u, 512u, 3u, 1536u);
	check("rd-s512-trace", trace_is('R', 1u, 1536u, 0u, 0u, 0u));
	read_run("rd-s96", 3000u, 96u, 16u, 1536u);
	check("rd-s96-trace", trace_is('R', 1u, 1536u, 0u, 0u, 0u));
	read_run("rd-s7", 3000u, 7u, 217u, 1519u);
	check("rd-s7-trace", trace_is('R', 1u, 1519u, 0u, 0u, 0u));

	/* Lengths at the buffer size and the descriptor limit. */
	read_run("rd-511", 6000u, 1u, 511u, 511u);
	check("rd-511-trace", trace_is('R', 1u, 512u, 0u, 0u, 0u));
	read_run("rd-512", 6000u, 1u, 512u, 512u);
	check("rd-512-trace", trace_is('R', 1u, 512u, 0u, 0u, 0u));
	read_run("rd-513", 6000u, 1u, 513u, 513u);
	check("rd-513-trace", trace_is('R', 1u, 513u, 0u, 0u, 0u));
	read_run("rd-4095", 6000u, 1u, 4095u, 4095u);
	check("rd-4095-trace", trace_is('R', 1u, 4095u, 0u, 0u, 0u));
	read_run("rd-4096", 6000u, 1u, 4096u, 4096u);
	check("rd-4096-trace", trace_is('R', 1u, 4096u, 0u, 0u, 0u));
	read_run("rd-4097", 6000u, 1u, 4097u, 4097u);
	check("rd-4097-trace", trace_is('R', 2u, 4096u, 512u, 0u, 0u));
	read_run("rd-4096-s2", 6000u, 2u, 2048u, 4096u);
	check("rd-4096-s2-trace", trace_is('R', 1u, 4096u, 0u, 0u, 0u));

	/* A buffer already holding bytes is consumed first. */
	f = open_read(3000u);
	items = (unsigned int)fread(big, 1u, 10u, f);
	items += (unsigned int)fread(big + 10, 1u, 700u, f);
	check("rd-partial-small", items == 710u && ftell(f) == 710L &&
	      file_matches(0u, big, 710u) &&
	      trace_is('R', 2u, 512u, 512u, 0u, 0u));
	(void)fclose(f);
	f = open_read(3000u);
	items = (unsigned int)fread(big, 1u, 10u, f);
	items += (unsigned int)fread(big + 10, 3u, 500u, f) * 3u;
	check("rd-partial-direct", items == 1510u && ftell(f) == 1510L &&
	      file_matches(0u, big, 1510u) &&
	      trace_is('R', 2u, 512u, 998u, 0u, 0u));
	(void)fclose(f);

	/* Short reads continue; the position is the bytes delivered. */
	f = open_read(3000u);
	read_cap = 300u;
	items = (unsigned int)fread(big, 1u, 1000u, f);
	check("rd-short", items == 1000u && ftell(f) == 1000L &&
	      ferror(f) == 0 && feof(f) == 0 && file_matches(0u, big, 1000u) &&
	      trace_is('R', 4u, 1000u, 700u, 512u, 512u));
	(void)fclose(f);

	/* EOF inside an item: whole items only, the partial one consumed,
	 * the flag set. */
	f = open_read(1000u);
	items = (unsigned int)fread(big, 300u, 4u, f);
	check("rd-eof-item", items == 3u && feof(f) != 0 && ferror(f) == 0 &&
	      ftell(f) == 1000L && file_matches(0u, big, 1000u));
	(void)fclose(f);
	f = open_read(1000u);
	items = (unsigned int)fread(big, 1u, 1200u, f);
	check("rd-eof-s1", items == 1000u && feof(f) != 0 &&
	      ftell(f) == 1000L && file_matches(0u, big, 1000u));
	(void)fclose(f);

	/* A read error inside an item keeps the delivered bytes. */
	f = open_read(3000u);
	read_cap = 400u;
	read_fail_at = 800u;
	items = (unsigned int)fread(big, 300u, 4u, f);
	check("rd-error-item", items == 2u && ferror(f) != 0 &&
	      feof(f) == 0 && ftell(f) == 800L && file_matches(0u, big, 800u));
	(void)fclose(f);

	/* Arithmetic: totals past 16 bits touch only the bytes there are
	 * (the backend has 3000), and the item count never wraps. */
	f = open_read(3000u);
	items = (unsigned int)fread(big, 40000u, 3u, f);
	check("rd-wrap-size", items == 0u && feof(f) != 0 &&
	      ftell(f) == 3000L && file_matches(0u, big, 3000u));
	(void)fclose(f);
	f = open_read(3000u);
	items = (unsigned int)fread(big, 2u, 40000u, f);
	check("rd-wrap-count", items == 1500u && feof(f) != 0 &&
	      ftell(f) == 3000L && file_matches(0u, big, 3000u));
	(void)fclose(f);
	f = open_read(3000u);
	items = (unsigned int)fread(big, 0xFFFFu, 0xFFFFu, f);
	check("rd-max", items == 0u && feof(f) != 0 && ftell(f) == 3000L);
	(void)fclose(f);

	/* Zero size or count: no backend traffic, no flag. */
	f = open_read(3000u);
	check("rd-zero", fread(big, 0u, 5u, f) == 0u &&
	      fread(big, 5u, 0u, f) == 0u && tr_n == 0u &&
	      ferror(f) == 0 && feof(f) == 0 && ftell(f) == 0L);
	(void)fclose(f);

	/* Writes: the same equal-byte requests. */
	write_run("wr-s1", 1u, 1536u, 1536u);
	check("wr-s1-trace", trace_is('W', 1u, 1536u, 0u, 0u, 0u));
	write_run("wr-s3", 3u, 512u, 1536u);
	check("wr-s3-trace", trace_is('W', 1u, 1536u, 0u, 0u, 0u));
	write_run("wr-s512", 512u, 3u, 1536u);
	check("wr-s512-trace", trace_is('W', 1u, 1536u, 0u, 0u, 0u));
	write_run("wr-s96", 96u, 16u, 1536u);
	check("wr-s96-trace", trace_is('W', 1u, 1536u, 0u, 0u, 0u));
	write_run("wr-s7", 7u, 217u, 1519u);
	check("wr-s7-trace", trace_is('W', 1u, 1519u, 0u, 0u, 0u));
	write_run("wr-511", 1u, 511u, 511u);
	check("wr-511-trace", trace_is('W', 1u, 511u, 0u, 0u, 0u));
	write_run("wr-512", 1u, 512u, 512u);
	check("wr-512-trace", trace_is('W', 1u, 512u, 0u, 0u, 0u));
	write_run("wr-513", 1u, 513u, 513u);
	check("wr-513-trace", trace_is('W', 1u, 513u, 0u, 0u, 0u));
	write_run("wr-4096", 1u, 4096u, 4096u);
	check("wr-4096-trace", trace_is('W', 1u, 4096u, 0u, 0u, 0u));
	write_run("wr-4097", 1u, 4097u, 4097u);
	check("wr-4097-trace", trace_is('W', 2u, 4096u, 1u, 0u, 0u));

	/* Pending bytes go out first, in order. */
	f = open_write();
	(void)fwrite(wsrc, 1u, 10u, f);
	(void)fwrite(wsrc + 10, 1u, 700u, f);
	check("wr-pending-direct", ftell(f) == 710L && fclose(f) == 0 &&
	      file_matches(0u, wsrc, 710u) &&
	      trace_is('W', 2u, 10u, 700u, 0u, 0u));
	f = open_write();
	(void)fwrite(wsrc, 1u, 10u, f);
	(void)fwrite(wsrc + 10, 1u, 505u, f);
	check("wr-pending-fill", ftell(f) == 515L && tr_n == 1u &&
	      fclose(f) == 0 && file_matches(0u, wsrc, 515u) &&
	      trace_is('W', 2u, 512u, 3u, 0u, 0u));
	f = open_write();
	(void)fwrite(wsrc, 1u, 10u, f);
	(void)fwrite(wsrc + 10, 1u, 300u, f);
	check("wr-pending-small", tr_n == 0u && fclose(f) == 0 &&
	      file_matches(0u, wsrc, 310u) &&
	      trace_is('W', 1u, 310u, 0u, 0u, 0u));

	/* Short writes continue; the tail is buffered and flushed. */
	f = open_write();
	write_cap = 300u;
	items = (unsigned int)fwrite(wsrc, 1u, 1000u, f);
	check("wr-short", items == 1000u && ferror(f) == 0 &&
	      ftell(f) == 1000L && fclose(f) == 0 &&
	      file_matches(0u, wsrc, 1000u) &&
	      trace_is('W', 4u, 1000u, 700u, 400u, 100u));

	/* An error inside an item: whole items only; the bytes the file
	 * took stay. */
	f = open_write();
	budget_on = 1u;
	write_budget = 700u;
	items = (unsigned int)fwrite(wsrc, 300u, 8u, f);
	check("wr-error-item", items == 2u && ferror(f) != 0 &&
	      ftell(f) == 700L && file_size == 700u &&
	      file_matches(0u, wsrc, 700u));
	(void)fclose(f);

	/* A flush error before a direct write is the call's error and the
	 * accepted bytes stay accepted. */
	f = open_write();
	(void)fwrite(wsrc, 1u, 100u, f);
	budget_on = 1u;
	items = (unsigned int)fwrite(wsrc + 100, 1u, 600u, f);
	check("wr-flush-error", items == 0u && ferror(f) != 0 &&
	      ftell(f) == 100L && trace_is('W', 1u, 100u, 0u, 0u, 0u));
	(void)fclose(f);

	/* Arithmetic on the write side: the request is 120000 or 65535 x
	 * 2 bytes; the file takes 5000 and the call reports the whole
	 * items it accepted. */
	f = open_write();
	budget_on = 1u;
	write_budget = 5000u;
	items = (unsigned int)fwrite(wsrc, 40000u, 3u, f);
	check("wr-wrap-size", items == 0u && ferror(f) != 0 &&
	      ftell(f) == 5000L && file_size == 5000u &&
	      file_matches(0u, wsrc, 5000u));
	(void)fclose(f);
	f = open_write();
	budget_on = 1u;
	write_budget = 5000u;
	items = (unsigned int)fwrite(wsrc, 2u, 40000u, f);
	check("wr-wrap-count", items == 2500u && ferror(f) != 0 &&
	      ftell(f) == 5000L && file_size == 5000u);
	(void)fclose(f);
	f = open_write();
	check("wr-zero", fwrite(wsrc, 0u, 5u, f) == 0u &&
	      fwrite(wsrc, 5u, 0u, f) == 0u && tr_n == 0u &&
	      ferror(f) == 0 && ftell(f) == 0L);
	(void)fclose(f);

	/* fputs goes through the same span: a long string is one direct
	 * write, a short one is buffered. */
	f = open_write();
	for (i = 0u; i < 600u; ++i) {
		big[i] = (unsigned char)'a' + (unsigned char)(i % 26u);
	}
	big[600] = 0u;
	check("fputs-long", fputs((const char *)big, f) == 0 &&
	      trace_is('W', 1u, 600u, 0u, 0u, 0u) && ftell(f) == 600L);
	(void)fclose(f);
	f = open_write();
	check("fputs-short", fputs("hello", f) == 0 && tr_n == 0u &&
	      fclose(f) == 0 && file_size == 5u &&
	      trace_is('W', 1u, 5u, 0u, 0u, 0u));
}

void probe_main(void)
{
	FILE *streams[5];
	unsigned char buffer[1000];
	unsigned int i;
	int result;

	/* The mode string: "r"/"w"/"a" with an ignored 'b'. */
	fill_file(0u);
	streams[0] = fopen("f", "r");
	check("mode-r", streams[0] != (FILE *)0);
	(void)fclose(streams[0]);
	streams[0] = fopen("f", "rb");
	check("mode-rb", streams[0] != (FILE *)0);
	(void)fclose(streams[0]);
	streams[0] = fopen("f", "w");
	check("mode-w", streams[0] != (FILE *)0);
	(void)fclose(streams[0]);
	streams[0] = fopen("f", "wb");
	check("mode-wb", streams[0] != (FILE *)0);
	(void)fclose(streams[0]);
	streams[0] = fopen("f", "a");
	check("mode-a", streams[0] != (FILE *)0);
	(void)fclose(streams[0]);
	streams[0] = fopen("f", "ab");
	check("mode-ab", streams[0] != (FILE *)0);
	(void)fclose(streams[0]);
	check("mode-empty", fopen("f", "") == (FILE *)0);
	check("mode-x", fopen("f", "x") == (FILE *)0);
	check("mode-r-plus", fopen("f", "r+") == (FILE *)0);
	check("mode-rx", fopen("f", "rx") == (FILE *)0);
	check("mode-b-first", fopen("f", "b") == (FILE *)0);

	/* The pool is four streams: the fifth fopen is NULL and a fclose
	 * frees a slot. */
	fill_file(0u);
	for (i = 0u; i < 4u; ++i) {
		streams[i] = fopen("f", "r");
		check(i == 0u ? "pool-first" : "pool-more",
		      streams[i] != (FILE *)0);
	}
	check("pool-exhausted", fopen("f", "r") == (FILE *)0);
	(void)fclose(streams[0]);
	streams[4] = fopen("f", "r");
	check("pool-reused", streams[4] != (FILE *)0);
	for (i = 1u; i < 5u; ++i) {
		(void)fclose(streams[i]);
	}
	streams[0] = fopen("f", "r");
	check("pool-empty", streams[0] != (FILE *)0);
	(void)fclose(streams[0]);

	/* Write a pattern, read it back. */
	for (i = 0u; i < 1000u; ++i) {
		pattern[i] = (unsigned char)(i * 3u + 5u);
	}
	fill_file(0u);
	streams[0] = fopen("f", "w");
	result = (int)fwrite(pattern, 1u, 1000u, streams[0]);
	check("fwrite-count", result == 1000);
	check("fclose-flushes", fclose(streams[0]) == 0);
	check("file-written", file_size == 1000u &&
	      file_matches(0u, pattern, 1000u));
	streams[0] = fopen("f", "r");
	check("fread-count", fread(buffer, 1u, 1000u, streams[0]) == 1000u);
	check("fread-bytes", file_matches(0u, buffer, 1000u));
	check("fread-items", fread(buffer, 100u, 10u, streams[0]) == 0u);
	check("eof-after-read", fread(buffer, 1u, 1u, streams[0]) == 0u &&
	      feof(streams[0]) != 0);
	check("fclose-read", fclose(streams[0]) == 0);

	/* fgets reads through the same buffer, newline kept. */
	fill_file(0u);
	streams[0] = fopen("f", "w");
	(void)fputs("alpha\nbeta\n", streams[0]);
	(void)fclose(streams[0]);
	streams[0] = fopen("f", "r");
	check("fgets-line", fgets((char *)buffer, 100, streams[0]) != (char *)0 &&
	      buffer[0] == 'a' && buffer[5] == '\n' && buffer[6] == '\0');
	check("fgets-second", fgets((char *)buffer, 100, streams[0]) != (char *)0 &&
	      buffer[0] == 'b' && buffer[4] == '\n');
	check("fgets-eof", fgets((char *)buffer, 100, streams[0]) == (char *)0 &&
	      feof(streams[0]) != 0);
	(void)fclose(streams[0]);

	/* A seek inside the buffer drops it: the next read comes from the
	 * offset asked for, not from the buffered bytes. */
	{
		unsigned int writes_before = file_writes;

		fill_file(1200u);
		streams[0] = fopen("f", "r");
		check("read-head", fread(buffer, 1u, 4u, streams[0]) == 4u);
		check("fseek-inside", fseek(streams[0], 10L, KFS_SEEK_SET) == 0 &&
		      fread(buffer, 1u, 1u, streams[0]) == 1u &&
		      buffer[0] == file_data[10]);
		check("ftell-after-reads", ftell(streams[0]) == 11L);
		check("fseek-cur", fseek(streams[0], -2L, KFS_SEEK_CUR) == 0 &&
		      ftell(streams[0]) == 9L &&
		      fread(buffer, 1u, 1u, streams[0]) == 1u &&
		      buffer[0] == file_data[9]);
		check("fseek-end", fseek(streams[0], 0L, KFS_SEEK_END) == 0 &&
		      ftell(streams[0]) == 1200L);
		rewind(streams[0]);
		check("rewind", ftell(streams[0]) == 0L &&
		      fread(buffer, 1u, 1u, streams[0]) == 1u &&
		      buffer[0] == file_data[0]);
		check("seek-error",
		      fseek(streams[0], (long)FILE_CAPACITY + 1L,
			    KFS_SEEK_SET) < 0 &&
		      ftell(streams[0]) == 1L && ferror(streams[0]) != 0);
		/* None of those seeks sent anything back: the bytes a read
		 * stream holds in its buffer are not ours to write (the
		 * provider counts every write call). */
		check("seek-no-write", file_writes == writes_before);
		(void)fclose(streams[0]);
	}

	/* A write after a seek lands at the offset asked for, and ftell
	 * accounts for the bytes still buffered. */
	fill_file(0u);
	streams[0] = fopen("f", "w");
	(void)fwrite(pattern, 1u, 300u, streams[0]);
	check("write-ftell", ftell(streams[0]) == 300L);
	check("write-seek", fseek(streams[0], 100L, KFS_SEEK_SET) == 0 &&
	      ftell(streams[0]) == 100L);
	(void)fwrite("\xaa\xbb\xcc\xdd", 1u, 4u, streams[0]);
	check("write-after-seek-ftell", ftell(streams[0]) == 104L);
	check("write-after-seek-close", fclose(streams[0]) == 0);
	check("write-after-seek", file_data[99] == pattern[99] &&
	      file_data[100] == 0xAAu && file_data[103] == 0xDDu &&
	      file_data[104] == pattern[104]);

	/* fflush(NULL) writes every stream's buffered bytes out without
	 * waiting for fclose, and append mode starts at the end. */
	fill_file(0u);
	streams[0] = fopen("f", "w");
	(void)fwrite("0123456789", 1u, 10u, streams[0]);
	check("fflush-null", fflush((FILE *)0) == 0 && file_size == 10u &&
	      file_matches(0u, (const unsigned char *)"0123456789", 10u));
	streams[1] = fopen("f", "a");
	check("append-ftell", ftell(streams[1]) == 10L);
	(void)fputs("tail", streams[1]);
	check("append-close", fclose(streams[1]) == 0 && file_size == 14u &&
	      file_matches(10u, (const unsigned char *)"tail", 4u));
	(void)fclose(streams[0]);

	/* Error paths: a failed open is NULL, a stale stream is refused,
	 * and a write error is sticky. */
	file_open_fails = 1u;
	check("open-fails", fopen("f", "r") == (FILE *)0);
	file_open_fails = 0u;
	streams[0] = fopen("f", "r");
	(void)fclose(streams[0]);
	check("stale-ftell", ftell(streams[0]) == -1L);
	check("stale-fclose", fclose(streams[0]) < 0);
	fill_file(0u);
	streams[0] = fopen("f", "r");
	check("write-on-read-stream", fwrite("x", 1u, 1u, streams[0]) == 0u &&
	      ferror(streams[0]) != 0);
	(void)fclose(streams[0]);
	streams[0] = fopen("f", "w");
	file_write_fails = 1u;
	check("write-error", fwrite(pattern, 1u, 600u, streams[0]) < 600u &&
	      ferror(streams[0]) != 0);
	file_write_fails = 0u;
	(void)fclose(streams[0]);

	bulk_cases();
}
"""

PLAN_PROBE = r"""
#include "kos.h"

char probe_log[512];
unsigned int probe_log_len;

void (*kos_stream_flush)(void);

int console_flush(void)
{
	return 0;
}

static void check(const char *label, int ok)
{
	while (*label != '\0') {
		probe_log[probe_log_len++] = *label++;
	}
	probe_log[probe_log_len++] = ok ? '+' : '-';
	probe_log[probe_log_len++] = '\n';
}

/* The two documented consumer fixtures: 100 KiB of file over 180 s and
 * over 30 s, refilled in 512-byte chunks, with a figure from a card
 * benchmark - 18.5 ms mean and 150 ms worst per refill - and the 20 ms
 * attention period. */
static void fixture(const char *tag, unsigned long rate)
{
	struct kos_stream_need need;
	struct kos_stream_plan plan;

	need.rate = rate;
	need.period = 782u;      /* 20 ms in TIMER counts */
	need.chunk = 512u;
	need.mean = 723u;        /* 18.5 ms */
	need.worst = 5859u;      /* 150 ms */
	need.safety = 0u;
	check(tag, stream_plan(&need, &plan) == 0);
	if (tag[0] == '1') {
		check("fixture-1", plan.ticks_per_refill == 45u &&
		      plan.prefetch == 1024ul &&
		      plan.utilisation_pct == 2u && plan.feasible != 0u &&
		      plan.startup_ms == 37ul &&
		      plan.bytes_per_tick_millis == 11370ul);
	} else {
		check("fixture-2", plan.ticks_per_refill == 7u &&
		      plan.prefetch == 1536ul &&
		      plan.utilisation_pct == 12u && plan.feasible != 0u &&
		      plan.startup_ms == 55ul &&
		      plan.bytes_per_tick_millis == 68325ul);
	}
}

void probe_main(void)
{
	struct kos_stream_need need;
	struct kos_stream_plan plan;

	fixture("1", 102400ul / 180ul);
	fixture("2", 102400ul / 30ul);

	/* A consumer past the medium's rate is infeasible, and a need
	 * that names no time or no bytes is refused. */
	need.rate = 20000ul;
	need.period = 782u;
	need.chunk = 512u;
	need.mean = 723u;
	need.worst = 5859u;
	need.safety = 0u;   /* the default factor of 2 */
	check("plan-ok", stream_plan(&need, &plan) == 0);
	check("infeasible", plan.feasible == 0u &&
	      plan.utilisation_pct >= 50u);
	check("safety-default", need.safety == 0u &&
	      plan.prefetch == 6656ul);
	need.rate = 0ul;
	check("need-zero-rate", stream_plan(&need, &plan) == -KOS_EINVAL);
	need.rate = 1000ul;
	need.chunk = 0u;
	check("need-zero-chunk", stream_plan(&need, &plan) == -KOS_EINVAL);
	need.chunk = 512u;
	need.mean = 0u;
	check("need-zero-mean", stream_plan(&need, &plan) == -KOS_EINVAL);
	check("need-null", stream_plan((struct kos_stream_need *)0,
				       &plan) == -KOS_EINVAL);
}
"""


class TestConsoleBufferProbe(ProbeTestBase):
    """The console client's buffering: the flush rules on the wire."""

    def setUp(self):
        super().setUp()
        self.probe = SdkProbe(
            self.tmp,
            ["console_proto.c", "console_io.c", "console_line.c"],
            CONSOLE_PROBE).build().run()

    def test_every_case_passes(self):
        self.assert_cases(
            self.probe, "line-flush", "buffer-full",
            "short-write-buffered", "flush-writes-it",
            "long-write-flushes-first", "read-flushes-first",
            "poll-flushes-first", "getchar-flushes-first",
            "readline-edits", "readline-order", "flush-error-returned",
            "putchar-keeps-byte", "write-reports-flush-error")


class TestStreamProbe(ProbeTestBase):
    """The stream layer: modes, pool, buffered I/O and positioning."""

    def setUp(self):
        super().setUp()
        self.probe = SdkProbe(
            self.tmp,
            ["stdio.c", "fread.c", "fwrite.c", "fseek.c", "feof.c",
             "klibc/sdcc_arith.c", "klibc/sdcc_arith_signed.c"],
            STREAM_PROBE,
            probe_asm=ARITH_ASM + STRING_ASM +
            MUL_ASM).build().run(limit=300000000)

    def test_every_case_passes(self):
        self.assert_cases(
            self.probe, "mode-r", "mode-rb", "mode-w", "mode-wb", "mode-a",
            "mode-ab", "mode-empty", "mode-x", "mode-r-plus", "mode-rx",
            "mode-b-first", "pool-first", "pool-more", "pool-exhausted",
            "pool-reused", "pool-empty", "fwrite-count", "fclose-flushes",
            "file-written", "fread-count", "fread-bytes", "fread-items",
            "eof-after-read", "fclose-read", "fgets-line", "fgets-second",
            "fgets-eof", "read-head", "fseek-inside", "ftell-after-reads",
            "fseek-cur", "fseek-end", "rewind", "seek-error",
            "seek-no-write",
            "write-ftell", "write-seek", "write-after-seek-ftell",
            "write-after-seek-close", "write-after-seek", "fflush-null",
            "append-ftell", "append-close", "open-fails", "stale-ftell",
            "stale-fclose", "write-on-read-stream", "write-error",
            "rd-s1", "rd-s1-trace", "rd-s3", "rd-s3-trace", "rd-s512",
            "rd-s512-trace", "rd-s96", "rd-s96-trace", "rd-s7",
            "rd-s7-trace", "rd-511", "rd-511-trace", "rd-512",
            "rd-512-trace", "rd-513", "rd-513-trace", "rd-4095",
            "rd-4095-trace", "rd-4096", "rd-4096-trace", "rd-4097",
            "rd-4097-trace", "rd-4096-s2", "rd-4096-s2-trace",
            "rd-partial-small", "rd-partial-direct", "rd-short",
            "rd-eof-item", "rd-eof-s1", "rd-error-item", "rd-wrap-size",
            "rd-wrap-count", "rd-max", "rd-zero",
            "wr-s1", "wr-s1-trace", "wr-s3", "wr-s3-trace", "wr-s512",
            "wr-s512-trace", "wr-s96", "wr-s96-trace", "wr-s7",
            "wr-s7-trace", "wr-511", "wr-511-trace", "wr-512",
            "wr-512-trace", "wr-513", "wr-513-trace", "wr-4096",
            "wr-4096-trace", "wr-4097", "wr-4097-trace",
            "wr-pending-direct", "wr-pending-fill", "wr-pending-small",
            "wr-short", "wr-error-item", "wr-flush-error", "wr-wrap-size",
            "wr-wrap-count", "wr-zero", "fputs-long", "fputs-short")


class TestStreamPlanProbe(ProbeTestBase):
    """stream_plan's arithmetic, on the documented fixtures."""

    def setUp(self):
        super().setUp()
        self.probe = SdkProbe(
            self.tmp,
            ["stream.c", "klibc/sdcc_arith.c",
             "klibc/sdcc_arith_signed.c"],
            PLAN_PROBE,
            probe_asm=ARITH_ASM).build().run(limit=4000000)

    def test_every_case_passes(self):
        self.assert_cases(
            self.probe, "1", "fixture-1", "2", "fixture-2", "plan-ok",
            "infeasible", "safety-default", "need-zero-rate",
            "need-zero-chunk", "need-zero-mean", "need-null")


if __name__ == "__main__":
    unittest.main()
