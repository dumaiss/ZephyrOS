"""The sound driver behind `/dev/sound` on the production image.

The production replay boots the shell with the emulated Afternoon Blend
card attached, runs the card program `tests/sound/sound.c` and asserts
both the program's report and the card's exact write log: the init's
silence, a batched sequence in order, an unknown target and an odd byte
count writing nothing, a sequence longer than one descriptor landing
whole, the exclusive hold's reset and refusal, a direct write reaching
the port, and the reset a close and an exit without one both perform.

The static half pins the manifest, the system file and the image's place
in the driver tier's reserve.
"""

import json
import os
import sys
import unittest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(ROOT, "tests"))

from test_hwtest_sim import HwtestReplay  # noqa: E402
from replay import SHELL_PROMPT  # noqa: E402
from z80sim import Ctc, IocCommandPeer, SoundPeer  # noqa: E402

SOUND_EXE = os.path.join(ROOT, "build", "user", "sound.exe")
MANIFEST = os.path.join(ROOT, "drivers", "sound", "sound.json")
SOUND_IO = os.path.join(ROOT, "drivers", "sound", "sound_io.asm")
SOUND_PROTO = os.path.join(ROOT, "drivers", "sound", "sound_proto.c")
SOUND_SDK = os.path.join(ROOT, "user", "sdk", "src", "sound.c")
PRODUCTION = os.path.join(ROOT, "systems", "production.json")
DRIVER_PLAN = os.path.join(ROOT, "build", "modules", "drivers.plan.json")
SOUND_REPORT = os.path.join(ROOT, "build", "modules", "sound.report.json")

# The INFO record: version 1, direct I/O + batched write + DAC, four PSGs
# at E0h-E3h, the DAC at E4h, 3,579,545 Hz little-endian, 12 zero bytes.
INFO_RECORD = ("01 07 04 E4 E0 E1 E2 E3 99 9E 36 00 "
               "00 00 00 00 00 00 00 00 00 00 00 00")

BIG_COUNT = 2050


def read(path):
    with open(path, encoding="utf-8") as handle:
        return handle.read()


def silence():
    """The reset's (port, value) sequence."""
    return SoundPeer().silence()


def expected_program_writes():
    """The card writes the card program makes, in issue order."""
    writes = [
        (0xE0, 0x8E), (0xE3, 0xF1), (0xE4, 0x80), (0xE1, 0x9F),
    ]
    writes += [(0xE0 + (index & 3), (index ^ 0x5A) & 0xFF)
               for index in range(BIG_COUNT)]
    writes += silence()                       # the exclusive acquire
    writes += [(0xE0, 0xAB), (0xE4, 0x55)]    # sound_out
    writes += silence()                       # the close's release
    writes += silence()                       # the second acquire
    writes += [(0xE0, 0x8E), (0xE0, 0x00)]    # the final tone
    writes += silence()                       # the exit's release
    return writes


