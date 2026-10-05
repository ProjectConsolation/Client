"""Dump original QoS Xbox images, without PC donors or material substitutions."""
import argparse
from collections import Counter
import hashlib
import io
import json
from pathlib import Path
import struct

import dump_pc_images as pc_dump
import xenon_ff


def collect_images(value):
    found = {}
    def visit(node):
        if isinstance(node, dict):
            if "pixel_offset" in node and "pixel_bytes" in node and isinstance(node.get("name"), str):
                found.setdefault((node["name"], node["pixel_offset"]), node)
            for child in node.values():
                visit(child)
        elif isinstance(node, list):
            for child in node:
                visit(child)
    visit(value)
    return list(found.values())


def linear_levels(image):
    base = image.get("pc_base_level")
    if not base:
        raise ValueError(image.get("pc_base_level_error", "no decoded source pixels"))
    chain = image.get("pc_mip_chain", {})
    complete = chain.get("levels") == chain.get("decoded_levels")
    # Never export the converter's approximated mip-tail data as original data.
    selected = chain if complete else base
    count = chain["levels"] if complete else 1
    width, height = image["width"], image["height"]
    faces = 6 if image.get("pc_map_type") == 5 else 1
    format_name = base["format"]
    block = 8 if format_name == "DXT1" else 16
    sizes = [(max(1, width >> level) * max(1, height >> level) * 4
              if format_name == "ARGB8" else
              ((max(1, width >> level) + 3) // 4) * ((max(1, height >> level) + 3) // 4) * block)
             for level in range(count)]
    data = bytes.fromhex(selected["data"])
    if len(data) != sum(sizes) * faces or image["depth"] != 1:
        raise ValueError("unsupported source dimensions/mip payload extent")
    return format_name, sizes, faces, data, complete


def source_dds(image):
    format_name, sizes, faces, data, _ = linear_levels(image)
    header = bytearray(128)
    count = len(sizes)
    uncompressed = format_name == "ARGB8"
    struct.pack_into("<4s7I", header, 0, b"DDS ", 124,
        0x1007 | (8 if uncompressed else 0x80000) | (0x20000 if count > 1 else 0),
        image["height"], image["width"], image["width"] * 4 if uncompressed else sizes[0], 0, count)
    if uncompressed:
        struct.pack_into("<8I", header, 76, 32, 0x41, 0, 32, 0xFF0000, 0xFF00, 0xFF, 0xFF000000)
    else:
        fourcc = {"DXT1": b"DXT1", "DXT2_3": b"DXT3", "DXT4_5": b"DXT5", "DXN": b"ATI2"}[format_name]
        struct.pack_into("<2I4s", header, 76, 32, 4, fourcc)
    struct.pack_into("<2I", header, 108, 0x1000 | (0x400008 if count > 1 else 0)
                     | (8 if faces == 6 else 0), 0xFE00 if faces == 6 else 0)
    # Untiler returns largest-mip-first, mip-major. DDS is face-major.
    chunks = {}
    offset = 0
    for level, size in enumerate(sizes):
        for face in range(faces):
            chunks[level, face] = data[offset:offset + size]
            offset += size
    return bytes(header) + b"".join(chunks[level, face] for face in range(faces) for level in range(count))


def pc_record(image, normal_slopes=False):
    format_name, sizes, faces, data, _ = linear_levels(image)
    if format_name == "ARGB8":
        raise ValueError("ARGB8 override available as source DDS/PNG, not compressed IWI")
    if format_name == "DXN":
        data = xenon_ff.transcode_dxn_to_dxt5(data, normal_slopes)
    chunks = []
    offset = 0
    for size in sizes:
        chunks.append(data[offset:offset + size * faces])
        offset += size * faces
    data = b"".join(reversed(chunks))  # Native PC/IWI order, unlike untile output.
    count = len(sizes)
    flags = 1 | (2 if count == 1 else 0) | (4 if faces == 6 else 0)
    header = bytearray(36)
    struct.pack_into("<2I", header, 0, 5 if faces == 6 else 3, xenon_ff.INSERT)
    struct.pack_into("<2I", header, 16, len(data), len(data))
    struct.pack_into("<3H", header, 24, image["width"], image["height"], 1)
    header[11] = 2
    header[30] = 3
    struct.pack_into("<I", header, 32, xenon_ff.INLINE)
    serialized = bytes(header) + image["name"].encode() + b"\0" + struct.pack("<2B3H4sI",
        count, flags, image["width"], image["height"], 1,
        xenon_ff.PC_TEXTURE_FOURCC[format_name], len(data)) + data
    return {"name": image["name"], "serialized": serialized}


def dump_images(source, destination, png=False, pc_overrides=False, normal_slopes=False):
    report = xenon_ff.inspect(source, True, True)
    images = collect_images(report)
    reader, _, _ = xenon_ff.read_zone(source)
    if png:
        from PIL import Image
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=False)
    inventory = {"source": str(Path(source).resolve()),
        "source_sha256": hashlib.sha256(Path(source).read_bytes()).hexdigest(),
        "inline_image_records": len(images), "images": [],
        "scope": "original embedded Xbox pixels only; no PC donors or guessed packed references",
        "pc_normal_conversion": "QoS slope probe" if normal_slopes else "DXT5nm unit XY",
        "duplicate_names": {name: count for name, count in Counter(i["name"] for i in images).items() if count > 1}}
    for image in images:
        entry = {key: image.get(key) for key in ("name", "offset", "width", "height", "depth",
            "pixel_offset", "pixel_bytes", "pixel_sha256", "load_definition", "load_reference", "texture_resource_words")}
        inventory["images"].append(entry)
        header_offset = int(image["offset"], 16)
        entry["xenon_header_hex"] = reader.data[header_offset:header_offset + 40].hex()
        if not image["pixel_bytes"]:
            entry["status"] = "external/shared reference without embedded pixels"
            continue
        try:
            basename = pc_dump.image_filename(image["name"])
        except ValueError as error:
            entry["error"] = str(error)
            continue
        entry["override_basename"] = basename
        if image["name"] in inventory["duplicate_names"]:
            basename = "variants/" + image["offset"] + "/" + basename
        entry["files"] = []
        def save(group, suffix, data):
            path = destination / group / (basename + suffix)
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("xb") as file:
                file.write(data)
            entry["files"].append(path.relative_to(destination).as_posix())
            return path
        offset = int(image["pixel_offset"], 16)
        pixels = reader.data[offset:offset + image["pixel_bytes"]]
        if hashlib.sha256(pixels).hexdigest() != image["pixel_sha256"]:
            raise ValueError("original tiled pixel extent/hash mismatch")
        save("raw", ".xenos.bin", pixels)
        try:
            format_name, sizes, faces, _, complete = linear_levels(image)
            entry["source_format"] = format_name
            entry["exported_levels"] = len(sizes)
            entry["all_source_mips_decoded"] = complete
            dds = source_dds(image)
            save("dds", ".dds", dds)
            if png:
                with Image.open(io.BytesIO(dds)) as preview:
                    preview.load()
                    path = destination / "png" / (basename + (".face-positive-x.png" if faces == 6 else ".png"))
                    path.parent.mkdir(parents=True, exist_ok=True)
                    preview.convert("RGBA").save(path)
                    entry["files"].append(path.relative_to(destination).as_posix())
            if pc_overrides:
                if format_name == "ARGB8":
                    save("pc-overrides/dds", ".dds", dds)
                else:
                    record = pc_record(image, normal_slopes)
                    save("pc-overrides/dds", ".dds", pc_dump.encode_dds(record))
                    try:
                        save("pc-overrides/iwi", ".iwi", pc_dump.encode_iwi(record))
                    except ValueError as error:
                        entry["iwi_error"] = str(error)
        except (ValueError, KeyError, OSError) as error:
            entry["error"] = str(error)
    inventory["dds_exports"] = sum(any(p.startswith("dds/") for p in i.get("files", [])) for i in inventory["images"])
    inventory["errors"] = sum("error" in i for i in inventory["images"])
    inventory["external_without_pixels"] = sum("status" in i for i in inventory["images"])
    with (destination / "manifest.json").open("x", encoding="utf-8") as file:
        json.dump(inventory, file, indent=2)
    return inventory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path, help="new dump directory; existing directories are never overwritten")
    parser.add_argument("--png", action="store_true", help="export decoded PNG previews (Pillow required)")
    parser.add_argument("--pc-overrides", action="store_true", help="also export PC-compatible compressed DDS/IWI (DXN is transcoded)")
    parser.add_argument("--normal-slopes", action="store_true", help="use the experimental QoS slope encoding only for PC DXN overrides")
    args = parser.parse_args()
    try:
        result = dump_images(args.source, args.destination, args.png, args.pc_overrides, args.normal_slopes)
    except (OSError, ValueError, ImportError) as error:
        parser.exit(1, f"Xbox image dump failed: {error}\n")
    print(json.dumps({key: result[key] for key in ("inline_image_records", "dds_exports", "external_without_pixels", "errors")}, indent=2))


if __name__ == "__main__":
    main()
