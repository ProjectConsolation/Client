import struct
import tempfile
import unittest
from unittest import mock
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

    def test_xenos_gpu_endian_modes(self):
        data = bytes(range(8))
        self.assertEqual(xenon_ff.apply_xenos_gpu_endian(data, 0), data)
        self.assertEqual(xenon_ff.apply_xenos_gpu_endian(data, 1),
                         bytes((1, 0, 3, 2, 5, 4, 7, 6)))
        self.assertEqual(xenon_ff.apply_xenos_gpu_endian(data, 2),
                         bytes((3, 2, 1, 0, 7, 6, 5, 4)))
        self.assertEqual(xenon_ff.apply_xenos_gpu_endian(data, 3),
                         bytes((2, 3, 0, 1, 6, 7, 4, 5)))

    def test_xenos_texture_layout_matches_observed_canals_dxt1(self):
        layout = xenon_ff._xenos_texture_layout(256, 256, 0x12)
        self.assertEqual(layout[4:], (512, 32768, 32768))

    def test_xenos_dxt1_tile_untile_round_trip_with_gpu_endian(self):
        width, height = 68, 36
        linear_size = xenon_ff._xenos_texture_layout(width, height, 0x12)[5]
        linear = bytes((index * 37 + 11) & 0xFF for index in range(linear_size))

        tiled = xenon_ff.tile_xenos_texture(
            width, height, 0x12, linear, endian=1)
        restored = xenon_ff.untile_xenos_texture(
            width, height, 0x12 | (1 << 6), tiled)

        self.assertEqual(restored, linear)

    def test_xenos_mip_chain_converts_independently_tiled_levels(self):
        levels = []
        tiled = bytearray()
        for level, (width, height) in enumerate(((64, 64), (32, 32))):
            size = xenon_ff._xenos_texture_layout(width, height, 0x12)[5]
            linear = bytes(((index * 13) + level) & 0xFF for index in range(size))
            levels.append(linear)
            tiled.extend(xenon_ff.tile_xenos_texture(width, height, 0x12, linear))

        restored, decoded_levels = xenon_ff.untile_xenos_texture_levels(
            64, 64, 0x12, bytes(tiled), 2)

        self.assertEqual(decoded_levels, 2)
        self.assertEqual(restored, b"".join(levels))

    def test_xenos_texture_rejects_unknown_format(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "unsupported Xenos"):
            xenon_ff.untile_xenos_texture(64, 64, 0x2F, bytes(4096))

    def test_world_texture_summary(self):
        world = {"geometry": {"surface_materials": [
            {"textures": [
                {"name": "a", "pc_base_level": {
                    "format": "DXT1", "bytes": 32, "sha256": "a"}},
                {"name": "b", "pc_base_level_error": "bad texture"},
            ]},
            {"reference": "0x40000001"},
            {"textures": [
                {"name": "a", "pc_base_level": {
                    "format": "DXT4_5", "bytes": 64, "sha256": "b"}},
            ]},
        ]}}

        summary = xenon_ff.summarize_world_textures(world)

        self.assertEqual(summary["surface_count"], 3)
        self.assertEqual(summary["inline_materials"], 2)
        self.assertEqual(summary["packed_material_references"], 1)
        self.assertEqual(summary["inline_images"], 3)
        self.assertEqual(summary["unique_image_names"], 2)
        self.assertEqual(summary["decoded_base_levels"], 2)
        self.assertEqual(summary["decoded_base_bytes"], 96)
        self.assertEqual(summary["formats"], {"DXT1": 1, "DXT4_5": 1})
        self.assertEqual(summary["errors"], [
            {"name": "b", "error": "bad texture"}])

    def test_pc_image_base_level_serialization(self):
        payload = bytearray()
        image = {
            "name": "test_image", "width": 8, "height": 4, "depth": 1,
            "pc_base_level": {
                "format": "DXT1", "bytes": 16,
                "sha256": "unused", "data": bytes(range(16)).hex(),
            },
        }

        xenon_ff.write_pc_image(payload, image)

        header = payload[:36]
        self.assertEqual(struct.unpack_from("<2I", header),
                         (3, xenon_ff.INSERT))
        self.assertEqual(header[11], 2)
        self.assertEqual(struct.unpack_from("<2I", header, 16), (16, 16))
        self.assertEqual(struct.unpack_from("<3H", header, 24), (8, 4, 1))
        self.assertEqual(header[30], 3)
        self.assertEqual(struct.unpack_from("<I", header, 32)[0],
                         xenon_ff.INLINE)
        load_offset = 36 + len("test_image") + 1
        self.assertEqual(
            struct.unpack_from("<2B3H4sI", payload, load_offset),
            (0, 0, 8, 4, 1, b"DXT1", 16))
        self.assertEqual(payload[load_offset + 16:], bytes(range(16)))

    def test_pc_image_serialization_transcodes_xbox_dxn(self):
        image = {
            "name": "normal", "width": 4, "height": 4, "depth": 1,
            "pc_base_level": {
                "format": "DXN", "bytes": 16,
                "sha256": "unused", "data": bytes(16).hex(),
            },
        }
        payload = bytearray()

        xenon_ff.write_pc_image(payload, image)

        load_offset = 36 + len("normal") + 1
        self.assertEqual(
            struct.unpack_from("<2B3H4sI", payload, load_offset)[5], b"DXT5")
        self.assertEqual(len(payload[load_offset + 16:]), 16)

    def test_dxn_to_dxt5_preserves_x_block_and_encodes_green(self):
        x_block = bytes((240, 16, 0, 0, 0, 0, 0, 0))
        y_block = bytes((220, 20, 0, 0, 0, 0, 0, 0))

        converted = xenon_ff.transcode_dxn_to_dxt5(x_block + y_block)

        self.assertEqual(converted[:8], x_block)
        color0, color1 = struct.unpack_from("<2H", converted, 8)
        self.assertGreater(color0, color1)
        self.assertEqual(color0 & 0xF81F, 0)
        self.assertEqual(color1 & 0xF81F, 0)

    def test_pc_material_serialization_uses_manifest_aliases(self):
        source = bytearray(96)
        source[4:8] = b"\x01\x02\x03\x04"
        struct.pack_into(">3I", source, 8, 1, 2, 3)
        source[20:60] = bytes(range(40))
        source[60:63] = bytes((1, 1, 1))
        definition = struct.pack(">I4B", 0x12345678, 1, 2, 3, 2) + struct.pack(">I", 1)
        material = {
            "header": source.hex(), "name": "test_material",
            "techset_name": "test_techset",
            "textures": [{"definition": definition.hex(), "name": "test_image"}],
            "constants": (bytes(range(32))).hex(),
            "state_bits": bytes(range(8)).hex(),
        }
        payload = bytearray()

        xenon_ff.write_pc_material(
            payload, material, 0x40000101, [0x40000201])

        header = payload[:104]
        self.assertEqual(struct.unpack_from("<I", header)[0], xenon_ff.INLINE)
        self.assertEqual(header[67:70], bytes((1, 1, 1)))
        self.assertEqual(struct.unpack_from("<4I", header, 84),
                         (0x40000101, xenon_ff.INLINE,
                          xenon_ff.INLINE, xenon_ff.INLINE))
        self.assertEqual(header[20:60], bytes(range(40)))
        nested = 104 + len("test_material") + 1
        self.assertEqual(struct.unpack_from("<I", payload, nested)[0],
                         0x12345678)
        self.assertEqual(struct.unpack_from("<I", payload, nested + 8)[0],
                         0x40000201)

    def test_pc_material_serialization_uses_filtered_texture_count(self):
        source = bytearray(96)
        source[60] = 4
        material = {
            "header": source.hex(), "name": "filtered",
            "techset_name": "test_techset", "textures": [],
        }
        payload = bytearray()

        xenon_ff.write_pc_material(payload, material, 0x40000101, [])

        self.assertEqual(payload[67], 0)

    def test_pc_load_zone_preserves_2d_materials_images_and_rawfile(self):
        source = bytearray(96)
        source[16:20] = bytes.fromhex("002b0101")
        source[60] = 1
        definition = struct.pack(">I4B", 0, 0, 0, 0, 2) + struct.pack(">I", 1)
        image = {
            "name": "loadscreen_mp_test",
            "width": 4, "height": 4, "depth": 1,
            "load_definition": {"levels": 1},
            "pc_base_level": {
                "width": 4, "height": 4, "depth": 1,
                "format": "DXT1", "bytes": 8, "data": bytes(8).hex(),
            },
        }
        report = {
            "script_strings": [],
            "assets": [
                {"type": "techset", "name": ",2d"},
                {
                    "type": "material", "header": source.hex(),
                    "name": "$levelbriefing",
                    "textures": [{**image, "definition": definition.hex()}],
                },
                {
                    "type": "rawfile", "name": "mp_test_load",
                    "data": b"\0".hex(),
                },
            ],
        }

        with mock.patch.object(xenon_ff, "inspect", return_value=report):
            converted = xenon_ff.build_pc_load_zone("unused.ff")

        version, payload_size = struct.unpack_from("<2I", converted)
        decoder = zlib.decompressobj()
        payload = decoder.decompress(converted[28:])
        self.assertEqual(version, 470)
        self.assertEqual(payload_size, len(payload))
        self.assertTrue(decoder.eof)
        self.assertEqual(len(converted) % 32, 0)
        self.assertEqual(struct.unpack_from("<4I", payload),
                         (0, 0, 4, xenon_ff.INLINE))
        self.assertEqual([entry[0] for entry in struct.iter_unpack(
            "<2I", payload[16:48])], [7, 7, 6, 32])
        material_offset = 48 + 184 + len(",sm2/2d") + 1 + 184 + len(",2d") + 1
        self.assertEqual(payload[material_offset + 16:material_offset + 20],
                         bytes.fromhex("002b0101"))
        self.assertEqual(struct.unpack_from("<I", payload, material_offset + 84),
                         (0x4000000D,))
        image_offset = material_offset + 104 + len("$levelbriefing") + 1 + 12
        self.assertEqual(payload[image_offset + 10:image_offset + 12], b"\x01\x00")
        load_offset = image_offset + 36 + len("loadscreen_mp_test") + 1
        self.assertEqual(payload[load_offset:load_offset + 2], b"\x01\x02")
        self.assertIn(b",2d\0", payload)
        self.assertIn(b"$levelbriefing\0", payload)
        self.assertIn(b"loadscreen_mp_test\0", payload)
        self.assertIn(b"mp_test_load\0\0", payload)

    def test_material_constant_conversion_preserves_ascii_name(self):
        source = (struct.pack(">I", 0x12345678)
                  + b"colorTint\0\0\0"
                  + struct.pack(">4f", 1.0, 0.5, -1.0, 0.0))

        converted = xenon_ff._convert_material_constants(source)

        self.assertEqual(struct.unpack_from("<I", converted)[0], 0x12345678)
        self.assertEqual(converted[4:16], b"colorTint\0\0\0")
        self.assertEqual(struct.unpack_from("<4f", converted, 16),
                         (1.0, 0.5, -1.0, 0.0))

    def test_material_constant_conversion_rejects_partial_record(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "material constant"):
            xenon_ff._convert_material_constants(bytes(31))

    def test_pc_techset_selection_prefers_exact_and_similar_channels(self):
        self.assertEqual(
            xenon_ff.select_pc_techset(",wc_l_sm_b0c0n0s0p0"),
            ",wc_l_sm_b0c0n0s0p0")
        self.assertEqual(
            xenon_ff.select_pc_techset("wc_l_sm_b0c0d0n0s0p0"),
            "wc_l_sm_b0c0n0s0p0")
        self.assertEqual(
            xenon_ff.select_pc_techset("wc_l_sm_r0c0d0n0s0"),
            "wc_l_sm_b0c0n0s0p0")
        self.assertIsNone(xenon_ff.select_pc_techset("wc_water"))

    def test_gfx_surface_material_references_target_earlier_slots(self):
        surfaces = bytes(4 * 72)
        base = 0x12340
        first = {"name": "first"}
        second = {"name": "second"}
        materials = [
            first,
            {"reference": hex(0x40000001 + base + 40)},
            second,
            {"reference": hex(0x40000001 + base + 2 * 72 + 40)},
        ]

        resolved, resolved_base = xenon_ff.resolve_gfx_surface_materials(
            surfaces, materials)

        self.assertEqual(resolved_base, base)
        self.assertIs(resolved[1], first)
        self.assertIs(resolved[3], second)

    def test_material_image_references_reuse_decoded_predecessor(self):
        definition = bytes.fromhex("59d30d0f6e700b05ffffffff")
        decoded = {
            "name": "wall_n", "definition": definition.hex(),
            "pc_base_level": {"format": "DXN"},
        }
        materials = [
            {"textures": [decoded]},
            {"textures": [{
                "definition": definition.hex(), "reference": "0x40500001",
            }]},
        ]

        report = xenon_ff.resolve_material_image_references(materials)

        resolved = materials[1]["textures"][0]
        self.assertEqual(resolved["name"], "wall_n")
        self.assertEqual(resolved["resolved_reference"], "0x40500001")
        self.assertEqual(report["resolved_image_references"], 1)

    def test_material_image_first_external_reference_stays_external(self):
        definition = bytes.fromhex("34ecccb373700b08ffffffff")
        materials = [{"textures": [{
            "definition": definition.hex(), "reference": "0x4008184d",
        }]}]

        report = xenon_ff.resolve_material_image_references(materials)

        self.assertNotIn("name", materials[0]["textures"][0])
        self.assertEqual(report["external_image_references"], ["0x4008184d"])

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

    def test_xsurface_header_conversion(self):
        source = bytearray(200)
        source[0:2] = b"\x03\x01"
        struct.pack_into(">2H", source, 2, 7, 11)
        source[6:8] = b"\x05\x06"
        struct.pack_into(">4h", source, 12, 1, 2, 3, 4)
        struct.pack_into(">I", source, 136, 9)
        struct.pack_into(">6I", source, 176, *range(10, 16))
        surface = {
            "header": source.hex(),
            "indices": "00",
            "blend_indices": "00",
            "blend_vertices": "00",
            "pc_vertices": "00",
            "pc_secondary_vertices": None,
            "rigid_vertices": "00",
        }

        converted = xenon_ff.convert_xsurface_header(surface)

        self.assertEqual(len(converted), 80)
        self.assertEqual(converted[:2], b"\x03\x01")
        self.assertEqual(struct.unpack_from("<2H", converted, 2), (7, 11))
        self.assertEqual(converted[6:8], b"\x05\x06")
        self.assertEqual(struct.unpack_from("<4h", converted, 12), (1, 2, 3, 4))
        self.assertEqual(struct.unpack_from("<7I", converted, 20),
                         (xenon_ff.INLINE, xenon_ff.INLINE,
                          xenon_ff.INLINE, 0, 0, 0, 9))
        self.assertEqual(struct.unpack_from("<I", converted, 48)[0],
                         xenon_ff.INLINE)
        self.assertEqual(struct.unpack_from("<I", converted, 52)[0], 0)
        self.assertEqual(struct.unpack_from("<6I", converted, 56),
                         tuple(range(10, 16)))

    def test_xsurface_header_clears_counts_for_missing_geometry(self):
        source = bytearray(200)
        struct.pack_into(">2H", source, 2, 7, 4)
        struct.pack_into(">4h", source, 12, 1, 2, 3, 4)
        struct.pack_into(">I", source, 136, 9)
        surface = {
            "header": source.hex(),
            "indices": None,
            "blend_indices": None,
            "blend_vertices": None,
            "pc_vertices": None,
            "pc_secondary_vertices": None,
            "rigid_vertices": None,
        }

        converted = xenon_ff.convert_xsurface_header(surface)

        self.assertEqual(struct.unpack_from("<2H", converted, 2), (0, 0))
        self.assertEqual(struct.unpack_from("<4h", converted, 12), (0, 0, 0, 0))
        self.assertEqual(struct.unpack_from("<I", converted, 44), (0,))

    def test_xsurface_header_conversion_rejects_wrong_size(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "xsurface header"):
            xenon_ff.convert_xsurface_header({"header": bytes(199).hex()})

    def test_xmodel_header_conversion_strips_collision_and_physics(self):
        source = bytearray(240)
        source[4:8] = bytes((2, 1, 3, 4))
        struct.pack_into(">I", source, 176, 0x12345678)
        struct.pack_into(">7I", source, 184, *range(1, 8))
        struct.pack_into(">Hh", source, 212, 2, -1)
        struct.pack_into(">I", source, 220, 99)
        source[224:228] = bytes((5, 1, 0, 0))
        model = {
            "header": source.hex(),
            "bone_names": "00",
            "parent_list": None,
            "quaternions": None,
            "translations": None,
            "part_classification": "00",
            "base_matrices": "00",
            "surfaces": [{}],
            "materials": [{}],
            "bone_info": "00",
        }

        converted = xenon_ff.convert_xmodel_header(model)

        self.assertEqual(len(converted), 240)
        self.assertEqual(struct.unpack_from("<I", converted)[0], xenon_ff.INLINE)
        self.assertEqual(converted[4:8], bytes((2, 1, 3, 4)))
        self.assertEqual(struct.unpack_from("<8I", converted, 8),
                         (xenon_ff.INLINE, 0, 0, 0, xenon_ff.INLINE,
                          xenon_ff.INLINE, xenon_ff.INLINE, xenon_ff.INLINE))
        self.assertEqual(struct.unpack_from("<3I", converted, 168),
                         (0, 0, 0x12345678))
        self.assertEqual(struct.unpack_from("<7I", converted, 184),
                         tuple(range(1, 8)))
        self.assertEqual(struct.unpack_from("<Hh", converted, 212), (2, -1))
        self.assertEqual(struct.unpack_from("<I", converted, 220)[0], 99)
        self.assertEqual(converted[224:228], bytes((5, 1, 0, 0)))
        self.assertEqual(converted[228:240], bytes(12))

    def test_pc_xmodel_probe_omits_xenon_gpu_streams(self):
        source = bytearray(240)
        source[4:8] = bytes((1, 0, 1, 0))
        surface_source = bytearray(200)
        surface = {
            "header": surface_source.hex(),
            "blend_indices": "0001",
            "blend_vertices": bytes(48).hex(),
            "pc_vertices": bytes(40).hex(),
            "pc_secondary_vertices": bytes(16).hex(),
            "rigid_vertices": bytes(8).hex(),
            "indices": bytes(6).hex(),
        }
        model = {
            "header": source.hex(),
            "name": "m",
            "bone_names": "0001",
            "parent_list": "00",
            "quaternions": bytes(8).hex(),
            "translations": bytes(16).hex(),
            "part_classification": "00",
            "base_matrices": bytes(32).hex(),
            "surfaces": [surface],
            "materials": [{}],
            "bone_info": bytes(40).hex(),
        }
        payload = bytearray()

        xenon_ff.write_pc_xmodel(payload, model)

        surface_offset = 240 + 2 + 2 + 1 + 8 + 16 + 1 + 32
        material_array_offset = surface_offset + 80
        converted_surface = payload[surface_offset:material_array_offset]
        self.assertEqual(struct.unpack_from("<2H", converted_surface, 2), (0, 0))
        self.assertEqual(struct.unpack_from("<7I", converted_surface, 20),
                         (0, 0, 0, 0, 0, 0, 0))
        self.assertEqual(struct.unpack_from("<I", payload, material_array_offset)[0],
                         xenon_ff.INLINE)

    def test_static_model_draw_conversion(self):
        source = (struct.pack(">4f", 1500.0, 64.0, 1604.0, -48.0)
                  + struct.pack(">3I", 0x0007FC00, 0x00000201, 0x1FF00000)
                  + struct.pack(">fI", 1.0, 0x4000062D)
                  + b"\x01\x00\x00\x00")

        converted = xenon_ff.convert_static_model_draws(source)

        self.assertEqual(len(converted), 64)
        self.assertEqual(struct.unpack_from("<4f", converted),
                         (1500.0, 64.0, 1604.0, -48.0))
        self.assertAlmostEqual(struct.unpack_from("<f", converted, 16)[0], 0.0)
        self.assertAlmostEqual(struct.unpack_from("<f", converted, 20)[0], 1.0)
        self.assertAlmostEqual(struct.unpack_from("<f", converted, 24)[0], 0.0)
        self.assertAlmostEqual(struct.unpack_from("<f", converted, 28)[0], -1.0)
        self.assertAlmostEqual(struct.unpack_from("<f", converted, 32)[0], 0.0)
        self.assertAlmostEqual(struct.unpack_from("<f", converted, 36)[0], 0.0)
        self.assertEqual(struct.unpack_from("<3f", converted, 40), (0.0, 0.0, 1.0))
        self.assertEqual(struct.unpack_from("<fI", converted, 52),
                         (1.0, 0x4000062D))
        self.assertEqual(converted[60:64], b"\x01\x00\x00\x00")

    def test_static_model_draw_conversion_rejects_partial_record(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "static-model draw"):
            xenon_ff.convert_static_model_draws(bytes(39))

    def test_static_model_draw_relocates_model_pointer(self):
        source = bytes(32) + struct.pack(">I", 0x4000062D) + bytes(4)

        converted = xenon_ff.convert_static_model_draws(
            source, [0x4000088D])

        self.assertEqual(struct.unpack_from("<I", converted, 56)[0],
                         0x4000088D)

    def test_static_model_draw_rejects_pointer_count_mismatch(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "pointer count"):
            xenon_ff.convert_static_model_draws(bytes(40), [])

    def test_static_model_instance_conversion(self):
        source = (struct.pack(">6fI", -1.0, -2.0, -3.0, 1.0, 2.0, 3.0,
                              0x004C4C65)
                  + b"\x01\x00\x00\x00")

        converted = xenon_ff.convert_static_model_instances(source)

        self.assertEqual(struct.unpack_from("<6fI", converted),
                         (-1.0, -2.0, -3.0, 1.0, 2.0, 3.0, 0x004C4C65))
        self.assertEqual(converted[28:32], b"\x01\x00\x00\x00")

    def test_static_model_instance_conversion_rejects_partial_record(self):
        with self.assertRaisesRegex(xenon_ff.FormatError, "static-model instance"):
            xenon_ff.convert_static_model_instances(bytes(31))

    def test_manifest_reference_resolution(self):
        entries = [(8, xenon_ff.INLINE), (5, xenon_ff.INLINE),
                   (5, xenon_ff.INLINE), (6, xenon_ff.INLINE),
                   (5, xenon_ff.INLINE)]
        pointers = [0x4000062D, 0x40000635, 0x40000645]

        indices, base = xenon_ff.resolve_manifest_references(pointers, entries, 5)

        self.assertEqual(indices, [1, 2, 4])
        self.assertEqual(base, 0x624)

    def test_pc_gfx_world_geometry_layout(self):
        plane = struct.pack(">4f4B", 1.0, 2.0, 3.0, 4.0, 5, 6, 7, 8)
        surface = bytearray(72)
        struct.pack_into(">IIHHI", surface, 0, 9, 10, 11, 12, 13)
        surface[44:48] = bytes((14, 15, 16, 17))
        struct.pack_into(">6f", surface, 48, 18.0, 19.0, 20.0,
                         21.0, 22.0, 23.0)
        vertex = struct.pack(">11I", *range(24, 35))
        tree_raw = bytearray(48)
        struct.pack_into(">4I", tree_raw, 32, 1, xenon_ff.INLINE, 0, 0)
        cell_raw = bytearray(52)
        struct.pack_into(">6f", cell_raw, 0, -1.0, -2.0, -3.0,
                         1.0, 2.0, 3.0)
        struct.pack_into(">I", cell_raw, 24, xenon_ff.INLINE)
        cell_raw[44] = 1
        struct.pack_into(">I", cell_raw, 48, xenon_ff.INLINE)
        asset = {
            "name": "mp_test",
            "world_name": "maps/mp/test.d3dbsp",
            "names": [{"block_reference": "0x40000001"}, "mp_test"],
            "geometry": {
                "planes": plane.hex(),
                "nodes": struct.pack(">H", 36).hex(),
                "indices": struct.pack(">H", 37).hex(),
                "surfaces": surface.hex(),
                "brush_models": struct.pack(">42I", *range(39, 81)).hex(),
                "dpvs_worlds": struct.pack(">15I", *range(81, 96)).hex(),
                "static_model_draws": bytes(40).hex(),
                "static_model_insts": bytes(32).hex(),
                "pc_static_model_pointers": [0x4000088D],
                "cells": [{
                    "raw": cell_raw.hex(),
                    "tree": {"raw": tree_raw.hex(),
                             "indexes": struct.pack(">I", 54).hex(),
                             "children": []},
                    "portals": [],
                    "cull_groups": "",
                    "reflection_probes": "37",
                }],
                "sky_start_surfs": struct.pack(">I", 38).hex(),
                "vertices": vertex.hex(),
                "vertex_layers": "2728",
            },
        }
        payload = bytearray()
        xenon_ff.write_pc_gfx_world(payload, asset, 3)
        header = payload[:728]
        self.assertEqual([struct.unpack_from("<I", header, offset)[0]
                          for offset in (8, 16, 24, 32, 60, 80, 92, 252,
                                         276, 280, 284, 288, 292, 352)],
                         [1, 1, 1, 1, 1, 1, 2, 3, 0, 0, 0, 1, 1, 1])
        self.assertIn(struct.pack("<4f4B", 1.0, 2.0, 3.0, 4.0,
                                  5, 6, 7, 8), payload)
        self.assertIn(struct.pack("<IIHHI", 9, 10, 11, 12, 13), payload)
        self.assertIn(struct.pack("<11I", *range(24, 35)), payload)
        self.assertIn(struct.pack("<42I", *range(39, 81)), payload)
        self.assertIn(struct.pack("<15I", *range(81, 96)), payload)
        self.assertIn(struct.pack("<6f", -1.0, -2.0, -3.0,
                                  1.0, 2.0, 3.0), payload)
        pc_cell = xenon_ff._pc_gfx_cell_header(
            asset["geometry"]["cells"][0], False)
        self.assertEqual(struct.unpack_from("<I", pc_cell, 24)[0], 0)
        empty_tree_cell = xenon_ff._pc_gfx_cell_header(
            asset["geometry"]["cells"][0], False, True)
        self.assertEqual(struct.unpack_from("<I", empty_tree_cell, 24)[0],
                         xenon_ff.INLINE)
        self.assertEqual(empty_tree_cell[44], 0)
        self.assertEqual(struct.unpack_from("<I", empty_tree_cell, 48)[0], 0)
        self.assertIn(empty_tree_cell, payload)
        self.assertNotIn(xenon_ff._pc_gfx_aabb_header(
            asset["geometry"]["cells"][0]["tree"]), payload)
        self.assertIn(b",white\0", payload)

        shared_payload = bytearray()
        xenon_ff.write_pc_gfx_world(
            shared_payload, asset, 3, [0x40001235])
        surface_offset = shared_payload.find(
            struct.pack("<IIHHI", 9, 10, 11, 12, 13))
        self.assertNotEqual(surface_offset, -1)
        self.assertEqual(
            struct.unpack_from("<I", shared_payload, surface_offset + 16)[0],
            0x40001235)
        self.assertNotIn(b",white\0", shared_payload)

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

    def test_col_map_captures_collision_arrays(self):
        header = bytearray(324)
        struct.pack_into(">I", header, 0, xenon_ff.INLINE)
        struct.pack_into(">2I", header, 8, 1, xenon_ff.INLINE)
        struct.pack_into(">2I", header, 24, 1, xenon_ff.INLINE)
        plane = struct.pack(">4f4B", 1.0, 0.0, 0.0, 32.0, 0, 0, 0, 0)
        material = b"concrete\0".ljust(64, b"\0") + struct.pack(">2I", 4, 8)
        payload = struct.pack(">4I", 0, 0, 1, xenon_ff.INLINE)
        payload += struct.pack(">2I", 13, xenon_ff.INLINE)
        payload += header + b"maps/mp/test.d3dbsp\0" + plane + material

        report = self.inspect_blob(self.zone(payload), True)
        collision = report["assets"][0]["collision"]
        self.assertEqual(bytes.fromhex(collision["planes"]["data"]), plane)
        self.assertEqual(bytes.fromhex(collision["materials"]["data"]), material)
        self.assertEqual(report["unconsumed_payload_bytes"], 0)

    def test_pc_gfx_world_preserves_dpvs_draw_records(self):
        asset = {
            "world_name": "maps/mp/test.d3dbsp",
            "name": "mp_test",
            "geometry": {
                "planes": "", "nodes": "", "indices": "", "surfaces": "",
                "brush_models": bytes(4 * 168).hex(),
                "dpvs_worlds": bytes(4 * 60).hex(),
                "sky_start_surfs": "", "vertices": "", "vertex_layers": "",
                "static_model_draws": "", "static_model_insts": "",
                "cells": [],
            },
        }
        payload = bytearray()
        xenon_ff.write_pc_gfx_world(payload, asset, 0)
        self.assertEqual(struct.unpack_from("<I", payload, 232),
                         (xenon_ff.INLINE,))
        self.assertEqual(struct.unpack_from("<2I", payload, 352),
                         (4, xenon_ff.INLINE))
        self.assertEqual(struct.unpack_from("<2I", payload, 360),
                         (4, xenon_ff.INLINE))
        self.assertEqual(struct.unpack_from("<2I", payload, 664),
                         (xenon_ff.INLINE, xenon_ff.INLINE))
        self.assertEqual(struct.unpack_from("<2I", payload, 576), (1, 1))
        self.assertEqual(struct.unpack_from("<8I", payload, 584),
                         (xenon_ff.INLINE,) * 8)
        self.assertEqual(struct.unpack_from("<2I", payload, 680),
                         (xenon_ff.INLINE, xenon_ff.INLINE))
        self.assertEqual(struct.unpack_from("<4I", payload, 616),
                         (xenon_ff.INLINE,) * 4)
        self.assertEqual(struct.unpack_from("<I", payload, 700),
                         (xenon_ff.INLINE,))
        self.assertEqual(struct.unpack_from("<I", payload, 712), (0,))
        names = (b"maps/mp/test.d3dbsp\0mp_test\0")
        self.assertEqual(len(payload), 728 + len(names) + 68 + 4 * 168 + 4 * 60)
        self.assertEqual(payload[-(4 * 168 + 4 * 60):],
                         bytes(4 * 168 + 4 * 60))

        lit_payload = bytearray()
        xenon_ff.write_pc_gfx_world(lit_payload, asset, 3)
        self.assertEqual(struct.unpack_from("<I", lit_payload, 712),
                         (xenon_ff.INLINE,))
        self.assertEqual(lit_payload[-36:], bytes(36))

    def test_pc_map_probe_preserves_entities_and_root_names(self):
        model = bytearray(240)
        struct.pack_into(">I", model, 0, xenon_ff.INLINE)
        com = bytearray(44)
        struct.pack_into(">I", com, 0, xenon_ff.INLINE)
        game = struct.pack(">I", xenon_ff.INLINE)
        clip = bytearray(324)
        struct.pack_into(">II", clip, 0, xenon_ff.INLINE, 1)
        visibility = b"\xff\xff\xff\xff"
        struct.pack_into(">3I", clip, 164, 1, len(visibility), xenon_ff.INLINE)
        struct.pack_into(">I", clip, 180, xenon_ff.INLINE)
        entities = (b'{\n"classname" "worldspawn"\n}\n'
                    b'{\n"classname" "script_model"\n"model" "*1"\n}\0')
        map_ents = struct.pack(">3I", xenon_ff.INLINE, xenon_ff.INLINE,
                               len(entities))
        gfx = bytearray(828)
        struct.pack_into(">II", gfx, 0, xenon_ff.INLINE, xenon_ff.INLINE)
        rawfile_data = b"main() {\n}\n\0"
        rawfile = struct.pack(">3I", xenon_ff.INLINE,
                              len(rawfile_data) - 1, xenon_ff.INLINE)
        entries = ((5, xenon_ff.INLINE),
                   (14, xenon_ff.INLINE), (16, xenon_ff.INLINE),
                   (13, xenon_ff.INLINE), (18, xenon_ff.INLINE),
                   (33, xenon_ff.INLINE))
        payload = struct.pack(">4I", 0, 0, len(entries), xenon_ff.INLINE)
        payload += b"".join(struct.pack(">2I", *entry) for entry in entries)
        payload += model + b"entity_only_model\0"
        payload += com + b"maps/mp/test.d3dbsp\0"
        payload += game + b"mp_test\0"
        payload += clip + b"maps/mp/test.d3dbsp\0" + visibility
        payload += map_ents + b"maps/mp/test.d3dbsp\0" + entities
        payload += gfx + b"maps/mp/test.d3dbsp\0mp_test\0"
        payload += rawfile + b"maps/mp/mp_test.gsc\0" + rawfile_data

        with tempfile.TemporaryDirectory(prefix="qos-xenon-probe-") as directory:
            source = Path(directory) / "source.ff"
            source.write_bytes(self.zone(payload))
            converted = xenon_ff.build_pc_map_probe(source)

        version, payload_size, _, runtime_block_size = struct.unpack_from(
            "<4I", converted)
        decoder = zlib.decompressobj()
        pc_payload = decoder.decompress(converted[28:])
        self.assertEqual(version, 470)
        self.assertEqual(runtime_block_size, 2 * 1024 * 1024)
        self.assertEqual(payload_size, len(pc_payload))
        self.assertTrue(decoder.eof)
        self.assertEqual(len(converted) % 32, 0)
        self.assertLess(len(decoder.unused_data), 32)
        self.assertEqual(struct.unpack_from("<4I", pc_payload),
                         (0, 0, 6, xenon_ff.INLINE))
        self.assertEqual([entry[0] for entry in struct.iter_unpack(
            "<2I", pc_payload[16:64])], [12, 5, 13, 17, 15, 32])
        self.assertIn(b"entity_only_model\0", pc_payload)
        self.assertIn(b'"classname" "worldspawn"', pc_payload)
        self.assertIn(b'"model" "*1"', pc_payload)
        self.assertIn(b"maps/mp/test.d3dbsp\0mp_test\0", pc_payload)
        self.assertIn(b"maps/mp/mp_test.gsc\0" + rawfile_data, pc_payload)
        clip_start = 64
        self.assertEqual(struct.unpack_from("<2I", pc_payload, clip_start + 8),
                         (0, 0))
        self.assertEqual(struct.unpack_from("<2I", pc_payload, clip_start + 48),
                         (0, 0))
        self.assertEqual(struct.unpack_from("<2I", pc_payload, clip_start + 56),
                         (0, 0))
        self.assertEqual(struct.unpack_from("<I", pc_payload, clip_start + 184),
                         (xenon_ff.INLINE,))
        self.assertEqual(struct.unpack_from("<3I", pc_payload, clip_start + 164),
                         (1, len(visibility), xenon_ff.INLINE))
        visibility_start = clip_start + 324 + len(b"maps/mp/test.d3dbsp\0")
        self.assertEqual(pc_payload[visibility_start:visibility_start + len(visibility)],
                         visibility)
        map_ents_start = visibility_start + len(visibility)
        box_brush_start = (map_ents_start + 12
                           + len(b"maps/mp/test.d3dbsp\0")
                           + len(entities))
        self.assertEqual(pc_payload[box_brush_start:box_brush_start + 80],
                         bytes(80))

    def test_collision_pointer_relocation(self):
        source = {
            "planes": (0x1000, 40),
            "brush_sides": (0x2000, 24),
        }
        destination = {
            "planes": (0x3000, 40),
            "brush_sides": (0x4000, 24),
        }
        self.assertEqual(xenon_ff._relocate_collision_pointer(
            0x40000001 + 0x1014, source, destination),
            0x40000001 + 0x3014)
        self.assertEqual(xenon_ff._relocate_collision_pointer(
            xenon_ff.INLINE, source, destination), xenon_ff.INLINE)
        with self.assertRaisesRegex(xenon_ff.FormatError,
                                    "unresolved collision"):
            xenon_ff._relocate_collision_pointer(
                0x40000001 + 0x5000, source, destination)

    def test_shared_clip_planes_use_gfx_world_allocation(self):
        header = bytearray(324)
        struct.pack_into(">2I", header, 8, 2, 0x40001235)
        planes = b"".join((
            struct.pack(">4f4B", 1.0, 0.0, 0.0, 32.0, 3, 5, 0, 0),
            struct.pack(">4f4B", 0.0, 1.0, 0.0, 64.0, 1, 2, 0, 0),
        ))
        clip_map = {
            "header": header.hex(),
            "collision": {"planes": {"data": bytes(40).hex()}},
        }
        gfx_world = {"geometry": {"planes": planes.hex()}}

        xenon_ff._bind_shared_clip_planes(clip_map, gfx_world)

        self.assertEqual(bytes.fromhex(
            clip_map["collision"]["planes"]["data"]), planes)
        self.assertEqual(struct.unpack_from(">I", bytes.fromhex(
            clip_map["header"]), 8)[0], 2)

    def test_pc_clip_cursor_bias_matches_runtime_plane_relocation(self):
        previous_planned_plane_base = 0xB54
        additional_bias = xenon_ff.PC_CLIP_BLOCK2_CURSOR_BIAS - (-0x104)

        self.assertEqual(previous_planned_plane_base + additional_bias, 0xB2C)

    def test_shared_clip_planes_reject_invalid_sign_mask(self):
        header = bytearray(324)
        struct.pack_into(">2I", header, 8, 1, 0x40001235)
        plane = struct.pack(">4f4B", 1.0, 0.0, 0.0, 32.0, 3, 8, 0, 0)
        clip_map = {
            "header": header.hex(),
            "collision": {"planes": {"data": bytes(20).hex()}},
        }
        gfx_world = {"geometry": {"planes": plane.hex()}}

        with self.assertRaisesRegex(xenon_ff.FormatError, "sign mask"):
            xenon_ff._bind_shared_clip_planes(clip_map, gfx_world)

    def test_clip_header_keeps_brush_count_in_first_halfword(self):
        header = bytearray(324)
        struct.pack_into(">2H", header, 156, 8878, 3)
        converted = xenon_ff._convert_clip_header({"header": header.hex()})
        self.assertEqual(struct.unpack_from("<2H", converted, 156),
                         (8878, 3))

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
