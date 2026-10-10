"""Package stance, controller glyphs and original console art as native PC 2D assets."""
import argparse
import io
from pathlib import Path
import struct
import sys
import zlib

sys.path.insert(0, str(Path(__file__).parent / "xenon-converter"))
import dump_pc_images as dump
import xenon_ff as x


def encode_white_coverage(rgba):
    """BC3 white HUD mask: do not compress transparent black into the RGB edge.

    Keep the source alpha coverage; fit each block against explicit BC3 alpha
    palettes instead of the general-purpose DDS encoder's endpoint heuristic.
    Prone icons are monochrome masks. Native stand/crouch retain their RGB.
    """
    if rgba.size != (128, 128):
        raise ValueError("HUD coverage must be 128x128")
    alpha = rgba.getchannel("A")
    output = bytearray()
    for y in range(0, 128, 4):
        for bx in range(0, 128, 4):
            samples = list(alpha.crop((bx, y, bx + 4, y + 4)).getdata())
            nonzero = [a for a in samples if a]
            lo, hi = min(samples), max(samples)
            pairs = {(255, 0), (hi, lo), (lo, hi)}
            if nonzero:
                pairs.add((max(nonzero), min(nonzero)))
                pairs.add((min(nonzero), max(nonzero)))
            best = None
            for a0, a1 in sorted(pairs):
                palette = [a0, a1]
                if a0 > a1:
                    palette += [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
                else:
                    palette += [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255]
                indices = [min(range(8), key=lambda i: abs(a - palette[i])) for a in samples]
                # Preserve fully transparent pixels and opaque interiors exactly.
                if any(a in (0, 255) and palette[i] != a for a, i in zip(samples, indices)):
                    continue
                error = sum((a - palette[i]) ** 2 for a, i in zip(samples, indices))
                packed = sum(index << (3 * i) for i, index in enumerate(indices))
                candidate = (error, a0, a1, packed)
                if best is None or candidate < best:
                    best = candidate
            _, a0, a1, packed = best
            output.extend(bytes((a0, a1)) + packed.to_bytes(6, "little"))
            output.extend(struct.pack("<HHI", 0xFFFF, 0xFFFF, 0))
    return bytes(output)


def donor(payload, name):
    offset = payload.index(name.encode() + b"\0")
    header = payload[offset - 104:offset]
    if len(header) != 104 or struct.unpack_from("<I", header)[0] != x.INLINE:
        raise ValueError("Invalid native material")
    if list(header[67:70]) != [1, 0, 1]:
        raise ValueError("Expected one texture, no constants, one blend state")
    start = offset + len(name) + 1
    textures = payload[start:start + 12]
    if textures[8:] != b"\xff" * 4:
        raise ValueError("Expected inline native image")
    load = start + 12 + 36
    end = load + 16 + struct.unpack_from("<I", payload, load + 12)[0]
    return dict(header=header, textures=textures, constants=b"",
                state_bits=payload[end:end + 8])


def image_record(name, pixels, width=128, height=128, fourcc=b"DXT5"):
    header = bytearray(36)
    struct.pack_into("<2I", header, 0, 3, x.INSERT)
    header[10] = 1
    header[30] = 3
    struct.pack_into("<2I", header, 16, len(pixels), len(pixels))
    struct.pack_into("<3H", header, 24, width, height, 1)
    struct.pack_into("<I", header, 32, x.INLINE)
    raw = bytes(header) + name.encode() + b"\0"
    raw += struct.pack("<2B3H4sI", 1, 2, width, height, 1, fourcc, len(pixels)) + pixels
    record = x.pc_image_record_at(raw, name, 36)
    if record is None:
        raise ValueError("Invalid texture serialization")
    return record


def controller_records(directory, xbox_directory=None, ps3_directory=None):
    from PIL import Image
    from export_controller_prompt_icons import BUTTONS
    records = {}
    for family in ("xbox", "ps3"):
        for button in BUTTONS.values():
            name = f"qos_controller_{family}_{button}"
            with Image.open(directory / f"controller_{family}_{button}.png") as source:
                rgba = source.convert("RGBA")
                if rgba.size != (32, 32) or rgba.getbbox() is None:
                    raise ValueError(f"{name}: invalid controller sprite")
                dds = io.BytesIO()
                rgba.save(dds, format="DDS", pixel_format="DXT5")
            records[name] = image_record(name, dds.getvalue()[128:], 32, 32)
    for console_directory, required in (
            (xbox_directory, {"xenon_controller_ingame", "xenon_controller_lines_mp"}),
            (ps3_directory, {"ps3_controller_ingame", "ps3_controller_lines_mp"})):
        if console_directory is None:
            continue
        # Preserve original names, dimensions and compressed pixels. Includes the
        # full controller, leader-line overlay, and stick/shoulder diagrams.
        paths = sorted(console_directory.glob("*.dds"))
        if not required <= {path.stem for path in paths}:
            raise ValueError("original console controller picture/lines are required")
        for path in paths:
            data = path.read_bytes()
            if data[:4] != b"DDS " or len(data) < 128:
                raise ValueError(f"{path}: invalid DDS")
            height, width = struct.unpack_from("<2I", data, 12)
            fourcc = data[84:88]
            if path.stem in records:
                raise ValueError(f"{path}: duplicate console asset name")
            if fourcc not in (b"DXT1", b"DXT3", b"DXT5") or not width or not height:
                raise ValueError(f"{path}: unsupported console texture format")
            # Exported atlases can include mips; keep the original base level
            # byte-for-byte instead of resampling console controller artwork.
            size = ((width + 3) // 4) * ((height + 3) // 4) * (8 if fourcc == b"DXT1" else 16)
            if len(data) < 128 + size:
                raise ValueError(f"{path}: truncated console texture")
            records[path.stem] = image_record(path.stem, data[128:128 + size], width, height, fourcc)
    return records


def build(source, directory, target, controller_directory=None, xbox_directory=None, ps3_directory=None):
    from PIL import Image
    if target.exists():
        raise ValueError(f"Refusing to overwrite {target}")
    native = x._read_pc_zone_payload(source)
    names = ["qos_stance_" + stance + faction
             for faction in ("", "_org", "_mi6")
             for stance in ("stand", "crouch", "prone")]
    records = {}
    for name in names:
        png = directory / (name + ".png")
        with Image.open(png) as source_image:
            rgba = source_image.convert("RGBA").resize((128, 128), Image.Resampling.LANCZOS)
            alpha_min, alpha_max = rgba.getchannel("A").getextrema()
            # Area-filtered previews can peak at 254 rather than exactly 255.
            if alpha_min != 0 or alpha_max < 250:
                raise ValueError(f"{name}: missing transparent background or opaque silhouette")
            dds = io.BytesIO()
            rgba.save(dds, format="DDS", pixel_format="DXT5")
        encoded = dds.getvalue()
        if encoded[84:88] != b"DXT5" or len(encoded) != 128 + 16384:
            raise ValueError("Unexpected DDS encoder output")
        pixels = encode_white_coverage(rgba) if "_prone" in name else encoded[128:]
        record = image_record(name, pixels)
        records[name] = record
        iwi = directory / (name + ".iwi")
        if not iwi.exists():
            iwi.write_bytes(dump.encode_iwi(record))
    if controller_directory is not None:
        records.update(controller_records(controller_directory, xbox_directory, ps3_directory))
        names = list(records)
    # Same external-technique manifest layout as build_pc_load_zone.
    entries = [(7, ",sm2/2d"), (7, ",2d")] + [(6, name) for name in names]
    payload = bytearray(struct.pack("<4I", 0, 0, len(entries), x.INLINE))
    payload.extend(b"".join(struct.pack("<2I", kind, x.INLINE) for kind, _ in entries))
    tech_pointer = 0x40000001 + 4 + 8
    for name in (",sm2/2d", ",2d"):
        payload.extend(x._pc_external_techset(name))
    for name in names:
        original = (name.removeprefix("qos_").replace("prone", "crouch")
                    if name.startswith("qos_stance_") else "stance_stand")
        material = dict(name=name, header=bytes(96).hex(), techset_name=",2d",
                        pc_material_donor=donor(native, original),
                        textures=[dict(name=name, pc_image_donor=records[name])])
        start = len(payload)
        x.write_pc_material(payload, material, tech_pointer, [x.INLINE], True)
        parsed = x._pc_material_record_at(payload, name, start + 104)
        if parsed is None or parsed["end_offset"] != len(payload):
            raise ValueError(f"{name}: material did not round-trip")
    allocation = len(payload) + 65536
    blob = struct.pack("<7I", 470, len(payload), allocation, 65536, allocation, 0, 0)
    blob += zlib.compress(payload, 1)
    blob += bytes(-len(blob) % 32)
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(blob)
    if x._read_pc_zone_payload(target) != payload:
        raise ValueError("Fastfile round-trip failed")
    found = dump.find_images(payload)
    if set(found) != set(names):
        raise ValueError("Fastfile image inventory mismatch")
    print(f"Created {target}: {len(names)} materials and {len(found)} inline images")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("images", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--controller-images", type=Path,
                        default=Path(__file__).resolve().parents[1] / "consolation/controller_assets/glyphs")
    parser.add_argument("--xbox-menu-assets", type=Path,
                        default=Path(__file__).resolve().parents[1] / "consolation/controller_assets/xbox")
    parser.add_argument("--ps3-menu-assets", type=Path,
                        default=Path(__file__).resolve().parents[1] / "consolation/controller_assets/ps3")
    args = parser.parse_args()
    build(args.source, args.images, args.output, args.controller_images, args.xbox_menu_assets, args.ps3_menu_assets)
