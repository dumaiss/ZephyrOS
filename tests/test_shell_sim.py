#!/usr/bin/env python3
"""Native shell session replay: the production image and a card.

The shell is an application, and it is the production image's boot
program: the replays type their sessions at its prompt through the
console lane, with a card in the drive.

The first session (`test_one_scripted_session`) covers the loop and the
shell's own built-ins:

* an empty line, then quotes and escapes through `echo`;
* `cd` absolute (`/system`, `/`), relative (`SUB` on the card) and `..`,
  with the displayed CWD in the prompt, in `pwd` and after `cd //`;
* the `path` edit (`path /system`, `path`, and the restore);
* a bare name found in `/system` (`hwtest`);
* an explicit path (`/system/fs_child.exe tail-42`), the child's non-zero
  exit status, a missing command, a wrong argument count and a line over
  the splitter's word budget;
* `exit 3`, which the launcher reports as the boot program's status
  before the shell starts again.

The second (`test_file_commands`) covers the file built-ins
(`ls cat stat cp mv rm mkdir rmdir`), every write inside `/fs/SHTEST`,
which the session creates and removes again:

* `ls` of `//`, `/`, `/system`, `/dev`, a file and two arguments;
* `mkdir`, `cp` into a directory and `cp` to a name;
* `cat` of the copy, read back byte for byte from the card;
* `stat` of a file and of a directory;
* `mv` inside `/fs` (a rename) and from `/system` (a copy whose unlink
  fails `read-only`, so the copy stays);
* `rm` of several files, one of them missing; `rmdir` of a non-empty and
  of an empty directory;
* `cp` onto `/system`, refused `read-only`.

The `hwtest` row runs the store's exerciser as the shell's child: the
shell boots, so the exerciser's own two nested-exec stages fit the
kernel's four-deep exec limit and its verdict is asserted here too.

The same sessions run without a card: tests/test_boot_policy.py replays
the card-less boot (the startup CWD branch that keeps the machine in
`/system`, the store's listing and the `no device` messages of `ls /` and
`mkdir /X`), which the linked-image tests (tests/test_shell.py) pinned
while the shell itself lived on the card.
"""

import os
import re
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import romfspack  # noqa: E402
from replay import STEP_LIMIT  # noqa: E402
from test_hwtest_sim import (CHUNK, KEEP_CONTENT, KEEP_NAME,  # noqa: E402
                             STEP_BUDGET, HwtestReplay)

STORE_ROMFS = os.path.join(ROOT, "build", "recovery.romfs")

# The launcher's line when the boot program exits with a status.
SHELL_EXIT = "boot: /system/shell.exe exit %d\n"

# The banner the shell prints at every start (the build date follows
# SDCC's __DATE__, so it is pinned by the compile, not the replay).  The
# ABI level is the kernel's system-info record's; the IOC level is the
# emulated controller's PING answer (4Ah, the deployed firmware's).
SHELL_BANNER_RE = (
    r"KonaOS v1\.0 \(build date: \d{4}-\d{2}-\d{2}\)\n"
    r"ABI version 1\n"
    r"IOC version 74\n"
    r"Copyright \(C\) Sebastien Dumais 2026\n")


def store_entries():
    """The packed store's entries, keyed by name (`ls /system` order)."""
    with open(STORE_ROMFS, "rb") as handle:
        _header, entries = romfspack.parse_image(handle.read())
    return {entry["name"]: entry for entry in entries}


def store_listing():
    """What `ls /system` prints, name and size per entry."""
    return "".join("%s  %u\n" % (entry["name"], entry["size"])
                   for entry in store_entries().values())


# The card this replay loads: one file outside the directory the session
# enters, and the directory that file creates.
SUB_NAME = "SUB/ORIG.TXT"
SUB_CONTENT = b"card"

