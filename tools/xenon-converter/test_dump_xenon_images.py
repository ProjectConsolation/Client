import struct
import unittest

import dump_pc_images
import dump_xenon_images as dump


def image(format_name="DXT1", complete=True):
    block = 8 if format_name == "DXT1" else 16
    data = b"".join(bytes([level]) * block for level in range(3))
    return {"name": "test", "pixel_offset": "0x100", "pixel_bytes": 4096,
        "width": 4, "height": 4, "depth": 1,
        "pc_base_level": {"format": format_name, "data": data[:block].hex()},
        "pc_mip_chain": {"format": format_name, "data": data.hex(),
                         "levels": 3, "decoded_levels": 3 if complete else 1}}


class XboxImageDumpTests(unittest.TestCase):
    def test_source_dds_keeps_largest_mip_first(self):
        value = image()
        data = dump.source_dds(value)
        self.assertEqual(data[128:], bytes.fromhex(value["pc_mip_chain"]["data"]))
        self.assertEqual(struct.unpack_from("<I", data, 28)[0], 3)

    def test_pc_copy_reverses_mips_without_donors(self):
        value = image()
        record = dump.pc_record(value)
        iwi = dump_pc_images.encode_iwi(record)
        self.assertEqual(iwi[28:], b"".join(bytes([level]) * 8 for level in (2, 1, 0)))
        self.assertEqual(dump_pc_images.encode_dds(record), dump.source_dds(value))

    def test_approximated_mips_are_not_exported_as_source(self):
        value = image(complete=False)
        data = dump.source_dds(value)
        self.assertEqual(struct.unpack_from("<I", data, 28)[0], 1)
        self.assertEqual(data[128:], bytes(8))

    def test_dxn_source_remains_bc5(self):
        value = image("DXN")
        data = dump.source_dds(value)
        self.assertEqual(data[84:88], b"ATI2")
        self.assertEqual(data[128:], bytes.fromhex(value["pc_mip_chain"]["data"]))
        converted = dump_pc_images.encode_dds(dump.pc_record(value, True))
        self.assertEqual(converted[84:88], b"DXT5")

    def test_collector_deduplicates_by_source_offset_not_name(self):
        first = image()
        second = dict(first, pixel_offset="0x200")
        result = dump.collect_images({"assets": [first, first, {"nested": second}]})
        self.assertEqual(len(result), 2)

    def test_cubemap_face_order(self):
        value = image()
        value["pc_map_type"] = 5
        value["pc_mip_chain"].update(levels=1, decoded_levels=1, data=b"".join(bytes([i]) * 8 for i in range(6)).hex())
        value["pc_base_level"]["data"] = value["pc_mip_chain"]["data"]
        data = dump.source_dds(value)
        self.assertEqual(data[128:], bytes.fromhex(value["pc_mip_chain"]["data"]))
        self.assertEqual(struct.unpack_from("<I", data, 112)[0], 0xFE00)


if __name__ == "__main__":
    unittest.main()
