#!/usr/bin/env python3
"""Each application launched from the shell of the production image.

The image is the one of BOARD (mk2 by default, mk1; tests/replay.py).  The
application's EXE, built for that board by `make -C UserSoftware/<app>
BOARD=<board>`, sits on the card with its data files; the test types the
command line at the shell and checks what a person would see: the first
screen, the reaction to one input, and the return to the prompt.  No
internals of the application or the OS are asserted, so the same steps
run on both boards.

`APPS=KonaEdit,PView` limits the run to those applications.  An
application whose EXE is missing or built for the other board is skipped
with the reason.
"""

import os
import re
import sys
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import exepack  # noqa: E402
import kimg  # noqa: E402
import pcmconv  # noqa: E402
from replay import app_board, board_replay, v9958_screen  # noqa: E402
from z80sim import SoundPeer  # noqa: E402

BOARD = app_board()
USER = os.path.join(ROOT, "UserSoftware")
SONGS = os.path.join(USER, "SNTracker", "songs")
SND_FIXTURES = os.path.join(HERE, "snd", "fixtures")
BOOT_BUDGET = 60_000_000
BUDGET = 30_000_000     # steps per wait; `budget=` overrides per application
WALL_SECONDS = 40         # a stalled run that executes garbage is slow
F2, F10 = b"\x1bOQ", b"\x1b[21~"


def read(path):
    with open(path, "rb") as handle:
        return handle.read()


def pcm_file(samples):
    payload = bytes(((i * 7) ^ (i >> 8)) & 0xFF for i in range(samples))
    return pcmconv.pack_header(16000, samples, "Launch") + payload


def kimg_file():
    """A 16x16 GRAPHIC 7 picture: a gradient whose bytes are distinct."""
    payload = bytes(range(16 * 16))
    header = (b"KIMG" + bytes((kimg.VERSION, 1, kimg.FLAGS, 0)) +
              (16).to_bytes(2, "little") + (16).to_bytes(2, "little") +
              len(payload).to_bytes(4, "little"))
    return header + payload, payload


class App:
    """One launch: the card's files, the command line, what the first
    screen shows (`first`, a regex over the screen or the console
    transcript, `where`), one input (`keys`) with its visible effect
    (`after`), and how the program ends: by itself (`leave` None) or on
    a further input."""

    def __init__(self, name, exe, line, first, files=None, keys=None,
                 after=None, leave=None, where="trace", check=None,
                 sound=False, variant="", budget=BUDGET):
        self.name = name
        self.exe = exe
        self.line = line
        self.first = first
        self.files = files or {}
        self.keys = keys
        self.after = after
        self.leave = leave
        self.where = where
        self.check = check
        self.sound = sound
        self.variant = variant
        self.budget = budget


def apps():
    g7, g7_payload = kimg_file()

    def picture_shown(z, _text):
        # GRAPHIC 7 is linear: the 16x16 picture sits centred in 256x212.
        return b"".join(bytes(z.vdp.vram[(98 + row) * 256 + 120:
                                         (98 + row) * 256 + 136])
                        for row in range(16)) == g7_payload

    def ke_saved(z):
        assert bytes(z.hid.fs2_files[("ALPHA.TXT",)]) == b"Xalpha\n"

    return [
        App("KonaEdit", "kedit", "/fs/kedit.exe /fs/ALPHA.TXT", r"alpha",
            files={"ALPHA.TXT": b"alpha\n"}, keys=b"X", after=r"Xalpha",
            leave=F2 + F10, where="screen", check=ke_saved),
        App("KonaCommander", "kc", "/fs/kc.exe", r"File Edit View Help",
            keys=b"\x1b[B", leave=b"q", where="screen",
            files={"ALPHA.TXT": b"alpha\n", "BETA.TXT": b"beta\n"}),
        App("SDBench", "sdbench", "/fs/sdbench.exe /fs/BENCH.BIN",
            r"SDBench for KonaOS",
            files={"BENCH.BIN": bytes((i * 7) & 255
                                      for i in range(16384))},
            after=r"4096 B", budget=300_000_000),
        App("SDBench", "sdbench", "/fs/sdbench.exe -n /fs/BENCH.BIN",
            r"SDBench for KonaOS", variant="-n",
            files={"BENCH.BIN": bytes((i * 7) & 255
                                      for i in range(16384))},
            after=r"consistent|checksum|reads", budget=300_000_000),
        App("ColecoGo", "colecogo", "/fs/colecogo.exe",
            r"ColecoGo 0\.6 for KonaOS", after=r"usage: colecogo GAME\.ROM"),
        App("PCMPlay", "pcmplay", "/fs/pcmplay.exe /fs/L.PCM",
            r"Playing", files={"L.PCM": pcm_file(160000)}, keys=b" ",
            after=r"Playback stopped\.", sound=True),
        App("PView", "pview", "/fs/pview.exe /fs/T.KIM",
            picture_shown, files={"T.KIM": g7}, keys=b" ",
            after=r"pview: done"),
        App("SC2", "sc2kos", "/fs/sc2kos.exe", r"Ready - type \? for help",
            keys=b"/", after=r"Command: B C D", leave=b"Q",
            where="screen"),
        App("SNTracker", "sntrack", "/fs/sntrack.exe /fs/NIGHT.ZTR",
            r"Playing: /fs/NIGHT\.ZTR",
            files={"NIGHT.ZTR": read(os.path.join(SONGS,
                                                  "night-market.ztr"))},
            keys=b"q", after=r"Playback aborted\.", sound=True),
        App("SNTracker", "sntview", "/fs/sntview.exe /fs/NIGHT.ZTR",
            r"sntview|\|",
            files={"NIGHT.ZTR": read(os.path.join(SONGS,
                                                  "night-market.ztr"))},
            keys=b"q", sound=True, where="screen"),
        App("VGMPlayer", "vgmplay", "/fs/vgmplay.exe /fs/NIGHT.ZVG",
            r"Playing; press any key to stop\.",
            files={"NIGHT.ZVG": read(os.path.join(SND_FIXTURES,
                                                  "NIGHT.ZVG"))},
            keys=b" ", after=r"Playback aborted\.", sound=True),
        App("ZTRCheck", "ztrcheck", "/fs/ztrcheck.exe /fs/NIGHT.ZTR",
            r"OK: ",
            files={"NIGHT.ZTR": read(os.path.join(SONGS,
                                                  "night-market.ztr"))}),
    ]


