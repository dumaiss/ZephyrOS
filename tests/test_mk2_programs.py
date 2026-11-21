#!/usr/bin/env python3
"""Placed programs on the Zephyr-80 Mk2 board.

The Mk2 production image (`make BOARD=mk2`, build-mk2/konaos-mk2.bin)
boots to the shell on the simulator's Mk2 board, and the programs run
from the card: the SDK demo, the process and EXE v2 outcome programs and
the placement probe as the Mk2 build makes them (build-mk2/sys/user),
and PCMPlay and the SDK's far-data example built out of tree with the
standalone Mk2 SDK (`make sdk BOARD=mk2`), as a developer builds them.

What the tests pin (architecture 2.7.6, docs/abi.md section 8.3,
docs/exe-format.md):

* every program returns its status: exetest as the launcher's child,
  the EXE v2 programs over pages the test dirtied first (BSS and segment
  BSS zeroed, nothing else cleared), the far-code and data segments as
  page-list objects (SEG_INFO, SEG_MAP, XMEM_MAP);
* far calls across segments (home, far 1, far 2, resident), a syscall
  made from far code returning with its window, and a timer event
  delivered while far code runs: the handler runs with the far window
  closed, and the window the handler leaves open is replaced by the
  interrupted one on return;
* the far-window vector's cost (informational);
* PCMPlay: the real-time interrupt and the PCM ring in common memory,
  byte for byte, released at the end;
* an EXE built for one board is refused on the other.

Skipped when the images or programs have not been built.
"""

import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))

import exepack  # noqa: E402
import pcmconv  # noqa: E402
from replay import STEP_LIMIT  # noqa: E402
from test_mk2_shell import (CARD_PROMPT, MK2_DIR, MK2_IMAGE,  # noqa: E402
                            MK2_MAP, Mk1ShellReplay, Mk2ShellReplay)
from z80sim import (Ctc, IocCommandPeer, SoundPeer, V9958Peer,  # noqa: E402
                    Z80, read_symbols)

USER = os.path.join(ROOT, MK2_DIR, "sys", "user")
MK1_USER = os.path.join(ROOT, "build", "user")
SDK_TARBALL = os.path.join(ROOT, MK2_DIR, "sys", "kos-sdk-0.2.0-mk2.tar.gz")
SDK_NAME = "kos-sdk-0.2.0-mk2"
PCMPLAY = os.path.join(ROOT, "UserSoftware", "PCMPlay")

PAGE = 0x2000
SRAM_FIRST, SRAM_PAGES = 0x40, 0x40
COMMON_PAGE = 0x7F
FAR_VECTOR = 0x0028
WINDOW = range(0x8000, 0xC000)
RT_VECTOR, RT_IDLE = 0xEF08, 0xEF0B     # memory.inc KOS_RT_VECTOR, _IDLE
DAC = 0xE4
BUDGET = 400_000_000
BOOT_PATH = b"/system/shell.exe"

# tests/target/include/process_selftest.h
EXETEST_PASS, EXE2_PASS = 0xA500, 0xB500
MARK = bytes([0x5A, 0x69, 0xA5, 0xC3])  # KOS_PCST_EXE2_MARK_0..3 at D000h


def program(name, root=USER):
    with open(os.path.join(root, name + ".exe"), "rb") as handle:
        return handle.read()


def built(*names):
    return all(os.path.isfile(os.path.join(USER, name + ".exe"))
               for name in names)


def common(addr):
    """A common-page address as a physical one."""
    return COMMON_PAGE * PAGE + (addr - 0xE000)


