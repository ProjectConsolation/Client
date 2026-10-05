"""Export validated inline QoS PC v470 compressed images for disk overrides.

DDS layout: Microsoft DDS_HEADER, face-major/largest-mip-first.
IWI v6 layout: KisakCOD r_image.h, mip-major/smallest-mip-first.
This is a payload dumper, not a parser for every asset type or an Xbox decoder.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import re
import struct

import xenon_ff


def safe_name(name):
    if not name or len(name) > 240 or name.startswith((",", "/", "\\")):
        return False
    if any(ord(c) < 32 or c in ':*?"<>|' for c in name):
        return False
    reserved = {"CON", "PRN", "AUX", "NUL"} | {f"{p}{i}" for p in ("COM", "LPT") for i in range(1, 10)}
    return all(part and part not in (".", "..") and not part.endswith((".", " "))
               and part.split(".")[0].upper() not in reserved
               for part in name.replace("\\", "/").split("/"))


def image_filename(name):
    encoded = "".join(f"%{c:02X}" if c < 32 or c >= 127 or chr(c) in ':*?"<>|%'
                      else "/" if c == 92 else chr(c) for c in name.encode("utf-8"))
    if not safe_name(encoded):
        raise ValueError("unsafe image asset path")
    return encoded


def image_parts(record):
    raw = record["serialized"]
    start = 36 + len(record["name"].encode()) + 1
    levels, flags, width, height, depth, fourcc, size = struct.unpack_from("<2B3H4sI", raw, start)
    count = levels or (1 if flags & 2 else max(width, height).bit_length())
    block = 8 if fourcc == b"DXT1" else 16
    faces = 6 if flags & 4 else 1
    sizes = [((max(1, width >> level) + 3) // 4)
             * ((max(1, height >> level) + 3) // 4) * block for level in range(count)]
    pixels = raw[start + 16:]
    if depth != 1 or size != len(pixels) or sum(sizes) * faces != size:
        raise ValueError("image payload size mismatch")
    return flags, width, height, fourcc, sizes, faces, pixels


def encode_iwi(record):
    flags, width, height, fourcc, sizes, faces, pixels = image_parts(record)
    expected_levels = 1 if flags & 2 else max(width, height).bit_length()
    if len(sizes) != expected_levels:
        raise ValueError("partial mip chain cannot be represented by override IWI")
    header = bytearray(28)
    struct.pack_into("<3s3B3H", header, 0, b"IWi", 6,
                     {b"DXT1": 11, b"DXT3": 12, b"DXT5": 13}[fourcc], flags, width, height, 1)
    for picmip in range(4):
        struct.pack_into("<I", header, 12 + picmip * 4,
                         28 + sum(sizes[min(picmip, len(sizes) - 1):]) * faces)
    return bytes(header) + pixels


def encode_dds(record):
    flags, width, height, fourcc, sizes, faces, pixels = image_parts(record)
    header = bytearray(128)
    mip_count = len(sizes)
    struct.pack_into("<4s7I", header, 0, b"DDS ", 124,
                     0x81007 | (0x20000 if mip_count > 1 else 0), height, width,
                     sizes[0], 0, mip_count)
    struct.pack_into("<2I4s", header, 76, 32, 4, fourcc)
    struct.pack_into("<2I", header, 108,
                     0x1000 | (0x400008 if mip_count > 1 else 0) | (8 if faces == 6 else 0),
                     0xFE00 if faces == 6 else 0)
    chunks = {}
    cursor = 0
    for level in reversed(range(mip_count)):
        for face in range(faces):
            chunks[level, face] = pixels[cursor:cursor + sizes[level]]
            cursor += sizes[level]
    return bytes(header) + b"".join(chunks[level, face] for face in range(faces) for level in range(mip_count))


def find_images(payload):
    records = {}
    # Exact GfxImage map type + inline/inserted loadDef prefix; every hit is
    # revalidated against dimensions, flags, data extent and card-memory size.
    for match in re.finditer(rb"[\x03\x05]\x00\x00\x00[\xfe\xff]\xff\xff\xff", payload):
        name_offset = match.start() + 36
        end = payload.find(b"\0", name_offset, name_offset + 241)
        if end <= name_offset:
            continue
        raw_name = payload[name_offset:end]
        if not all(32 <= c < 127 for c in raw_name):
            continue
        name = raw_name.decode("ascii")
        record = xenon_ff.pc_image_record_at(payload, name, name_offset)
        if record is None:
            continue
        if name in records and records[name]["pixel_sha256"] != record["pixel_sha256"]:
            raise ValueError(f"ambiguous duplicate image {name!r}; refusing to select pixels")
        records[name] = record
    return records


def dump_images(source, destination, png=False):
    payload = xenon_ff._read_pc_zone_payload(source)
    records = find_images(payload)
    if not records:
        raise ValueError("no validated inline DXT images found")
    if png:
        from PIL import Image  # Optional previews, not needed for DDS/IWI dumping.
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)  # Never overwrite an edited dump.
    inventory = {"source": str(Path(source).resolve()),
                 "source_sha256": hashlib.sha256(Path(source).read_bytes()).hexdigest(),
                 "validated_inline_images": len(records), "images": [],
                 "scope": "inline DXT1/3/5 payloads; external/shared references and non-DXT images are not synthesized"}
    inventory["external_images_without_pixels"] = [name for name in sorted(xenon_ff.PC_EXTERNAL_IMAGES)
        if bytes(32) + struct.pack("<I", xenon_ff.INLINE) + name.encode() + b"\0" in payload]
    for name, record in sorted(records.items()):
        entry = {key: record[key] for key in ("name", "width", "height", "map_type", "semantic", "fourcc", "pixel_sha256")}
        inventory["images"].append(entry)
        try:
            filename = image_filename(name)
        except ValueError:
            entry["skipped"] = "unsafe Windows image filename"
            continue
        entry["override_basename"] = filename
        relative = Path(filename)
        dds = encode_dds(record)
        entry["files"] = []
        exports = [("dds", dds)]
        try:
            exports.append(("iwi", encode_iwi(record)))
        except ValueError as error:
            entry["iwi_error"] = str(error)
        for extension, data in exports:
            target = destination / extension / (str(relative) + "." + extension)
            target.parent.mkdir(parents=True, exist_ok=True)
            with target.open("xb") as file:
                file.write(data)
            entry["files"].append(str(target.relative_to(destination)).replace("\\", "/"))
        if png:
            with Image.open(io.BytesIO(dds)) as image:
                image.load()
                target = destination / "png" / (str(relative) + ".png")
                if record["map_type"] == 5:
                    # A PNG cannot represent a cube. Export an explicitly named
                    # face preview, never a misleading whole-cube override.
                    target = destination / "png" / (str(relative) + ".face-positive-x.png")
                target.parent.mkdir(parents=True, exist_ok=True)
                image.convert("RGBA").save(target)
                entry["files"].append(str(target.relative_to(destination)).replace("\\", "/"))
    with (destination / "manifest.json").open("x", encoding="utf-8") as file:
        json.dump(inventory, file, indent=2)
    return inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="converted QoS PC fastfile")
    parser.add_argument("destination", type=Path, help="new dump directory (must not already exist)")
    parser.add_argument("--png", action="store_true", help="also export largest-mip PNG previews (requires Pillow)")
    args = parser.parse_args()
    try:
        inventory = dump_images(args.source, args.destination, args.png)
    except (OSError, ValueError, xenon_ff.FormatError, ImportError) as error:
        parser.exit(1, f"image dump failed: {error}\n")
    print(json.dumps({"images": inventory["validated_inline_images"], "destination": str(args.destination)}, indent=2))


if __name__ == "__main__":
    main()