def program_path(app):
    return os.path.join(USER, app.name, "build", "user", app.exe + ".exe")


def screen_text(z):
    return "\n".join("".join(cell[0] for cell in row)
                     for row in v9958_screen(z.vdp.vram, z.vdp.regs[23]))


class Launch(unittest.TestCase):
    app = None

    def setUp(self):
        path = program_path(self.app)
        if not os.path.isfile(path):
            self.skipTest("%s is not built (make -C UserSoftware/%s "
                          "BOARD=%s)" % (path, self.app.name, BOARD))
        board = exepack.parse_header(read(path))["board"]
        if board != exepack.BOARDS[BOARD]:
            self.skipTest("%s is built for another board" % path)

    @staticmethod
    def state(z):
        return ("PC=%04X SP=%04X halted=%s iff1=%s tstates=%d "
                "console tail=%r" % (z.pc, z.sp, z.halted, z.iff1,
                                     z.tstates, "".join(z.glyphs)[-200:]))

    def seen(self, z, start, where):
        self.replay.pump(z)
        if where == "screen":
            # Decoding the screen is slow: only when the VDP was written.
            key = (len(z.vdp.vram_writes), len(z.vdp.commands),
                   z.vdp.regs[23])
            if getattr(self, "screen_key", None) != key:
                self.screen_key = key
                self.screen_cache = screen_text(z)
            return self.screen_cache
        return "".join(z.glyphs[start:])

    def reached(self, z, what, start, where):
        text = self.seen(z, start, where)
        if callable(what):
            return what(z, text)
        return re.search(what, text) is not None

    def wait(self, z, what, start, where, step):
        limit = self.app.budget
        deadline = time.monotonic() + WALL_SECONDS

        def tick(_step):
            return (self.reached(z, what, start, where) or
                    time.monotonic() > deadline)
        if not self.reached(z, what, start, where):
            self.replay.drive(z, limit, tick=tick,
                              every=0x3FFFF if where == "screen" else 0x3FF)
        if not self.reached(z, what, start, where):
            self.fail("%s: %r not seen (%s)" % (step, what, self.state(z)))

    def test_launch(self):
        app = self.app
        self.replay = replay = board_replay(BOARD)
        files = dict(app.files)
        files[app.exe.upper() + ".EXE"] = read(program_path(app))
        z = replay.make_cpu()
        for name, data in files.items():
            z.hid.fs2_add(name, data)
        if app.sound:
            z.sound_peer = SoundPeer()
        replay.boot_to_shell(z, limit=BOOT_BUDGET)
        start = len(z.glyphs)
        z.hid.hid_queue.extend(app.line.encode("ascii") + b"\r")
        self.wait(z, app.first, start, app.where, "first screen")
        if app.keys is not None:
            mark = len(z.glyphs)
            z.hid.hid_queue.extend(app.keys)
            if app.after:
                self.wait(z, app.after, mark, app.where, "after the input")
        if app.leave:
            z.hid.hid_queue.extend(app.leave)
        self.wait(z, r"/\n?> $", start, "trace", "return to the prompt")
        self.assertNotIn("built for another board",
                         self.seen(z, start, "trace"))
        if app.check:
            app.check(z)


def generate():
    only = os.environ.get("APPS")
    keep = {item.strip().lower() for item in only.split(",")} if only \
        else None
    for app in apps():
        if keep is not None and app.name.lower() not in keep:
            continue
        name = "Launch_%s%s" % (app.exe, app.variant.replace("-", "_"))
        globals()[name] = type(name, (Launch,), {"app": app})


generate()
del Launch  # only the generated cases are collected

if __name__ == "__main__":
    unittest.main()