class TestSoundContract(unittest.TestCase):
    """The image's shape and the places the ABI record is composed."""

    def test_manifest_shape(self):
        manifest = json.loads(read(MANIFEST))
        self.assertEqual(manifest["schema"], "kos-driver-1")
        self.assertEqual(manifest["name"], "sound")
        self.assertEqual(manifest["init"], "sound_driver_init")
        self.assertIn("sound_io.rel", manifest["objects"])
        self.assertIn("sound_driver.rel", manifest["objects"])
        self.assertEqual([segment["role"] for segment in manifest["segments"]],
                         ["hot", "io"])
        hot, io = manifest["segments"]
        self.assertEqual(hot["image_areas"], ["_CODE"])
        self.assertEqual(io["bss_areas"], ["_DRIVER_IO"])
        endpoint = manifest["endpoints"][0]
        self.assertEqual((endpoint["class"], endpoint["instance"]), (9, 0))
        self.assertEqual(endpoint["version"], 1)
        self.assertEqual(endpoint["dispatch"], "sound_driver_dispatch")
        self.assertTrue(endpoint["release"])
        self.assertNotIn("events", endpoint)
        publication = manifest["publishes"][0]
        self.assertEqual(publication["path"], "/dev/sound")
        self.assertEqual(publication["type"], "SOUND")
        self.assertEqual(publication["endpoint"], {"class": 9, "instance": 0})
        self.assertEqual(manifest["depends"], [])

    def test_the_info_record_is_the_abi_record(self):
        source = read(SOUND_PROTO)
        self.assertIn("KSOUND_INFO_VERSION", source)
        self.assertIn("KSOUND_CAP_DIRECT_IO | KSOUND_CAP_BATCH_WRITE", source)
        self.assertIn("KOS_SOUND_PSG_COUNT", source)
        self.assertIn("KOS_SOUND_DAC_PORT", source)
        self.assertIn("KOS_SOUND_PSG0_PORT", source)
        self.assertIn("KOS_SOUND_PSG1_PORT", source)
        self.assertIn("KOS_SOUND_PSG2_PORT", source)
        self.assertIn("KOS_SOUND_PSG3_PORT", source)
        self.assertIn("KOS_SOUND_PSG_CLOCK_HZ", source)
        self.assertIn("KOS_IPC_PAYLOAD_SIZE", source)

    def test_every_port_access_is_in_the_one_assembly_file(self):
        source = read(SOUND_IO)
        for value in ("0x9F", "0xBF", "0xDF", "0xFF", "0x80"):
            self.assertIn(value, source)
        self.assertIn("KOS_SOUND_PSG0_PORT", source)
        self.assertIn("KOS_SOUND_DAC_PORT", source)
        self.assertIn("sound_io_write_sequence", source)
        self.assertIn("sound_io_call_hl", source)
        # The C half never names a port.
        for text in (read(SOUND_PROTO),):
            self.assertNotIn("0xE0", text)
            self.assertNotIn("0xE1", text)
            self.assertNotIn("0xE2", text)
            self.assertNotIn("0xE3", text)
            self.assertNotIn("0xE4", text)

    def test_the_write_sequence_validates_before_any_out(self):
        source = read(SOUND_IO)
        validate = source.index("sound_io_validate_loop")
        write = source.index("sound_io_write_begin")
        self.assertLess(validate, write)
        self.assertIn("cp\t#(KOS_SOUND_TARGET_MAX + 1)", source)
        self.assertIn("KOS_SOUND_TARGET_MAX = KOS_SOUND_DAC_PORT - "
                      "KOS_SOUND_PSG0_PORT", source)

    def test_the_sdk_splits_a_long_sequence_at_the_descriptor_limit(self):
        source = read(SOUND_SDK)
        self.assertIn("KOS_IPC_DESC_MAX / sizeof(struct ksound_write)",
                      source)

    def test_production_loads_it_before_the_filesystem_provider(self):
        system = json.loads(read(PRODUCTION))
        self.assertIn("sound", system["drivers"])
        self.assertEqual(system["drivers"][-1], "iocfat")
        self.assertLess(system["drivers"].index("sound"),
                        system["drivers"].index("iocfat"))

    def test_the_driver_tier_keeps_its_reserve(self):
        if not os.path.exists(DRIVER_PLAN):
            self.skipTest("build/modules/drivers.plan.json missing; run make")
        plan = json.loads(read(DRIVER_PLAN))["nucleus"]
        free = (int(plan["driver_hot_capacity_bytes"]) -
                int(plan["driver_hot_bytes"]))
        self.assertGreaterEqual(free, 3584,
                                "driver hot tier has only %d bytes free"
                                % free)

    def test_the_sound_image_fits_its_share(self):
        if not os.path.exists(SOUND_REPORT):
            self.skipTest("build/modules/sound.report.json missing; run make")
        report = json.loads(read(SOUND_REPORT))
        segments = {segment["role"]: segment for segment in report["segments"]}
        hot = segments["hot"]["image_size"] + segments["hot"]["bss_size"]
        io = segments["io"]["image_size"] + segments["io"]["bss_size"]
        self.assertLessEqual(hot + io, 513,
                             "sound image hot %d + io %d" % (hot, io))


@unittest.skipUnless(os.path.exists(SOUND_EXE), "run make")
class TestSoundReplay(HwtestReplay):
    """The production image and the emulated card it drives."""

    def test_the_program_and_the_card_agree(self):
        z = self.make_cpu()
        Ctc().attach(z)
        z.sound_peer = SoundPeer()
        with open(SOUND_EXE, "rb") as handle:
            executable = handle.read()
        z.hid = z.ioc_peer = IocCommandPeer(
            fs2_media=True, fs2_files={"SOUND.EXE": executable})
        self.boot_to_shell(z, prompt=SHELL_PROMPT)

        # The driver's init leaves the card silent, before any program
        # runs.
        boot = [(port, value) for port, value, _tstate in
                z.sound_peer.writes]
        self.assertEqual(boot, silence())
        self.assertEqual(len(z.sound_peer.writes), len(silence()))

        since_writes = len(z.sound_peer.writes)
        since_glyphs = len(z.glyphs)
        z.hid.hid_queue.extend(b"/fs/sound.exe\r")
        self.run_to_pattern(z, r"sound: PASS", since=since_glyphs)
        # The exit-without-close reset lands before the shell's prompt.
        self.run_until_count(z, SHELL_PROMPT, 2)

        trace = self.trace(z, since_glyphs)
        self.assertIn("sound: info " + INFO_RECORD, trace)
        self.assertIn("sound: batch 4", trace)
        self.assertIn("sound: unknown -2", trace)
        self.assertIn("sound: odd -2", trace)
        self.assertIn("sound: big %d" % BIG_COUNT, trace)
        self.assertIn("sound: close 0", trace)
        self.assertIn("sound: exclusive ok", trace)
        self.assertIn("sound: busy -4", trace)
        self.assertIn("sound: direct", trace)
        self.assertIn("sound: release 0", trace)
        self.assertIn("sound: exclusive2 ok", trace)
        self.assertIn("sound: final 2", trace)
        self.assertIn("sound: PASS", trace)

        writes = z.sound_peer.writes[since_writes:]
        self.assertEqual([(port, value) for port, value, _tstate in writes],
                         expected_program_writes())
        # Every write carries the T-state it happened at, in issue order.
        states = [tstate for _port, _value, tstate in writes]
        self.assertEqual(states, sorted(states))
        self.assertEqual(len(states), len(expected_program_writes()))


if __name__ == "__main__":
    unittest.main()
