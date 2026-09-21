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

    def test_xsurface_vertex_stream_conversion(self):
        primary = struct.pack(">4f", 1.0, 2.0, 3.0, -1.0)
        attributes = (struct.pack(">2I", 0x11223344, 0x55667788)
                      + struct.pack(">2e", 0.25, 0.75)
                      + struct.pack(">I", 0x99AABBCC))
        secondary = struct.pack(">4f", 4.0, 5.0, 6.0, 7.0)

        verts0, verts1 = xenon_ff.convert_xsurface_vertices(
            primary, attributes, secondary, 1)

        self.assertEqual(struct.unpack_from("<4f", verts0), (1.0, 2.0, 3.0, -1.0))
        self.assertEqual(struct.unpack_from("<2I", verts0, 16), (0x11223344, 0))
        self.assertEqual(struct.unpack_from("<2f", verts0, 24), (0.25, 0.75))
        self.assertEqual(struct.unpack_from("<2I", verts0, 32),
                         (0x99AABBCC, 0x55667788))
        self.assertEqual(struct.unpack("<4f", verts1), (4.0, 5.0, 6.0, 7.0))

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

    def test_pc_asset_type_skips_xenon_pixel_shader_slot(self):
        self.assertEqual(xenon_ff.pc_asset_type(6), 6)
        self.assertIsNone(xenon_ff.pc_asset_type(7))
        self.assertEqual(xenon_ff.pc_asset_type(8), 7)
        self.assertEqual(xenon_ff.pc_asset_type(38), 37)

    def test_report_exposes_pc_layout_mismatches(self):
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 33, xenon_ff.INLINE)
        payload += struct.pack(">3I", xenon_ff.INLINE, 0, 0) + b"empty/raw\0"
        report = self.inspect_blob(self.zone(payload), True)
        self.assertEqual(report["pc_asset_counts"], {"rawfile": 1})
        self.assertEqual(report["incompatible_pc_layouts"]["xsurface"],
                         {"xenon_bytes": 200, "pc_bytes": 80})

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

    def test_minimal_xmodel(self):
        header = bytearray(240)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 5, xenon_ff.INLINE)
        payload += header + b"minimal_model\0"
        report = self.inspect_blob(self.zone(payload), True)
        model = report["assets"][0]
        self.assertEqual(model["name"], "minimal_model")
        self.assertEqual(model["surface_count"], 0)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_xmodel_surface_and_material_reference(self):
        header = bytearray(240)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        header[6] = 1
        struct.pack_into(">I", header, 32, xenon_ff.INLINE)
        struct.pack_into(">I", header, 36, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 5, xenon_ff.INLINE)
        payload += header + b"one_surface\0" + bytes(200)
        payload += struct.pack(">I", 0x40000010)
        report = self.inspect_blob(self.zone(payload), True)
        model = report["assets"][0]
        self.assertEqual(model["surface_count"], 1)
        self.assertEqual(model["materials"], [{"reference": "0x40000010"}])
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_techset_with_empty_pass(self):
        header = bytearray(156)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        struct.pack_into(">I", header, 12, xenon_ff.INLINE)
        technique = bytearray(8)
        struct.pack_into(">I", technique, 0, xenon_ff.INLINE)
        struct.pack_into(">H", technique, 6, 1)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 8, xenon_ff.INLINE)
        payload += header + b"test_techset\0"
        payload += technique + bytes(100) + b"test_technique\0"
        report = self.inspect_blob(self.zone(payload), True)
        techset = report["assets"][0]
        self.assertEqual(techset["techniques"][0]["name"], "test_technique")
        self.assertEqual(techset["techniques"][0]["pass_count"], 1)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_xmodel_empty_physics_geometry(self):
        header = bytearray(240)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        struct.pack_into(">I", header, 232, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 5, xenon_ff.INLINE)
        payload += header + b"physics_model\0" + bytes(20)
        report = self.inspect_blob(self.zone(payload), True)
        physics = report["assets"][0]["physics_geometry"]
        self.assertEqual(physics, {"geometry_count": 0, "shape_count": 0})
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_minimal_com_map(self):
        header = bytearray(44)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 14, xenon_ff.INLINE)
        payload += header + b"maps/mp/test.d3dbsp\0"
        report = self.inspect_blob(self.zone(payload), True)
        com_map = report["assets"][0]
        self.assertEqual(com_map["name"], "maps/mp/test.d3dbsp")
        self.assertEqual(com_map["primary_light_count"], 0)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_minimal_lightdef(self):
        header = bytearray(16)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 19, xenon_ff.INLINE)
        payload += header + b"lights/test\0"
        report = self.inspect_blob(self.zone(payload), True)
        self.assertEqual(report["assets"][0]["name"], "lights/test")
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_minimal_game_map_mp(self):
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 16, xenon_ff.INLINE)
        payload += struct.pack(">I", xenon_ff.INLINE) + b"mp_test\0"
        report = self.inspect_blob(self.zone(payload), True)
        self.assertEqual(report["assets"][0]["name"], "mp_test")
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_minimal_gfx_map(self):
        header = bytearray(828)
        struct.pack_into(">I", header, 4, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 18, xenon_ff.INLINE)
        payload += header + b"mp_test\0"
        report = self.inspect_blob(self.zone(payload), True)
        gfx_map = report["assets"][0]
        self.assertEqual(gfx_map["name"], "mp_test")
        self.assertEqual(gfx_map["zero_fill_fields"], 0)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_minimal_col_map_mp(self):
        header = bytearray(324)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 13, xenon_ff.INLINE)
        payload += header + b"maps/mp/test.d3dbsp\0"
        report = self.inspect_blob(self.zone(payload), True)
        col_map = report["assets"][0]
        self.assertEqual(col_map["name"], "maps/mp/test.d3dbsp")
        self.assertEqual(col_map["brush_count"], 0)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_sound_with_minimal_alias(self):
        sound_header = struct.pack(">3I", xenon_ff.INLINE,
                                   xenon_ff.INLINE, 1)
        alias = bytearray(96)
        struct.pack_into(">I", alias, 0, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 10, xenon_ff.INLINE)
        payload += sound_header + b"weapons/test\0" + alias + b"test_alias\0"
        report = self.inspect_blob(self.zone(payload), True)
        sound = report["assets"][0]
        self.assertEqual(sound["name"], "weapons/test")
        self.assertEqual(sound["aliases"][0]["name"], "test_alias")
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_sound_rejects_impossible_alias_count(self):
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 10, xenon_ff.INLINE)
        payload += struct.pack(">3I", xenon_ff.INLINE,
                               xenon_ff.INLINE, 2) + b"bad_sound\0"
        with self.assertRaisesRegex(xenon_ff.FormatError,
                                    "invalid alias count"):
            self.inspect_blob(self.zone(payload), True)

    def test_sound_with_streamed_file(self):
        sound_header = struct.pack(">3I", xenon_ff.INLINE,
                                   xenon_ff.INLINE, 1)
        alias = bytearray(96)
        struct.pack_into(">II", alias, 0, xenon_ff.INLINE,
                         0)
        struct.pack_into(">I", alias, 16, xenon_ff.INLINE)
        sound_file = bytearray(20)
        struct.pack_into(">I", sound_file, 8, xenon_ff.INLINE)
        sound_file[16] = 3
        streamed = struct.pack(">3I", xenon_ff.INLINE,
                               xenon_ff.INLINE, 3)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 10, xenon_ff.INLINE)
        payload += sound_header + b"weapons/streamed\0" + alias
        payload += b"streamed_alias\0" + sound_file
        payload += streamed + b"streamed_file\0abc"
        report = self.inspect_blob(self.zone(payload), True)
        sound_file = report["assets"][0]["aliases"][0]["sound_file"]
        self.assertEqual(sound_file["type"], 3)
        self.assertEqual(sound_file["streamed"]["data_bytes"], 3)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_sound_speaker_map_reads_both_channel_records(self):
        sound_header = struct.pack(">3I", xenon_ff.INLINE,
                                   xenon_ff.INLINE, 1)
        alias = bytearray(96)
        struct.pack_into(">I", alias, 92, xenon_ff.INLINE)
        speaker_map = bytearray(56)
        struct.pack_into(">I", speaker_map, 4, xenon_ff.INLINE)
        speaker_map[16] = 3
        struct.pack_into(">I", speaker_map, 20, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 10, xenon_ff.INLINE)
        payload += sound_header + b"weapons/speaker\0" + alias
        payload += speaker_map + b"speaker_map\0" + bytes(24)
        report = self.inspect_blob(self.zone(payload), True)
        channel_maps = report["assets"][0]["aliases"][0]["speaker_map"]["channel_maps"]
        self.assertEqual(channel_maps[0]["bytes"], 24)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_fx_with_inline_visual_array_and_trail(self):
        header = bytearray(32)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        struct.pack_into(">I", header, 16, 1)
        struct.pack_into(">I", header, 28, xenon_ff.INLINE)
        element = bytearray(252)
        element[176] = 8
        element[177] = 2
        struct.pack_into(">I", element, 188, xenon_ff.INLINE)
        struct.pack_into(">I", element, 244, xenon_ff.INLINE)
        visuals = struct.pack(">2I", xenon_ff.INLINE, xenon_ff.INLINE)
        trail = bytearray(28)
        struct.pack_into(">II", trail, 12, 1, xenon_ff.INLINE)
        struct.pack_into(">II", trail, 20, 2, xenon_ff.INLINE)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 27, xenon_ff.INLINE)
        payload += header + b"fx/test\0" + element + visuals
        payload += b"visual_a\0visual_b\0" + trail + bytes(24)
        report = self.inspect_blob(self.zone(payload), True)
        effect = report["assets"][0]
        self.assertEqual(effect["name"], "fx/test")
        self.assertEqual(effect["element_count"], 1)
        self.assertEqual(effect["elements"][0]["visual_count"], 2)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)


if __name__ == "__main__":
    unittest.main()
