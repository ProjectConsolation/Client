"""Read-only QoS Xenon v470 fastfile inspection. Does not emit PC-loadable zones.

Layout evidence: Xenon sub_821E15F8, sub_821E8188, sub_821E7D60,
sub_821E7C78, sub_821E6100 in default_mp.xex. Asset names: 0x82547BA0.
Offsets in reports refer to the decompressed stream, not runtime block addresses.
"""

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
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
INLINE = 0xFFFFFFFF
INSERT = 0xFFFFFFFE
MAX_BYTES = 256 * 1024 * 1024


class FormatError(ValueError):
    pass


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
    for offset, field in ((28, "vertices"), (64, "secondary vertices"),
                          (100, "tertiary vertices")):
        _inline_bytes(reader, u32(header, offset), vertex_bytes,
                      f"xsurface {index} {field}")
    _inline_bytes(reader, u32(header, 140), u32(header, 136) * 8,
                  f"xsurface {index} rigid vertices")
    _inline_bytes(reader, u32(header, 8), result["triangle_count"] * 6,
                  f"xsurface {index} indices")
    result["blend_counts"] = blend_counts
    result["rigid_vertex_count"] = u32(header, 136)
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
        "type": u32(header, 16),
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
        payload_bytes = channel_map[0] * 8 if u32(channel_map, 4) else 0
        if payload_bytes:
            reader.take(payload_bytes)
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

    if u32(header, 180) in (INLINE, INSERT):
        raw = reader.take(12)
        reader.string(u32(raw))
        if u32(raw, 4):
            reader.take(u32(raw, 8))
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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--details", action="store_true",
                        help="decode the limited loading-screen profile; reject unsupported assets")
    args = parser.parse_args()
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
