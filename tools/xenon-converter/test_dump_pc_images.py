import json
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

import dump_pc_images as dump
import xenon_ff


def record(cube=False, name="test_image"):
    faces = 6 if cube else 1
    header = bytearray(36)
    size = 24 * faces
    struct.pack_into("<2I", header, 0, 5 if cube else 3, xenon_ff.INSERT)
    header[11] = 2
    struct.pack_into("<2I", header, 16, size, size)
    struct.pack_into("<3H", header, 24, 4, 4, 1)
    header[30] = 3
    struct.pack_into("<I", header, 32, xenon_ff.INLINE)
    pixels = b"".join(bytes([level * faces + face]) * 8
                      for level in (2, 1, 0) for face in range(faces))
    blob = bytes(header) + name.encode() + b"\0" + struct.pack("<2B3H4sI", 3, 4 if cube else 0,
        4, 4, 1, b"DXT1", size) + pixels
    return xenon_ff.pc_image_record_at(blob, name, 36)


class ImageDumpTests(unittest.TestCase):
    def test_iwi_preserves_native_pixels_and_picmip_table(self):
        for cube in (False, True):
            value = record(cube)
            data = dump.encode_iwi(value)
            self.assertEqual(data[:4], b"IWi\6")
            self.assertEqual(data[28:], dump.image_parts(value)[-1])
            faces = 6 if cube else 1
            self.assertEqual(struct.unpack_from("<4I", data, 12),
                             tuple(28 + n * 8 * faces for n in (3, 2, 1, 1)))

    def test_dds_face_mip_order(self):
        value = record(True)
        data = dump.encode_dds(value)
        self.assertEqual(data[:4], b"DDS ")
        self.assertEqual(struct.unpack_from("<I", data, 112)[0], 0xFE00)
        self.assertEqual(data[128:], b"".join(bytes([level * 6 + face]) * 8
            for face in range(6) for level in (0, 1, 2)))

    def test_scan_rejects_invalid_layout(self):
        raw = bytearray(record()["serialized"])
        self.assertEqual(list(dump.find_images(raw)), ["test_image"])
        raw[16] ^= 1
        self.assertEqual(dump.find_images(raw), {})

    def test_scan_rejects_conflicting_duplicate(self):
        first = record()["serialized"]
        second = bytearray(first)
        second[-1] ^= 1
        with self.assertRaises(ValueError):
            dump.find_images(first + second)

    def test_windows_filename_encoding_matches_runtime(self):
        self.assertEqual(dump.image_filename("*lightmap0_primary"), "%2Alightmap0_primary")
        self.assertEqual(dump.image_filename("a%2Ab"), "a%252Ab")
        self.assertEqual(dump.image_filename("custom\\brick"), "custom/brick")
        for name in ("../a", "/a", "a//b", "a/..", "nul", "a/COM1", ",identity"):
            with self.assertRaises(ValueError):
                dump.image_filename(name)

    def test_dump_refuses_existing_destination(self):
        with tempfile.TemporaryDirectory(prefix="qos-image-dump-test-") as temporary:
            root = Path(temporary)
            source = root / "fixture.ff"
            payload = record(name="*lightmap0_primary")["serialized"]
            source.write_bytes(struct.pack("<7I", 470, len(payload), 0, 0, 0, 0, 0) + zlib.compress(payload))
            target = root / "dump"
            inventory = dump.dump_images(source, target)
            self.assertEqual(inventory["validated_inline_images"], 1)
            self.assertTrue((target / "dds/%2Alightmap0_primary.dds").is_file())
            self.assertEqual(json.loads((target / "manifest.json").read_text())["images"][0]["name"],
                             "*lightmap0_primary")
            with self.assertRaises(FileExistsError):
                dump.dump_images(source, target)


if __name__ == "__main__":
    unittest.main()
