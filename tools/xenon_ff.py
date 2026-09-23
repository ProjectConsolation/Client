"""Inspect QoS Xenon v470 fastfiles and emit an experimental PC map probe.

Layout evidence: Xenon sub_821E15F8, sub_821E8188, sub_821E7D60,
sub_821E7C78, sub_821E6100, sub_821E2AD8, and sub_821E99F8 in
default_mp.xex. Asset names: 0x82547BA0.
Offsets in reports refer to the decompressed stream, not runtime block addresses.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib


ASSET_NAMES = (
    "xmodelpieces physpreset physconstraints destructibledef xanim xmodel "
    "material pixelshader techset image sound sndcurve col_map_sp col_map_mp "
    "com_map game_map_sp game_map_mp map_ents gfx_map lightdef ui_map font "
    "menufile menu localize weapon snddriverglobals fx impactfx aitype mptype "
    "character xmodelalias rawfile stringtable xmltree scene_animation cutscene "
    "custom_camera"
).split()
PC_ASSET_NAMES = ASSET_NAMES[:7] + ASSET_NAMES[8:]
PC_LAYOUTS = {
    "material": (96, 104),
    "techset": (156, 184),
    "image": (40, 36),
    "sound_alias": (96, 80),
    "xsurface": (200, 80),
    "gfx_map": (828, 728),
    "xmodel": (240, 240),
    "col_map_mp": (324, 324),
    "com_map": (44, 44),
    "game_map_mp": (4, 4),
    "lightdef": (16, 16),
    "fx": (32, 32),
}
INLINE = 0xFFFFFFFF
INSERT = 0xFFFFFFFE
PC_CLIP_BLOCK2_CURSOR_BIAS = -0x12C
MAX_BYTES = 256 * 1024 * 1024

# Xbox 360 GPU texture format IDs used by QoS. The tiling equations below are
# adapted from michaeloliverx/codxe's xenos_texture implementation.
XENOS_TEXTURE_FORMATS = {
    0x12: (4, 4, 8, "DXT1"),
    0x13: (4, 4, 16, "DXT2_3"),
    0x14: (4, 4, 16, "DXT4_5"),
    0x31: (4, 4, 16, "DXN"),
}
PC_TEXTURE_FOURCC = {
    "DXT1": b"DXT1",
    "DXT2_3": b"DXT3",
    "DXT4_5": b"DXT5",
    "DXN": b"ATI2",
}


class FormatError(ValueError):
    pass


def pc_asset_type(xenon_type):
    if not 0 <= xenon_type < len(ASSET_NAMES):
        raise FormatError(f"invalid Xenon asset type {xenon_type}")
    if xenon_type == 7:
        return None
    return xenon_type if xenon_type < 7 else xenon_type - 1


def resolve_manifest_references(pointers, entries, expected_type):
    """Resolve block-2 references to a unique eight-byte manifest table base."""
    target_indices = [index for index, (kind, _) in enumerate(entries)
                      if kind == expected_type]
    offsets = []
    for pointer in pointers:
        encoded = pointer - 1
        if pointer <= 0 or encoded >> 29 != 2:
            raise FormatError(f"asset reference is not in Xenon block 2: {pointer:#x}")
        offsets.append(encoded & 0x1FFFFFFF)

    if not offsets:
        return [], None
    if not target_indices:
        raise FormatError(f"manifest has no asset type {expected_type}")
    target_index_set = set(target_indices)
    bases = set()
    for target_index in target_indices:
        candidate = offsets[0] - target_index * 8
        if candidate < 0:
            continue
        resolved = [(offset - candidate) // 8 for offset in offsets]
        if all(offset >= candidate and (offset - candidate) % 8 == 0
               for offset in offsets) and all(
                   index in target_index_set for index in resolved):
            bases.add(candidate)
    if len(bases) != 1:
        raise FormatError(
            f"could not uniquely resolve packed asset table base: {sorted(bases)}")
    base = bases.pop()
    indices = [(offset - base) // 8 for offset in offsets]
    return indices, base


def u32(data, offset=0):
    return struct.unpack_from(">I", data, offset)[0]


def u16(data, offset=0):
    return struct.unpack_from(">H", data, offset)[0]


def _divide_round_up(value, divisor):
    return (value + divisor - 1) // divisor


def _align(value, alignment):
    return _divide_round_up(value, alignment) * alignment


def _xenos_texture_layout(width, height, gpu_format, base_pitch=0):
    if width <= 0 or height <= 0:
        raise FormatError("Xenos texture dimensions must be positive")
    try:
        block_width, block_height, bytes_per_block, _ = XENOS_TEXTURE_FORMATS[gpu_format]
    except KeyError as error:
        raise FormatError(f"unsupported Xenos texture format {gpu_format:#x}") from error

    width_blocks = max(1, _divide_round_up(width, block_width))
    height_blocks = max(1, _divide_round_up(height, block_height))
    if base_pitch:
        pitch = base_pitch
    elif block_width > 1:
        pitch = _align(width_blocks, 32) // 8
    else:
        pitch = _align(width, 32) // 32
    row_pitch_texels = pitch << 5
    row_pitch = max(1, _divide_round_up(
        row_pitch_texels, block_width)) * bytes_per_block
    stored_width_blocks = row_pitch // bytes_per_block
    stored_height_blocks = _align(height_blocks, 32)
    tiled_size = _align(row_pitch * stored_height_blocks, 4096)
    linear_row_pitch = width_blocks * bytes_per_block
    linear_size = linear_row_pitch * height_blocks
    return (width_blocks, height_blocks, stored_width_blocks,
            bytes_per_block, linear_row_pitch, linear_size, tiled_size)


def _xenos_log2_bytes_per_block(bytes_per_block):
    return bytes_per_block // 4 + ((bytes_per_block // 2) >> (bytes_per_block // 4))


def _xenos_tiled_row_offset(y, width, log2_bytes_per_block):
    macro = ((y // 32) * (width // 32)) << (log2_bytes_per_block + 7)
    micro = ((y & 6) << 2) << log2_bytes_per_block
    return (macro + ((micro & ~0xF) << 1) + (micro & 0xF)
            + ((y & 8) << (3 + log2_bytes_per_block)) + ((y & 1) << 4))


def _xenos_tiled_column_offset(x, y, log2_bytes_per_block, base_offset):
    macro = (x // 32) << (log2_bytes_per_block + 7)
    micro = (x & 7) << log2_bytes_per_block
    offset = base_offset + macro + ((micro & ~0xF) << 1) + (micro & 0xF)
    return (((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2)
            + (offset & 0x3F) + ((y & 16) << 7)
            + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def apply_xenos_gpu_endian(data, endian):
    """Return bytes transformed from one Xenos GPU endian mode to the other."""
    result = bytearray(data)
    widths = {0: 1, 1: 2, 2: 4, 3: 4}
    if endian not in widths:
        raise FormatError(f"invalid Xenos GPU endian mode {endian}")
    width = widths[endian]
    for offset in range(0, len(result) - width + 1, width):
        if endian in (1, 2):
            result[offset:offset + width] = reversed(result[offset:offset + width])
        elif endian == 3:
            result[offset:offset + 4] = (result[offset + 2:offset + 4]
                                         + result[offset:offset + 2])
    return bytes(result)


def _copy_xenos_texture_blocks(width, height, gpu_format, source,
                               base_pitch, to_tiled):
    (width_blocks, height_blocks, stored_width_blocks, bytes_per_block,
     linear_row_pitch, linear_size, tiled_size) = _xenos_texture_layout(
         width, height, gpu_format, base_pitch)
    source_size = linear_size if to_tiled else tiled_size
    destination_size = tiled_size if to_tiled else linear_size
    if len(source) < source_size:
        raise FormatError(
            f"truncated Xenos texture: need {source_size} bytes, have {len(source)}")

    result = bytearray(destination_size)
    log2_bytes = _xenos_log2_bytes_per_block(bytes_per_block)
    for y in range(height_blocks):
        row_offset = _xenos_tiled_row_offset(y, stored_width_blocks, log2_bytes)
        for x in range(width_blocks):
            tiled_block = _xenos_tiled_column_offset(
                x, y, log2_bytes, row_offset) >> log2_bytes
            linear_offset = y * linear_row_pitch + x * bytes_per_block
            tiled_offset = tiled_block * bytes_per_block
            source_offset, destination_offset = (
                (linear_offset, tiled_offset) if to_tiled
                else (tiled_offset, linear_offset))
            if (source_offset + bytes_per_block > source_size
                    or destination_offset + bytes_per_block > destination_size):
                raise FormatError("Xenos tiled texture offset exceeds allocation")
            result[destination_offset:destination_offset + bytes_per_block] = \
                source[source_offset:source_offset + bytes_per_block]
    return bytes(result)


def tile_xenos_texture(width, height, gpu_format, linear, base_pitch=0,
                       endian=0):
    tiled = _copy_xenos_texture_blocks(
        width, height, gpu_format, linear, base_pitch, True)
    return apply_xenos_gpu_endian(tiled, endian)


def untile_xenos_texture(width, height, format_word, tiled, base_pitch=0):
    """Convert the base Xenos texture level to linear PC block-compressed data."""
    gpu_format = format_word & 0x3F
    gpu_endian = (format_word >> 6) & 0x3
    native_tiled = apply_xenos_gpu_endian(tiled, gpu_endian)
    return _copy_xenos_texture_blocks(
        width, height, gpu_format, native_tiled, base_pitch, False)


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, size):
        if size < 0 or size > len(self.data) - self.pos:
            raise FormatError(f"truncated stream at 0x{self.pos:x}: need {size} bytes")
        start = self.pos
        self.pos += size
        return self.data[start:self.pos]

    def string(self, pointer):
        if pointer == 0:
            return None
        if pointer != INLINE:
            return {"block_reference": hex(pointer)}
        end = self.data.find(b"\0", self.pos)
        if end == -1:
            raise FormatError(f"unterminated string at 0x{self.pos:x}")
        return self.take(end + 1 - self.pos)[:-1].decode("utf-8", "backslashreplace")


def read_zone(path):
    with Path(path).open("rb") as stream:
        blob = stream.read(MAX_BYTES + 1)
    if len(blob) > MAX_BYTES or len(blob) < 28:
        raise FormatError("file exceeds limit or lacks the 28-byte header")
    header = struct.unpack_from(">7I", blob)
    if header[0] != 470:
        if struct.unpack_from("<I", blob)[0] == 470:
            raise FormatError("little-endian PC zone; not a Xenon v470 input")
        raise FormatError(f"unsupported version {header[0]}")
    if not 16 <= header[1] <= MAX_BYTES:
        raise FormatError("invalid decompressed size")
    try:
        decoder = zlib.decompressobj()
        data = decoder.decompress(blob[28:], header[1] + 1)
    except zlib.error as error:
        raise FormatError(f"invalid zlib stream: {error}") from error
    if len(data) != header[1] or not decoder.eof:
        raise FormatError("zlib stream is incomplete or disagrees with declared size")
    reader = Reader(data)
    strings, string_ptr, count, asset_ptr = struct.unpack(">4I", reader.take(16))
    if strings > len(data) // 4 or count > len(data) // 8:
        raise FormatError("invalid string or asset count")
    if (strings and string_ptr != INLINE) or (count and asset_ptr != INLINE):
        raise FormatError("unsupported top-level array pointer")
    pointers = struct.iter_unpack(">I", reader.take(strings * 4))
    script_strings = [reader.string(p[0]) for p in pointers]
    table_offset = reader.pos
    entries = list(struct.iter_unpack(">2I", reader.take(count * 8)))
    if any(t >= len(ASSET_NAMES) for t, _ in entries):
        raise FormatError("invalid asset type")
    report = {
        "file": str(path), "version": header[0], "is_xenon": True,
        "file_bytes": len(blob), "payload_bytes": len(data),
        "block_bytes": list(header[2:]), "trailing_bytes": len(decoder.unused_data),
        "script_string_count": strings, "asset_count": count,
        "asset_table_offset": hex(table_offset),
        "asset_counts": dict(Counter(ASSET_NAMES[t] for t, _ in entries)),
        "pc_asset_counts": dict(Counter(
            PC_ASSET_NAMES[pc_asset_type(t)] for t, _ in entries
            if pc_asset_type(t) is not None)),
        "incompatible_pc_layouts": {
            name: {"xenon_bytes": sizes[0], "pc_bytes": sizes[1]}
            for name, sizes in PC_LAYOUTS.items() if sizes[0] != sizes[1]
        },
        "script_strings": script_strings,
    }
    return reader, entries, report


def image(reader, pointer, capture=False):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    start = reader.pos
    header = reader.take(40)
    name = reader.string(u32(header, 36))
    pixels_offset = reader.pos
    pixels = reader.take(u32(header, 12)) if u32(header, 24) else b""
    load_pointer = u32(header, 4)
    result = {
        "name": name, "offset": hex(start),
        "width": struct.unpack_from(">H", header, 16)[0],
        "height": struct.unpack_from(">H", header, 18)[0],
        "depth": struct.unpack_from(">H", header, 20)[0],
        "pixel_offset": hex(pixels_offset), "pixel_bytes": len(pixels),
        "pixel_sha256": hashlib.sha256(pixels).hexdigest(),
    }
    if load_pointer in (INLINE, INSERT):
        load = reader.take(16)
        format_word = u32(load, 8)
        result["load_definition"] = {
            "levels": load[0], "flags": load[1],
            "dimensions": struct.unpack_from(">3H", load, 2),
            "format": hex(format_word),
        }
        if u32(load, 12):
            result["texture_resource_words"] = struct.unpack(">13I", reader.take(52))
        if capture and pixels:
            try:
                linear = untile_xenos_texture(
                    result["width"], result["height"], format_word, pixels)
                result["pc_base_level"] = {
                    "format": XENOS_TEXTURE_FORMATS[format_word & 0x3F][3],
                    "bytes": len(linear),
                    "sha256": hashlib.sha256(linear).hexdigest(),
                    "data": linear.hex(),
                }
            except FormatError as error:
                result["pc_base_level_error"] = str(error)
    elif load_pointer:
        result["load_reference"] = hex(load_pointer)
    return result


def shader(reader, pointer, allow_insert=False):
    inline_pointers = (INLINE, INSERT) if allow_insert else (INLINE,)
    if pointer not in inline_pointers:
        return {"reference": hex(pointer)}
    header = reader.take(16)
    result = {"name": reader.string(u32(header))}
    program = header[4:16]
    if u32(program, 4):
        result["metadata_bytes"] = u16(program, 10)
        reader.take(result["metadata_bytes"])
    if u32(program):
        result["bytecode_bytes"] = u16(program, 8)
        reader.take(result["bytecode_bytes"])
    return result


def technique_pass(reader, header):
    result = {"vertex_shaders": []}
    if u32(header) == INLINE:
        reader.take(80)
        result["inline_vertex_declaration"] = True
    elif u32(header):
        result["vertex_declaration_reference"] = hex(u32(header))

    for offset in range(4, 80, 4):
        pointer = u32(header, offset)
        if pointer:
            result["vertex_shaders"].append(shader(reader, pointer))
    if u32(header, 80):
        result["vertex_shaders"].append(shader(reader, u32(header, 80)))
    if u32(header, 84):
        result["pixel_shader"] = shader(reader, u32(header, 84), True)

    argument_count = header[88] + header[89] + header[90]
    if u32(header, 96):
        arguments = reader.take(argument_count * 8)
        for offset in range(0, len(arguments), 8):
            argument_type = u16(arguments, offset)
            pointer = u32(arguments, offset + 4)
            if argument_type in (1, 7) and pointer == INLINE:
                reader.take(16)
    result["argument_count"] = argument_count
    return result


def technique(reader):
    header = reader.take(8)
    pass_count = u16(header, 6)
    pass_headers = reader.take(pass_count * 100)
    passes = [technique_pass(reader, pass_headers[index * 100:(index + 1) * 100])
              for index in range(pass_count)]
    return {
        "name": reader.string(u32(header)),
        "pass_count": pass_count,
        "passes": passes,
    }


def techset(reader):
    header = reader.take(156)
    name = reader.string(u32(header))
    techniques = []
    for slot in range(36):
        pointer = u32(header, 12 + slot * 4)
        if pointer == INLINE:
            techniques.append({"slot": slot, **technique(reader)})
        elif pointer:
            techniques.append({"slot": slot, "reference": hex(pointer)})
    return {"name": name, "techniques": techniques}


def material(reader, capture=False):
    start = reader.pos
    header = reader.take(96)
    result = {"name": reader.string(u32(header)), "offset": hex(start),
              "textures": []}
    pointer = u32(header, 76)
    if pointer in (INLINE, INSERT):
        result["techset"] = techset(reader)
    else:
        result["techset_reference"] = hex(pointer)
    pointer = u32(header, 80)
    if pointer == INLINE:
        textures = reader.take(header[60] * 12)
        for offset in range(0, len(textures), 12):
            if textures[offset + 7] == 11:
                raise FormatError("water texture requires a separate decoder")
            image_value = image(reader, u32(textures, offset + 8), capture)
            if capture:
                image_value["definition"] = textures[offset:offset + 12].hex()
            result["textures"].append(image_value)
    elif pointer:
        result["texture_reference"] = hex(pointer)
    for offset, count, size, name in ((84, header[61], 32, "constants"),
                                      (88, header[62], 8, "state_bits")):
        pointer = u32(header, offset)
        if pointer == INLINE:
            result[name] = reader.take(count * size).hex()
        elif pointer:
            result[name + "_reference"] = hex(pointer)
    if capture:
        result["header"] = header.hex()
    return result


def _inline_bytes(reader, pointer, size, field):
    if pointer == INLINE:
        return reader.take(size)
    if pointer == INSERT:
        raise FormatError(f"{field} unexpectedly uses an insert pointer")
    return None


def convert_xsurface_vertices(primary, attributes, secondary, count):
    if count and (primary is None or attributes is None):
        raise FormatError("xsurface is missing a required Xenon vertex stream")
    if not count:
        return b"", b"" if secondary is not None else None

    verts0 = bytearray(count * 40)
    verts1 = bytearray(count * 16) if secondary is not None else None
    for index in range(count):
        source = index * 16
        target = index * 40
        for component in range(4):
            value = struct.unpack_from(">f", primary, source + component * 4)[0]
            struct.pack_into("<f", verts0, target + component * 4, value)
        struct.pack_into("<I", verts0, target + 16, u32(attributes, source))
        struct.pack_into("<I", verts0, target + 20, 0)
        for component in range(2):
            value = struct.unpack_from(">e", attributes, source + 8 + component * 2)[0]
            struct.pack_into("<f", verts0, target + 24 + component * 4, value)
        struct.pack_into("<I", verts0, target + 32, u32(attributes, source + 12))
        struct.pack_into("<I", verts0, target + 36, u32(attributes, source + 4))
        if verts1 is not None:
            for component in range(4):
                value = struct.unpack_from(">f", secondary, source + component * 4)[0]
                struct.pack_into("<f", verts1, source + component * 4, value)
    return bytes(verts0), bytes(verts1) if verts1 is not None else None


def convert_xsurface_header(surface, include_geometry=True):
    """Collapse a 200-byte Xenos XSurface header to the 80-byte PC layout."""
    source = bytes.fromhex(surface["header"])
    if len(source) != 200:
        raise FormatError("invalid Xbox xsurface header")

    converted = bytearray(80)
    converted[0:2] = source[0:2]
    # Do not advertise geometry that the PC renderer cannot upload. Some Xenon
    # surfaces use packed/shared pointers that the probe cannot relocate yet;
    # retaining their counts with null PC pointers crashes in the D3D buffer
    # creation path while it copies the missing source data.
    vertex_count = (u16(source, 2) if include_geometry
                    and surface["pc_vertices"] is not None else 0)
    triangle_count = (u16(source, 4) if include_geometry
                      and surface["indices"] is not None else 0)
    struct.pack_into("<2H", converted, 2, vertex_count, triangle_count)
    converted[6:8] = source[6:8]
    struct.pack_into("<I", converted, 8,
                     INLINE if include_geometry and surface["indices"] is not None else 0)
    has_blend_data = (include_geometry and surface["blend_indices"] is not None
                      and surface["blend_vertices"] is not None)
    for slot in range(4):
        struct.pack_into("<h", converted, 12 + slot * 2,
                         (struct.unpack_from(">h", source, 12 + slot * 2)[0]
                          if has_blend_data else 0))
    struct.pack_into("<I", converted, 20,
                     INLINE if include_geometry and surface["blend_indices"] is not None else 0)
    struct.pack_into("<I", converted, 24,
                     INLINE if include_geometry and surface["blend_vertices"] is not None else 0)
    struct.pack_into("<I", converted, 28,
                     INLINE if include_geometry and surface["pc_vertices"] is not None else 0)
    struct.pack_into("<I", converted, 36,
                     INLINE if include_geometry and surface["pc_secondary_vertices"] is not None else 0)
    struct.pack_into("<I", converted, 44,
                     (u32(source, 136) if include_geometry
                      and surface["rigid_vertices"] is not None else 0))
    struct.pack_into("<I", converted, 48,
                     INLINE if include_geometry and surface["rigid_vertices"] is not None else 0)
    converted[56:80] = _little_endian_words(source, 176, 200)[176:200]
    return bytes(converted)


def _unpack_xenon_unit_vec(value):
    # QoS uses the engine's third-based PackedUnitVec encoding, not SNORM10.
    components = []
    for shift in (0, 10, 20):
        component = (value >> shift) & 0x3FF
        bits = (component - 2 * (component & 0x200) + 0x40400000) & 0xFFFFFFFF
        encoded = struct.unpack("<f", struct.pack("<I", bits))[0]
        components.append((encoded - 3.0) * 8208.0312)
    return components


def convert_static_model_draws(records, model_pointers=None):
    """Expand Xenon packed placements into the PC 64-byte record layout."""
    if len(records) % 40:
        raise FormatError("invalid Xbox static-model draw record array")

    converted = bytearray(len(records) // 40 * 64)
    if model_pointers is not None and len(model_pointers) != len(records) // 40:
        raise FormatError("static-model pointer count does not match draw records")
    for index in range(len(records) // 40):
        source = records[index * 40:(index + 1) * 40]
        target = index * 64
        struct.pack_into("<4f", converted, target,
                         *struct.unpack_from(">4f", source))
        axis = []
        for offset in (16, 20, 24):
            axis.extend(_unpack_xenon_unit_vec(u32(source, offset)))
        struct.pack_into("<9f", converted, target + 16, *axis)
        struct.pack_into("<f", converted, target + 52,
                         struct.unpack_from(">f", source, 28)[0])
        struct.pack_into("<I", converted, target + 56,
                         (model_pointers[index] if model_pointers is not None
                          else u32(source, 32)))
        converted[target + 60:target + 64] = source[36:40]
    return bytes(converted)


def convert_static_model_instances(records):
    if len(records) % 32:
        raise FormatError("invalid Xbox static-model instance array")

    converted = bytearray(len(records))
    for index in range(len(records) // 32):
        source = records[index * 32:(index + 1) * 32]
        target = index * 32
        converted[target:target + 28] = _little_endian_words(source, 0, 28)[:28]
        # The final word is four byte-sized fields on both platforms.
        converted[target + 28:target + 32] = source[28:32]
    return bytes(converted)


def xsurface(reader, header, index, capture=False):
    result = {
        "vertex_count": u16(header, 2),
        "triangle_count": u16(header, 4),
    }

    blend_counts = [struct.unpack_from(">h", header, offset)[0]
                    for offset in range(12, 20, 2)]
    if any(count < 0 for count in blend_counts):
        raise FormatError(f"xsurface {index} has a negative blend count")
    blend_total = sum(blend_counts)
    blend_indices = sum((1, 3, 5, 7)[slot] * count
                        for slot, count in enumerate(blend_counts))
    blend_index_data = _inline_bytes(
        reader, u32(header, 20), blend_indices * 2,
        f"xsurface {index} blend indices")
    blend_vertex_data = _inline_bytes(
        reader, u32(header, 24), blend_total * 48,
        f"xsurface {index} blend vertices")

    vertex_bytes = result["vertex_count"] * 16
    vertex_streams = []
    vertex_stream_offsets = []
    for offset, field in ((28, "vertices"), (64, "secondary vertices"),
                          (100, "tertiary vertices")):
        stream_offset = reader.pos
        pointer = u32(header, offset)
        stream = _inline_bytes(reader, pointer, vertex_bytes,
                               f"xsurface {index} {field}")
        vertex_streams.append(stream)
        vertex_stream_offsets.append(hex(stream_offset) if pointer == INLINE else None)
    pc_verts0 = pc_verts1 = None
    if not result["vertex_count"] or all(stream is not None for stream in vertex_streams[:2]):
        pc_verts0, pc_verts1 = convert_xsurface_vertices(*vertex_streams,
                                                         result["vertex_count"])
    rigid_vertices = _inline_bytes(
        reader, u32(header, 140), u32(header, 136) * 8,
        f"xsurface {index} rigid vertices")
    indices = _inline_bytes(
        reader, u32(header, 8), result["triangle_count"] * 6,
        f"xsurface {index} indices")
    result["blend_counts"] = blend_counts
    result["rigid_vertex_count"] = u32(header, 136)
    result["vertex_stream_prefixes"] = [stream[:16].hex() if stream else None
                                         for stream in vertex_streams]
    result["vertex_stream_pointers"] = [hex(u32(header, offset))
                                         for offset in (28, 64, 100)]
    result["vertex_stream_offsets"] = vertex_stream_offsets
    result["pc_vertex_prefix"] = pc_verts0[:40].hex() if pc_verts0 else None
    result["pc_secondary_vertex_prefix"] = pc_verts1[:16].hex() if pc_verts1 else None
    if capture:
        result.update({
            "header": header.hex(),
            "blend_indices": blend_index_data.hex() if blend_index_data is not None else None,
            "blend_vertices": blend_vertex_data.hex() if blend_vertex_data is not None else None,
            "vertex_streams": [
                stream.hex() if stream is not None else None for stream in vertex_streams
            ],
            "pc_vertices": pc_verts0.hex() if pc_verts0 is not None else None,
            "pc_secondary_vertices": pc_verts1.hex() if pc_verts1 is not None else None,
            "rigid_vertices": rigid_vertices.hex() if rigid_vertices is not None else None,
            "indices": indices.hex() if indices is not None else None,
        })
    return result


def _phys_preset(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(48)
    return {
        "name": reader.string(u32(header)),
        "secondary_name": reader.string(u32(header, 28)),
    }


def physics_geometry(reader, pointer):
    if pointer != INLINE:
        return {"reference": hex(pointer)}
    header = reader.take(20)
    geometry_count = u32(header)
    result = {"geometry_count": geometry_count, "shape_count": 0}
    if not u32(header, 4):
        return result

    geometries = reader.take(geometry_count * 68)
    for index in range(geometry_count):
        geometry = geometries[index * 68:(index + 1) * 68]
        if u32(geometry) != INLINE:
            continue
        shape = reader.take(96)
        shape_count = u32(shape, 28)
        result["shape_count"] += shape_count
        if u32(shape, 32):
            shape_headers = reader.take(shape_count * 12)
            for shape_index in range(shape_count):
                if u32(shape_headers, shape_index * 12) == INLINE:
                    reader.take(20)
        if u32(shape, 48):
            reader.take(u32(shape, 80))
        _inline_bytes(reader, u32(shape, 76), u32(shape, 72) * 12,
                      f"physics geometry {index} vertices")
        _inline_bytes(reader, u32(shape, 84), shape_count * 20,
                      f"physics geometry {index} planes")
    return result


def com_map(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(44)
    result = {
        "name": reader.string(u32(header)),
        "primary_light_count": u32(header, 12),
        "water_light_count": u32(header, 36),
        "primary_light_names": [],
        "header": header.hex(),
        "primary_lights": [],
        "unknown_cells": [],
    }
    if u32(header, 16):
        lights = reader.take(result["primary_light_count"] * 68)
        for index in range(result["primary_light_count"]):
            raw = lights[index * 68:(index + 1) * 68]
            pointer = u32(raw, 64)
            name = None
            if pointer == INLINE:
                name = reader.string(pointer)
                result["primary_light_names"].append(name)
            elif pointer:
                result["primary_light_names"].append({"reference": hex(pointer)})
            result["primary_lights"].append({"raw": raw.hex(), "def_name": name})
    if u32(header, 40):
        lights = reader.take(result["water_light_count"] * 36)
        for index in range(result["water_light_count"]):
            raw = lights[index * 36:(index + 1) * 36]
            data = reader.take(5120) if u32(raw, 32) else b""
            result["unknown_cells"].append({"raw": raw.hex(), "data": data.hex()})
    return result


def _little_endian_words(raw, start=0, end=None):
    result = bytearray(raw)
    end = len(result) if end is None else end
    if start % 4 or end % 4 or end > len(result):
        raise FormatError("invalid word-swap range")
    for offset in range(start, end, 4):
        struct.pack_into("<I", result, offset, u32(raw, offset))
    return result


def write_pc_com_world(payload, asset):
    header = _little_endian_words(bytes.fromhex(asset["header"]))
    lights = asset["primary_lights"]
    cells = asset["unknown_cells"]
    struct.pack_into("<I", header, 0, INLINE)
    struct.pack_into("<I", header, 12, len(lights))
    struct.pack_into("<I", header, 16, INLINE if lights else 0)
    struct.pack_into("<I", header, 36, len(cells))
    struct.pack_into("<I", header, 40, INLINE if cells else 0)
    payload.extend(header)
    payload.extend(asset["name"].encode() + b"\0")

    for light in lights:
        raw = bytes.fromhex(light["raw"])
        converted = _little_endian_words(raw, 4, 64)
        struct.pack_into("<I", converted, 64, INLINE if light["def_name"] else 0)
        payload.extend(converted)
    for light in lights:
        if light["def_name"]:
            payload.extend(light["def_name"].encode() + b"\0")

    for cell in cells:
        raw = bytes.fromhex(cell["raw"])
        converted = _little_endian_words(raw, 0, 32)
        struct.pack_into("<I", converted, 32, INLINE if cell["data"] else 0)
        payload.extend(converted)
    for cell in cells:
        if cell["data"]:
            payload.extend(bytes.fromhex(cell["data"]))


def lightdef(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(16)
    result = {"name": reader.string(u32(header))}
    if u32(header, 4):
        result["attenuation_image"] = image(reader, u32(header, 4))
    return result


def _gfx_aabb_tree(reader, header):
    indexes = reader.take(u32(header, 32) * 4) if u32(header, 36) == INLINE else b""
    children = []
    if u32(header, 44):
        records = reader.take(u32(header, 40) * 48)
        for offset in range(0, len(records), 48):
            raw = records[offset:offset + 48]
            children.append(_gfx_aabb_tree(reader, raw))
    return {"raw": header.hex(), "indexes": indexes.hex(), "children": children}


def _gfx_cell(reader, header):
    tree = None
    if u32(header, 24):
        tree = _gfx_aabb_tree(reader, reader.take(48))
    portals = []
    if u32(header, 32):
        entries = reader.take(u32(header, 28) * 68)
        for offset in range(0, len(entries), 68):
            entry = entries[offset:offset + 68]
            nested_cell = None
            if u32(entry, 32) == INLINE:
                nested_cell = _gfx_cell(reader, reader.take(52))
            vertices = reader.take(12 * entry[40]) if u32(entry, 36) else b""
            portals.append({"raw": entry.hex(), "cell": nested_cell,
                            "vertices": vertices.hex()})
    cull_groups = reader.take(u32(header, 36) * 4) if u32(header, 40) else b""
    reflection_probes = reader.take(header[44]) if u32(header, 48) else b""
    return {"raw": header.hex(), "tree": tree, "portals": portals,
            "cull_groups": cull_groups.hex(),
            "reflection_probes": reflection_probes.hex()}


def _gfx_dpvs_planes(reader, header):
    index = u32(header, 16)
    first_offset = 2 * (index + 2)
    last_offset = 2 * (index + 5)
    if last_offset + 2 > len(header):
        raise FormatError("gfx_map plane index exceeds its embedded header")
    if u32(header, 24):
        reader.take(2 * (u16(header, last_offset) - u16(header, first_offset) + 1))
    if u32(header, 32):
        reader.take(u32(header, 28))
    if u32(header, 40):
        reader.take(u32(header, 36) * 4)
    if u32(header, 48):
        reader.take(u32(header, 44) * 168)


def gfx_map(reader, pointer, capture=False):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(828)
    names = [reader.string(u32(header, offset)) for offset in (0, 4)]

    planes = _inline_bytes(reader, u32(header, 12), u32(header, 8) * 20,
                          "gfx_map planes")
    nodes = reader.take(u32(header, 16) * 2) if u32(header, 20) else b""
    indices = reader.take(u32(header, 24) * 2) if u32(header, 28) else b""

    surfaces = b""
    surface_materials = []
    if u32(header, 68):
        surfaces = reader.take(u32(header, 64) * 72)
        for offset in range(0, len(surfaces), 72):
            material_pointer = u32(surfaces, offset + 40)
            if material_pointer in (INLINE, INSERT):
                surface_materials.append(material(reader, capture))
            else:
                surface_materials.append({"reference": hex(material_pointer)})

    light_grid = header[72:88]
    if u32(light_grid, 4):
        reader.take(u32(light_grid) * 32)
    if u32(light_grid, 12):
        reader.take(u32(light_grid, 8) * 4)
    sky_start_surfs = reader.take(u32(header, 104) * 4) if u32(header, 108) else b""
    attenuation = image(
        reader, u32(header, 112), capture) if u32(header, 112) else None
    reader.string(u32(header, 120))

    if u32(header, 332) == INLINE:
        sun = reader.take(68)
        if u32(sun, 64):
            lightdef(reader, u32(sun, 64))
    if u32(header, 368):
        records = reader.take(u32(header, 364) * 16)
        for offset in range(0, len(records), 16):
            if u32(records, offset + 12) in (INLINE, INSERT):
                image(reader, u32(records, offset + 12), capture)
    if u32(header, 372):
        reader.take(u32(header, 360) * 32)
    static_model_draws = b""
    if u32(header, 380):
        static_model_draws = reader.take(u32(header, 376) * 40)
        for offset in range(0, len(static_model_draws), 40):
            if u32(static_model_draws, offset + 32) in (INLINE, INSERT):
                xmodel(reader)
    static_model_insts = b""
    if u32(header, 384):
        static_model_insts = reader.take(u32(header, 376) * 32)
    parsed_cells = []
    if u32(header, 396):
        cells = reader.take(u32(header, 388) * 52)
        for offset in range(0, len(cells), 52):
            parsed_cells.append(_gfx_cell(reader, cells[offset:offset + 52]))
    if u32(header, 404):
        records = reader.take(u32(header, 400) * 8)
        for offset in range(0, len(records), 4):
            if u32(records, offset) in (INLINE, INSERT):
                image(reader, u32(records, offset), capture)

    _gfx_dpvs_planes(reader, header[408:460])
    draw_surfaces = b""
    if u32(header, 464):
        draw_surfaces = reader.take(u32(header, 460) * 60)
    if u32(header, 500):
        records = reader.take(u32(header, 496) * 8)
        for offset in range(0, len(records), 8):
            if u32(records, offset) in (INLINE, INSERT):
                material(reader, capture)

    vertex_data = header[128:164]
    vertices = b""
    if u32(vertex_data):
        vertices = reader.take(u32(header, 124) * 44)
    index_data = header[168:204]
    vertex_layers = b""
    if u32(index_data):
        vertex_layers = reader.take(u32(header, 164))

    world_draw = header[504:600]
    for offset in (4, 8):
        if u32(world_draw, offset) in (INLINE, INSERT):
            material(reader, capture)
    if u32(header, 664):
        attenuation = image(reader, u32(header, 664), capture)

    surface_count = u32(header, 376)
    static_surface_count = u32(draw_surfaces, 48) if draw_surfaces else 0
    # Xbox DB stream 1 is runtime zero-fill storage. These pointer fields allocate
    # visibility buffers but sub_821F3900 does not consume archive bytes for them.
    zero_fill_offsets = tuple(range(684, 796, 4)) + (800, 804, 808)
    zero_fill_fields = sum(bool(u32(header, offset)) for offset in zero_fill_offsets)
    if u32(header, 796):
        reader.take(static_surface_count * 2)
    if u32(header, 812):
        records = reader.take(u32(header, 352) * 12)
        for offset in range(0, len(records), 12):
            if u32(records, offset + 4):
                reader.take(u16(records, offset) * 2)
            if u32(records, offset + 8):
                reader.take(u16(records, offset + 2) * 2)
    if u32(header, 820):
        reader.take(u32(header, 816) * 24)
    if u32(header, 824) in (INLINE, INSERT):
        material(reader, capture)

    result = {
        "name": names[1] or names[0],
        "names": names,
        "surface_count": u32(header, 64),
        "static_model_count": surface_count,
        "vertex_count": u32(header, 124),
        "index_count": u32(header, 24),
        "attenuation_image": attenuation,
        "zero_fill_fields": zero_fill_fields,
    }
    if capture:
        result["geometry"] = {
            "planes": (planes or b"").hex(),
            "nodes": nodes.hex(),
            "indices": indices.hex(),
            "surfaces": surfaces.hex(),
            "surface_materials": surface_materials,
            "brush_models": draw_surfaces.hex(),
            "static_model_draws": static_model_draws.hex(),
            "static_model_insts": static_model_insts.hex(),
            "cells": parsed_cells,
            "sky_start_surfs": sky_start_surfs.hex(),
            "vertices": vertices.hex(),
            "vertex_layers": vertex_layers.hex(),
        }
    return result


def game_map_mp(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(4)
    return {"name": reader.string(u32(header))}


def _sound_loaded_file(reader):
    header = reader.take(156)
    data_bytes = u32(header, 4) if u32(header) else 0
    if data_bytes:
        reader.take(data_bytes)
    if u32(header, 84):
        reader.take(1)
    seek_count = u32(header, 148)
    if u32(header, 152):
        reader.take(seek_count * 4)
    return {"data_bytes": data_bytes, "seek_count": seek_count}


def _sound_file(reader, pointer):
    if pointer != INLINE:
        return {"reference": hex(pointer)}
    header = reader.take(20)
    result = {
        "name": reader.string(u32(header)),
        "directory": reader.string(u32(header, 4)),
        "type": header[16],
    }
    payload_pointer = u32(header, 8)
    if result["type"] == 3:
        if payload_pointer == INLINE:
            streamed = reader.take(12)
            result["streamed"] = {
                "name": reader.string(u32(streamed)),
                "data_bytes": u32(streamed, 8) if u32(streamed, 4) else 0,
            }
            if result["streamed"]["data_bytes"]:
                reader.take(result["streamed"]["data_bytes"])
        elif payload_pointer:
            result["streamed_reference"] = hex(payload_pointer)
    elif payload_pointer:
        result["loaded"] = _sound_loaded_file(reader)
    return result


def _sound_speaker_map(reader, pointer):
    if pointer != INLINE:
        return {"reference": hex(pointer)}
    header = reader.take(56)
    result = {"name": reader.string(u32(header, 4)), "channel_maps": []}
    for offset in (8, 24, 40):
        channel_map = header[offset:offset + 16]
        payload_bytes = 0
        for record_offset in (0, 8):
            if u32(channel_map, record_offset + 4):
                record_bytes = channel_map[record_offset] * 8
                reader.take(record_bytes)
                payload_bytes += record_bytes
        result["channel_maps"].append({"bytes": payload_bytes})
    return result


def sound(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(12)
    name = reader.string(u32(header))
    alias_count = u32(header, 8)
    aliases = []
    alias_pointer = u32(header, 4)
    if alias_pointer == INLINE:
        if alias_count > (len(reader.data) - reader.pos) // 96:
            raise FormatError(f"sound {name!r} has invalid alias count {alias_count}")
        records = reader.take(alias_count * 96)
        for index in range(alias_count):
            record = records[index * 96:(index + 1) * 96]
            alias = {
                "name": reader.string(u32(record)),
                "subtitle": reader.string(u32(record, 4)),
                "secondary": reader.string(u32(record, 8)),
                "chain": reader.string(u32(record, 12)),
            }
            if u32(record, 16):
                alias["sound_file"] = _sound_file(reader, u32(record, 16))
            curve_pointer = u32(record, 76)
            if curve_pointer in (INLINE, INSERT):
                curve = reader.take(72)
                alias["curve"] = {"name": reader.string(u32(curve))}
            elif curve_pointer:
                alias["curve"] = {"reference": hex(curve_pointer)}
            if u32(record, 92):
                alias["speaker_map"] = _sound_speaker_map(reader, u32(record, 92))
            aliases.append(alias)
    elif alias_pointer:
        return {"name": name, "alias_count": alias_count,
                "alias_reference": hex(alias_pointer)}
    return {"name": name, "alias_count": alias_count, "aliases": aliases}


def _fx_visual(reader, effect_type, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)} if pointer else None
    if effect_type == 5:
        return xmodel(reader)
    if effect_type in (8, 10):
        return {"name": reader.string(pointer)}
    if effect_type in (6, 7):
        return {"value": hex(pointer)}
    return material(reader)


def _fx_element(reader, header, index):
    effect_type = header[176]
    visual_count = header[177]
    if u32(header, 180):
        reader.take((header[178] + 1) * 96)
    if u32(header, 184):
        reader.take((header[179] + 1) * 48)

    visual_pointer = u32(header, 188)
    visuals = []
    if effect_type == 9:
        if visual_pointer:
            records = reader.take(visual_count * 8)
            for offset in range(0, len(records), 4):
                pointer = u32(records, offset)
                if pointer in (INLINE, INSERT):
                    visuals.append(material(reader))
                elif pointer:
                    visuals.append({"reference": hex(pointer)})
    elif visual_count > 1:
        if visual_pointer:
            pointers = reader.take(visual_count * 4)
            for offset in range(0, len(pointers), 4):
                visual = _fx_visual(reader, effect_type, u32(pointers, offset))
                if visual is not None:
                    visuals.append(visual)
    else:
        visual = _fx_visual(reader, effect_type, visual_pointer)
        if visual is not None:
            visuals.append(visual)

    for offset in (216, 220, 224):
        reader.string(u32(header, offset))
    if u32(header, 244):
        trail = reader.take(28)
        if u32(trail, 16):
            reader.take(u32(trail, 12) * 20)
        if u32(trail, 24):
            reader.take(u32(trail, 20) * 2)
    return {"type": effect_type, "visual_count": visual_count,
            "visuals": visuals, "index": index}


def fx(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(32)
    name = reader.string(u32(header))
    element_count = sum(u32(header, offset) for offset in (16, 20, 24))
    elements = []
    if u32(header, 28):
        records = reader.take(element_count * 252)
        for index in range(element_count):
            elements.append(_fx_element(
                reader, records[index * 252:(index + 1) * 252], index))
    return {"name": name, "element_count": element_count, "elements": elements}


def _col_brush(reader, header):
    result = {"header": header.hex()}
    if u32(header, 32) == INLINE:
        shape = reader.take(12)
        result["inline_side"] = shape.hex()
        if u32(shape) == INLINE:
            result["inline_plane"] = reader.take(20).hex()
    if u32(header, 48) == INLINE:
        result["inline_base_adjacent_side"] = reader.take(1).hex()
    if u32(header, 76) == INLINE:
        result["inline_verts"] = reader.take(12).hex()
    return result


def _col_dyn_entity(reader, header, index):
    if u32(header, 32) in (INLINE, INSERT):
        xmodel(reader)
    for offset, field in ((40, "destructible definition"),
                          (44, "animation reference")):
        if u32(header, offset) in (INLINE, INSERT):
            raise FormatError(f"dynamic entity {index} has unsupported {field}")
    if u32(header, 48):
        _phys_preset(reader, u32(header, 48))


def col_map_mp(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(324)
    name = reader.string(u32(header))

    collision = {}

    def capture_array(key, count_offset, pointer_offset, stride, shared=False):
        pointer = u32(header, pointer_offset)
        if not pointer:
            collision[key] = {"offset": None, "data": ""}
            return b""
        size = u32(header, count_offset) * stride
        if shared and pointer != INLINE:
            encoded = pointer - 1
            if encoded >> 29 != 2:
                raise FormatError(f"col_map {key} reference is not in block 2")
            start = encoded & 0x1FFFFFFF
            if size > len(reader.data) - start:
                raise FormatError(f"col_map {key} packed reference is truncated")
            data = reader.data[start:start + size]
        else:
            start = reader.pos
            data = reader.take(size)
        collision[key] = {"offset": hex(start), "data": data.hex()}
        return data

    capture_array("planes", 8, 12, 20, shared=True)
    if u32(header, 20):
        start = reader.pos
        records = reader.take(u32(header, 16) * 80)
        collision["static_models"] = {"offset": hex(start), "data": records.hex()}
        for offset in range(0, len(records), 80):
            if u32(records, offset + 4) in (INLINE, INSERT):
                xmodel(reader)
    else:
        collision["static_models"] = {"offset": None, "data": ""}
    capture_array("materials", 24, 28, 72)
    if u32(header, 36):
        start = reader.pos
        shapes = reader.take(u32(header, 32) * 12)
        inline_planes = []
        for offset in range(0, len(shapes), 12):
            if u32(shapes, offset) == INLINE:
                inline_planes.append(reader.take(20).hex())
        collision["brush_sides"] = {
            "offset": hex(start), "data": shapes.hex(),
            "inline_planes": inline_planes,
        }
    else:
        collision["brush_sides"] = {"offset": None, "data": "", "inline_planes": []}
    capture_array("brush_edges", 40, 44, 1)
    if u32(header, 52):
        start = reader.pos
        records = reader.take(u32(header, 48) * 8)
        inline_planes = []
        for offset in range(0, len(records), 8):
            if u32(records, offset) == INLINE:
                inline_planes.append(reader.take(20).hex())
        collision["nodes"] = {
            "offset": hex(start), "data": records.hex(),
            "inline_planes": inline_planes,
        }
    else:
        collision["nodes"] = {"offset": None, "data": "", "inline_planes": []}
    capture_array("leaves", 56, 60, 44)
    capture_array("leaf_brushes", 72, 76, 2)
    if u32(header, 68):
        start = reader.pos
        records = reader.take(u32(header, 64) * 20)
        inline_brushes = []
        for offset in range(0, len(records), 20):
            count = struct.unpack_from(">h", records, offset + 2)[0]
            if count > 0 and u32(records, offset + 8) == INLINE:
                inline_brushes.append(reader.take(count * 2).hex())
        collision["leaf_brush_nodes"] = {
            "offset": hex(start), "data": records.hex(),
            "inline_brushes": inline_brushes,
        }
    else:
        collision["leaf_brush_nodes"] = {
            "offset": None, "data": "", "inline_brushes": [],
        }
    for count_offset, pointer_offset, stride in ((80, 84, 4), (88, 92, 12),
                                                  (96, 100, 12), (104, 108, 2),
                                                  (112, 116, 6)):
        key = {
            80: "leaf_surfaces", 88: "verts", 96: "brush_verts",
            104: "uinds", 112: "tri_indices",
        }[count_offset]
        capture_array(key, count_offset, pointer_offset, stride)
    if u32(header, 120):
        start = reader.pos
        data = reader.take(((3 * u32(header, 112) + 31) >> 3) & 0xFFFFFFFC)
        collision["tri_edge_walkable"] = {"offset": hex(start), "data": data.hex()}
    else:
        collision["tri_edge_walkable"] = {"offset": None, "data": ""}
    capture_array("borders", 124, 128, 28)
    if u32(header, 136):
        start = reader.pos
        records = reader.take(u32(header, 132) * 20)
        inline_borders = []
        for offset in range(0, len(records), 20):
            if u32(records, offset + 16) == INLINE:
                inline_borders.append(reader.take(28).hex())
        collision["partitions"] = {
            "offset": hex(start), "data": records.hex(),
            "inline_borders": inline_borders,
        }
    else:
        collision["partitions"] = {"offset": None, "data": "", "inline_borders": []}
    capture_array("aabb_trees", 140, 144, 32)
    capture_array("cmodels", 148, 152, 72)
    if u32(header, 160):
        start = reader.pos
        brushes = reader.take(u16(header, 156) * 80)
        nested = []
        for offset in range(0, len(brushes), 80):
            nested.append(_col_brush(reader, brushes[offset:offset + 80]))
        collision["brushes"] = {
            "offset": hex(start), "data": brushes.hex(), "nested": nested,
        }
    else:
        collision["brushes"] = {"offset": None, "data": "", "nested": []}
    visibility = b""
    if u32(header, 172):
        visibility = reader.take(u32(header, 164) * u32(header, 168))

    map_ents = None
    if u32(header, 180) in (INLINE, INSERT):
        raw = reader.take(12)
        entity_name = reader.string(u32(raw))
        entity_string = b""
        if u32(raw, 4):
            entity_string = reader.take(u32(raw, 8))
        map_ents = {
            "name": entity_name,
            "entity_string": entity_string.decode("latin-1"),
            "entity_bytes": len(entity_string),
        }
    box_brush = None
    if u32(header, 184) == INLINE:
        box_brush = _col_brush(reader, reader.take(80))

    entity_counts = (u16(header, 262), u16(header, 264))
    for pointer_offset, count in ((272, entity_counts[0]), (276, entity_counts[1])):
        if u32(header, pointer_offset):
            entities = reader.take(count * 88)
            for index in range(count):
                _col_dyn_entity(reader, entities[index * 88:(index + 1) * 88], index)
    # Offsets 280..308 are allocated in stream 1. The Xbox loader zero-fills
    # that virtual stream instead of consuming bytes from the archive.
    zero_fill_bytes = 0
    for pointer_offset, count, stride in (
            (280, entity_counts[0], 52), (284, entity_counts[1], 52),
            (288, u16(header, 266), 36), (292, u16(header, 268), 36),
            (296, entity_counts[0], 32), (300, entity_counts[1], 32),
            (304, u16(header, 266), 32), (308, u16(header, 268), 32)):
        if u32(header, pointer_offset):
            zero_fill_bytes += count * stride
    if u32(header, 316):
        reader.take(u32(header, 312) * 72)

    return {
        "name": name,
        "header": header.hex(),
        "plane_count": u32(header, 8),
        "brush_count": u16(header, 156),
        "dynamic_entity_counts": list(entity_counts),
        "visibility_count": u32(header, 164),
        "visibility_stride": u32(header, 168),
        "visibility": visibility.hex(),
        "zero_fill_bytes": zero_fill_bytes,
        "map_ents": map_ents,
        "box_brush": box_brush,
        "collision": collision,
    }


def xmodel(reader, capture=False):
    start = reader.pos
    header = reader.take(240)
    name = reader.string(u32(header))
    bone_count = header[4]
    root_bone_count = header[5]
    surface_count = header[6]
    if root_bone_count > bone_count:
        raise FormatError(f"xmodel {name!r} has more root bones than bones")
    child_bones = bone_count - root_bone_count

    bone_names = _inline_bytes(reader, u32(header, 8), bone_count * 2,
                               f"xmodel {name!r} bone names")
    parent_list = _inline_bytes(reader, u32(header, 12), child_bones,
                                f"xmodel {name!r} parent list")
    quaternions = _inline_bytes(reader, u32(header, 16), child_bones * 8,
                                f"xmodel {name!r} quaternions")
    translations = _inline_bytes(reader, u32(header, 20), child_bones * 16,
                                 f"xmodel {name!r} translations")
    part_classification = _inline_bytes(
        reader, u32(header, 24), bone_count,
        f"xmodel {name!r} part classification")
    base_matrices = _inline_bytes(reader, u32(header, 28), bone_count * 32,
                                  f"xmodel {name!r} base matrices")

    surfaces = []
    surface_pointer = u32(header, 32)
    if surface_pointer:
        surface_headers = reader.take(surface_count * 200)
        for index in range(surface_count):
            surface = surface_headers[index * 200:(index + 1) * 200]
            surfaces.append(xsurface(reader, surface, index, capture))

    materials = []
    material_pointer = u32(header, 36)
    if material_pointer:
        references = reader.take(surface_count * 4)
        for index in range(surface_count):
            pointer = u32(references, index * 4)
            if pointer in (INLINE, INSERT):
                materials.append(material(reader, capture))
            else:
                materials.append({"reference": hex(pointer)})

    unknown_records = reader.take(u32(header, 172) * 36) if u32(header, 168) else b""
    bone_info = reader.take(bone_count * 40) if u32(header, 180) else b""
    collision_count = u16(header, 44)
    collisions = reader.take(collision_count * 24) if u32(header, 216) else b""

    phys_preset = None
    if u32(header, 228):
        phys_preset = _phys_preset(reader, u32(header, 228))
    physics = None
    if u32(header, 232):
        physics = physics_geometry(reader, u32(header, 232))
    if u32(header, 236):
        raise FormatError(f"xmodel {name!r} has unsupported collision tree at "
                          f"stream offset 0x{reader.pos:x}")

    result = {
        "name": name,
        "offset": hex(start),
        "bone_count": bone_count,
        "root_bone_count": root_bone_count,
        "surface_count": surface_count,
        "surfaces": surfaces,
        "materials": materials,
        "phys_preset": phys_preset,
        "physics_geometry": physics,
    }
    if capture:
        result.update({
            "header": header.hex(),
            "bone_names": bone_names.hex() if bone_names is not None else None,
            "parent_list": parent_list.hex() if parent_list is not None else None,
            "quaternions": quaternions.hex() if quaternions is not None else None,
            "translations": translations.hex() if translations is not None else None,
            "part_classification": (
                part_classification.hex() if part_classification is not None else None),
            "base_matrices": base_matrices.hex() if base_matrices is not None else None,
            "unknown_records": unknown_records.hex(),
            "bone_info": bone_info.hex(),
            "collisions": collisions.hex(),
        })
    return result


def _captured_bytes(value):
    return bytes.fromhex(value) if value is not None else b""


def write_pc_xsurface_nested(payload, surface):
    payload.extend(_little_endian_u16_array(
        _captured_bytes(surface["blend_indices"])))
    payload.extend(_little_endian_words(
        _captured_bytes(surface["blend_vertices"])))
    payload.extend(_captured_bytes(surface["pc_vertices"]))
    payload.extend(_captured_bytes(surface["pc_secondary_vertices"]))
    payload.extend(_little_endian_u16_array(
        _captured_bytes(surface["rigid_vertices"])))
    payload.extend(_little_endian_u16_array(
        _captured_bytes(surface["indices"])))


def convert_xmodel_header(model):
    source = bytes.fromhex(model["header"])
    if len(source) != 240:
        raise FormatError("invalid Xbox xmodel header")
    converted = bytearray(240)
    struct.pack_into("<I", converted, 0, INLINE)
    converted[4:8] = source[4:8]
    for offset, field in (
            (8, "bone_names"), (12, "parent_list"), (16, "quaternions"),
            (20, "translations"), (24, "part_classification"),
            (28, "base_matrices")):
        struct.pack_into("<I", converted, offset,
                         INLINE if model[field] is not None else 0)
    struct.pack_into("<I", converted, 32, INLINE if model["surfaces"] else 0)
    struct.pack_into("<I", converted, 36, INLINE if model["materials"] else 0)
    for offset in range(40, 168, 32):
        converted[offset:offset + 28] = _little_endian_words(
            source, offset, offset + 28)[offset:offset + 28]
        converted[offset + 28:offset + 32] = source[offset + 28:offset + 32]

    # Visual probe models intentionally omit collision and physics.
    struct.pack_into("<2I", converted, 168, 0, 0)
    struct.pack_into("<I", converted, 176, u32(source, 176))
    struct.pack_into("<I", converted, 180, INLINE if model["bone_info"] else 0)
    converted[184:212] = _little_endian_words(source, 184, 212)[184:212]
    struct.pack_into("<Hh", converted, 212, u16(source, 212),
                     struct.unpack_from(">h", source, 214)[0])
    converted[216:220] = source[216:220]
    struct.pack_into("<I", converted, 220, u32(source, 220))
    converted[224:228] = source[224:228]
    return bytes(converted)


def write_pc_xmodel(payload, model):
    payload.extend(convert_xmodel_header(model))
    payload.extend(model["name"].encode() + b"\0")
    payload.extend(_little_endian_u16_array(_captured_bytes(model["bone_names"])))
    payload.extend(_captured_bytes(model["parent_list"]))
    payload.extend(_little_endian_u16_array(_captured_bytes(model["quaternions"])))
    payload.extend(_little_endian_words(_captured_bytes(model["translations"])))
    payload.extend(_captured_bytes(model["part_classification"]))
    payload.extend(_little_endian_words(_captured_bytes(model["base_matrices"])))

    # The map-entry probe keeps XModel identity and bounds, but omits all Xenon
    # GPU streams until their PC sizes and semantics are verified end to end.
    for surface in model["surfaces"]:
        payload.extend(convert_xsurface_header(surface, include_geometry=False))

    for _ in model["materials"]:
        payload.extend(struct.pack("<I", INLINE))
    for _ in model["materials"]:
        payload.extend(_pc_external_material())
    payload.extend(_little_endian_words(_captured_bytes(model["bone_info"])))


def inspect(path, details=False, capture_map=False):
    reader, entries, report = read_zone(path)
    if details:
        assets = []
        for index, (kind, pointer) in enumerate(entries):
            if pointer not in (INLINE, INSERT):
                raise FormatError(f"asset {index} has unsupported pointer {pointer:#x}")
            start = reader.pos
            if kind == 8:
                asset = techset(reader)
            elif kind == 5:
                asset = xmodel(reader, capture_map)
            elif kind == 1:
                asset = _phys_preset(reader, pointer)
            elif kind == 14:
                asset = com_map(reader, pointer)
            elif kind == 19:
                asset = lightdef(reader, pointer)
            elif kind == 18:
                asset = gfx_map(reader, pointer, capture_map)
            elif kind == 16:
                asset = game_map_mp(reader, pointer)
            elif kind == 13:
                asset = col_map_mp(reader, pointer)
            elif kind == 10:
                asset = sound(reader, pointer)
            elif kind == 27:
                asset = fx(reader, pointer)
            elif kind == 6:
                asset = material(reader, capture_map)
            elif kind == 33:
                header = reader.take(12)
                asset = {"name": reader.string(u32(header))}
                payload = reader.take(u32(header, 4) + 1) if u32(header, 8) else b""
                asset["bytes"] = len(payload)
                asset["sha256"] = hashlib.sha256(payload).hexdigest()
                if capture_map:
                    asset["data"] = payload.hex()
            else:
                raise FormatError(f"asset {index}: {ASSET_NAMES[kind]} details unsupported "
                                  f"at stream offset 0x{start:x}")
            assets.append({"type": ASSET_NAMES[kind], "manifest_index": index,
                           "offset": hex(start), **asset})
        for asset in assets:
            if asset["type"] != "gfx_map" or "geometry" not in asset:
                continue
            draws = bytes.fromhex(asset["geometry"]["static_model_draws"])
            pointers = [u32(draws, offset + 32)
                        for offset in range(0, len(draws), 40)]
            indices, table_base = resolve_manifest_references(pointers, entries, 5)
            asset["geometry"]["static_model_asset_indices"] = indices
            asset["geometry"]["static_model_assets"] = [
                {"manifest_index": index, "name": assets[index].get("name")}
                for index in sorted(set(indices))
            ]
            asset["geometry"]["asset_table_block2_offset"] = (
                hex(table_base) if table_base is not None else None)
            if table_base is not None:
                for material_value in asset["geometry"]["surface_materials"]:
                    reference = material_value.get("techset_reference")
                    if reference is None:
                        continue
                    pointer = int(reference, 16)
                    encoded = pointer - 1
                    offset = encoded & 0x1FFFFFFF
                    if pointer <= 0 or encoded >> 29 != 2 or offset < table_base:
                        raise FormatError("material technique reference is not in the asset table")
                    delta = offset - table_base
                    if delta % 8:
                        raise FormatError("material technique reference is not asset-aligned")
                    technique_index = delta // 8
                    if (technique_index >= len(entries)
                            or entries[technique_index][0] != 8):
                        raise FormatError("material technique reference does not target a techset")
                    material_value["techset_manifest_index"] = technique_index
                    material_value["techset_name"] = assets[technique_index].get("name")
        report["assets"] = assets
        report["unconsumed_payload_bytes"] = len(reader.data) - reader.pos
        if reader.pos != len(reader.data):
            raise FormatError(f"unconsumed payload at 0x{reader.pos:x}")
    return report


def _little_endian_u16_array(raw):
    if len(raw) % 2:
        raise FormatError("unaligned 16-bit array")
    return b"".join(struct.pack("<H", u16(raw, offset))
                    for offset in range(0, len(raw), 2))


def _pc_external_material(name=",white"):
    header = bytearray(104)
    struct.pack_into("<I", header, 0, INLINE)
    return bytes(header) + name.encode() + b"\0"


def _pc_external_techset(name):
    header = bytearray(184)
    struct.pack_into("<I", header, 0, INLINE)
    return bytes(header) + name.encode() + b"\0"


def write_pc_material(payload, material_value, techset_pointer,
                      image_pointers):
    source = bytes.fromhex(material_value["header"])
    if len(source) != 96 or len(image_pointers) != len(material_value["textures"]):
        raise FormatError("invalid captured Xbox material")
    name = material_value.get("name")
    if not isinstance(name, str) or not isinstance(material_value.get("techset_name"), str):
        raise FormatError("material is missing a PC-resolvable name or technique set")

    converted = bytearray(104)
    struct.pack_into("<I", converted, 0, INLINE)
    converted[4:8] = source[4:8]
    struct.pack_into("<3I", converted, 8, u32(source, 8), u32(source, 12),
                     u32(source, 16))
    struct.pack_into("<H", converted, 20, u16(source, 20))
    converted[24:67] = b"\xFF" * 43
    converted[24:64] = source[20:60]
    converted[67:70] = source[60:63]
    converted[70:83] = source[63:76]
    struct.pack_into("<4I", converted, 84, techset_pointer,
                     INLINE if image_pointers else 0,
                     INLINE if material_value.get("constants") else 0,
                     INLINE if material_value.get("state_bits") else 0)
    payload.extend(converted)
    payload.extend(name.encode() + b"\0")

    for texture, image_pointer in zip(material_value["textures"], image_pointers):
        definition = bytes.fromhex(texture["definition"])
        if len(definition) != 12:
            raise FormatError(f"material {name!r} has an invalid texture definition")
        payload.extend(struct.pack("<I", u32(definition)))
        payload.extend(definition[4:8])
        payload.extend(struct.pack("<I", image_pointer))
    payload.extend(_little_endian_words(bytes.fromhex(
        material_value.get("constants", ""))))
    payload.extend(_little_endian_words(bytes.fromhex(
        material_value.get("state_bits", ""))))


def write_pc_image(payload, image_value):
    base = image_value.get("pc_base_level")
    if not base or "data" not in base:
        raise FormatError(f"image {image_value.get('name')!r} has no decoded base level")
    name = image_value.get("name")
    if not isinstance(name, str):
        raise FormatError("decoded image has no string name")
    try:
        fourcc = PC_TEXTURE_FOURCC[base["format"]]
    except KeyError as error:
        raise FormatError(f"unsupported PC texture format {base['format']}") from error
    data = bytes.fromhex(base["data"])
    if len(data) != base["bytes"]:
        raise FormatError(f"image {name!r} decoded byte count changed")

    width = image_value["width"]
    height = image_value["height"]
    depth = max(1, image_value["depth"])
    header = bytearray(36)
    struct.pack_into("<2I", header, 0, 3, INSERT)
    header[11] = 2  # TS_COLOR_MAP; material bindings may override semantics later.
    struct.pack_into("<2I", header, 16, len(data), len(data))
    struct.pack_into("<3H", header, 24, width, height, depth)
    header[30] = 3  # IMG_CATEGORY_LOAD_FROM_FILE
    struct.pack_into("<I", header, 32, INLINE)
    payload.extend(header)
    payload.extend(name.encode() + b"\0")
    payload.extend(struct.pack("<2B3H4sI", 0, 0, width, height, depth,
                               fourcc, len(data)))
    payload.extend(data)


def _pc_gfx_aabb_header(tree):
    raw = bytes.fromhex(tree["raw"])
    indexes = bytes.fromhex(tree["indexes"])
    children = tree["children"]
    converted = _little_endian_words(raw, 0, 32)
    struct.pack_into("<4I", converted, 32, len(indexes) // 4,
                     INLINE if indexes else 0, len(children),
                     INLINE if children else 0)
    return converted


def _write_pc_gfx_aabb_nested(payload, tree):
    indexes = bytes.fromhex(tree["indexes"])
    payload.extend(_little_endian_words(indexes))
    children = tree["children"]
    for child in children:
        payload.extend(_pc_gfx_aabb_header(child))
    for child in children:
        _write_pc_gfx_aabb_nested(payload, child)


def _pc_gfx_cell_header(cell, include_static_models=False,
                        include_empty_tree=False):
    raw = bytes.fromhex(cell["raw"])
    converted = _little_endian_words(raw, 0, 24)
    cull_groups = bytes.fromhex(cell["cull_groups"])
    # Cell probe bytes index the GfxWorld-wide reflection-probe origin array.
    # That PC array is not serialized yet, so advertising source indices would
    # make the renderer dereference a null GfxWorld+268 pointer.
    reflection_probes = b""
    struct.pack_into("<5I", converted, 24,
                     INLINE if ((include_static_models or include_empty_tree)
                                and cell["tree"]) else 0,
                     len(cell["portals"]), INLINE if cell["portals"] else 0,
                     len(cull_groups) // 4, INLINE if cull_groups else 0)
    converted[44] = len(reflection_probes)
    converted[45:48] = raw[45:48]
    struct.pack_into("<I", converted, 48, INLINE if reflection_probes else 0)
    return converted


def _write_pc_gfx_cell_nested(payload, cell, include_static_models=False,
                              include_empty_tree=False):
    if include_static_models and cell["tree"]:
        payload.extend(_pc_gfx_aabb_header(cell["tree"]))
        _write_pc_gfx_aabb_nested(payload, cell["tree"])
    elif include_empty_tree and cell["tree"]:
        payload.extend(bytes(48))

    for portal in cell["portals"]:
        raw = bytes.fromhex(portal["raw"])
        converted = _little_endian_words(raw, 0, 32)
        converted[40:68] = raw[40:68]
        struct.pack_into("<2I", converted, 32,
                         INLINE if portal["cell"] else 0,
                         INLINE if portal["vertices"] else 0)
        payload.extend(converted)
    for portal in cell["portals"]:
        if portal["cell"]:
            payload.extend(_pc_gfx_cell_header(
                portal["cell"], include_static_models, include_empty_tree))
            _write_pc_gfx_cell_nested(
                payload, portal["cell"], include_static_models,
                include_empty_tree)
        payload.extend(_little_endian_words(bytes.fromhex(portal["vertices"])))

    payload.extend(_little_endian_words(bytes.fromhex(cell["cull_groups"])))
    # Reflection-probe indices are omitted with their root allocation.


def write_pc_gfx_world(payload, asset, primary_light_count,
                       converted_materials=None):
    geometry = asset["geometry"]
    planes = bytes.fromhex(geometry["planes"])
    nodes = bytes.fromhex(geometry["nodes"])
    indices = bytes.fromhex(geometry["indices"])
    xbox_surfaces = bytes.fromhex(geometry["surfaces"])
    xbox_brush_models = bytes.fromhex(geometry.get("brush_models", ""))
    sky_start_surfs = bytes.fromhex(geometry["sky_start_surfs"])
    xbox_vertices = bytes.fromhex(geometry["vertices"])
    vertex_layers = bytes.fromhex(geometry["vertex_layers"])
    static_draws = bytes.fromhex(geometry.get("static_model_draws", ""))
    static_instances = bytes.fromhex(geometry.get("static_model_insts", ""))
    static_model_pointers = geometry.get("pc_static_model_pointers")
    cells = geometry.get("cells", [])

    if len(xbox_surfaces) % 72 or len(xbox_brush_models) % 60 or len(xbox_vertices) % 44:
        raise FormatError("invalid captured Xbox world geometry")
    surface_count = len(xbox_surfaces) // 72
    vertex_count = len(xbox_vertices) // 44
    brush_model_count = len(xbox_brush_models) // 60
    static_model_count = len(static_draws) // 40
    if len(static_draws) % 40 or len(static_instances) != static_model_count * 32:
        raise FormatError("invalid captured Xbox static-model arrays")
    have_static_models = static_model_count > 0 and static_model_pointers is not None
    if have_static_models and len(static_model_pointers) != static_model_count:
        raise FormatError("invalid relocated static-model pointer array")
    # Static-model culling is one indivisible PC subsystem: worker jobs assume
    # the cell tree, global index array, and mark buffers all exist whenever the
    # world advertises a nonzero model count. The latter buffers are not mapped
    # yet, so omit world static models while retaining standalone XModel assets.
    include_static_models = False
    include_cell_trees = False
    include_empty_cell_trees = True

    pc_planes = bytearray(len(planes))
    for offset in range(0, len(planes), 20):
        pc_planes[offset:offset + 16] = _little_endian_words(planes[offset:offset + 20], 0, 16)[:16]
        pc_planes[offset + 16:offset + 20] = planes[offset + 16:offset + 20]

    pc_surfaces = bytearray(surface_count * 48)
    for index in range(surface_count):
        source = xbox_surfaces[index * 72:(index + 1) * 72]
        target = index * 48
        struct.pack_into("<IIHHI", pc_surfaces, target,
                         u32(source, 0), u32(source, 4), u16(source, 8),
                         u16(source, 10), u32(source, 12))
        struct.pack_into("<I", pc_surfaces, target + 16, INLINE)
        pc_surfaces[target + 20:target + 24] = source[44:48]
        pc_surfaces[target + 24:target + 48] = _little_endian_words(source, 48, 72)[48:72]

    pc_vertices = _little_endian_words(xbox_vertices)
    pc_header = bytearray(728)
    struct.pack_into("<2I", pc_header, 0, INLINE, INLINE)
    struct.pack_into("<2I", pc_header, 8, len(planes) // 20, INLINE if planes else 0)
    struct.pack_into("<2I", pc_header, 16, len(nodes) // 2, INLINE if nodes else 0)
    struct.pack_into("<2I", pc_header, 24, len(indices) // 2, INLINE if indices else 0)
    struct.pack_into("<2I", pc_header, 32, surface_count, INLINE if surface_count else 0)
    struct.pack_into("<2I", pc_header, 60, len(sky_start_surfs) // 4,
                     INLINE if sky_start_surfs else 0)
    struct.pack_into("<2I", pc_header, 80, vertex_count, INLINE if vertex_count else 0)
    struct.pack_into("<2I", pc_header, 92, len(vertex_layers), INLINE if vertex_layers else 0)
    # The renderer dereferences this 68-byte world-sun record while bringing
    # up a map. Its final light-definition pointer remains null in the probe.
    struct.pack_into("<I", pc_header, 232, INLINE)
    struct.pack_into("<I", pc_header, 252, primary_light_count)
    struct.pack_into("<3I", pc_header, 276,
                     static_model_count if include_static_models else 0,
                     INLINE if include_static_models else 0,
                     INLINE if include_static_models else 0)
    struct.pack_into("<3I", pc_header, 288, len(cells),
                     (len(cells) + 31) // 32, INLINE if cells else 0)
    struct.pack_into("<2I", pc_header, 352, brush_model_count,
                     INLINE if brush_model_count else 0)
    # R_UpdateScene always reads the first 60-byte DPVS world record before
    # checking its internal surface count.
    struct.pack_into("<2I", pc_header, 360, 1, INLINE)
    # Renderer visibility initialization clears these buffers even when the
    # associated secondary visibility counts are zero. The free-list arrays
    # need one terminator entry beyond the world cell count.
    visibility_capacity = len(cells) + 1
    struct.pack_into("<2I", pc_header, 576,
                     visibility_capacity, visibility_capacity)
    # Four one-byte-per-static-model visibility arrays are allocated from
    # stream 1. They are runtime zero-fill storage and consume no archive data.
    struct.pack_into("<4I", pc_header, 584, INLINE, INLINE, INLINE, INLINE)
    struct.pack_into("<2I", pc_header, 664, INLINE, INLINE)
    struct.pack_into("<2I", pc_header, 680, INLINE, INLINE)
    # These three renderer work buffers are stream-1 zero-fill allocations.
    # The native loader sizes them from the DPVS surface count and cell count;
    # they consume virtual block space but no compressed archive bytes.
    struct.pack_into("<3I", pc_header, 620, INLINE, INLINE, INLINE)
    # Primary-light visibility is a stream-1 bitset sized by the native loader.
    struct.pack_into("<I", pc_header, 700, INLINE)
    struct.pack_into("<I", pc_header, 712,
                     INLINE if primary_light_count else 0)

    names = asset.get("names", [])
    world_name = names[0] if len(names) > 0 and isinstance(names[0], str) else asset["world_name"]
    base_name = names[1] if len(names) > 1 and isinstance(names[1], str) else asset["name"]
    payload.extend(pc_header)
    payload.extend(world_name.encode() + b"\0")
    payload.extend(base_name.encode() + b"\0")
    payload.extend(pc_planes)
    payload.extend(_little_endian_u16_array(nodes))
    payload.extend(_little_endian_u16_array(indices))
    payload.extend(pc_surfaces)
    if converted_materials is not None and len(converted_materials) != surface_count:
        raise FormatError("converted material count does not match world surfaces")
    for index in range(surface_count):
        material_value = converted_materials[index] if converted_materials else None
        if material_value is None:
            payload.extend(_pc_external_material())
        else:
            write_pc_material(payload, *material_value)
    payload.extend(_little_endian_words(sky_start_surfs))
    payload.extend(bytes(68))
    if include_static_models:
        payload.extend(convert_static_model_draws(
            static_draws, static_model_pointers))
        payload.extend(convert_static_model_instances(static_instances))
    for cell in cells:
        payload.extend(_pc_gfx_cell_header(
            cell, include_cell_trees, include_empty_cell_trees))
    for cell in cells:
        _write_pc_gfx_cell_nested(
            payload, cell, include_cell_trees, include_empty_cell_trees)
    # PC GfxWorld brush models are 168 bytes (Xbox records are 60). The
    # reduced probe has no brush collision, so retain the count with empty
    # PC-sized records instead of shifting every subsequent asset in the zone.
    payload.extend(bytes(brush_model_count * 168))
    payload.extend(bytes(60))
    payload.extend(pc_vertices)
    payload.extend(vertex_layers)
    payload.extend(bytes(primary_light_count * 12))


def _swap_record_fields(raw, stride, words=(), halves=()):
    if len(raw) % stride:
        raise FormatError(f"record array is not a multiple of {stride} bytes")
    converted = bytearray(raw)
    for base in range(0, len(raw), stride):
        for offset in words:
            struct.pack_into("<I", converted, base + offset,
                             u32(raw, base + offset))
        for offset in halves:
            struct.pack_into("<H", converted, base + offset,
                             u16(raw, base + offset))
    return converted


def _convert_clip_array(kind, raw):
    schemas = {
        "planes": (20, (0, 4, 8, 12), ()),
        "materials": (72, (64, 68), ()),
        "brush_sides": (12, (0, 4), (8,)),
        "nodes": (8, (0,), (4, 6)),
        "leaves": (44, (4, 8, 12, 16, 20, 24, 28, 32, 36),
                   (0, 2, 40)),
        "leaf_brush_nodes": (20, (4, 8, 12), (2, 16, 18)),
        "leaf_surfaces": (4, (0,), ()),
        "verts": (12, (0, 4, 8), ()),
        "brush_verts": (12, (0, 4, 8), ()),
        "borders": (28, (0, 4, 8, 12, 16, 20, 24), ()),
        "partitions": (20, (4, 8, 12, 16), ()),
        "aabb_trees": (32, (0, 4, 8, 16, 20, 24, 28), (12, 14)),
        "cmodels": (72, tuple(range(0, 28, 4))
                    + tuple(range(32, 68, 4)), (28, 30, 68)),
        "brushes": (80, tuple(range(0, 36, 4)) + (48, 72, 76),
                    tuple(range(36, 48, 2)) + tuple(range(52, 64, 2))),
    }
    if kind in ("brush_edges", "tri_edge_walkable"):
        return bytearray(raw)
    if kind in ("leaf_brushes", "uinds", "tri_indices"):
        return bytearray(_little_endian_u16_array(raw))
    stride, words, halves = schemas[kind]
    return _swap_record_fields(raw, stride, words, halves)


def _packed_block2_offset(pointer):
    if pointer in (0, INLINE, INSERT):
        return None
    encoded = pointer - 1
    if encoded >> 29 != 2:
        raise FormatError(f"collision pointer is not in block 2: {pointer:#x}")
    return encoded & 0x1FFFFFFF


def _clip_pointer_values(raw, stride, pointer_offsets):
    values = []
    for base in range(0, len(raw), stride):
        for offset in pointer_offsets:
            pointer = u32(raw, base + offset)
            packed = _packed_block2_offset(pointer)
            if packed is not None:
                values.append(packed)
    return values


def _source_collision_regions(collision):
    regions = {}
    planes = collision["planes"]
    if planes["data"] and planes["offset"] is not None:
        regions["planes"] = (int(planes["offset"], 16),
                              len(bytes.fromhex(planes["data"])))

    brushes = bytes.fromhex(collision["brushes"]["data"])
    if not brushes and not collision["brush_sides"]["data"]:
        return regions
    side_references = _clip_pointer_values(brushes, 80, (32,))
    edge_references = _clip_pointer_values(brushes, 80, (48,))
    if not side_references or not edge_references:
        raise FormatError("cannot establish Xenon collision allocation base")

    side_base = min(side_references)
    side_bytes = len(bytes.fromhex(collision["brush_sides"]["data"]))
    inline_side_bytes = sum(len(bytes.fromhex(value))
                            for value in collision["brush_sides"]["inline_planes"])
    if min(edge_references) != side_base + side_bytes + inline_side_bytes:
        raise FormatError("Xenon brush-side allocation base is ambiguous")

    alignments = {
        "brush_sides": 4, "brush_edges": 1, "nodes": 4, "leaves": 4,
        "leaf_brushes": 2, "leaf_brush_nodes": 4,
        "leaf_surfaces": 4, "verts": 4, "brush_verts": 4,
        "uinds": 2, "tri_indices": 2, "tri_edge_walkable": 1,
        "borders": 4, "partitions": 4, "aabb_trees": 4,
        "cmodels": 4, "brushes": 16,
    }
    nested_sizes = {
        "brush_sides": inline_side_bytes,
        "nodes": sum(len(bytes.fromhex(value))
                     for value in collision["nodes"]["inline_planes"]),
        "leaf_brush_nodes": sum(len(bytes.fromhex(value))
                                for value in collision["leaf_brush_nodes"]["inline_brushes"]),
        "partitions": sum(len(bytes.fromhex(value))
                          for value in collision["partitions"]["inline_borders"]),
    }
    cursor = side_base
    for kind, alignment in alignments.items():
        data = bytes.fromhex(collision[kind]["data"])
        if not data:
            continue
        cursor = _align_block2(cursor, alignment)
        regions[kind] = (cursor, len(data))
        cursor += len(data) + nested_sizes.get(kind, 0)

    leaf_nodes = bytes.fromhex(collision["leaf_brush_nodes"]["data"])
    leaf_references = []
    for offset in range(0, len(leaf_nodes), 20):
        if struct.unpack_from(">h", leaf_nodes, offset + 2)[0] > 0:
            pointer = _packed_block2_offset(u32(leaf_nodes, offset + 8))
            if pointer is not None:
                leaf_references.append(pointer)
    partitions = bytes.fromhex(collision["partitions"]["data"])
    border_references = []
    for offset in range(0, len(partitions), 20):
        if partitions[offset + 1] > 0:
            pointer = _packed_block2_offset(u32(partitions, offset + 16))
            if pointer is not None:
                border_references.append(pointer)

    references = {
        "planes": (_clip_pointer_values(
            bytes.fromhex(collision["brush_sides"]["data"]), 12, (0,))
            + _clip_pointer_values(bytes.fromhex(collision["nodes"]["data"]),
                                   8, (0,)), 20),
        "brush_sides": (side_references, 12),
        "brush_edges": (edge_references, 1),
        "brush_verts": (_clip_pointer_values(brushes, 80, (76,)), 12),
        "leaf_brushes": (leaf_references, 2),
        "borders": (border_references, 28),
    }
    for kind, (pointers, stride) in references.items():
        if not pointers:
            continue
        if kind not in regions:
            raise FormatError(f"{kind} has references but no captured array")
        base, size = regions[kind]
        if any(pointer < base or pointer >= base + size
               or (pointer - base) % stride for pointer in pointers):
            raise FormatError(f"{kind} packed references do not match its allocation")
    return regions


def _align_block2(cursor, alignment):
    return (cursor + alignment - 1) & -alignment


def _plan_pc_collision_regions(collision, cursor):
    alignments = {
        "planes": 4, "materials": 4, "brush_sides": 4,
        "brush_edges": 1, "nodes": 4, "leaves": 4,
        "leaf_brushes": 2, "leaf_brush_nodes": 4,
        "leaf_surfaces": 4, "verts": 4, "brush_verts": 4,
        "uinds": 2, "tri_indices": 2, "tri_edge_walkable": 1,
        "borders": 4, "partitions": 4, "aabb_trees": 4,
        "cmodels": 4, "brushes": 16,
    }
    nested_sizes = {
        "brush_sides": sum(len(bytes.fromhex(value))
                           for value in collision["brush_sides"]["inline_planes"]),
        "nodes": sum(len(bytes.fromhex(value))
                     for value in collision["nodes"]["inline_planes"]),
        "leaf_brush_nodes": sum(len(bytes.fromhex(value))
                                for value in collision["leaf_brush_nodes"]["inline_brushes"]),
        "partitions": sum(len(bytes.fromhex(value))
                          for value in collision["partitions"]["inline_borders"]),
        "brushes": sum(
            len(bytes.fromhex(value))
            for nested in collision["brushes"]["nested"]
            for key, value in nested.items() if key != "header"),
    }
    order = (
        "planes", "materials", "brush_sides", "brush_edges", "nodes",
        "leaves", "leaf_brushes", "leaf_brush_nodes", "leaf_surfaces",
        "verts", "brush_verts", "uinds", "tri_indices",
        "tri_edge_walkable", "borders", "partitions", "aabb_trees",
        "cmodels", "brushes",
    )
    regions = {}
    for kind in order:
        size = len(bytes.fromhex(collision[kind]["data"]))
        if not size:
            continue
        cursor = _align_block2(cursor, alignments[kind])
        regions[kind] = (cursor, size)
        cursor += size + nested_sizes.get(kind, 0)
    return regions, cursor


def _relocate_collision_pointer(pointer, source_regions, destination_regions):
    source_offset = _packed_block2_offset(pointer)
    if source_offset is None:
        return pointer
    for kind, (source_base, size) in source_regions.items():
        if source_base <= source_offset < source_base + size:
            if kind not in destination_regions:
                raise FormatError(f"collision pointer targets omitted {kind}")
            destination_base, _ = destination_regions[kind]
            return 0x40000001 + destination_base + source_offset - source_base
    raise FormatError(f"unresolved collision block-2 pointer {pointer:#x}")


def _patch_collision_pointers(kind, converted, source, source_regions,
                              destination_regions):
    pointer_fields = {
        "brush_sides": (12, (0,)),
        "nodes": (8, (0,)),
        "leaf_brush_nodes": (20, (8,)),
        "partitions": (20, (16,)),
        "brushes": (80, (32, 48, 76)),
    }
    if kind not in pointer_fields:
        return
    stride, offsets = pointer_fields[kind]
    for base in range(0, len(source), stride):
        for offset in offsets:
            if (kind == "leaf_brush_nodes" and offset == 8
                    and struct.unpack_from(">h", source, base + 2)[0] <= 0):
                continue
            if kind == "partitions" and offset == 16 and source[base + 1] == 0:
                continue
            pointer = u32(source, base + offset)
            relocated = _relocate_collision_pointer(
                pointer, source_regions, destination_regions)
            struct.pack_into("<I", converted, base + offset, relocated)


def _convert_clip_header(asset):
    source = bytes.fromhex(asset["header"])
    converted = bytearray(324)
    for offset in range(4, 188, 4):
        struct.pack_into("<I", converted, offset, u32(source, offset))
    struct.pack_into("<2H", converted, 156, u16(source, 156),
                     u16(source, 158))
    converted[188:260] = _convert_clip_array("cmodels", source[188:260])
    converted[260:262] = source[260:262]
    for offset in range(262, 270, 2):
        struct.pack_into("<H", converted, offset, u16(source, offset))
    converted[270:272] = source[270:272]
    for offset in range(272, 324, 4):
        struct.pack_into("<I", converted, offset, u32(source, offset))
    return converted


def _bind_shared_clip_planes(asset, gfx_world):
    source = bytes.fromhex(asset["header"])
    plane_pointer = u32(source, 12)
    if plane_pointer in (0, INLINE, INSERT):
        return

    plane_count = u32(source, 8)
    planes = bytes.fromhex(gfx_world["geometry"]["planes"])
    if len(planes) != plane_count * 20:
        raise FormatError(
            "shared clipMap plane count does not match GfxWorld planes")
    for offset in range(0, len(planes), 20):
        if planes[offset + 17] >= 8:
            raise FormatError("GfxWorld plane has an invalid sign mask")

    # Xenon clipMap points into the GfxWorld block-2 plane allocation. Copy the
    # shared allocation without changing its count or fabricating sentinels;
    # packed references are relocated separately by write_pc_clip_map.
    asset["collision"]["planes"]["data"] = planes.hex()


def write_pc_clip_map(payload, asset, block2_cursor, clip_name, entity_string,
                      entity_name):
    collision = asset["collision"]
    # Live QoS PC 1.1 evidence places the plane allocation at block-2 offset
    # 0xB2C. The generated cursor inferred from the archive is 0x12C too high.
    # Apply that measured logical-address correction without changing archive
    # byte order or array lengths.
    block2_cursor += PC_CLIP_BLOCK2_CURSOR_BIAS + 324
    destination_regions, collision_end = _plan_pc_collision_regions(
        collision, block2_cursor)
    source_regions = _source_collision_regions(collision)

    header = _convert_clip_header(asset)
    struct.pack_into("<I", header, 0, INLINE)
    root_arrays = {
        12: "planes", 20: "static_models", 28: "materials",
        36: "brush_sides", 44: "brush_edges", 52: "nodes",
        60: "leaves", 68: "leaf_brush_nodes", 76: "leaf_brushes",
        84: "leaf_surfaces", 92: "verts", 100: "brush_verts",
        108: "uinds", 116: "tri_indices", 120: "tri_edge_walkable",
        128: "borders", 136: "partitions", 144: "aabb_trees",
        152: "cmodels", 160: "brushes",
    }
    for offset, kind in root_arrays.items():
        struct.pack_into("<I", header, offset,
                         INLINE if kind in destination_regions else 0)
    struct.pack_into("<2I", header, 16, 0, 0)  # Omit static XModels for now.
    visibility = bytes.fromhex(asset.get("visibility", ""))
    struct.pack_into("<I", header, 172, INLINE if visibility else 0)
    struct.pack_into("<I", header, 180, INLINE)
    struct.pack_into("<I", header, 184, INLINE)
    header[262:270] = bytes(8)
    header[272:320] = bytes(48)

    payload.extend(header)
    payload.extend(clip_name.encode() + b"\0")

    nested_fields = {
        "brush_sides": "inline_planes",
        "nodes": "inline_planes",
        "leaf_brush_nodes": "inline_brushes",
        "partitions": "inline_borders",
    }
    for kind in (
            "planes", "materials", "brush_sides", "brush_edges", "nodes",
            "leaves", "leaf_brushes", "leaf_brush_nodes", "leaf_surfaces",
            "verts", "brush_verts", "uinds", "tri_indices",
            "tri_edge_walkable", "borders", "partitions", "aabb_trees",
            "cmodels", "brushes"):
        source = bytes.fromhex(collision[kind]["data"])
        if not source:
            continue
        converted = _convert_clip_array(kind, source)
        _patch_collision_pointers(kind, converted, source, source_regions,
                                  destination_regions)
        payload.extend(converted)
        if kind in nested_fields:
            for raw in collision[kind][nested_fields[kind]]:
                nested = bytes.fromhex(raw)
                payload.extend(_little_endian_u16_array(nested)
                               if kind == "leaf_brush_nodes"
                               else _convert_clip_array(
                                   "planes" if kind in ("brush_sides", "nodes")
                                   else "borders", nested))
        elif kind == "brushes":
            for nested in collision[kind]["nested"]:
                if "inline_side" in nested:
                    side = bytes.fromhex(nested["inline_side"])
                    converted_side = _convert_clip_array("brush_sides", side)
                    _patch_collision_pointers(
                        "brush_sides", converted_side, side, source_regions,
                        destination_regions)
                    payload.extend(converted_side)
                if "inline_plane" in nested:
                    payload.extend(_convert_clip_array(
                        "planes", bytes.fromhex(nested["inline_plane"])))
                if "inline_base_adjacent_side" in nested:
                    payload.extend(bytes.fromhex(
                        nested["inline_base_adjacent_side"]))
                if "inline_verts" in nested:
                    payload.extend(_convert_clip_array(
                        "brush_verts", bytes.fromhex(nested["inline_verts"])))

    payload.extend(visibility)
    payload.extend(struct.pack("<3I", INLINE, INLINE, len(entity_string)))
    payload.extend(entity_name.encode() + b"\0")
    payload.extend(entity_string)

    box_brush = asset.get("box_brush")
    if box_brush:
        source = bytes.fromhex(box_brush["header"])
        converted = _convert_clip_array("brushes", source)
        _patch_collision_pointers("brushes", converted, source,
                                  source_regions, destination_regions)
        payload.extend(converted)
    else:
        payload.extend(bytes(80))
    return collision_end


def build_pc_map_probe(path, include_images=False, include_materials=False):
    """Build a reduced PC zone for testing map and world deserialization.

    QoS PC mp_barge.ff confirms that PC and Xenon both use version 470 and the
    same 28-byte header. PC stores all seven words little-endian and orders its
    map roots as ComWorld, GfxWorld, GameWorldMp, then clipMap.
    """
    report = inspect(path, True, True)

    def one(kind):
        matches = [asset for asset in report["assets"] if asset["type"] == kind]
        if len(matches) != 1:
            raise FormatError(f"map probe requires exactly one {kind} asset")
        return matches[0]

    com_world = one("com_map")
    game_world = one("game_map_mp")
    clip_map = one("col_map_mp")
    gfx_world = one("gfx_map")
    _bind_shared_clip_planes(clip_map, gfx_world)
    map_ents = clip_map.get("map_ents")
    if not map_ents or not map_ents.get("entity_string"):
        raise FormatError("map probe requires inline map entities")

    gfx_names = gfx_world.get("names", [])
    world_name = gfx_names[0] if len(gfx_names) > 0 and isinstance(gfx_names[0], str) else com_world["name"]
    base_name = gfx_names[1] if len(gfx_names) > 1 and isinstance(gfx_names[1], str) else gfx_world["name"]
    game_name = game_world.get("name")
    if not isinstance(game_name, str):
        game_name = base_name
    clip_name = clip_map.get("name")
    if not isinstance(clip_name, str):
        clip_name = com_world["name"]
    entity_name = map_ents.get("name")
    if not isinstance(entity_name, str):
        entity_name = clip_name
    entity_string = map_ents["entity_string"].encode("latin-1")
    visibility = bytes.fromhex(clip_map.get("visibility", ""))
    visibility_count = clip_map.get("visibility_count", 0)
    visibility_stride = clip_map.get("visibility_stride", 0)
    if len(visibility) != visibility_count * visibility_stride:
        raise FormatError("clipMap visibility size does not match its dimensions")
    trailing_nul = entity_string.endswith(b"\0")
    text = entity_string.rstrip(b"\0").decode("latin-1")
    entities = re.findall(r"\{.*?\}\s*", text, re.DOTALL)
    if not entities or "".join(entities).rstrip() != text.rstrip():
        raise FormatError("map probe cannot safely split the entity string")
    entity_string = "".join(entities).encode("latin-1") + (b"\0" if trailing_nul else b"")
    gfx_world["world_name"] = com_world["name"]

    models_by_index = {
        asset["manifest_index"]: asset for asset in report["assets"]
        if asset["type"] == "xmodel"
    }
    used_model_indices = sorted(set(
        gfx_world["geometry"]["static_model_asset_indices"]))
    if any(index not in models_by_index for index in used_model_indices):
        raise FormatError("static model reference has no captured XModel asset")
    # Entity-driven models (for example script_model and skybox models) do not
    # appear in the GfxWorld static-draw list. Keep every manifest XModel so the
    # server can resolve those names without entering native default creation.
    models = [models_by_index[index] for index in sorted(models_by_index)]
    rawfiles = [
        asset for asset in report["assets"]
        if asset["type"] == "rawfile"
        and isinstance(asset.get("name"), str)
        and "data" in asset
    ]
    include_images = include_images or include_materials
    images_by_name = {}
    if include_images:
        for material_value in gfx_world["geometry"]["surface_materials"]:
            for image_value in material_value.get("textures", []):
                if "pc_base_level" in image_value:
                    images_by_name.setdefault(image_value["name"], image_value)
    images = list(images_by_name.values())
    convertible_materials = []
    techsets_by_name = {}
    if include_materials:
        for material_value in gfx_world["geometry"]["surface_materials"]:
            convertible = ("header" in material_value
                           and isinstance(material_value.get("techset_name"), str)
                           and all(image_value.get("name") in images_by_name
                                   for image_value in material_value.get("textures", [])))
            convertible_materials.append(material_value if convertible else None)
            if convertible:
                techsets_by_name.setdefault(
                    material_value["techset_name"], material_value)
    techset_names = list(techsets_by_name)
    assets = ([(12, "clip")]
              + [(5, model) for model in models]
              + [(7, name) for name in techset_names]
              + [(13, "com"), (17, "gfx"), (15, "game")]
              + [(8, image_value) for image_value in images]
              + [(32, rawfile) for rawfile in rawfiles])
    script_strings = report["script_strings"]
    payload = bytearray(struct.pack(
        "<4I", len(script_strings), INLINE if script_strings else 0,
        len(assets), INLINE))
    payload.extend(struct.pack("<I", INLINE) * len(script_strings))
    for value in script_strings:
        payload.extend(value.encode() + b"\0")
    asset_table_offset = len(payload)
    payload.extend(b"".join(struct.pack("<2I", kind, INLINE)
                            for kind, _ in assets))

    source_table_base = gfx_world["geometry"].get("asset_table_block2_offset")
    # Packed block-2 offsets are relative to the XFile stream after its
    # 11-byte runtime prefix, while report offsets start at the payload root.
    inferred_table_base = asset_table_offset - 11
    asset_table_base = (int(source_table_base, 16)
                        if source_table_base else inferred_table_base)
    if source_table_base and asset_table_base != inferred_table_base:
        raise FormatError(
            "generated script-string layout changed the verified block-2 table base")
    destination_indices = {
        model["manifest_index"]: destination_index + 1
        for destination_index, model in enumerate(models)
    }
    gfx_world["geometry"]["pc_static_model_pointers"] = [
        0x40000001 + asset_table_base + destination_indices[source_index] * 8
        for source_index in gfx_world["geometry"]["static_model_asset_indices"]
    ] if models else []

    block2_cursor = asset_table_base + len(assets) * 8
    clip_block2_end = write_pc_clip_map(
        payload, clip_map, block2_cursor, clip_name, entity_string, entity_name)

    for model in models:
        write_pc_xmodel(payload, model)

    for name in techset_names:
        payload.extend(_pc_external_techset(name))

    write_pc_com_world(payload, com_world)

    converted_material_bindings = None
    if include_materials:
        techset_start = 1 + len(models)
        techset_pointers = {
            name: 0x40000001 + asset_table_base + (techset_start + index) * 8
            for index, name in enumerate(techset_names)
        }
        image_start = techset_start + len(techset_names) + 3
        image_pointers = {
            image_value["name"]: 0x40000001 + asset_table_base
            + (image_start + index) * 8
            for index, image_value in enumerate(images)
        }
        converted_material_bindings = [
            ((material_value,
              techset_pointers[material_value["techset_name"]],
              [image_pointers[image_value["name"]]
               for image_value in material_value["textures"]])
             if material_value else None)
            for material_value in convertible_materials
        ]
    write_pc_gfx_world(payload, gfx_world, len(com_world["primary_lights"]),
                       converted_material_bindings)

    payload.extend(struct.pack("<I", INLINE))
    payload.extend(game_name.encode() + b"\0")

    for image_value in images:
        write_pc_image(payload, image_value)

    for rawfile in rawfiles:
        data = bytes.fromhex(rawfile["data"])
        if not data or not data.endswith(b"\0"):
            raise FormatError(f"rawfile {rawfile['name']} is not NUL-terminated")
        payload.extend(struct.pack("<3I", INLINE, len(data) - 1, INLINE))
        payload.extend(rawfile["name"].encode() + b"\0")
        payload.extend(data)

    # Native PC zones use distinct allocation sizes for five XFile blocks. The
    # probe has no physical/runtime payload, while its small temporary and
    # virtual streams safely fit in this conservative bound.
    allocation = max(len(payload), clip_block2_end) + 65536
    runtime_allocation = 65536
    result = bytearray(struct.pack("<7I", 470, len(payload), allocation,
                                   runtime_allocation, allocation, 0, 0))
    result.extend(zlib.compress(payload, 1))
    # Native PC and Xenon QoS fastfiles pad the zlib stream to 32 bytes.
    result.extend(bytes((-len(result)) % 32))
    return bytes(result)


def summarize_world_textures(gfx_world):
    materials = gfx_world.get("geometry", {}).get("surface_materials", [])
    images = [texture for material_value in materials
              if "textures" in material_value
              for texture in material_value["textures"]]
    decoded = [value for value in images if "pc_base_level" in value]
    errors = [{"name": value.get("name"),
               "error": value["pc_base_level_error"]}
              for value in images if "pc_base_level_error" in value]
    formats = Counter(value["pc_base_level"]["format"] for value in decoded)
    return {
        "surface_count": len(materials),
        "inline_materials": sum("textures" in value for value in materials),
        "packed_material_references": sum(
            "reference" in value for value in materials),
        "inline_images": len(images),
        "unique_image_names": len(set(
            value["name"] for value in images if isinstance(value.get("name"), str))),
        "decoded_base_levels": len(decoded),
        "decoded_base_bytes": sum(
            value["pc_base_level"]["bytes"] for value in decoded),
        "formats": dict(sorted(formats.items())),
        "errors": errors,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--details", action="store_true",
                        help="decode supported asset schemas and require exact stream consumption")
    parser.add_argument("--convert-map-probe", type=Path, metavar="OUTPUT",
                        help="emit a reduced PC v470 map zone for loader testing")
    parser.add_argument("--include-images", action="store_true",
                        help="include decoded base-level images in a converted map probe")
    parser.add_argument("--include-materials", action="store_true",
                        help="bind directly serialized world materials to converted images")
    parser.add_argument("--texture-summary", action="store_true",
                        help="validate captured GfxWorld base textures without emitting a zone")
    args = parser.parse_args()
    if args.texture_summary:
        if len(args.files) != 1:
            parser.error("--texture-summary requires exactly one input")
        try:
            report = inspect(args.files[0], True, True)
            worlds = [asset for asset in report["assets"]
                      if asset["type"] == "gfx_map"]
            if len(worlds) != 1:
                raise FormatError("texture summary requires exactly one gfx_map asset")
            print(json.dumps(summarize_world_textures(worlds[0]), indent=2))
            return 0
        except (OSError, FormatError) as error:
            print(json.dumps({"file": str(args.files[0]), "error": str(error)}))
            return 1
    if args.convert_map_probe:
        if len(args.files) != 1:
            parser.error("--convert-map-probe requires exactly one input")
        try:
            args.convert_map_probe.write_bytes(build_pc_map_probe(
                args.files[0], args.include_images, args.include_materials))
            print(json.dumps({"input": str(args.files[0]),
                              "output": str(args.convert_map_probe),
                              "bytes": args.convert_map_probe.stat().st_size}))
            return 0
        except (OSError, FormatError) as error:
            print(json.dumps({"file": str(args.files[0]), "error": str(error)}))
            return 1
    failed = False
    for path in args.files:
        try:
            print(json.dumps(inspect(path, args.details), indent=2))
        except (OSError, FormatError) as error:
            print(json.dumps({"file": str(path), "error": str(error)}))
            failed = True
    return int(failed)


if __name__ == "__main__":
    raise SystemExit(main())
