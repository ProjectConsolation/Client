"""Inspect QoS Xenon v470 fastfiles and emit an experimental PC map-root probe.

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
MAX_BYTES = 256 * 1024 * 1024


class FormatError(ValueError):
    pass


def pc_asset_type(xenon_type):
    if not 0 <= xenon_type < len(ASSET_NAMES):
        raise FormatError(f"invalid Xenon asset type {xenon_type}")
    if xenon_type == 7:
        return None
    return xenon_type if xenon_type < 7 else xenon_type - 1


def u32(data, offset=0):
    return struct.unpack_from(">I", data, offset)[0]


def u16(data, offset=0):
    return struct.unpack_from(">H", data, offset)[0]


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


def image(reader, pointer):
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
        result["load_definition"] = {
            "levels": load[0], "flags": load[1],
            "dimensions": struct.unpack_from(">3H", load, 2),
            "format": hex(u32(load, 8)),
        }
        if u32(load, 12):
            result["texture_resource_words"] = struct.unpack(">13I", reader.take(52))
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


def material(reader):
    header = reader.take(96)
    result = {"name": reader.string(u32(header)), "textures": []}
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
            result["textures"].append(image(reader, u32(textures, offset + 8)))
    elif pointer:
        result["texture_reference"] = hex(pointer)
    for offset, count, size, name in ((84, header[61], 32, "constants"),
                                      (88, header[62], 8, "state_bits")):
        pointer = u32(header, offset)
        if pointer == INLINE:
            result[name] = reader.take(count * size).hex()
        elif pointer:
            result[name + "_reference"] = hex(pointer)
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


def xsurface(reader, header, index):
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
    _inline_bytes(reader, u32(header, 20), blend_indices * 2,
                  f"xsurface {index} blend indices")
    _inline_bytes(reader, u32(header, 24), blend_total * 48,
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
    _inline_bytes(reader, u32(header, 140), u32(header, 136) * 8,
                  f"xsurface {index} rigid vertices")
    _inline_bytes(reader, u32(header, 8), result["triangle_count"] * 6,
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
    }
    if u32(header, 16):
        lights = reader.take(result["primary_light_count"] * 68)
        for index in range(result["primary_light_count"]):
            pointer = u32(lights, index * 68 + 64)
            if pointer == INLINE:
                result["primary_light_names"].append(reader.string(pointer))
            elif pointer:
                result["primary_light_names"].append({"reference": hex(pointer)})
    if u32(header, 40):
        lights = reader.take(result["water_light_count"] * 36)
        for index in range(result["water_light_count"]):
            if u32(lights, index * 36 + 32):
                reader.take(5120)
    return result


def lightdef(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(16)
    result = {"name": reader.string(u32(header))}
    if u32(header, 4):
        result["attenuation_image"] = image(reader, u32(header, 4))
    return result


def _gfx_portal(reader, header):
    if u32(header, 36) == INLINE:
        reader.take(u32(header, 32) * 4)
    if u32(header, 44):
        portals = reader.take(u32(header, 40) * 48)
        for offset in range(0, len(portals), 48):
            _gfx_portal(reader, portals[offset:offset + 48])


def _gfx_cell(reader, header):
    if u32(header, 24):
        _gfx_portal(reader, reader.take(48))
    if u32(header, 32):
        entries = reader.take(u32(header, 28) * 68)
        for offset in range(0, len(entries), 68):
            entry = entries[offset:offset + 68]
            if u32(entry, 32) == INLINE:
                _gfx_cell(reader, reader.take(52))
            if u32(entry, 36):
                reader.take(12 * entry[40])
    if u32(header, 40):
        reader.take(u32(header, 36) * 4)
    if u32(header, 48):
        reader.take(header[44])


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


def gfx_map(reader, pointer):
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    header = reader.take(828)
    names = [reader.string(u32(header, offset)) for offset in (0, 4)]

    _inline_bytes(reader, u32(header, 12), u32(header, 8) * 20,
                  "gfx_map planes")
    if u32(header, 20):
        reader.take(u32(header, 16) * 2)
    if u32(header, 28):
        reader.take(u32(header, 24) * 2)

    if u32(header, 68):
        surfaces = reader.take(u32(header, 64) * 72)
        for offset in range(0, len(surfaces), 72):
            material_pointer = u32(surfaces, offset + 40)
            if material_pointer in (INLINE, INSERT):
                material(reader)

    light_grid = header[72:88]
    if u32(light_grid, 4):
        reader.take(u32(light_grid) * 32)
    if u32(light_grid, 12):
        reader.take(u32(light_grid, 8) * 4)
    if u32(header, 108):
        reader.take(u32(header, 104) * 4)
    attenuation = image(reader, u32(header, 112)) if u32(header, 112) else None
    reader.string(u32(header, 120))

    if u32(header, 332) == INLINE:
        sun = reader.take(68)
        if u32(sun, 64):
            lightdef(reader, u32(sun, 64))
    if u32(header, 368):
        records = reader.take(u32(header, 364) * 16)
        for offset in range(0, len(records), 16):
            if u32(records, offset + 12) in (INLINE, INSERT):
                image(reader, u32(records, offset + 12))
    if u32(header, 372):
        reader.take(u32(header, 360) * 32)
    if u32(header, 380):
        records = reader.take(u32(header, 376) * 40)
        for offset in range(0, len(records), 40):
            if u32(records, offset + 32) in (INLINE, INSERT):
                xmodel(reader)
    if u32(header, 384):
        reader.take(u32(header, 376) * 32)
    if u32(header, 396):
        cells = reader.take(u32(header, 388) * 52)
        for offset in range(0, len(cells), 52):
            _gfx_cell(reader, cells[offset:offset + 52])
    if u32(header, 404):
        records = reader.take(u32(header, 400) * 8)
        for offset in range(0, len(records), 4):
            if u32(records, offset) in (INLINE, INSERT):
                image(reader, u32(records, offset))

    _gfx_dpvs_planes(reader, header[408:460])
    draw_surfaces = b""
    if u32(header, 464):
        draw_surfaces = reader.take(u32(header, 460) * 60)
    if u32(header, 500):
        records = reader.take(u32(header, 496) * 8)
        for offset in range(0, len(records), 8):
            if u32(records, offset) in (INLINE, INSERT):
                material(reader)

    vertex_data = header[128:164]
    if u32(vertex_data):
        reader.take(u32(header, 124) * 44)
    index_data = header[168:204]
    if u32(index_data):
        reader.take(u32(header, 164))

    world_draw = header[504:600]
    for offset in (4, 8):
        if u32(world_draw, offset) in (INLINE, INSERT):
            material(reader)
    if u32(header, 664):
        attenuation = image(reader, u32(header, 664))

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
        material(reader)

    return {
        "name": names[1] or names[0],
        "names": names,
        "surface_count": surface_count,
        "model_count": u32(header, 376),
        "attenuation_image": attenuation,
        "zero_fill_fields": zero_fill_fields,
    }


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
    if u32(header, 32) == INLINE:
        shape = reader.take(12)
        if u32(shape) == INLINE:
            reader.take(20)
    if u32(header, 48) == INLINE:
        reader.take(1)
    if u32(header, 76) == INLINE:
        reader.take(12)


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

    _inline_bytes(reader, u32(header, 12), u32(header, 8) * 20,
                  "col_map planes")
    if u32(header, 20):
        records = reader.take(u32(header, 16) * 80)
        for offset in range(0, len(records), 80):
            if u32(records, offset + 4) in (INLINE, INSERT):
                xmodel(reader)
    if u32(header, 28):
        reader.take(u32(header, 24) * 72)
    if u32(header, 36):
        shapes = reader.take(u32(header, 32) * 12)
        for offset in range(0, len(shapes), 12):
            if u32(shapes, offset) == INLINE:
                reader.take(20)
    if u32(header, 44):
        reader.take(u32(header, 40))
    if u32(header, 52):
        records = reader.take(u32(header, 48) * 8)
        for offset in range(0, len(records), 8):
            if u32(records, offset) == INLINE:
                reader.take(20)
    if u32(header, 60):
        reader.take(u32(header, 56) * 44)
    if u32(header, 76):
        reader.take(u32(header, 72) * 2)
    if u32(header, 68):
        records = reader.take(u32(header, 64) * 20)
        for offset in range(0, len(records), 20):
            count = struct.unpack_from(">h", records, offset + 2)[0]
            if count > 0 and u32(records, offset + 8) == INLINE:
                reader.take(count * 2)
    for count_offset, pointer_offset, stride in ((80, 84, 4), (88, 92, 12),
                                                  (96, 100, 12), (104, 108, 2),
                                                  (112, 116, 6)):
        if u32(header, pointer_offset):
            reader.take(u32(header, count_offset) * stride)
    if u32(header, 120):
        reader.take(((3 * u32(header, 112) + 31) >> 3) & 0xFFFFFFFC)
    if u32(header, 128):
        reader.take(u32(header, 124) * 28)
    if u32(header, 136):
        records = reader.take(u32(header, 132) * 20)
        for offset in range(0, len(records), 20):
            if u32(records, offset + 16) == INLINE:
                reader.take(28)
    if u32(header, 144):
        reader.take(u32(header, 140) * 32)
    if u32(header, 152):
        reader.take(u32(header, 148) * 72)
    if u32(header, 160):
        brushes = reader.take(u16(header, 156) * 80)
        for offset in range(0, len(brushes), 80):
            _col_brush(reader, brushes[offset:offset + 80])
    if u32(header, 172):
        reader.take(u32(header, 164) * u32(header, 168))

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
    if u32(header, 184) == INLINE:
        _col_brush(reader, reader.take(80))

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
        "plane_count": u32(header, 8),
        "brush_count": u16(header, 156),
        "dynamic_entity_counts": list(entity_counts),
        "zero_fill_bytes": zero_fill_bytes,
        "map_ents": map_ents,
    }


def xmodel(reader):
    start = reader.pos
    header = reader.take(240)
    name = reader.string(u32(header))
    bone_count = header[4]
    root_bone_count = header[5]
    surface_count = header[6]
    if root_bone_count > bone_count:
        raise FormatError(f"xmodel {name!r} has more root bones than bones")
    child_bones = bone_count - root_bone_count

    _inline_bytes(reader, u32(header, 8), bone_count * 2,
                  f"xmodel {name!r} bone names")
    _inline_bytes(reader, u32(header, 12), child_bones,
                  f"xmodel {name!r} parent list")
    _inline_bytes(reader, u32(header, 16), child_bones * 8,
                  f"xmodel {name!r} quaternions")
    _inline_bytes(reader, u32(header, 20), child_bones * 16,
                  f"xmodel {name!r} translations")
    _inline_bytes(reader, u32(header, 24), bone_count,
                  f"xmodel {name!r} part classification")
    _inline_bytes(reader, u32(header, 28), bone_count * 32,
                  f"xmodel {name!r} base matrices")

    surfaces = []
    surface_pointer = u32(header, 32)
    if surface_pointer:
        surface_headers = reader.take(surface_count * 200)
        for index in range(surface_count):
            surface = surface_headers[index * 200:(index + 1) * 200]
            surfaces.append(xsurface(reader, surface, index))

    materials = []
    material_pointer = u32(header, 36)
    if material_pointer:
        references = reader.take(surface_count * 4)
        for index in range(surface_count):
            pointer = u32(references, index * 4)
            if pointer in (INLINE, INSERT):
                materials.append(material(reader))
            else:
                materials.append({"reference": hex(pointer)})

    if u32(header, 168):
        reader.take(u32(header, 172) * 36)
    if u32(header, 180):
        reader.take(bone_count * 40)
    collision_count = u16(header, 44)
    if u32(header, 216):
        reader.take(collision_count * 24)

    phys_preset = None
    if u32(header, 228):
        phys_preset = _phys_preset(reader, u32(header, 228))
    physics = None
    if u32(header, 232):
        physics = physics_geometry(reader, u32(header, 232))
    if u32(header, 236):
        raise FormatError(f"xmodel {name!r} has unsupported collision tree at "
                          f"stream offset 0x{reader.pos:x}")

    return {
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


def inspect(path, details=False):
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
                asset = xmodel(reader)
            elif kind == 1:
                asset = _phys_preset(reader, pointer)
            elif kind == 14:
                asset = com_map(reader, pointer)
            elif kind == 19:
                asset = lightdef(reader, pointer)
            elif kind == 18:
                asset = gfx_map(reader, pointer)
            elif kind == 16:
                asset = game_map_mp(reader, pointer)
            elif kind == 13:
                asset = col_map_mp(reader, pointer)
            elif kind == 10:
                asset = sound(reader, pointer)
            elif kind == 27:
                asset = fx(reader, pointer)
            elif kind == 6:
                asset = material(reader)
            elif kind == 33:
                header = reader.take(12)
                asset = {"name": reader.string(u32(header))}
                payload = reader.take(u32(header, 4) + 1) if u32(header, 8) else b""
                asset["bytes"] = len(payload)
                asset["sha256"] = hashlib.sha256(payload).hexdigest()
            else:
                raise FormatError(f"asset {index}: {ASSET_NAMES[kind]} details unsupported "
                                  f"at stream offset 0x{start:x}")
            assets.append({"type": ASSET_NAMES[kind], "offset": hex(start), **asset})
        report["assets"] = assets
        report["unconsumed_payload_bytes"] = len(reader.data) - reader.pos
        if reader.pos != len(reader.data):
            raise FormatError(f"unconsumed payload at 0x{reader.pos:x}")
    return report


def build_pc_map_probe(path):
    """Build a geometry-free PC zone for testing map-root deserialization."""
    report = inspect(path, True)

    def one(kind):
        matches = [asset for asset in report["assets"] if asset["type"] == kind]
        if len(matches) != 1:
            raise FormatError(f"map probe requires exactly one {kind} asset")
        return matches[0]

    com_world = one("com_map")
    game_world = one("game_map_mp")
    clip_map = one("col_map_mp")
    gfx_world = one("gfx_map")
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
    trailing_nul = entity_string.endswith(b"\0")
    text = entity_string.rstrip(b"\0").decode("latin-1")
    entities = re.findall(r"\{.*?\}\s*", text, re.DOTALL)
    if not entities or "".join(entities).rstrip() != text.rstrip():
        raise FormatError("map probe cannot safely split the entity string")
    entities = [entity for entity in entities
                if '"classname" "worldspawn"' in entity or '"model" "*' not in entity]
    entity_string = "".join(entities).encode("latin-1") + (b"\0" if trailing_nul else b"")

    assets = ((13, "com"), (15, "game"), (12, "clip"), (17, "gfx"))
    payload = bytearray(struct.pack("<4I", 0, 0, len(assets), INLINE))
    payload.extend(b"".join(struct.pack("<2I", kind, INLINE) for kind, _ in assets))

    com_header = bytearray(44)
    struct.pack_into("<2I", com_header, 0, INLINE, 1)
    payload.extend(com_header)
    payload.extend(com_world["name"].encode() + b"\0")

    payload.extend(struct.pack("<I", INLINE))
    payload.extend(game_name.encode() + b"\0")

    clip_header = bytearray(324)
    struct.pack_into("<2I", clip_header, 0, INLINE, 1)
    struct.pack_into("<2I", clip_header, 148, 1, INLINE)
    struct.pack_into("<I", clip_header, 180, INLINE)
    payload.extend(clip_header)
    payload.extend(clip_name.encode() + b"\0")
    payload.extend(bytes(72))  # world cmodel; brush-model entities were removed above
    payload.extend(struct.pack("<3I", INLINE, INLINE, len(entity_string)))
    payload.extend(entity_name.encode() + b"\0")
    payload.extend(entity_string)

    gfx_header = bytearray(728)
    struct.pack_into("<2I", gfx_header, 0, INLINE, INLINE)
    payload.extend(gfx_header)
    payload.extend(world_name.encode() + b"\0")
    payload.extend(base_name.encode() + b"\0")

    allocation = len(payload) + 65536
    result = bytearray(struct.pack("<7I", 470, len(payload), allocation, 0,
                                   allocation, 0, 0))
    result.extend(zlib.compress(payload, 1))
    result.extend(bytes((-len(result)) % 0x20000))
    return bytes(result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--details", action="store_true",
                        help="decode supported asset schemas and require exact stream consumption")
    parser.add_argument("--convert-map-probe", type=Path, metavar="OUTPUT",
                        help="emit a geometry-free PC v470 zone for loader testing")
    args = parser.parse_args()
    if args.convert_map_probe:
        if len(args.files) != 1:
            parser.error("--convert-map-probe requires exactly one input")
        try:
            args.convert_map_probe.write_bytes(build_pc_map_probe(args.files[0]))
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
