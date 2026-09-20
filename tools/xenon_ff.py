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


def techset(reader):
    header = reader.take(156)
    name = reader.string(u32(header))
    if any(header[12:]):
        raise FormatError(f"techset {name!r} has shader techniques; unsupported")
    return {"name": name, "empty_techniques": True}


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
            elif kind == 6:
                asset = material(reader)
            elif kind == 33:
                header = reader.take(12)
                asset = {"name": reader.string(u32(header))}
                payload = reader.take(u32(header, 4) + 1) if u32(header, 8) else b""
                asset["bytes"] = len(payload)
                asset["sha256"] = hashlib.sha256(payload).hexdigest()
            else:
                raise FormatError(f"asset {index}: {ASSET_NAMES[kind]} details unsupported")
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