# One row per session line: the line, the output that follows its echoed
# line, the marker that says the shell is ready for the next line, and
# `long` for the one child that runs for tens of millions of steps.  The
# marker is the last thing the console writes for the row, so matching it
# means the whole output has been decoded.
SESSION = (
    # An empty line does nothing: one line end and the prompt.
    ("", "/> ", "/> ", False),
    # Quotes, escapes and empty words: one literal, one escaped quote,
    # one escaped space, two empty words and one escaped backslash.
    ("echo 'a b' \"c\\\"d\" e\\ f '' \"\" x\\\\y",
     "a b c\"d e f   x\\y\n/> ", "/> ", False),
    ("cd /system", "/system> ", "/system> ", False),
    ("cd ..", "//> ", "//> ", False),
    ("cd //", "//> ", "//> ", False),
    ("pwd", "//\n//> ", "//> ", False),
    ("cd /", "/> ", "/> ", False),
    ("cd SUB", "/SUB> ", "/SUB> ", False),
    ("cd ..", "/> ", "/> ", False),
    ("path /system", "/> ", "/> ", False),
    ("path", "/system\n/> ", "/> ", False),
    ("help",
     "built-ins: cd pwd echo path help exit reboot ls cat stat cp mv rm "
     "mkdir rmdir\n/> ", "/> ", False),
    ("pwd x", "usage: pwd\n/> ", "/> ", False),
    ("cd /nope", "cd: /nope: not found\n/> ", "/> ", False),
    ("nosuchcmd", "nosuchcmd: command not found\n/> ", "/> ", False),
    ("echo " + " ".join("w%u" % index for index in range(16)),
     "shell: too many arguments\n/> ", "/> ", False),
    # A bare name found in /system: the store's exerciser starts like any
    # other child and reports its own transcript (the verdict is asserted
    # after the session: the shell boots, so its nested stages fit the
    # exec limit).
    ("hwtest", "hwtest: begin\n", "/> ", True),
    ("/system/fs_child.exe tail-42",
     "fs-child: stdout ok\n"
     "fs-child: tail \"tail-42\" argc 1\n"
     "fs-child: leaked 3 handles\n"
     "fs-child: PASS\n[exit 42]\n/> ", "/> ", False),
    ("path . /BIN /system", "/> ", "/> ", False),
    ("path", ".\n/BIN\n/system\n/> ", "/> ", False),
    ("exit 3", "boot: /system/shell.exe exit 3\n", "/> ", False),
)

# The file built-ins' session: one row per line.  Every write is under
# `/fs/SHTEST`; the card ends the session exactly as it started it.
def file_session():
    return (
        ("ls //", "fs/\nsystem/\ndev/\n/> ", "/> ", False),
        ("ls /", "KEEP.TXT  19\nSUB/\n/> ", "/> ", False),
        ("ls /system", store_listing() + "/> ", "/> ", False),
        ("ls /dev", "serial/\nioc\ninput/\nvideo\nconsole\ntimer\nsound\n/> ",
         "/> ", False),
        ("ls /dev/input", "keyboard\n/> ", "/> ", False),
        ("ls /SUB/ORIG.TXT", "ORIG.TXT  4\n/> ", "/> ", False),
        ("ls /SUB /system", "/SUB:\nORIG.TXT  4\n/system:\n"
         + store_listing() + "/> ", "/> ", False),
        ("cp /SUB/ORIG.TXT", "usage: cp src dst\n/> ", "/> ", False),
        ("cat", "usage: cat file ...\n/> ", "/> ", False),
        ("mkdir /SHTEST", "/> ", "/> ", False),
        ("ls /SHTEST", "/> ", "/> ", False),
        ("cp /SUB/ORIG.TXT /SHTEST", "/> ", "/> ", False),
        ("cp /system/drivers.man /SHTEST/MAN.TXT", "/> ", "/> ", False),
        ("cat /SHTEST/ORIG.TXT", "card/> ", "/> ", False),
        ("stat /SHTEST/ORIG.TXT",
         "/SHTEST/ORIG.TXT:\ntype: file\nattributes: 00\nsize: 4\n"
         "mtime: 00000000\n/> ", "/> ", False),
        ("stat /SHTEST",
         "/SHTEST:\ntype: dir\nattributes: 00\nsize: 0\n"
         "mtime: 00000000\n/> ", "/> ", False),
        ("mv /SHTEST/ORIG.TXT /SHTEST/MOVED.TXT", "/> ", "/> ", False),
        ("mv /system/drivers.man /SHTEST/STORE.MAN",
         "mv: /system/drivers.man: read-only\n/> ", "/> ", False),
        ("rm /SHTEST/MOVED.TXT /SHTEST/MAN.TXT /SHTEST/MISSING.TXT",
         "rm: /SHTEST/MISSING.TXT: not found\n/> ", "/> ", False),
        ("rmdir /SHTEST", "rmdir: /SHTEST: not empty\n/> ", "/> ", False),
        ("rm /SHTEST/STORE.MAN", "/> ", "/> ", False),
        ("rmdir /SHTEST", "/> ", "/> ", False),
        ("cp /SUB/ORIG.TXT /system/X.TXT",
         "cp: /system/X.TXT: read-only\n/> ", "/> ", False),
        ("exit 0", "boot: /system/shell.exe exit 0\n", "/> ", False),
    )


