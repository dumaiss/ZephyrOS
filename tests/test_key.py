#!/usr/bin/env python3
"""kos/key.h: the keyboard key decoder, on the host.

Compiles user/sdk/src/key.c and key_get.c against a scripted
console_read() and checks the decoding: ordinary bytes, every editing and
function key sequence, a lone ESC with no continuation pending, ESC before
an ordinary byte, malformed and unknown sequences (ESC then their bytes
as characters), NUL skipping, key_pending, key_reset dropping a partial
sequence, and key_get's pacing calls.  The decoder links no TUI.

Run with: python3 -m unittest tests.test_key
"""

from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

PROBE = r'''
#include "kos/key.h"
#include "kos/errno.h"
#include <assert.h>
#include <string.h>

static const char *input = "";
static unsigned left;
static int idles, readies;
int console_read(char *out, unsigned int n)
{
	(void)n;
	if (!left) return 0;
	*out = *input++;
	--left;
	return 1;
}
static void feed(const char *bytes, unsigned length)
{
	input = bytes;
	left = length;
}
void idle_input(void) { ++idles; if (!left) feed("q", 1); }
void idle_input_ready(void) { ++readies; }

static void expect(const char *bytes, const key_code_t *keys, unsigned n)
{
	unsigned i;

	feed(bytes, (unsigned)strlen(bytes));
	for (i = 0; i < n; ++i) assert(key_poll() == keys[i]);
	assert(key_poll() == KEY_NONE);
}

int main(void)
{
	static const key_code_t plain[] = {'a', '\r', 0x82};
	static const key_code_t arrows[] = {KEY_UP, KEY_DOWN, KEY_RIGHT,
		KEY_LEFT, KEY_HOME, KEY_END};
	static const key_code_t tilde[] = {KEY_INSERT, KEY_DELETE, KEY_PGUP,
		KEY_PGDN, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10,
		KEY_F11, KEY_F12};
	static const key_code_t ss3[] = {KEY_F1, KEY_F2, KEY_F3, KEY_F4};
	static const key_code_t lone[] = {KEY_ESC};
	static const key_code_t esc_x[] = {KEY_ESC, 'x'};
	static const key_code_t unknown[] = {KEY_ESC, '[', '9', '9', '~'};
	static const key_code_t bad_ss3[] = {KEY_ESC, 'O', 'Z'};
	static const key_code_t half[] = {KEY_ESC, '[', '?'};
	static const key_code_t z[] = {'z'};

	assert(key_pending() == 0 && key_poll() == KEY_NONE);
	expect("a\r\x82", plain, 3);
	expect("\033[A\033[B\033[C\033[D\033[H\033[F", arrows, 6);
	expect("\033[2~\033[3~\033[5~\033[6~\033[15~\033[17~\033[18~"
	       "\033[19~\033[20~\033[21~\033[23~\033[24~", tilde, 12);
	expect("\033OP\033OQ\033OR\033OS", ss3, 4);
	expect("\033", lone, 1);
	expect("\033x", esc_x, 2);
	expect("\033[99~", unknown, 5);
	expect("\033OZ", bad_ss3, 3);
	expect("\033[?", half, 3);
	feed("\0b", 2);
	assert(key_pending() == 1 && key_poll() == 'b');
	/* A partial sequence is dropped by the reset. */
	feed("\033[1", 3);
	assert(key_poll() == KEY_NONE);
	key_reset();
	expect("z", z, 1);
	assert(idles == 0);
	/* key_get waits through idle_input, then marks input ready. */
	feed("", 0);
	assert(key_get() == 'q' && idles == 1 && readies == 1);
	return 0;
}
'''


class KeyDecoderTests(unittest.TestCase):
    def test_decoding_on_the_host(self):
        with tempfile.TemporaryDirectory() as temp:
            temp = Path(temp)
            (temp / "kos.h").write_text(
                '#include "kos/types.h"\n#include "kos/errno.h"\n'
                'void idle_input(void);\nvoid idle_input_ready(void);\n')
            (temp / "probe.c").write_text(PROBE)
            exe = temp / "keys"
            subprocess.run(
                ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-I" + str(temp), "-I" + str(ROOT / "user/sdk/kres/host"),
                 "-I" + str(ROOT / "abi"),
                 "-I" + str(ROOT / "user/sdk/include"),
                 str(temp / "probe.c"), str(ROOT / "user/sdk/src/key.c"),
                 str(ROOT / "user/sdk/src/key_get.c"), "-o", str(exe)],
                check=True)
            subprocess.run([str(exe)], check=True)
            symbols = subprocess.check_output(["nm", str(exe)], text=True)
            self.assertNotIn(" tui_", symbols)


if __name__ == "__main__":
    unittest.main()
