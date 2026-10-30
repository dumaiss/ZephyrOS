import os
import shutil
import subprocess
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
TWIN = os.path.join(ROOT, "build", "host", "ztrcheck")
SONGS = os.path.join(ROOT, "..", "SNTracker", "songs")


def run(path):
    return subprocess.run([TWIN, path], stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT)


class HostTwinTests(unittest.TestCase):
    def test_both_songs_validate_with_their_stamp(self):
        for name in ("night-market.ztr", "RunawayCircuit.ztr"):
            result = run(os.path.join(SONGS, name))
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(b"OK: ", result.stdout)
            self.assertIn(b"crc=ok ", result.stdout)

    def test_a_covered_byte_flip_is_a_crc_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "flipped.ztr")
            shutil.copy(os.path.join(SONGS, "night-market.ztr"), path)
            with open(path, "r+b") as handle:
                handle.seek(100)
                byte = handle.read(1)
                handle.seek(100)
                handle.write(bytes([byte[0] ^ 0x01]))
            result = run(path)
            self.assertEqual(result.returncode, 4, result.stdout)
            self.assertIn(b"FAIL crc at 0016: MISMATCH stored=",
                          result.stdout)

    def test_an_unstamped_song_still_validates(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "unstamped.ztr")
            shutil.copy(os.path.join(SONGS, "night-market.ztr"), path)
            with open(path, "r+b") as handle:
                handle.seek(0x16)
                handle.write(b"\0\0")
            result = run(path)
            self.assertEqual(result.returncode, 0, result.stdout)
            self.assertIn(b"crc=none", result.stdout)

    def test_a_truncated_song_fails_with_its_offset(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "truncated.ztr")
            with open(os.path.join(SONGS, "night-market.ztr"), "rb") as f:
                data = f.read(200)
            with open(path, "wb") as f:
                f.write(data)
            result = run(path)
            self.assertEqual(result.returncode, 4, result.stdout)
            self.assertIn(b"FAIL ", result.stdout)


if __name__ == "__main__":
    unittest.main()
