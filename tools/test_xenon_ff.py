import struct
import tempfile
import unittest
from pathlib import Path
import zlib

import xenon_ff


class FastfileTests(unittest.TestCase):
    def inspect_blob(self, blob, details=False):
        with tempfile.TemporaryDirectory(prefix="qos-xenon-test-") as directory:
            path = Path(directory) / "fixture.ff"
            path.write_bytes(blob)
            return xenon_ff.inspect(path, details)

    def zone(self, payload, size=None):
        return struct.pack(">7I", 470, len(payload) if size is None else size,
                           64, 0, 64, 0, 0) + zlib.compress(payload)

    def test_rawfile_byte_content_is_not_swapped(self):
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 33, xenon_ff.INLINE)
        payload += struct.pack(">3I", xenon_ff.INLINE, 3, xenon_ff.INLINE)
        payload += b"test/raw\0abc\0"
        report = self.inspect_blob(self.zone(payload), True)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)
        self.assertEqual(report["assets"][0]["name"], "test/raw")
        self.assertEqual(report["assets"][0]["bytes"], 4)

    def test_pc_version_is_distinguished(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "PC zone"):
            self.inspect_blob(struct.pack("<7I", 470, 16, 0, 0, 0, 0, 0))

    def test_size_mismatch(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "declared size"):
            self.inspect_blob(self.zone(bytes(16), 17))

    def test_truncated_zlib(self):
        with self.assertRaises(xenon_ff.FormatError):
            self.inspect_blob(self.zone(bytes(16))[:-1])

    def test_invalid_asset_id(self):
        payload = struct.pack(">6I", 0, 0, 1, xenon_ff.INLINE, 99, xenon_ff.INLINE)
        with self.assertRaisesRegex(xenon_ff.FormatError, "asset type"):
            self.inspect_blob(self.zone(payload))

    def test_unterminated_string(self):
        payload = struct.pack(">5I", 1, xenon_ff.INLINE, 0, 0, xenon_ff.INLINE)
        with self.assertRaisesRegex(xenon_ff.FormatError, "unterminated"):
            self.inspect_blob(self.zone(payload + b"no terminator"))

    def test_unconsumed_payload(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "unconsumed"):
            self.inspect_blob(self.zone(bytes(17)), True)

    def test_reference_does_not_consume_string_bytes(self):
        payload = struct.pack(">6I", 2, xenon_ff.INLINE, 0, 0,
                              xenon_ff.INLINE, 0x40000001) + b"same\0"
        report = self.inspect_blob(self.zone(payload), True)
        self.assertEqual(report["script_strings"],
                         ["same", {"block_reference": "0x40000001"}])


if __name__ == "__main__":
    unittest.main()
