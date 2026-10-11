"""Append controller movies/config references without re-encoding common HUD art.

Input must be the verified two-techset/inline-material common_consolation zone.
Output is a new file; installation and rollback are separate operations.
"""
import argparse
from pathlib import Path
import struct
import zlib
import package_common_consolation as common

ROOT = Path(__file__).resolve().parents[1]


def resources(root=ROOT):
    files = {}
    for path in sorted((root / 'required_files/consolation/scaleform').glob('*.gfx')):
        files['consolation/scaleform/' + path.name.lower()] = path.read_bytes()
    for path in sorted((root / 'consolation/controller_assets/native_xbox/code_post_gfx_mp').glob('*.cfg')):
        files['consolation/controller/native_xbox/' + path.name.lower()] = path.read_bytes()
    if len(files) != 24:
        raise ValueError('Expected 13 shared/controller movies and 11 native Xbox config references')
    if any(not data for data in files.values()):
        raise ValueError('Empty controller resource')
    return files


def split(payload):
    count = struct.unpack_from('<I', payload, 8)[0]
    table_end = 16 + count * 8
    entries = [struct.unpack_from('<2I', payload, 16 + i * 8) for i in range(count)]
    images = common.dump.find_images(payload)
    prefix = [(7, common.x.INLINE)] * 2 + [(6, common.x.INLINE)] * len(images)
    if entries[:len(prefix)] != prefix or any(e != (32, common.x.INLINE) for e in entries[len(prefix):]):
        raise ValueError('Unsupported common-zone manifest')
    material_end = table_end
    for name in images:
        offset, material = -1, None
        while material is None:
            offset = payload.find(name.encode() + b'\0', offset + 1)
            if offset < 0:
                raise ValueError('Missing native material: ' + name)
            material = common.x._pc_material_record_at(payload, name, offset)
        material_end = max(material_end, material['end_offset'])
    raw = {}
    cursor = material_end
    for _ in entries[len(prefix):]:
        nameptr, size, bufptr = struct.unpack_from('<3I', payload, cursor)
        if nameptr != common.x.INLINE or bufptr != common.x.INLINE:
            raise ValueError('Unsupported rawfile pointers')
        end = payload.index(0, cursor + 12)
        name = payload[cursor + 12:end].decode('ascii')
        cursor = end + 1
        if name in raw or cursor + size >= len(payload) or payload[cursor + size] != 0:
            raise ValueError('Invalid rawfile record')
        raw[name] = bytes(payload[cursor:cursor + size])
        cursor += size + 1
    if cursor != len(payload):
        raise ValueError('Unrecognized trailing asset bytes')
    return table_end, material_end, prefix, images, raw


def package(source, target, root=ROOT):
    if target.exists():
        raise ValueError('Refusing to overwrite output')
    original = common.x._read_pc_zone_payload(source)
    table_end, end, prefix, images, previous = split(original)
    files = resources(root)
    if set(previous) - set(files):
        raise ValueError('Refusing to discard unrelated rawfiles')
    payload = bytearray(original[:16])
    struct.pack_into('<I', payload, 8, len(prefix) + len(files))
    for entry in prefix + [(32, common.x.INLINE)] * len(files):
        payload.extend(struct.pack('<2I', *entry))
    payload.extend(original[table_end:end])
    for name, data in files.items():
        payload.extend(struct.pack('<3I', common.x.INLINE, len(data), common.x.INLINE))
        payload.extend(name.encode('ascii') + b'\0' + data + b'\0')
    _, _, _, found, raw = split(payload)
    if raw != files or {n: i['pixel_sha256'] for n, i in found.items()} != {n: i['pixel_sha256'] for n, i in images.items()}:
        raise ValueError('Resource/image round-trip failed')
    allocation = len(payload) + 65536
    blob = struct.pack('<7I', 470, len(payload), allocation, 65536, allocation, 0, 0)
    blob += zlib.compress(payload, 1)
    blob += bytes(-len(blob) % 32)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(blob)
    if common.x._read_pc_zone_payload(target) != payload:
        raise ValueError('Fastfile round-trip failed')
    print(f'Bundled {len(files)} rawfiles; preserved {len(images)} image/material records')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    package(args.source, args.output)
