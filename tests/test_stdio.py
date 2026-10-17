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
#define FILE_CAPACITY 2048u

static unsigned char file_data[FILE_CAPACITY];
static unsigned int file_size;
static unsigned int file_pos;
static unsigned int file_writes;
static unsigned char file_open_fails;
static unsigned char file_write_fails;

char probe_log[1024];
unsigned int probe_log_len;
unsigned char pattern[1000];

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
	if (file_write_fails) {
		return -KOS_EIO;
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
		check("seek-error", fseek(streams[0], 4096L, KFS_SEEK_SET) < 0 &&
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
            ["console_proto.c", "console_io.c"],
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
            probe_asm=ARITH_ASM + STRING_ASM).build().run(limit=8000000)

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
            "stale-fclose", "write-on-read-stream", "write-error")


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
