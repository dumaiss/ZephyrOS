import importlib.util
import os
import subprocess
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "kcommander_host", ROOT / "host" / "kcommander_host.py")
host = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
sys.modules[SPEC.name] = host
SPEC.loader.exec_module(host)


def path_payload(value: str) -> bytes:
    data = value.encode()
    return bytes((len(data),)) + data


class ProtocolTests(unittest.TestCase):
    def test_crc_golden(self):
        self.assertEqual(host.crc16(b"123456789"), 0x29B1)

    def test_frame_and_cobs_round_trip(self):
        frame = host.Frame(host.REQUEST, host.CMD_PING, 9,
                           payload=b"a\0b" + bytes(range(256)) * 2)
        raw = frame.pack()
        encoded = host.cobs_encode(raw)
        self.assertNotIn(0, encoded)
        self.assertEqual(host.Frame.unpack(host.cobs_decode(encoded)), frame)
        damaged = bytearray(raw)
        damaged[12] ^= 1
        with self.assertRaises(ValueError):
            host.Frame.unpack(bytes(damaged))

    def test_stdio_entry_point(self):
        request = host.Frame(host.REQUEST, host.CMD_HELLO, 3).pack()
        with tempfile.TemporaryDirectory() as directory:
            process = subprocess.run(
                [sys.executable, str(ROOT / "host" / "kcommander_host.py"),
                 "--stdio", "--root", directory, "--name", "PIPE"],
                input=host.cobs_encode(request) + b"\0", capture_output=True,
                timeout=5, check=True)
        record = process.stdout.rstrip(b"\0")
        response = host.Frame.unpack(host.cobs_decode(record))
        self.assertEqual(response.sequence, 3)
        self.assertEqual(response.payload[3:7], b"PIPE")
        self.assertEqual(response.payload[7], 0)

    def test_target_writes_are_paced_one_byte_at_a_time(self):
        class Writer:
            def __init__(self):
                self.writes = []
                self.flushes = 0

            def write(self, data):
                self.writes.append(data)

            def flush(self):
                self.flushes += 1

        writer = Writer()
        with mock.patch.object(host.time, "sleep") as sleep:
            host.write_paced(writer, b"abc", 0.004)
        self.assertEqual(writer.writes, [b"a", b"b", b"c"])
        self.assertEqual(writer.flushes, 3)
        self.assertEqual(sleep.call_count, 3)


class ServiceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        (self.root / "adir").mkdir()
        (self.root / "Bdir").mkdir()
        (self.root / "alpha.txt").write_bytes(b"hello\nworld\n")
        (self.root / "Zulu.bin").write_bytes(bytes(range(256)) * 4)
        self.service = host.FileService(self.root, "TEST")
        self.sequence = 1

    def tearDown(self):
        self.service.close()
        self.temp.cleanup()

    def request(self, command: int, payload: bytes = b"") -> host.Frame:
        request = host.Frame(host.REQUEST, command, self.sequence,
                             payload=payload)
        self.sequence += 1
        return host.Frame.unpack(self.service.process(request.pack()))

    def test_hello_and_paged_list(self):
        hello = self.request(host.CMD_HELLO)
        self.assertEqual(hello.status, host.OK)
        self.assertEqual(hello.payload[3:7], b"TEST")
        self.assertEqual(hello.payload[7], 0)
        first = self.request(host.CMD_LIST,
                             path_payload("/") + struct.pack("<IB", 0, 2))
        self.assertEqual(first.status, host.OK)
        cursor, count = struct.unpack_from("<IB", first.payload)
        self.assertEqual(count, 2)
        self.assertNotEqual(cursor, 0)
        self.assertEqual(first.payload[5], 1)
        second = self.request(host.CMD_LIST,
                              path_payload("/") + struct.pack("<IB", cursor, 32))
        self.assertEqual(second.status, host.OK)
        self.assertEqual(struct.unpack_from("<I", second.payload)[0], 0)

    def test_open_positioned_read_close_and_duplicate(self):
        encoded = path_payload("/alpha.txt")
        raw = host.Frame(host.REQUEST, host.CMD_OPEN, 44,
                         payload=encoded).pack()
        first_raw = self.service.process(raw)
        second_raw = self.service.process(raw)
        self.assertEqual(first_raw, second_raw)
        self.assertEqual(len(self.service.handles), 1)
        opened = host.Frame.unpack(first_raw)
        handle, size = struct.unpack("<BI", opened.payload)
        self.assertEqual(size, 12)
        read = self.request(host.CMD_READ,
                            bytes((handle,)) + struct.pack("<IH", 6, 5))
        self.assertEqual(read.payload[6:], b"world")
        self.assertEqual(self.request(host.CMD_CLOSE, bytes((handle,))).status,
                         host.OK)
        self.assertEqual(self.request(host.CMD_CLOSE, bytes((handle,))).status,
                         host.OK)

    def test_root_escape_and_bad_handle(self):
        response = self.request(host.CMD_OPEN, path_payload("/../secret"))
        self.assertEqual(response.status, host.DENIED)
        response = self.request(host.CMD_READ,
                                b"\x07" + struct.pack("<IH", 0, 16))
        self.assertEqual(response.status, host.BAD_HANDLE)

    def test_writes_require_explicit_opt_in(self):
        response = self.request(host.CMD_CREATE, path_payload("/new.bin"))
        self.assertEqual(response.status, host.DENIED)
        self.assertFalse((self.root / "new.bin").exists())

        self.service.close()
        self.service = host.FileService(self.root, "WRITE", allow_write=True)
        response = self.request(host.CMD_CREATE,
                                path_payload("/../escaped.bin"))
        self.assertEqual(response.status, host.DENIED)

    def test_atomic_upload_commit_duplicate_and_abort(self):
        self.service.close()
        self.service = host.FileService(self.root, "WRITE", allow_write=True)
        (self.root / "replace.bin").write_bytes(b"old")

        hello = self.request(host.CMD_HELLO)
        self.assertEqual(hello.payload[-1], host.CAP_WRITE)
        created = self.request(host.CMD_CREATE,
                               path_payload("/replace.bin"))
        self.assertEqual(created.status, host.OK)
        handle = created.payload[0]
        self.assertEqual((self.root / "replace.bin").read_bytes(), b"old")

        payload = bytes((handle,)) + struct.pack("<IH", 0, 3) + b"new"
        raw = host.Frame(host.REQUEST, host.CMD_WRITE, 77,
                         payload=payload).pack()
        first = self.service.process(raw)
        self.assertEqual(self.service.process(raw), first)
        self.assertEqual(self.service.handles[handle].position, 3)
        committed = self.request(host.CMD_COMMIT, bytes((handle,)))
        self.assertEqual(committed.status, host.OK)
        self.assertEqual((self.root / "replace.bin").read_bytes(), b"new")

        created = self.request(host.CMD_CREATE, path_payload("/aborted.bin"))
        handle = created.payload[0]
        self.assertEqual(self.request(host.CMD_ABORT, bytes((handle,))).status,
                         host.OK)
        self.assertFalse((self.root / "aborted.bin").exists())
        self.assertFalse(any(self.root.glob(".kcommander-*.tmp")))

    @unittest.skipUnless(hasattr(os, "symlink"), "symlink unavailable")
    def test_symlink_escape_is_denied(self):
        outside = Path(self.temp.name).parent / "kc-outside-test"
        outside.write_text("secret")
        try:
            (self.root / "escape").symlink_to(outside)
            response = self.request(host.CMD_OPEN, path_payload("/escape"))
            self.assertEqual(response.status, host.DENIED)
            listing = self.request(host.CMD_LIST,
                                   path_payload("/") + struct.pack("<IB", 0, 32))
            self.assertNotIn(b"escape", listing.payload)
        finally:
            outside.unlink(missing_ok=True)


if __name__ == "__main__":
    unittest.main()