# The logo's 46 row addresses: (10 + row) * 256 + 4.
LOGO_ROW_ADDRESSES = frozenset((10 + row) * 256 + 4 for row in range(46))

class ShellSession(HwtestReplay):
    """The production image, the shell at boot and the emulated card."""

    def send(self, z, line, marker, limit=STEP_LIMIT):
        """Type one line and return the fresh visible output.

        The glyph model grows in console order, so matching `marker`
        means everything the console wrote before it has been decoded."""
        since = len(z.glyphs)
        start = len("".join(z.glyphs))
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_to(z, marker, limit, since=since)
        self.pump(z)
        return "".join(z.glyphs)[start:]

    def send_long(self, z, line, marker, budget=STEP_BUDGET):
        """Type one line whose child runs for tens of millions of steps.

        The model is decoded once per chunk (the replay helpers decode it
        once per step, which costs more than the emulation itself at this
        length), exactly as the exerciser replay does."""
        start = len("".join(z.glyphs))
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        steps = 0
        while steps < budget:
            for _ in range(CHUNK):
                self.skip_delay(z)
                self.feed(z)
                z.step()
            steps += CHUNK
            self.pump(z)
            if marker in "".join(z.glyphs)[start:]:
                return "".join(z.glyphs)[start:]
        self.fail("%r reached no output in %u steps (PC=%04X)"
                  % (line, budget, z.pc))

    def assert_logo(self, z, starts):
        """The banner's logo: written straight into VRAM through the data
        port, one 40-byte row per address, over the pinned 80 x 46
        rectangle at (8, 10) - 46 row addresses per shell start and no
        command engine involved.  (Text is data-port writes too, at cell
        boundaries, so the logo is counted by its row addresses.)"""
        lmmc = [cmd for cmd in z.vdp.commands if cmd["code"] == 0xB0]
        self.assertEqual(lmmc, [], "the logo uses no LMMC")
        rows = [addr for _now, addr in z.vdp.addr_writes
                if addr in LOGO_ROW_ADDRESSES]
        self.assertEqual(len(rows), starts * 46,
                         "logo rows addressed per start")
        # The rectangle holds the dump packed two pixels per byte.
        with open(os.path.join(ROOT, "user", "shell",
                               "konaos-logo-80x46.lmmc"), "rb") as handle:
            dump = handle.read()
        for row in range(46):
            base = (10 + row) * 256 + 4
            packed = bytes(((dump[row * 80 + col * 2] << 4) |
                            dump[row * 80 + col * 2 + 1])
                           for col in range(40))
            self.assertEqual(bytes(z.vdp.vram[base:base + 40]), packed,
                             "logo row %d" % row)

    def test_one_scripted_session(self):
        z = self.start(card=True, extra_files={SUB_NAME: SUB_CONTENT})
        peer = z.ioc_peer

        for line, output, marker, long in SESSION:
            if long:
                fresh = self.send_long(z, line, marker)
            else:
                fresh = self.send(z, line, marker)
            self.assertIn(line + "\n" + output, fresh,
                          "%r: %r" % (line, fresh[-160:]))

        # The launcher reported the boot program's status and started it
        # again (the production policy).
        trace = self.trace(z)
        self.assertIn(SHELL_EXIT % 3, trace)
        self.assertEqual(trace.count("boot: "), 1)
        # The relaunch starts on the banner, then the prompt.
        self.assertRegex(trace,
                         re.escape(SHELL_EXIT % 3) + SHELL_BANNER_RE +
                         r"/> ")
        # One LMMC logo per shell start: the boot and the relaunch.
        self.assert_logo(z, 2)
        # The exerciser reached its own verdict: every nested stage ran.
        self.assertIn("hwtest: 4/4 PASS", trace)

        # The card is exactly as the session found it, plus the empty
        # scratch file the explicit-path child creates and leaves behind.
        self.assertEqual({parts: bytes(content)
                          for parts, content in peer.fs2_files.items()},
                         {KEEP_NAME: KEEP_CONTENT,
                          ("SUB", "ORIG.TXT"): SUB_CONTENT,
                          ("CHILD.TMP",): b""})
        self.assertEqual(peer.fs2_dirs, {(), ("SUB",)})

    def test_history_and_clear_screen(self):
        """The line editor: the up arrow recalls the last lines (newest
        first), the down arrow walks back to an empty line, and CTRL-L
        redraws the prompt and the line typed so far."""
        z = self.start(card=True)
        up, down = b"\x1b[A", b"\x1b[B"
        self.send(z, "echo first", "first\n/> ")
        self.send(z, "echo second", "second\n/> ")
        # Up twice: the older line, run again.
        since = len(z.glyphs)
        z.hid.hid_queue.extend(up + up + b"\r")
        self.run_to(z, "first\n/> ", since=since)
        # Now the newest is `echo first`: one up recalls it, a second the
        # line before, and down brings the newer one back.  (The glyph
        # model keeps what was drawn, so the lines browsed past show too;
        # the last line drawn is the one that ran.)
        since = len(z.glyphs)
        z.hid.hid_queue.extend(up + up + down + b"\r")
        self.run_to(z, "first\n/> ", since=since)
        self.assertTrue(self.trace(z, since).endswith(
            "echo first\nfirst\n/> "))
        # Down past the newest is an empty line, which runs nothing.
        since = len(z.glyphs)
        z.hid.hid_queue.extend(up + down + b"echo third\r")
        self.run_to(z, "third\n/> ", since=since)
        self.assertTrue(self.trace(z, since).endswith(
            "echo third\nthird\n/> "))
        # CTRL-L mid-line: the screen clears, the prompt and the line so
        # far are drawn again, and the rest of the line completes it.
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"echo fo\x0c" + b"urth\r")
        self.run_to(z, "fourth\n/> ", since=since)
        self.assertIn("/> echo fourth", self.trace(z, since))

    def test_file_commands(self):
        """`ls cat stat cp mv rm mkdir rmdir` on a card.

        The rows assert the console; the card is snapshotted after every
        row, so the copy a `cat` read back, the rename inside `/fs` and
        the copy `/system` cannot unlink are all asserted where they
        happen, and the session's end proves the card unchanged.
        """
        if not os.path.exists(STORE_ROMFS):
            raise unittest.SkipTest("recovery.romfs has not been built")
        z = self.start(card=True, extra_files={SUB_NAME: SUB_CONTENT})
        peer = z.ioc_peer

        states = {}
        for line, output, marker, long in file_session():
            if long:
                fresh = self.send_long(z, line, marker)
            else:
                fresh = self.send(z, line, marker)
            self.assertIn(line + "\n" + output, fresh,
                          "%r: %r" % (line, fresh[-200:]))
            states[line] = ({parts: bytes(content)
                             for parts, content in peer.fs2_files.items()},
                            set(peer.fs2_dirs))

        # The launcher reported the boot program's status and started it
        # again (the production policy).
        trace = self.trace(z)
        self.assertIn(SHELL_EXIT % 0, trace)
        self.assertEqual(trace.count("boot: "), 1)
        # The relaunch starts on the banner, then the prompt.
        self.assertRegex(trace,
                         re.escape(SHELL_EXIT % 0) + SHELL_BANNER_RE +
                         r"/> ")
        # One LMMC logo per shell start: the boot and the relaunch.
        self.assert_logo(z, 2)

        # `cat` read the copy of the card's text file back, byte for
        # byte (the row above holds its console output).
        self.assertEqual(states["cat /SHTEST/ORIG.TXT"][0]
                         [("SHTEST", "ORIG.TXT")], SUB_CONTENT)

        # `cp` to a name copied the store's file whole: the `cat` above
        # proves the text path, this proves the bytes.
        man = store_entries()["drivers.man"]["payload"]
        self.assertEqual(
            states["cp /system/drivers.man /SHTEST/MAN.TXT"][0]
            [("SHTEST", "MAN.TXT")], man)

        # `mv` inside `/fs` renamed: one name gone, the other there.
        renamed, _dirs = states["mv /SHTEST/ORIG.TXT /SHTEST/MOVED.TXT"]
        self.assertIn(("SHTEST", "MOVED.TXT"), renamed)
        self.assertNotIn(("SHTEST", "ORIG.TXT"), renamed)

        # `mv` from `/system` copied (the store is read-only, so the
        # source's unlink was refused) and kept the copy: same bytes, and
        # the row above reported `read-only`.
        moved, _dirs = states["mv /system/drivers.man /SHTEST/STORE.MAN"]
        self.assertEqual(moved[("SHTEST", "STORE.MAN")], man)

        # The card is exactly as the session found it: `/SHTEST` and
        # everything in it is gone, and the source is untouched.
        self.assertEqual({parts: bytes(content)
                          for parts, content in peer.fs2_files.items()},
                         {KEEP_NAME: KEEP_CONTENT,
                          ("SUB", "ORIG.TXT"): SUB_CONTENT})
        self.assertEqual(peer.fs2_dirs, {(), ("SUB",)})


if __name__ == "__main__":
    unittest.main()