class Mk2ProgramReplay(Mk2ShellReplay):
    """The Mk2 production image with the card's files and the sound
    card."""

    def make_cpu(self, card=True, files=None):
        z = Z80(self.image, b"", board="mk2")
        z.vdp = z.vdp_peer = V9958Peer()
        z.hid = z.ioc_peer = IocCommandPeer(fs2_media=card, fs2_files=files)
        z.sound_peer = SoundPeer()
        Ctc().attach(z)
        z.glyphs = []
        z.glyph_pos = 0
        z.glyph_last = None
        z.glyph_cells = {}
        z.glyph_next = None
        z.glyph_burst = False
        return z

    def boot(self, files):
        z = self.make_cpu(True, files)
        self.run_to(z, CARD_PROMPT)
        return z

    def run_line(self, z, line, limit=STEP_LIMIT):
        """Type `line` at the shell; the transcript up to the next
        prompt."""
        since = len(z.glyphs)
        z.hid.hid_queue.extend(line.encode("ascii") + b"\r")
        self.run_to_pattern(z, r"\n/> $", limit=limit, since=since)
        return self.trace(z, since)

    def status(self, transcript):
        """The child's exit status the shell reported (0: none).  The
        shell takes a status from 8000h up for a negative run result and
        prints the negated code."""
        match = re.search(r"\[exit (\d+)\]", transcript)
        if match:
            return int(match.group(1))
        match = re.search(r": error ([0-9A-Fa-f]+)\n", transcript)
        return 0x10000 - int(match.group(1), 16) if match else 0

    def kword(self, z, name):
        addr = self.syms[name]
        if addr >= 0xE000:
            at = common(addr)
        else:
            at = self.nucleus(addr)
        return z.phys[at] | (z.phys[at + 1] << 8)

    def window(self, z):
        """The far window's two working entries."""
        return tuple(z.mapper.entries[12:14])

    def running_image(self, z):
        at = common(self.kword(z, "_kos_dat_running"))
        return bytes(z.phys[at:at + 8])

    def dirty_free_pages(self, z):
        """Every free SRAM page full of the exe2 mark: a child's BSS and
        segment BSS read zero only if the loader zeroed them, and its
        D000h keeps the mark only if nothing else was cleared."""
        owner = self.nucleus(self.syms["_kos_page_owner"])
        free = [SRAM_FIRST + i for i in range(SRAM_PAGES)
                if z.phys[owner + i] == 0]
        for page in free:
            z.phys[page * PAGE:(page + 1) * PAGE] = MARK * (PAGE // 4)
        return free


@unittest.skipUnless(os.path.isfile(os.path.join(ROOT, MK2_DIR, MK2_IMAGE)),
                     "make BOARD=mk2 builds the image")
class Mk2ProgramCase(Mk2ProgramReplay, unittest.TestCase):
    def setUp(self):
        self.load_image(MK2_DIR, MK2_IMAGE, MK2_MAP)


@unittest.skipUnless(built("hello", "exetest", "exe2probe", "exe2seg",
                           "exe2big"), "make BOARD=mk2 builds the programs")
class TestPrograms(Mk2ProgramCase):
    def test_hello_prints_and_exits_zero(self):
        z = self.boot({"HELLO.EXE": program("hello")})
        fresh = self.run_line(z, "/fs/hello.exe one two")
        self.assertIn("Hello, KonaOS!\none\ntwo\n", fresh)
        self.assertNotIn("[exit", fresh)

    def test_exetest_as_the_launchers_child(self):
        """The launcher runs exetest (the boot path patched to the
        card): BSS, image, stack window, GETINFO with the kernel-parent
        flag, console output, PASS with its page-0 page."""
        self.assertEqual(self.image.count(BOOT_PATH + b"\0"), 1)
        at = self.image.index(BOOT_PATH + b"\0")
        path = b"/fs/ZT.EXE"
        image = bytearray(self.image)
        image[at:at + len(BOOT_PATH) + 1] = path.ljust(len(BOOT_PATH) + 1,
                                                       b"\0")
        self.image = bytes(image)
        z = self.make_cpu(True, {"ZT.EXE": program("exetest")})
        self.run_to_pattern(z, r"boot: /fs/ZT\.EXE exit \d+\n")
        trace = self.trace(z)
        self.assertIn("exetest: OK", trace)
        status = int(re.search(r"boot: /fs/ZT\.EXE exit (\d+)",
                               trace).group(1))
        self.assertEqual(status & 0xFF00, EXETEST_PASS)
        self.assertGreaterEqual(status & 0xFF, SRAM_FIRST)

    def test_exe2probe_bss_zeroed_and_nothing_else_cleared(self):
        z = self.boot({"PROBE.EXE": program("exe2probe")})
        self.dirty_free_pages(z)
        fresh = self.run_line(z, "/fs/probe.exe")
        self.assertIn("exe2probe: OK", fresh)
        self.assertEqual(self.status(fresh) & 0xFF00, EXE2_PASS, fresh)

    def test_exe2seg_data_segment_contents_and_bss(self):
        """A DATA segment is a page-list object: its payload, its zeroed
        BSS (over a dirtied page), XMEM_WRITE back, FREE refused, no
        second segment."""
        z = self.boot({"SEG.EXE": program("exe2seg")})
        self.dirty_free_pages(z)
        fresh = self.run_line(z, "/fs/seg.exe")
        self.assertIn("exe2seg: OK", fresh)
        self.assertEqual(self.status(fresh) & 0xFF00, EXE2_PASS, fresh)

    def test_exe2big_two_32k_segments_past_64k(self):
        z = self.boot({"BIG.EXE": program("exe2big")})
        free = len(self.dirty_free_pages(z))
        fresh = self.run_line(z, "/fs/big.exe")
        self.assertIn("exe2big: OK", fresh)
        self.assertEqual(self.status(fresh) & 0xFF00, EXE2_PASS, fresh)
        # Exit releases the image's and both segments' pages.
        self.idle(z)
        owner = self.nucleus(self.syms["_kos_page_owner"])
        self.assertEqual(sum(1 for i in range(SRAM_PAGES)
                             if z.phys[owner + i] == 0), free)


@unittest.skipUnless(built("placetest"), "make BOARD=mk2 builds placetest")
class TestPlacedProgram(Mk2ProgramCase):
    def setUp(self):
        super().setUp()
        self.place = read_symbols(os.path.join(USER, "placetest",
                                               "placetest.map"))

    def test_far_calls_and_an_event_in_far_code(self):
        """placetest: home, far 1, far 2 and resident calls through the
        veneers, then a timer event while far 1 runs.  The handler sees
        the window closed; the test makes it open far 2 through the
        far-window vector (measuring the vector), and the return puts far
        1 back."""
        z = self.boot({"PLACE.EXE": program("placetest")})
        self.idle(z)
        shell_page0 = z.mapper.entries[8]
        seen = {}

        def mine():
            return (z.mapper.dat_bank == 1 and
                    z.mapper.entries[8] != shell_page0)

        def at_main(_step):
            if not mine():
                return None
            seen["home"] = self.window(z)
            self.assertEqual(seen["home"], tuple(self.running_image(z)[4:6]))
            return True

        z.hid.hid_queue.extend(b"/fs/place.exe\r")
        self.assertIsNotNone(self.drive(z, BUDGET, watch={self.place["_main"]},
                                        hook=at_main))

        def at_spin(_step):
            if not mine() or self.window(z) == seen["home"]:
                return None
            seen["far1"] = self.window(z)
            seen["far1_handle"] = self.kword(z, "_kos_far_current")
            return True

        self.assertIsNotNone(self.drive(z, BUDGET,
                                        watch={self.place["_f1_spin"]},
                                        hook=at_spin))
        self.assertNotEqual(seen["far1_handle"], 0)
        self.assertLess(seen["far1_handle"], 0x100)
        # A syscall from far code (an unassigned number: the whole gate
        # crossing, KOS_ENOSYS) returns with the window it was made in.
        spin = self.place["_f1_spin"]
        regs = (z.a, z.f, z.bc, z.de, z.hl)
        z.push16(spin)
        z.a, z.hl, z.pc = 0x3F, 0x0000, 0x0030
        self.assertIsNotNone(self.drive(z, 100_000, watch={spin},
                                        hook=lambda _s: True))
        self.assertEqual((z.f & 1, z.a), (1, 0x01))        # KOS_ENOSYS
        self.assertEqual(self.window(z), seen["far1"])
        self.assertEqual(self.kword(z, "_kos_far_current"),
                         seen["far1_handle"])
        z.a, z.f, z.bc, z.de, z.hl = regs

        tick = self.place["_res_on_tick"]

        def at_tick(_step):
            return True if mine() else None

        self.assertIsNotNone(self.drive(z, BUDGET, watch={tick},
                                        hook=at_tick))
        # The handler runs with the window closed: the program's own
        # pages, handle 0.
        self.assertEqual(self.window(z), seen["home"])
        self.assertEqual(self.kword(z, "_kos_far_current"), 0)
        far2 = z.rd(self.place["_bcall_cache"] + 1)
        self.assertNotEqual(far2, 0)
        saved = (z.a, z.f, z.bc, z.de, z.hl, z.sp)

        def vector(handle):
            """RST 28h from the handler: the T-states from the RST to the
            instruction after it, and HL (the previous handle)."""
            z.push16(tick)
            z.hl = handle
            z.pc = FAR_VECTOR
            start = z.tstates
            self.assertIsNotNone(self.drive(z, 10_000, watch={tick},
                                            hook=lambda _s: True))
            return z.tstates - start + 11, z.hl

        open_cost, previous = vector(far2)
        self.assertEqual(previous, 0)
        seen["far2"] = self.window(z)
        self.assertNotIn(seen["far2"], (seen["home"], seen["far1"]))
        close_cost, previous = vector(0)
        self.assertEqual((previous, self.window(z)), (far2, seen["home"]))
        vector(far2)
        self.assertEqual(self.kword(z, "_kos_far_current"), far2)
        z.a, z.f, z.bc, z.de, z.hl, z.sp = saved
        sys.stderr.write("\nfar-window vector: open %d T, close %d T\n"
                         % (open_cost, close_cost))

        def back_in_far1(_step):
            return True if mine() else None

        self.assertIsNotNone(self.drive(z, BUDGET, watch=set(WINDOW),
                                        hook=back_in_far1))
        self.assertEqual(self.window(z), seen["far1"])
        self.assertEqual(self.kword(z, "_kos_far_current"),
                         seen["far1_handle"])

        self.run_to_pattern(z, r"\n/> $", limit=BUDGET)
        transcript = self.trace(z)
        for name in ("home to far1", "home to far1, stack arguments",
                     "home to far2, 8 bits", "far1 to far2 and home",
                     "far2 through a resident pointer",
                     "handler while far1 runs"):
            self.assertIn("place: %s ok" % name, transcript)
        self.assertIn("placetest PASS", transcript)


@unittest.skipUnless(os.path.isfile(SDK_TARBALL) and
                     shutil.which("make") is not None,
                     "make sdk BOARD=mk2 builds the Mk2 SDK")
class TestStandaloneSdk(Mk2ProgramCase):
    """Programs a developer builds with the Mk2 SDK tarball, out of the
    OS tree: PCMPlay (UserSoftware/PCMPlay, unchanged) and the SDK's
    far-data example."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix="kos-mk2-sdk-")
        with tarfile.open(SDK_TARBALL) as tar:
            tar.extractall(cls.tmp, filter="data")
        sdk = os.path.join(cls.tmp, SDK_NAME)
        player = os.path.join(cls.tmp, "PCMPlay")
        os.makedirs(player)
        shutil.copy(os.path.join(PCMPLAY, "Makefile"), player)
        shutil.copytree(os.path.join(PCMPLAY, "src"),
                        os.path.join(player, "src"))
        for directory, args in (
                (player, ["KOS_SDK_ARCHIVE=" + SDK_TARBALL,
                          "KOS_SDK_ROOT=build/" + SDK_NAME, "all"]),
                (os.path.join(sdk, "examples"), ["fardata"])):
            subprocess.run(["make", "-C", directory, "KOS_JOBS=1"] + args,
                           check=True, stdout=subprocess.DEVNULL,
                           stderr=subprocess.PIPE)
        with open(os.path.join(player, "build", "user", "pcmplay.exe"),
                  "rb") as handle:
            cls.pcmplay = handle.read()
        with open(os.path.join(sdk, "examples", "build", "user",
                               "fardata.exe"), "rb") as handle:
            cls.fardata = handle.read()

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_the_sdk_packs_for_mk2(self):
        for data in (self.pcmplay, self.fardata):
            self.assertEqual(exepack.parse_header(data)["board"],
                             exepack.BOARDS["mk2"])

    def test_fardata_maps_its_data_segment_and_calls_far_code(self):
        z = self.boot({"FARDATA.EXE": self.fardata})
        fresh = self.run_line(z, "/fs/fardata.exe")
        self.assertIn("fardata: window 8000", fresh)
        self.assertIn("fardata: table and history OK", fresh)
        self.assertIn("fardata: far sum 0030", fresh)   # 10h + 20h
        self.assertIn("fardata: done", fresh)
        self.assertNotIn("[exit", fresh)

    def test_pcmplay_plays_on_the_rt_interrupt(self):
        samples = 20000
        payload = bytes(((i * 7) ^ (i >> 8)) & 0xFF for i in range(samples))
        pcm = pcmconv.pack_header(16000, samples, "Mk2 tone") + payload
        z = self.boot({"PCMPLAY.EXE": self.pcmplay, "T.PCM": pcm})
        since = len(z.sound_peer.writes)
        fresh = self.run_line(z, "/fs/pcmplay.exe /fs/t.pcm", limit=BUDGET)
        self.assertIn("Title: Mk2 tone", fresh)
        self.assertIn("Playback complete.", fresh)
        self.assertIn("Samples 20000  underruns 0", fresh)
        dac = bytes(v for p, v, _t in z.sound_peer.writes[since:]
                    if p == DAC)
        start = dac.find(payload[:256])
        self.assertGreaterEqual(start, 0)
        self.assertEqual(dac[start:start + samples], payload)
        self.assertEqual(dac[-1], 0x80)
        # Every tick was an NMI on the common-page handler, and the
        # interrupt is released: the vector idle, the clock stopped.
        self.assertEqual(z.nmi_count, z.rt_ticks)
        self.assertGreater(z.rt_ticks, samples)
        self.assertEqual(z.ioc_peer.rt_requests[0], (16000, 20))
        self.assertEqual(z.ioc_peer.rt_requests[-1][0], 0)
        at = common(RT_VECTOR + 1)
        self.assertEqual(z.phys[at] | (z.phys[at + 1] << 8), RT_IDLE)
        self.assertEqual(z.rt_hz, 0)


class TestBoardCheck(unittest.TestCase):
    """The header's board field: the packer sets it, each loader refuses
    the other board's programs with KOS_EBOARD."""

    def test_the_header_names_the_board(self):
        if not (built("hello") and
                os.path.isfile(os.path.join(MK1_USER, "hello.exe"))):
            self.skipTest("make and make BOARD=mk2 build the programs")
        self.assertEqual(exepack.parse_header(program("hello"))["board"], 1)
        self.assertEqual(exepack.parse_header(
            program("hello", MK1_USER))["board"], 0)

    def test_a_mk1_program_is_refused_on_mk2(self):
        if not (os.path.isfile(os.path.join(ROOT, MK2_DIR, MK2_IMAGE)) and
                os.path.isfile(os.path.join(MK1_USER, "hello.exe"))):
            self.skipTest("make and make BOARD=mk2 build the image and "
                          "the program")
        replay = Mk2ProgramReplay.open(MK2_DIR, MK2_IMAGE, MK2_MAP)
        z = replay.boot({"HELLO.EXE": program("hello", MK1_USER)})
        fresh = replay.run_line(z, "/fs/hello.exe")
        self.assertIn("/fs/hello.exe: built for another board", fresh)
        self.assertNotIn("Hello", fresh)

    def test_a_mk2_program_is_refused_on_mk1(self):
        if not (os.path.isfile(os.path.join(ROOT, "build", "konaos.bin")) and
                built("hello")):
            self.skipTest("make and make BOARD=mk2 build the image and "
                          "the program")
        replay = Mk1ShellReplay.open("build", "konaos.bin")
        z = replay.make_cpu()
        z.hid = z.ioc_peer = IocCommandPeer(
            fs2_media=True, fs2_files={"HELLO.EXE": program("hello")})
        replay.start_kernel(z)
        replay.run_to(z, CARD_PROMPT)
        since = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/hello.exe\r")
        replay.run_to_pattern(z, r"\n/> $", since=since)
        fresh = replay.trace(z, since)
        self.assertIn("/fs/hello.exe: built for another board", fresh)
        self.assertNotIn("Hello", fresh)


if __name__ == "__main__":
    unittest.main()
