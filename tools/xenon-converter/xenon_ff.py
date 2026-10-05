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
import math
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


def _pc_clip_block2_cursor_bias(clip_name):
    """Return runtime-measured corrections for the incomplete block model."""
    bias = PC_CLIP_BLOCK2_CURSOR_BIAS
    # Stock PC Barge node 692 points to plane 160 exactly. The first converted
    # Xenon Barge run resolved every collision-plane reference four bytes past
    # its 20-byte record, proving that this archive's unmodeled block cursor is
    # four bytes lower than the Canals-derived baseline.
    if clip_name.replace("\\", "/").endswith("/mp_barge.d3dbsp"):
        bias -= 4
    return bias

# Xbox 360 GPU texture format IDs used by QoS. The tiling equations below are
# adapted from michaeloliverx/codxe's xenos_texture implementation.
XENOS_TEXTURE_FORMATS = {
    # Xenia TextureFormat::k_8_8_8_8 (ID 6). After tiled-address and GPU-endian
    # conversion, the bytes can be written directly with standard ARGB masks.
    0x06: (1, 1, 4, "ARGB8"),
    0x12: (4, 4, 8, "DXT1"),
    0x13: (4, 4, 16, "DXT2_3"),
    0x14: (4, 4, 16, "DXT4_5"),
    0x31: (4, 4, 16, "DXN"),
}
PC_TEXTURE_FOURCC = {
    "DXT1": b"DXT1",
    "DXT2_3": b"DXT3",
    "DXT4_5": b"DXT5",
    "DXN": b"DXT5",
}
PC_EXTERNAL_IMAGES = frozenset({",$identitynormalmap"})
SND_DRIVER_REVERB_COUNT = 26
SND_DRIVER_REVERB_SIZE = 52
PC_COMMON_TECHSETS = (
    ",wc_l_sm_b0c0n0s0p0",
    "wc_l_sm_b0c0",
    "wc_l_sm_b0c0n0p0",
    "wc_l_sm_b0c0n0s0p0",
    "wc_l_sm_b0c0p0",
    "wc_l_sm_b0c0s0",
    "wc_l_sm_b0c0s0p0",
)


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


def select_pc_techset(source_name, candidates=PC_COMMON_TECHSETS):
    """Choose a loaded PC world technique with the nearest channel signature."""
    if source_name in candidates:
        return source_name
    if not source_name.startswith(("wc_l_sm_", ",wc_l_sm_")):
        return None

    def signature(name):
        body = name.lstrip(",")
        base_match = re.search(r"(?:^|_)([brt]\d+c\d+)", body)
        features = frozenset(re.findall(r"[dnsp]\d+", body))
        return (name.startswith(","),
                base_match.group(1)[0] if base_match else None,
                features)

    source_comma, source_base, source_features = signature(source_name)
    scored = []
    for candidate in candidates:
        # Matching texture channels does not establish vertex compatibility.
        # PC 103819D0 rejects tree/model declarations for world vertex type 3.
        if not re.match(r"^,?wc_l_sm_[brt]\d+c\d+", candidate):
            continue
        candidate_comma, candidate_base, candidate_features = signature(candidate)
        if candidate_base is None:
            continue
        missing_features = source_features - candidate_features
        extra_features = candidate_features - source_features
        score = (8 * (candidate_comma != source_comma)
                 + 6 * (candidate_base != source_base)
                 + 3 * len(missing_features) + len(extra_features))
        scored.append((score, len(candidate_features), candidate))
    return min(scored)[2] if scored else None


def extract_pc_secondary_layer_vertices(vertices, layers, first_vertex,
                                        layer_offset, local_indices, stride):
    """Clone referenced PC vertices and promote a verified Xenon layer's UVs.

    Paired QoS Barge triangles establish float2 UVs at offset zero in 8-byte
    sign records and 12-byte swl_n1 decal records. The caller must establish
    the layout and blend contract; this does not enable layer splitting.
    """
    if stride not in (8, 12):
        raise FormatError("unverified secondary layer stride")
    if len(vertices) % 44 or first_vertex < 0 or layer_offset < 0:
        raise FormatError("invalid secondary layer vertex range")
    remap = {}
    output = bytearray()
    indices = []
    for local_index in local_indices:
        if local_index < 0:
            raise FormatError("negative secondary layer index")
        if local_index not in remap:
            start = (first_vertex + local_index) * 44
            uv_start = layer_offset + local_index * stride
            if start + 44 > len(vertices) or uv_start + stride > len(layers):
                raise FormatError("secondary layer index outside captured data")
            vertex = bytearray(vertices[start:start + 44])
            vertex[20:28] = _little_endian_words(layers[uv_start:uv_start + 8])
            remap[local_index] = len(output) // 44
            output.extend(vertex)
        indices.append(remap[local_index])
    return bytes(output), indices


def select_pc_material_techset(material_value, candidates=PC_COMMON_TECHSETS):
    """Select a verified PC techset and explain any provisional substitution."""
    source_name = material_value.get("techset_name", "")
    selected = select_pc_techset(source_name, candidates)
    if selected is not None:
        return selected, ("exact_techset" if selected == source_name
                          else "channel_signature")

    semantics = set()
    for image_value in material_value.get("textures", []):
        definition = image_value.get("definition")
        if isinstance(definition, str):
            raw = bytes.fromhex(definition)
            if len(raw) == 12:
                semantics.add(raw[7])

    if 5 in semantics and 8 in semantics:
        fallback = "wc_l_sm_b0c0n0s0p0"
    elif 5 in semantics:
        fallback = "wc_l_sm_b0c0n0p0"
    elif 8 in semantics:
        fallback = "wc_l_sm_b0c0s0p0"
    else:
        fallback = "wc_l_sm_b0c0"
    if fallback not in candidates:
        fallback = next((name for name in candidates
                         if re.match(r"^wc_l_sm_[brt]\d+c\d+", name)), None)
    layered = re.search(r"(?:^|_)b[1-9][0-9]*c[1-9][0-9]*", source_name)
    return fallback, ("layered_material_collapse" if layered
                      else "texture_semantic_fallback")


def _decode_bc4_block(block):
    if len(block) != 8:
        raise FormatError("BC4 block must be eight bytes")
    endpoint0, endpoint1 = block[0], block[1]
    if endpoint0 > endpoint1:
        palette = [endpoint0, endpoint1]
        palette.extend(((7 - index) * endpoint0 + index * endpoint1) // 7
                       for index in range(1, 7))
    else:
        palette = [endpoint0, endpoint1]
        palette.extend(((5 - index) * endpoint0 + index * endpoint1) // 5
                       for index in range(1, 5))
        palette.extend((0, 255))
    indices = int.from_bytes(block[2:8], "little")
    return [palette[(indices >> (pixel * 3)) & 7] for pixel in range(16)]


def _encode_bc4_values(values):
    high, low = max(values), min(values)
    palette = [high, low]
    if high > low:
        palette.extend(((7 - i) * high + i * low) // 7 for i in range(1, 7))
    else:
        palette.extend(((5 - i) * high + i * low) // 5 for i in range(1, 5))
        palette.extend((0, 255))
    indices = sum(min(range(8), key=lambda i: abs(palette[i] - value))
                  << (pixel * 3) for pixel, value in enumerate(values))
    return bytes((high, low)) + indices.to_bytes(6, "little")


def transcode_dxn_to_dxt5(data, pc_normal_slopes=False):
    """Map BC5/DXN normals to DXT5nm (X in alpha, Y in green)."""
    if len(data) % 16:
        raise FormatError("DXN data is not block-aligned")
    result = bytearray()
    for offset in range(0, len(data), 16):
        x_block = data[offset:offset + 8]
        green_values = _decode_bc4_block(data[offset + 8:offset + 16])
        if pc_normal_slopes:
            # Controlled probe: QoS PC lm_/lp_ SM2 shaders decode alpha/green
            # as slopes using 4.08*x-2.08 and (126/31)*y-(64/31).
            # The source BC5 unit-normal interpretation still needs runtime validation.
            x_values = _decode_bc4_block(x_block)
            alpha_values, encoded_green = [], []
            for x, y in zip(x_values, green_values):
                nx, ny = x * 2 / 255 - 1, y * 2 / 255 - 1
                nz = math.sqrt(max(1 / (255 * 255), 1 - nx * nx - ny * ny))
                alpha_values.append(max(0, min(255, round(
                    (nx / nz + 2.08) * 255 / 4.08))))
                encoded_green.append(max(0, min(255, round(
                    (ny / nz + 64 / 31) * 255 / (126 / 31)))))
            x_block = _encode_bc4_values(alpha_values)
            green_values = encoded_green
        green0 = max(green_values) * 63 // 255
        green1 = min(green_values) * 63 // 255
        if green0 == green1:
            if green0 < 63:
                green0 += 1
            else:
                green1 -= 1
        palette = (green0, green1, (2 * green0 + green1) // 3,
                   (green0 + 2 * green1) // 3)
        color_indices = 0
        for pixel, value in enumerate(green_values):
            quantized = value * 63 // 255
            index = min(range(4), key=lambda item: abs(palette[item] - quantized))
            color_indices |= index << (pixel * 2)
        result.extend(x_block)
        result.extend(struct.pack("<HHI", green0 << 5, green1 << 5,
                                  color_indices))
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


def _coarsen_compressed_level(width, height, gpu_format, linear):
    """Approximate one missing mip by selecting source blocks at half size."""
    block_width, block_height, block_size, _ = XENOS_TEXTURE_FORMATS[gpu_format]
    source_width = max(1, _divide_round_up(width, block_width))
    source_height = max(1, _divide_round_up(height, block_height))
    next_width = max(1, _divide_round_up(max(1, width // 2), block_width))
    next_height = max(1, _divide_round_up(max(1, height // 2), block_height))
    if len(linear) != source_width * source_height * block_size:
        raise FormatError("invalid decoded Xenos mip size")
    result = bytearray()
    for y in range(next_height):
        for x in range(next_width):
            source_x = min(source_width - 1, x * 2)
            source_y = min(source_height - 1, y * 2)
            start = (source_y * source_width + source_x) * block_size
            result.extend(linear[start:start + block_size])
    return bytes(result)


def _xenos_packed_mip_offset(width, height, gpu_format, level):
    # Adapted from Xenia texture_util::GetPackedMipOffset (BSD-3-Clause).
    # https://github.com/xenia-project/xenia/blob/master/src/xenia/gpu/texture_util.cc
    width_log = (width - 1).bit_length()
    height_log = (height - 1).bit_length()
    first = max(0, min(width_log, height_log) - 4)
    relative = level - first
    if relative < 0:
        raise FormatError("Xenos mip is outside the packed tail")
    if relative < 3:
        x, y = ((0, 16 >> relative) if width_log > height_log
                else (16 >> relative, 0))
    else:
        offset = (1 << (max(width_log, height_log) - first)) >> (relative - 2)
        x, y = ((offset, 0) if width_log > height_log else (0, offset))
    block_width, block_height, _, _ = XENOS_TEXTURE_FORMATS[gpu_format]
    return x // block_width, y // block_height


def _untile_xenos_packed_level(width, height, format_word, tile, level):
    gpu_format = format_word & 0x3F
    first = max(0, min((width - 1).bit_length(),
                       (height - 1).bit_length()) - 4)
    layout = _xenos_texture_layout(max(1, width >> first),
                                   max(1, height >> first), gpu_format)
    stored_width, block_size = layout[2:4]
    output = _xenos_texture_layout(max(1, width >> level),
                                   max(1, height >> level), gpu_format)
    offset_x, offset_y = _xenos_packed_mip_offset(width, height, gpu_format, level)
    native = apply_xenos_gpu_endian(tile, (format_word >> 6) & 3)
    log2_bytes = _xenos_log2_bytes_per_block(block_size)
    result = bytearray()
    for y in range(output[1]):
        row = _xenos_tiled_row_offset(y + offset_y, stored_width, log2_bytes)
        for x in range(output[0]):
            address = _xenos_tiled_column_offset(
                x + offset_x, y + offset_y, log2_bytes, row)
            if address + block_size > len(native):
                raise FormatError("Xenos packed mip exceeds its tile allocation")
            result.extend(native[address:address + block_size])
    return bytes(result)


def _image_has_packed_mips(image_value):
    words = image_value.get("texture_resource_words", ())
    if len(words) != 13 or image_value.get("depth", 1) != 1:
        return False
    # QoS serializes the six GPU fetch words at resource +28 in GPU byte order.
    fetch = [int.from_bytes(word.to_bytes(4, "big"), "little")
             for word in words[7:13]]
    if ((fetch[5] >> 9) & 3) != 1:
        return False
    if ((fetch[2] & 0x1FFF) + 1 != image_value["width"]
            or ((fetch[2] >> 13) & 0x1FFF) + 1 != image_value["height"]
            or (fetch[1] & 0xFF) != (int(
                image_value["load_definition"]["format"], 16) & 0xFF)):
        raise FormatError("Xenos texture fetch descriptor disagrees with image metadata")
    # Base-packed tiny textures need separate base/mip-address handling.
    return bool(fetch[5] & 0x800) and min(image_value["width"],
                                       image_value["height"]) > 16


def untile_xenos_cubemap_base(width, height, format_word, tiled, levels):
    """Preserve six independently tiled faces of a single-level QoS cubemap."""
    if levels != 1 or width != height:
        raise FormatError("unverified Xenon cubemap mip layout")
    face_size = _xenos_texture_layout(width, height, format_word & 0x3F)[6]
    if len(tiled) != face_size * 6:
        raise FormatError("Xenon cubemap does not contain six complete faces")
    return b"".join(untile_xenos_texture(
        width, height, format_word, tiled[offset:offset + face_size])
        for offset in range(0, len(tiled), face_size))


def untile_xenos_texture_levels(width, height, format_word, tiled, levels,
                                packed_mips=False):
    """Convert the independently tiled portion of a Xenon mip chain.

    Small Xenos mips share a packed mip tail. The QoS PC loader nevertheless
    expects the full linear chain size. Preserve independently tiled levels
    exactly and decode a verified packed tail from its shared tile. Without
    packing metadata, approximate unavailable levels from the preceding level.
    """
    if levels < 1:
        raise FormatError("Xenos texture has no mip levels")
    gpu_format = format_word & 0x3F
    result = bytearray()
    cursor = 0
    decoded_levels = 0
    previous_level = None
    packed_tail = False
    packed_first = (max(0, min((width - 1).bit_length(),
                               (height - 1).bit_length()) - 4)
                    if packed_mips else levels)
    tail_tile = None
    for level in range(levels):
        level_width = max(1, width >> level)
        level_height = max(1, height >> level)
        layout = _xenos_texture_layout(level_width, level_height, gpu_format)
        tiled_size = layout[6]
        if level >= packed_first:
            if tail_tile is None:
                if cursor + tiled_size > len(tiled):
                    raise FormatError("missing Xenos packed mip tile")
                tail_tile = tiled[cursor:cursor + tiled_size]
            current_level = _untile_xenos_packed_level(
                width, height, format_word, tail_tile, level)
            decoded_levels += 1
        elif not packed_tail and cursor + tiled_size <= len(tiled):
            current_level = untile_xenos_texture(
                level_width, level_height, format_word,
                tiled[cursor:cursor + tiled_size])
            cursor += tiled_size
            decoded_levels += 1
        else:
            packed_tail = True
            if previous_level is None:
                raise FormatError("missing Xenos base texture level")
            current_level = _coarsen_compressed_level(
                max(1, width >> (level - 1)),
                max(1, height >> (level - 1)), gpu_format, previous_level)
        result.extend(current_level)
        previous_level = current_level
    return bytes(result), decoded_levels


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
                levels = max(1, load[0])
                is_cube = bool(load[1] & 4)
                if is_cube:
                    linear = untile_xenos_cubemap_base(
                        result["width"], result["height"], format_word, pixels, levels)
                    decoded_levels = 1
                    result["pc_map_type"] = 5
                    result["pc_load_flags"] = (1, 6)
                else:
                    linear, decoded_levels = untile_xenos_texture_levels(
                        result["width"], result["height"], format_word,
                        pixels, levels, _image_has_packed_mips(result))
                base_size = _xenos_texture_layout(
                    result["width"], result["height"],
                    format_word & 0x3F)[5] * (6 if is_cube else 1)
                result["pc_base_level"] = {
                    "format": XENOS_TEXTURE_FORMATS[format_word & 0x3F][3],
                    "bytes": base_size,
                    "sha256": hashlib.sha256(linear[:base_size]).hexdigest(),
                    "data": linear[:base_size].hex(),
                }
                result["pc_mip_chain"] = {
                    "format": XENOS_TEXTURE_FORMATS[format_word & 0x3F][3],
                    "levels": levels, "decoded_levels": decoded_levels,
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


def snd_driver_globals(reader, pointer, capture=False):
    """Read the fixed QoS Xenon sound-driver globals asset.

    The root and XaReverbSettings declarations are adapted from the supplied
    QOSXenon_Assets.h. The 26-record count and nested order are verified against
    code_post_gfx_mp.ff and the QoS Wii SndDriverGlobals naming.
    """
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    start = reader.pos
    header = reader.take(8)
    settings_pointer = u32(header)
    if settings_pointer in (INLINE, INSERT):
        settings = reader.take(
            SND_DRIVER_REVERB_COUNT * SND_DRIVER_REVERB_SIZE)
    elif settings_pointer:
        settings = b""
    else:
        settings = b""
    result = {
        "name": reader.string(u32(header, 4)),
        "offset": hex(start),
        "reverb_settings_count": (
            SND_DRIVER_REVERB_COUNT if settings else 0),
        "reverb_settings_bytes": len(settings),
        "reverb_settings_sha256": hashlib.sha256(settings).hexdigest(),
    }
    if capture:
        result["header"] = header.hex()
        result["reverb_settings"] = settings.hex()
    return result


def string_table(reader, pointer, capture=False):
    """Read the QoS Xenon StringTable root and its row-major string cells."""
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    start = reader.pos
    header = reader.take(16)
    column_count = u32(header, 4)
    row_count = u32(header, 8)
    cell_count = column_count * row_count
    if column_count > 4096 or row_count > 1_000_000 or cell_count > 1_000_000:
        raise FormatError("invalid string-table dimensions")
    name = reader.string(u32(header))
    values_pointer = u32(header, 12)
    value_pointers = (reader.take(cell_count * 4)
                      if values_pointer in (INLINE, INSERT) else b"")
    values = []
    for offset in range(0, len(value_pointers), 4):
        value_pointer = u32(value_pointers, offset)
        if value_pointer in (INLINE, INSERT):
            values.append(reader.string(value_pointer))
        elif value_pointer:
            values.append({"reference": hex(value_pointer)})
        else:
            values.append(None)
    result = {
        "name": name,
        "offset": hex(start),
        "column_count": column_count,
        "row_count": row_count,
        "cell_count": cell_count,
        "values": values,
    }
    if capture:
        result["header"] = header.hex()
        result["value_pointers"] = value_pointers.hex()
    return result


def xanim(reader, pointer, capture=False):
    """Read a QoS Xenon XAnimParts asset without interpreting animation data.

    Field offsets follow the verified 92-byte Xenon layout. Nested load order
    and structure names follow QoS Wii Load_XAnimParts (0x8CF1C); the two
    platforms use different root layouts, so only the nested behavior is
    transferred.
    """
    if pointer not in (INLINE, INSERT):
        return {"reference": hex(pointer)}
    start = reader.pos
    header = reader.take(92)
    counts = {
        "data_byte": u16(header, 8),
        "data_short": u16(header, 10),
        "data_int": u16(header, 12),
        "random_data_byte": u16(header, 14),
        "random_data_int": u16(header, 16),
        "random_data_short": u32(header, 36),
        "indices": u32(header, 40),
    }
    numframes = u16(header, 18)
    bone_counts = list(header[22:32])
    notify_count = header[32]
    result = {
        "name": reader.string(u32(header)),
        "unknown_string": reader.string(u32(header, 4)),
        "offset": hex(start),
        "numframes": numframes,
        "loop": bool(header[20]),
        "delta": bool(header[21]),
        "bone_counts": bone_counts,
        "notify_count": notify_count,
        "asset_type": header[33],
        "is_default": bool(header[34]),
        "counts": counts,
    }

    names = reader.take(bone_counts[9] * 2) if u32(header, 52) else b""
    notifies = reader.take(notify_count * 8) if u32(header, 84) else b""

    delta_summary = None
    if u32(header, 88):
        delta = reader.take(8)
        delta_summary = {"translation": None, "quaternion": None}
        if u32(delta):
            trans = reader.take(4)
            size = u16(trans)
            small = bool(trans[2])
            if size:
                frames = reader.take(28)
                index_width = 2 if numframes >= 256 else 1
                reader.take((size + 1) * index_width)
                if u32(frames, 24):
                    reader.take((size + 1) * (3 if small else 6))
            else:
                reader.take(12)
            delta_summary["translation"] = {"size": size, "small": small}
        if u32(delta, 4):
            quat = reader.take(4)
            size = u16(quat)
            if size:
                frames = reader.take(4)
                index_width = 2 if numframes >= 256 else 1
                reader.take((size + 1) * index_width)
                if u32(frames):
                    reader.take((size + 1) * 4)
            else:
                reader.take(4)
            delta_summary["quaternion"] = {"size": size}

    arrays = {}
    for key, pointer_offset, element_size in (
            ("data_byte", 56, 1), ("data_short", 60, 2),
            ("data_int", 64, 4), ("random_data_short", 68, 2),
            ("random_data_byte", 72, 1), ("random_data_int", 76, 4)):
        raw = reader.take(counts[key] * element_size) if u32(header, pointer_offset) else b""
        arrays[key] = {
            "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()
        }
        if capture and key == "data_byte":
            arrays[key]["data"] = raw.hex()

    index_width = 2 if numframes >= 256 else 1
    indices = reader.take(counts["indices"] * index_width) if u32(header, 80) else b""
    result.update({
        "names_bytes": len(names),
        "notify_bytes": len(notifies),
        "delta_part": delta_summary,
        "arrays": arrays,
        "indices_bytes": len(indices),
    })
    if capture:
        result["header"] = header.hex()
    return result


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
        texture_stream_offset = reader.pos
        textures = reader.take(header[60] * 12)
        for offset in range(0, len(textures), 12):
            if textures[offset + 7] == 11:
                raise FormatError("water texture requires a separate decoder")
            image_value = image(reader, u32(textures, offset + 8), capture)
            if capture:
                image_value["definition"] = textures[offset:offset + 12].hex()
                image_value["definition_offset"] = hex(
                    texture_stream_offset + offset)
                image_value["image_pointer_offset"] = hex(
                    texture_stream_offset + offset + 8)
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


def _pack_pc_unit_vec(components):
    """Pack a decoded unit vector into QoS PC's biased byte representation."""
    if len(components) != 3:
        raise FormatError("PC unit vector requires three components")
    packed = [max(0, min(255, int(value * 127.0 + 127.5)))
              for value in components]
    return bytes((*packed, 0x3F))


def convert_world_vertices(records):
    """Convert 44-byte Xenon world vertices to the paired QoS PC layout."""
    if len(records) % 44:
        raise FormatError("invalid Xbox world-vertex array")

    converted = bytearray(len(records))
    for offset in range(0, len(records), 44):
        source = records[offset:offset + 44]
        converted[offset:offset + 36] = _little_endian_words(
            source, 0, 36)[:36]
        converted[offset + 36:offset + 40] = _pack_pc_unit_vec(
            _unpack_xenon_unit_vec(u32(source, 36)))
        converted[offset + 40:offset + 44] = _pack_pc_unit_vec(
            _unpack_xenon_unit_vec(u32(source, 40)))
    return bytes(converted)


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


def _pc_surface_remap(count):
    """Build the PC renderer's uint16 DPVS surface-index table."""
    if count < 0 or count > 0x10000:
        raise FormatError(f"invalid PC surface remap count {count}")
    return struct.pack(f"<{count}H", *range(count)) if count else b""


def _pc_surface_remap_from_xenon(source, count, surface_count):
    """Preserve the Xenon DPVS surface order used by AABB leaf ranges."""
    if not source:
        return _pc_surface_remap(count)
    if len(source) != count * 2:
        raise FormatError(
            "Xbox GfxWorld surface-remap size does not match DPVS count")
    indices = struct.unpack(f">{count}H", source)
    if any(index >= surface_count for index in indices):
        raise FormatError("Xbox GfxWorld surface remap exceeds surface array")
    return struct.pack(f"<{count}H", *indices)


def _pc_bounds_from_xenon(raw, offset=0, context="bounds"):
    """Convert Xenon mins/maxs to the PC Bounds midpoint/half-size form."""
    mins = struct.unpack_from(">3f", raw, offset)
    maxs = struct.unpack_from(">3f", raw, offset + 12)
    values = mins + maxs
    if not all(math.isfinite(value) for value in values):
        raise FormatError(f"non-finite Xenon {context}: mins={mins}, maxs={maxs}")
    float_max = struct.unpack(">f", b"\x7f\x7f\xff\xff")[0]
    if (all(value == float_max for value in mins)
            and all(value == -float_max for value in maxs)):
        # Empty Xenon AABB trees use the canonical accumulated-bounds
        # sentinel. PC midpoint/half-size bounds cannot represent it, and an
        # empty tree has no geometry to enclose, so emit a zero-volume bound.
        return bytes(24)
    if any(minimum > maximum for minimum, maximum in zip(mins, maxs)):
        raise FormatError(f"inverted Xenon {context}: mins={mins}, maxs={maxs}")
    midpoint = tuple((minimum + maximum) * 0.5
                     for minimum, maximum in zip(mins, maxs))
    half_size = tuple((maximum - minimum) * 0.5
                      for minimum, maximum in zip(mins, maxs))
    return struct.pack("<6f", *(midpoint + half_size))


def _pc_dpvs_worlds(raw):
    if len(raw) % 60:
        raise FormatError("invalid captured Xbox DPVS world array")
    converted = _little_endian_words(raw)
    for index, offset in enumerate(range(0, len(raw), 60)):
        converted[offset + 24:offset + 48] = _pc_bounds_from_xenon(
            raw, offset + 24, f"DPVS world {index}")
    return converted


def _convert_material_constants(raw):
    """Convert Xenon MaterialConstantDef records without swapping their names."""
    if len(raw) % 32:
        raise FormatError("invalid material constant array")

    converted = bytearray(len(raw))
    for offset in range(0, len(raw), 32):
        struct.pack_into("<I", converted, offset, u32(raw, offset))
        converted[offset + 4:offset + 16] = raw[offset + 4:offset + 16]
        struct.pack_into(
            "<4I", converted, offset + 16,
            *(u32(raw, offset + 16 + component * 4)
              for component in range(4)))
    return bytes(converted)


def _convert_material_state_slots(raw, state_mapping=None, world_layout=False):
    """Expand Xenon's 36 technique state slots to the 43-slot PC layout."""
    if len(raw) != 36:
        raise FormatError("invalid Xenon material state-slot table")
    state_mapping = state_mapping or {}
    remapped = bytes(
        state_mapping.get(value, value) if value != 0xFF else value
        for value in raw)
    active_state = next(
        (value for value in remapped[3:14] if value != 0xFF), 0xFF)
    state_count = len(state_mapping)

    # Paired stock-PC and Xenon mp_barge materials establish the complete
    # platform technique-position mapping. PC adds seven positions and repeats
    # the main lit state for its additional depth/shadow techniques. The first
    # four positions address the first four PC state records when available;
    # two-state emissive materials only populate the first position.
    leading_states = (bytes(range(4)) if state_count >= 4
                      else bytes((0xFF,) * 4) if world_layout
                      else bytes((0, 0xFF, 0xFF, 0xFF)))
    expanded_middle = (bytes((active_state,)) * 7
                       + bytes((0xFF,)) * 7 if world_layout and state_count >= 4
                       else bytes((active_state,)) * 14)
    return b"".join((
        leading_states,
        bytes((active_state, 0xFF, 0xFF)),
        expanded_middle,
        remapped[14:26],
        bytes((active_state,)) * 7,
        remapped[33:36],
    ))


def _convert_material_state_bits(raw):
    """Convert and expand platform-specific material render-state records.

    The record ordering and PC-only state are verified against paired Xenon/PC
    mp_barge materials. Two-state materials are already platform-common.
    Unknown record counts remain byte-order converted without speculative
    insertion; callers can report those materials for later comparison.
    """
    if len(raw) % 8:
        raise FormatError("invalid material state-bit array")
    records = [bytes(_little_endian_words(raw, offset, offset + 8)[offset:offset + 8])
               for offset in range(0, len(raw), 8)]
    if len(records) == 6:
        pc_only = struct.pack("<2I", 0x18124812,
                              struct.unpack_from("<I", records[2], 4)[0])
        converted = [records[index] for index in (0, 2, 1)]
        converted.append(pc_only)
        converted.extend(records[3:])
        return b"".join(converted), {0: 0, 1: 2, 2: 1, 3: 4, 4: 5, 5: 6}, True
    if len(records) == 5:
        pc_only = struct.pack("<2I", 0x18124812,
                              struct.unpack_from("<I", records[2], 4)[0])
        converted = [records[0], pc_only, *records[1:]]
        return b"".join(converted), {0: 0, 1: 2, 2: 3, 3: 4, 4: 5}, True
    mapping = {index: index for index in range(len(records))}
    return b"".join(records), mapping, len(records) in (0, 2)


def requires_pc_state_template(techset_name, source_state_count):
    alpha_test = re.search(r"(?:^|_)t[0-9]", techset_name.lstrip(",")) is not None
    return (source_state_count in (3, 4)
            or (alpha_test and source_state_count in (5, 6)))


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
    plane_indices = b""
    if u32(header, 24):
        plane_indices = reader.take(
            2 * (u16(header, last_offset) - u16(header, first_offset) + 1))
    cell_bits = b""
    if u32(header, 32):
        cell_bits = reader.take(u32(header, 28))
    scene_ent_cell_bits = b""
    if u32(header, 40):
        scene_ent_cell_bits = reader.take(u32(header, 36) * 4)
    brush_models = b""
    if u32(header, 48):
        brush_models = reader.take(u32(header, 44) * 168)
    return {
        "raw": header.hex(),
        "plane_indices": plane_indices.hex(),
        "cell_bits": cell_bits.hex(),
        "scene_ent_cell_bits": scene_ent_cell_bits.hex(),
        "brush_models": brush_models.hex(),
    }


def resolve_gfx_surface_materials(surfaces, materials):
    """Resolve Xenon packed material pointers through earlier surface slots.

    QoS serializes a repeated GfxSurface material as a block-2 pointer to the
    first surface's 32-bit material field, not as another Material payload.
    Derive the surface-array runtime base from all backward references and
    require one unique base before replacing any reference.
    """
    if len(surfaces) != len(materials) * 72:
        raise FormatError("GfxSurface material list does not match its array")
    references = [
        (index, _packed_block2_offset(int(value["reference"], 16)))
        for index, value in enumerate(materials) if "reference" in value
    ]
    if not references:
        return materials, None

    first_index, first_offset = references[0]
    inline_targets = [index for index in range(first_index)
                      if "reference" not in materials[index]]
    candidates = {
        first_offset - (target * 72 + 40) for target in inline_targets
        if first_offset >= target * 72 + 40
    }
    valid = []
    for base in candidates:
        targets = []
        for source, offset in references:
            delta = offset - base - 40
            if delta < 0 or delta % 72:
                break
            target = delta // 72
            if target >= source or target >= len(materials):
                break
            if "reference" in materials[target]:
                break
            targets.append(target)
        else:
            valid.append((base, targets))
    if len(valid) != 1:
        raise FormatError(
            f"could not uniquely resolve GfxSurface material slots: "
            f"{[hex(base) for base, _ in valid]}")

    base, targets = valid[0]
    result = list(materials)
    for (source, _), target in zip(references, targets):
        result[source] = materials[target]
    return result, base


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
    reflection_probes = []
    if u32(header, 368):
        records = reader.take(u32(header, 364) * 16)
        for offset in range(0, len(records), 16):
            record = records[offset:offset + 16]
            image_value = None
            if u32(record, 12) in (INLINE, INSERT):
                image_value = image(reader, u32(record, 12), capture)
            elif u32(record, 12):
                image_value = {"reference": hex(u32(record, 12))}
            if capture:
                reflection_probes.append({"raw": record.hex(),
                                          "image": image_value})
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
    lightmaps = []
    if u32(header, 404):
        records = reader.take(u32(header, 400) * 8)
        for offset in range(0, len(records), 8):
            images = []
            for pointer_offset in (0, 4):
                image_pointer = u32(records, offset + pointer_offset)
                if image_pointer in (INLINE, INSERT):
                    image_value = image(reader, image_pointer, capture)
                elif image_pointer:
                    image_value = {"reference": hex(image_pointer)}
                else:
                    image_value = None
                images.append(image_value)
            if capture:
                lightmaps.append({"raw": records[offset:offset + 8].hex(),
                                  "images": images})

    dpvs_planes = _gfx_dpvs_planes(reader, header[408:460])
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
    surface_remap = (reader.take(static_surface_count * 2)
                     if u32(header, 796) else b"")
    if u32(header, 812):
        records = reader.take(u32(header, 352) * 12)
        for offset in range(0, len(records), 12):
            if u32(records, offset + 4):
                reader.take(u16(records, offset) * 2)
            if u32(records, offset + 8):
                reader.take(u16(records, offset + 2) * 2)
    runtime_records = b""
    if u32(header, 820):
        runtime_records = reader.take(u32(header, 816) * 24)
    if u32(header, 824) in (INLINE, INSERT):
        material(reader, capture)

    material_slot_base = None
    if capture and surface_materials:
        surface_materials, material_slot_base = resolve_gfx_surface_materials(
            surfaces, surface_materials)

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
            "surface_material_slot_base": (
                hex(material_slot_base) if material_slot_base is not None else None),
            "brush_models": dpvs_planes["brush_models"],
            "dpvs_worlds": draw_surfaces.hex(),
            "static_model_draws": static_model_draws.hex(),
            "static_model_insts": static_model_insts.hex(),
            "dpvs_planes": dpvs_planes,
            "cells": parsed_cells,
            "reflection_probes": reflection_probes,
            "lightmaps": lightmaps,
            "sky_start_surfs": sky_start_surfs.hex(),
            "surface_draw_ranges": [u32(header, offset)
                                    for offset in (88, 92, 96, 100)],
            "surface_remap": surface_remap.hex(),
            "vertices": vertices.hex(),
            "vertex_layers": vertex_layers.hex(),
            # Xbox +0x294..+0x338 is the platform counterpart of PC
            # +0x230..+0x2D4. It contains renderer allocation counts and
            # stream markers that must survive conversion.
            "runtime_tail": header[660:828].hex(),
            "runtime_records": runtime_records.hex(),
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


def _renderable_rigid_surface(surface):
    source = bytes.fromhex(surface["header"])
    vertices, triangles = u16(source, 2), u16(source, 4)
    if not vertices or not triangles:
        return False
    if any(struct.unpack_from(">h", source, 12 + slot * 2)[0]
           for slot in range(4)):
        return False
    primary = surface.get("pc_vertices")
    indices = surface.get("indices")
    rigid = surface.get("rigid_vertices")
    if primary is None or indices is None or rigid is None:
        return False
    if (len(bytes.fromhex(primary)) != vertices * 40
            or len(bytes.fromhex(indices)) != triangles * 6
            or len(bytes.fromhex(rigid)) != u32(source, 136) * 8):
        raise FormatError("rigid XSurface stream sizes disagree with its counts")
    secondary = surface.get("pc_secondary_vertices")
    if secondary is not None and len(bytes.fromhex(secondary)) != vertices * 16:
        raise FormatError("rigid XSurface secondary stream size mismatch")
    if any(index >= vertices for (index,) in struct.iter_unpack(
            ">H", bytes.fromhex(indices))):
        raise FormatError("rigid XSurface triangle index exceeds its vertices")
    return True


def write_pc_xmodel(payload, model, material_pointers=None):
    if material_pointers is not None and len(material_pointers) != len(model["materials"]):
        raise FormatError("model material pointer count mismatch")
    payload.extend(convert_xmodel_header(model))
    payload.extend(model["name"].encode() + b"\0")
    payload.extend(_little_endian_u16_array(_captured_bytes(model["bone_names"])))
    payload.extend(_captured_bytes(model["parent_list"]))
    payload.extend(_little_endian_u16_array(_captured_bytes(model["quaternions"])))
    payload.extend(_little_endian_words(_captured_bytes(model["translations"])))
    payload.extend(_captured_bytes(model["part_classification"]))
    payload.extend(_little_endian_words(_captured_bytes(model["base_matrices"])))

    # PC 103D36D0 loads all surface roots before their nested GPU streams.
    # Restore complete rigid surfaces only; shared/skinned streams stay reduced.
    for surface in model["surfaces"]:
        payload.extend(convert_xsurface_header(
            surface, include_geometry=_renderable_rigid_surface(surface)))
    for surface in model["surfaces"]:
        if _renderable_rigid_surface(surface):
            write_pc_xsurface_nested(payload, surface)

    for index, _ in enumerate(model["materials"]):
        payload.extend(struct.pack("<I", material_pointers[index]
                                   if material_pointers is not None else INLINE))
    if material_pointers is None:
        for _ in model["materials"]:
            payload.extend(_pc_external_material())
    payload.extend(_little_endian_words(_captured_bytes(model["bone_info"])))


def _complete_pc_material_textures(material_value, images_by_name):
    """Keep a technique only when every referenced texture has PC image data."""
    textures = material_value.get("textures", [])
    if not textures or any(image_value.get("name") not in images_by_name
                           for image_value in textures):
        return None
    return textures


def inspect(path, details=False, capture_map=False, allow_partial=False):
    reader, entries, report = read_zone(path)
    if details:
        assets = []
        partial_error = None
        for index, (kind, pointer) in enumerate(entries):
            start = reader.pos
            try:
                if pointer not in (INLINE, INSERT):
                    raise FormatError(
                        f"asset {index} has unsupported pointer {pointer:#x}")
                if kind == 8:
                    asset = techset(reader)
                elif kind == 9:
                    asset = image(reader, pointer, capture_map)
                elif kind == 5:
                    asset = xmodel(reader, capture_map)
                elif kind == 4:
                    asset = xanim(reader, pointer, capture_map)
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
                elif kind == 26:
                    asset = snd_driver_globals(reader, pointer, capture_map)
                elif kind == 34:
                    asset = string_table(reader, pointer, capture_map)
                elif kind == 6:
                    asset = material(reader, capture_map)
                elif kind == 33:
                    header = reader.take(12)
                    asset = {"name": reader.string(u32(header))}
                    payload = (reader.take(u32(header, 4) + 1)
                               if u32(header, 8) else b"")
                    asset["bytes"] = len(payload)
                    asset["sha256"] = hashlib.sha256(payload).hexdigest()
                    if capture_map:
                        asset["data"] = payload.hex()
                else:
                    raise FormatError(
                        f"asset {index}: {ASSET_NAMES[kind]} details unsupported "
                        f"at stream offset 0x{start:x}")
            except FormatError as error:
                if not allow_partial:
                    raise
                partial_error = {
                    "asset_index": index,
                    "asset_type": ASSET_NAMES[kind],
                    "stream_offset": hex(start),
                    "message": str(error),
                }
                break
            assets.append({"type": ASSET_NAMES[kind], "manifest_index": index,
                           "offset": hex(start), **asset})
        for asset in assets if partial_error is None else ():
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
                material_values = asset["geometry"]["surface_materials"] + [
                    value for model_asset in assets if model_asset["type"] == "xmodel"
                    for value in model_asset.get("materials", [])
                    if "header" in value]
                world_material_ids = {id(value) for value in asset["geometry"]["surface_materials"]}
                for material_value in material_values:
                    if "techset" in material_value:
                        material_value["techset_name"] = material_value["techset"].get("name")
                        continue
                    reference = material_value.get("techset_reference")
                    if reference is None:
                        continue
                    pointer = int(reference, 16)
                    encoded = pointer - 1
                    offset = encoded & 0x1FFFFFFF
                    if id(material_value) not in world_material_ids:
                        delta = offset - table_base
                        if (pointer <= 0 or encoded >> 29 != 2 or delta < 0
                                or delta % 8 or delta // 8 >= len(entries)
                                or entries[delta // 8][0] != 8):
                            # Model materials can alias an earlier inline techset;
                            # leave these unresolved rather than inventing a name.
                            continue
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
        if partial_error is not None:
            report["partial_error"] = partial_error
        elif reader.pos != len(reader.data):
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
    # QoS PC DB_LinkXAssetEntry (0x103E0640) treats a leading comma as an
    # external reference and resolves the name without allocating a new asset.
    if not name.startswith(","):
        name = "," + name
    header = bytearray(184)
    struct.pack_into("<I", header, 0, INLINE)
    return bytes(header) + name.encode() + b"\0"


def _pc_material_record_at(payload, name, offset):
    """Validate a PC material whose name starts at a known stream offset."""
    needle = name.encode("latin-1") + b"\0"
    header_offset = offset - 104
    nested = offset + len(needle)
    if header_offset < 0 or payload[offset:nested] != needle:
        return None
    header = payload[header_offset:offset]
    if len(header) != 104 or u32(header) != INLINE:
        return None
    texture_count, constant_count, state_count = header[67:70]
    if texture_count > 32 or constant_count > 64 or state_count > 64:
        return None
    if struct.unpack_from("<I", header, 88)[0] not in (0, INLINE):
        return None

    textures_end = nested + texture_count * 12
    textures = payload[nested:textures_end]
    if len(textures) != texture_count * 12:
        return None
    child_offset = textures_end
    image_names = []
    for texture_offset in range(0, len(textures), 12):
        image_names.append(None)
        image_pointer = struct.unpack_from("<I", textures, texture_offset + 8)[0]
        if image_pointer not in (INLINE, INSERT):
            continue
        image_header = payload[child_offset:child_offset + 36]
        if len(image_header) != 36:
            return None
        child_offset += 36
        if struct.unpack_from("<I", image_header, 32)[0]:
            name_end = payload.find(b"\0", child_offset)
            if name_end < 0:
                return None
            image_names[-1] = payload[child_offset:name_end].decode('latin-1')
            child_offset = name_end + 1
        load_pointer = struct.unpack_from("<I", image_header, 4)[0]
        if load_pointer in (INLINE, INSERT):
            load = payload[child_offset:child_offset + 16]
            if len(load) != 16:
                return None
            child_offset += 16 + struct.unpack_from("<I", load, 12)[0]
    constants_end = child_offset + constant_count * 32
    states_end = constants_end + state_count * 8
    if states_end > len(payload):
        return None
    return {
        "header": header,
        "textures": textures,
        "constants": payload[child_offset:constants_end],
        "state_bits": payload[constants_end:states_end],
        "end_offset": states_end,
        "inline_image_names": image_names,
    }


def pc_material_record(payload, name):
    """Find a native PC material and its immediately serialized child arrays."""
    needle = name.encode("latin-1") + b"\0"
    offset = 0
    while True:
        offset = payload.find(needle, offset)
        if offset < 0:
            return None
        record = _pc_material_record_at(payload, name, offset)
        offset += len(needle)
        if record is not None:
            return record


def read_pc_material_donor_zones(paths, names, techsets, templates):
    """Retain ordered native candidates; never overwrite an earlier donor."""
    if isinstance(paths, (str, Path)):
        paths = [paths]
    candidates = {}
    for path in paths:
        donors = read_pc_material_donors(path, names, techsets, templates)
        for name, record in donors.items():
            record = dict(record, donor_zone=str(path))
            candidates.setdefault(name, []).append(record)
    return candidates


def select_pc_material_donor(textures, candidates):
    source_prefixes = b"".join(
        struct.pack("<I", u32(bytes.fromhex(value["definition"])))
        + bytes.fromhex(value["definition"])[4:8] for value in textures)
    return next((candidate for candidate in candidates
                 if candidate["header"][67] == len(textures)
                 and source_prefixes == b"".join(
                     candidate["textures"][offset:offset + 8]
                     for offset in range(0, len(candidate["textures"]), 12))), None)


def _read_pc_zone_payload(path):
    blob = Path(path).read_bytes()
    if len(blob) < 28 or struct.unpack_from("<I", blob)[0] != 470:
        raise FormatError("material donor is not a PC v470 fastfile")
    declared_size = struct.unpack_from("<I", blob, 4)[0]
    try:
        payload = zlib.decompress(blob[28:])
    except zlib.error as error:
        raise FormatError(f"invalid PC material donor: {error}") from error
    if len(payload) != declared_size:
        raise FormatError("PC material donor payload size does not match its header")
    return payload


def pc_image_record_at(payload, name, name_offset):
    """Validate an inline native PC compressed image without guessing pointers."""
    start = name_offset - 36
    if start < 0 or payload[name_offset:name_offset + len(name) + 1] != name.encode() + b"\0":
        return None
    header = payload[start:name_offset]
    map_type, load_pointer = struct.unpack_from("<2I", header)
    if (map_type not in (3, 5) or load_pointer not in (INLINE, INSERT)
            or struct.unpack_from("<I", header, 32)[0] != INLINE):
        return None
    width, height, depth = struct.unpack_from("<3H", header, 24)
    if not (0 < width <= 4096 and 0 < height <= 4096 and depth == 1):
        return None
    load_start = name_offset + len(name) + 1
    if load_start + 16 > len(payload):
        return None
    levels, flags, load_width, load_height, load_depth, fourcc, size = struct.unpack_from(
        "<2B3H4sI", payload, load_start)
    if ((load_width, load_height, load_depth) != (width, height, depth)
            or fourcc not in (b"DXT1", b"DXT3", b"DXT5") or flags & ~7
            or bool(flags & 4) != (map_type == 5) or levels > 13):
        return None
    if map_type == 5 and width != height:
        return None
    count = levels or (1 if flags & 2 else max(width, height).bit_length())
    block_size = 8 if fourcc == b"DXT1" else 16
    expected = sum(_divide_round_up(max(1, width >> level), 4)
                   * _divide_round_up(max(1, height >> level), 4) * block_size
                   for level in range(count)) * (6 if map_type == 5 else 1)
    end = load_start + 16 + size
    if (size != expected or end > len(payload)
            or struct.unpack_from("<2I", header, 16) != (size, size)):
        return None
    data = payload[load_start + 16:end]
    return {"name": name, "width": width, "height": height, "depth": depth,
            "map_type": map_type, "semantic": header[11], "fourcc": fourcc.decode(),
            "pixel_sha256": hashlib.sha256(data).hexdigest(),
            "serialized": payload[start:end]}


def scan_pc_zone_directory(directory, image_values, material_names, excluded=()):
    """Audit every PC zone, retaining only independently validated candidates."""
    image_candidates, material_candidates, techsets = {}, {}, []
    audit = {"zones": [], "image_matches": [], "image_conflicts": []}
    names = {value["name"] for value in image_values if not value.get("pc_external")}
    pattern = (re.compile(b"(?:" + b"|".join(re.escape(name.encode())
               for name in sorted(names, key=len, reverse=True)) + b")\0") if names else None)
    for path in sorted(Path(directory).glob("*.ff")):
        if path.name in excluded:
            audit["zones"].append({"zone": str(path), "excluded": True})
            continue
        entry = {"zone": str(path), "images": 0, "materials": 0}
        audit["zones"].append(entry)
        try:
            payload = _read_pc_zone_payload(path)
        except (OSError, FormatError) as error:
            entry["error"] = str(error)
            continue
        if pattern:
            for match in pattern.finditer(payload):
                name = match.group()[:-1].decode()
                record = pc_image_record_at(payload, name, match.start())
                if record:
                    record["donor_zone"] = str(path)
                    image_candidates.setdefault(name, []).append(record)
                    entry["images"] += 1
        zone_techsets = []
        try:
            donors = read_pc_material_donors(path, material_names, zone_techsets,
                                            validated_payload=payload)
        except FormatError as error:
            entry["material_error"] = str(error)
            continue
        techsets.extend(zone_techsets)
        entry["materials"] = len(donors)
        for name, record in donors.items():
            material_candidates.setdefault(name, []).append(dict(record, donor_zone=str(path)))
    for image_value in image_values:
        base = image_value.get("pc_base_level", {})
        expected_fourcc = PC_TEXTURE_FOURCC.get(base.get("format"), b"").decode()
        candidates = [record for record in image_candidates.get(image_value["name"], [])
                      if (record["width"], record["height"], record["depth"],
                          record["map_type"], record["semantic"], record["fourcc"])
                      == (image_value.get("width"), image_value.get("height"),
                          image_value.get("depth"), image_value.get("pc_map_type", 3),
                          image_value.get("pc_semantic", 2), expected_fourcc)]
        hashes = {record["pixel_sha256"] for record in candidates}
        if len(hashes) > 1:
            audit["image_conflicts"].append({"image": image_value["name"],
                "zones": [record["donor_zone"] for record in candidates]})
        elif candidates:
            image_value["pc_image_donor"] = candidates[0]
            audit["image_matches"].append({"image": image_value["name"],
                "zone": candidates[0]["donor_zone"], "pixel_sha256": candidates[0]["pixel_sha256"]})
    return material_candidates, techsets, audit


def read_pc_material_donors(path, names, techset_names_out=None,
                            templates_out=None, validated_payload=None):
    payload = (_read_pc_zone_payload(path) if validated_payload is None
               else validated_payload)

    script_string_count, _, asset_count, _ = struct.unpack_from("<4I", payload)
    table_offset = 16 + script_string_count * 4
    if table_offset > len(payload):
        raise FormatError("PC material donor has a truncated script-string table")
    script_pointers = struct.unpack_from(f"<{script_string_count}I", payload, 16)
    for pointer in script_pointers:
        # Native SP zones include a null script-string slot with no nested bytes.
        if pointer == 0:
            continue
        if pointer != INLINE:
            raise FormatError("unverified PC script-string pointer")
        table_offset = payload.find(b"\0", table_offset) + 1
        if table_offset == 0:
            raise FormatError("PC material donor has an unterminated script string")
    table_end = table_offset + asset_count * 8
    if table_end > len(payload):
        raise FormatError("PC material donor has a truncated asset table")
    entries = list(struct.iter_unpack(
        "<2I", payload[table_offset:table_end]))
    if any(kind >= len(PC_ASSET_NAMES) or pointer not in (INLINE, INSERT)
           for kind, pointer in entries):
        raise FormatError("invalid PC material donor manifest entries")
    techset_indices = [index for index, entry in enumerate(entries)
                       if entry[0] == 7]

    techset_roots = []
    root_offset = table_end
    while True:
        root_offset = payload.find(b"\xFF\xFF\xFF\xFF", root_offset)
        if root_offset < 0:
            break
        name_offset = root_offset + 184
        name_end = payload.find(b"\0", name_offset,
                                min(len(payload), name_offset + 129))
        if name_end > name_offset:
            raw_name = payload[name_offset:name_end]
            pointers = struct.unpack_from("<43I", payload, root_offset + 12)
            if (all(32 <= value <= 126 for value in raw_name)
                    and all(pointer in (0, INLINE)
                            or (pointer & 0xE0000001) == 0x40000001
                            for pointer in pointers)):
                techset_roots.append(raw_name.decode("ascii"))
        root_offset += 4
    if len(techset_roots) != len(techset_indices):
        raise FormatError(
            "PC material donor techset roots do not match its manifest")
    if techset_names_out is not None:
        techset_names_out.extend(techset_roots)
    techsets_by_index = dict(zip(techset_indices, techset_roots))
    # Native Gettler/Italia aliases land on the 4-byte-aligned block-2 table,
    # not its unaligned serialized byte offset after the runtime prefix.
    table_base = (table_offset - 11 + 3) & ~3

    donors = {}
    name_pattern = (re.compile(b"(?:" + b"|".join(re.escape(name.encode("latin-1"))
                    for name in sorted(names, key=len, reverse=True)) + b")\0") if names else None)
    for match in name_pattern.finditer(payload) if name_pattern else ():
        name = match.group()[:-1].decode("latin-1")
        if name in donors:
            continue
        record = _pc_material_record_at(payload, name, match.start())
        if record is None:
            continue
        pointer = struct.unpack_from("<I", record["header"], 84)[0]
        packed_offset = pointer - 0x40000001
        if (pointer < 0x40000001 or packed_offset < table_base
                or (packed_offset - table_base) % 8):
            continue
        manifest_index = (packed_offset - table_base) // 8
        techset_name = techsets_by_index.get(manifest_index)
        if techset_name is None:
            continue
        record["techset_name"] = techset_name
        donors[name] = record
    if templates_out is not None:
        seen = set(donors)
        for match in re.finditer(rb"[\x20-\x7e]{2,128}\x00", payload):
            name = match.group()[:-1].decode("ascii")
            if name in seen or not ("/" in name or name.startswith("*")):
                continue
            record = _pc_material_record_at(payload, name, match.start())
            if record is None:
                continue
            pointer = struct.unpack_from("<I", record["header"], 84)[0]
            packed_offset = pointer - 0x40000001
            if (pointer < 0x40000001 or packed_offset < table_base
                    or (packed_offset - table_base) % 8):
                continue
            manifest_index = (packed_offset - table_base) // 8
            techset_name = techsets_by_index.get(manifest_index)
            if techset_name is None:
                continue
            record["name"] = name
            record["techset_name"] = techset_name
            templates_out.append(record)
            seen.add(name)
    return donors


def _material_texture_signature_from_xenon(textures):
    return tuple(bytes.fromhex(value["definition"])[4:8]
                 for value in textures)


def _material_texture_signature_from_pc(record):
    return tuple(record["textures"][offset + 4:offset + 8]
                 for offset in range(0, len(record["textures"]), 12))


def write_pc_material(payload, material_value, techset_pointer,
                      image_pointers, inline_images=False):
    source = bytes.fromhex(material_value["header"])
    if len(source) != 96 or len(image_pointers) != len(material_value["textures"]):
        raise FormatError("invalid captured Xbox material")
    name = material_value.get("name")
    if not isinstance(name, str) or not isinstance(material_value.get("techset_name"), str):
        raise FormatError("material is missing a PC-resolvable name or techset")

    donor = material_value.get("pc_material_donor")
    state_donor = material_value.get("pc_state_donor")
    if donor is not None:
        converted = bytearray(donor["header"])
        state_bits = donor["state_bits"]
        constants = donor["constants"]
        texture_definitions = donor["textures"]
        if (len(converted) != 104
                or converted[67] != len(image_pointers)
                or len(texture_definitions) != len(image_pointers) * 12
                or converted[68] * 32 != len(constants)
                or converted[69] * 8 != len(state_bits)):
            raise FormatError(f"material {name!r} has an incompatible PC donor")
        state_layout_verified = True
    else:
        state_bits, state_mapping, state_layout_verified = _convert_material_state_bits(
            bytes.fromhex(material_value.get("state_bits", "")))
        constants = _convert_material_constants(bytes.fromhex(
            material_value.get("constants", "")))
        texture_definitions = None
        converted = bytearray(104)
        # MaterialInfo occupies the first 24 bytes on both platforms. PC expands
        # the following 36 Xenon technique-state slots to 43 entries.
        converted[:24] = source[:24]
        pc_techset_name = material_value.get("pc_techset_name", "")
        world_layout = pc_techset_name.lstrip(",").startswith("wc_")
        converted[24:67] = _convert_material_state_slots(
            source[24:60], state_mapping, world_layout)
        if state_donor is not None:
            converted[24:67] = state_donor["header"][24:67]
            state_bits = state_donor["state_bits"]
            state_layout_verified = True
        converted[67] = len(image_pointers)
        converted[68:75] = source[61:68]
        converted[69] = len(state_bits) // 8
    struct.pack_into("<I", converted, 0, INLINE)
    if donor is None:
        struct.pack_into("<2I", converted, 76, u32(source, 68), u32(source, 72))
    struct.pack_into("<4I", converted, 84, techset_pointer,
                     INLINE if image_pointers else 0,
                     INLINE if constants else 0,
                     INLINE if state_bits else 0)
    payload.extend(converted)
    payload.extend(name.encode() + b"\0")

    for index, (texture, image_pointer) in enumerate(zip(
            material_value["textures"], image_pointers)):
        definition = (texture_definitions[index * 12:(index + 1) * 12]
                      if texture_definitions is not None
                      else bytes.fromhex(texture["definition"]))
        if len(definition) != 12:
            raise FormatError(f"material {name!r} has an invalid texture definition")
        if texture_definitions is not None:
            payload.extend(definition[:8])
        else:
            payload.extend(struct.pack("<I", u32(definition)))
            payload.extend(definition[4:8])
        payload.extend(struct.pack("<I", image_pointer))
    if inline_images:
        for image_value in material_value["textures"]:
            write_pc_image(payload, image_value)
    payload.extend(constants)
    payload.extend(state_bits)
    material_value["pc_state_layout_verified"] = state_layout_verified


def write_pc_image(payload, image_value):
    if "pc_image_donor" in image_value:
        record = image_value["pc_image_donor"]
        serialized = record["serialized"]
        checked = pc_image_record_at(serialized, image_value["name"], 36)
        if checked is None or checked["pixel_sha256"] != record["pixel_sha256"]:
            raise FormatError("native PC image donor failed revalidation")
        payload.extend(serialized)
        return
    if image_value.get("pc_external"):
        name = image_value.get("name")
        if name not in PC_EXTERNAL_IMAGES:
            raise FormatError(f"unverified external PC image {name!r}")
        header = bytearray(36)
        struct.pack_into("<I", header, 32, INLINE)
        payload.extend(header)
        payload.extend(name.encode() + b"\0")
        return
    base = image_value.get("pc_mip_chain") or image_value.get("pc_base_level")
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

    if base["format"] == "DXN":
        data = transcode_dxn_to_dxt5(
            data, image_value.get("pc_normal_slope_probe", False))

    width = image_value["width"]
    height = image_value["height"]
    depth = max(1, image_value["depth"])
    header = bytearray(36)
    # QoS PC eco_hotel/mp_italia sp_bog_ft: map type 5, flags 6, six faces.
    map_type = image_value.get("pc_map_type", 3)
    if map_type not in (3, 5):
        raise FormatError(f"unverified PC image map type {map_type}")
    if map_type == 5:
        block_size = 8 if base["format"] == "DXT1" else 16
        expected = _divide_round_up(width, 4) * _divide_round_up(height, 4) * block_size * 6
        if width != height or depth != 1 or len(data) != expected:
            raise FormatError("PC cubemap requires six complete single-level faces")
    struct.pack_into("<2I", header, 0, map_type, INSERT)
    header[10] = image_value.get("pc_no_picmip", 1 if map_type == 5 else 0)
    header[11] = image_value.get("pc_semantic", 2)
    struct.pack_into("<2I", header, 16, len(data), len(data))
    struct.pack_into("<3H", header, 24, width, height, depth)
    header[30] = image_value.get("pc_category", 3)
    struct.pack_into("<I", header, 32, INLINE)
    payload.extend(header)
    payload.extend(name.encode() + b"\0")
    load_flags = image_value.get("pc_load_flags", (1, 6) if map_type == 5 else (0, 0))
    payload.extend(struct.pack("<2B3H4sI", load_flags[0], load_flags[1],
                               width, height, depth,
                               fourcc, len(data)))
    payload.extend(data)


def configure_pc_lightmap_image(image_value):
    """Apply the QoS PC GfxWorld lightmap image metadata contract."""
    image_value["pc_semantic"] = 1
    image_value["pc_category"] = 2
    image_value["pc_load_flags"] = (1, 2)


def _pc_gfx_aabb_header(tree, include_static_model_indexes=False):
    raw = bytes.fromhex(tree["raw"])
    indexes = bytes.fromhex(tree["indexes"])
    children = tree["children"]
    converted = bytearray(_little_endian_words(raw, 0, 32))
    # QoS PC sub_1036DF60/sub_1036DE70 select opposite corners directly
    # from these six floats; tree nodes retain mins/maxs on both platforms.
    index_count = len(indexes) // 4 if include_static_model_indexes else 0
    struct.pack_into("<4I", converted, 32, index_count,
                     INLINE if index_count else 0, len(children),
                     INLINE if children else 0)
    return converted


def _write_pc_gfx_aabb_nested(payload, tree,
                              include_static_model_indexes=False):
    indexes = bytes.fromhex(tree["indexes"])
    if include_static_model_indexes:
        payload.extend(_little_endian_words(indexes))
    children = tree["children"]
    for child in children:
        payload.extend(_pc_gfx_aabb_header(
            child, include_static_model_indexes))
    for child in children:
        _write_pc_gfx_aabb_nested(
            payload, child, include_static_model_indexes)


def _pc_gfx_dpvs_planes_header(dpvs_planes):
    raw = bytes.fromhex(dpvs_planes.get("raw", ""))
    if not raw:
        return bytes(52)
    if len(raw) != 52:
        raise FormatError("invalid Xbox GfxWorld DPVS-plane header")

    converted = bytearray(52)
    # The leading 16 bytes are eight uint16 partition boundaries. The rest is
    # composed of 32-bit scalars and archive pointers on both platforms.
    converted[:16] = _little_endian_u16_array(raw[:16])
    converted[16:] = _little_endian_words(raw[16:])
    for offset, field in ((24, "plane_indices"), (32, "cell_bits"),
                          (40, "scene_ent_cell_bits"), (48, "brush_models")):
        struct.pack_into("<I", converted, offset,
                         INLINE if dpvs_planes.get(field) else 0)
    return bytes(converted)


def _write_pc_gfx_dpvs_planes_nested(payload, dpvs_planes):
    payload.extend(_little_endian_u16_array(bytes.fromhex(
        dpvs_planes.get("plane_indices", ""))))
    payload.extend(bytes.fromhex(dpvs_planes.get("cell_bits", "")))
    payload.extend(_little_endian_words(bytes.fromhex(
        dpvs_planes.get("scene_ent_cell_bits", ""))))
    payload.extend(_little_endian_words(bytes.fromhex(
        dpvs_planes.get("brush_models", ""))))


def _pc_gfx_cell_header(cell, include_static_models=False,
                        include_empty_tree=False):
    raw = bytes.fromhex(cell["raw"])
    converted = bytearray(_little_endian_words(raw, 0, 24))
    # PC sub_103ABFA0 selects mins/maxs directly before queuing cell jobs.
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
                              include_empty_tree=False, include_model_indexes=False):
    if include_static_models and cell["tree"]:
        payload.extend(_pc_gfx_aabb_header(cell["tree"], include_model_indexes))
        _write_pc_gfx_aabb_nested(payload, cell["tree"], include_model_indexes)
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
                include_empty_tree, include_model_indexes)
        payload.extend(_little_endian_words(bytes.fromhex(portal["vertices"])))

    payload.extend(_little_endian_words(bytes.fromhex(cell["cull_groups"])))
    # Reflection-probe indices are omitted with their root allocation.


def write_pc_gfx_world(payload, asset, primary_light_count,
                       material_pointers=None, image_pointers=None):
    geometry = asset["geometry"]
    planes = bytes.fromhex(geometry["planes"])
    nodes = bytes.fromhex(geometry["nodes"])
    indices = bytes.fromhex(geometry["indices"])
    xbox_surfaces = bytes.fromhex(geometry["surfaces"])
    xbox_brush_models = bytes.fromhex(geometry.get("brush_models", ""))
    dpvs_planes = dict(geometry.get("dpvs_planes", {}))
    dpvs_planes.setdefault("brush_models", geometry.get("brush_models", ""))
    dpvs_worlds = bytes.fromhex(geometry.get("dpvs_worlds", ""))
    sky_start_surfs = bytes.fromhex(geometry["sky_start_surfs"])
    xbox_vertices = bytes.fromhex(geometry["vertices"])
    vertex_layers = bytes.fromhex(geometry["vertex_layers"])
    static_draws = bytes.fromhex(geometry.get("static_model_draws", ""))
    static_instances = bytes.fromhex(geometry.get("static_model_insts", ""))
    static_model_pointers = geometry.get("pc_static_model_pointers")
    cells = geometry.get("cells", [])
    reflection_probes = geometry.get("reflection_probes", [])
    lightmaps = geometry.get("lightmaps", [])
    runtime_tail = bytes.fromhex(geometry.get("runtime_tail", ""))
    runtime_records = bytes.fromhex(geometry.get("runtime_records", ""))
    image_pointers = image_pointers or {}

    if (len(xbox_surfaces) % 72 or len(xbox_brush_models) % 168
            or len(dpvs_worlds) % 60 or len(xbox_vertices) % 44):
        raise FormatError("invalid captured Xbox world geometry")
    surface_count = len(xbox_surfaces) // 72
    vertex_count = len(xbox_vertices) // 44
    brush_model_count = len(xbox_brush_models) // 168
    dpvs_world_count = len(dpvs_worlds) // 60
    surface_remap_count = u32(dpvs_worlds, 48) if dpvs_world_count else 0
    if surface_remap_count > surface_count:
        raise FormatError(
            "primary DPVS surface count exceeds GfxWorld surface count")
    surface_remap = _pc_surface_remap_from_xenon(
        bytes.fromhex(geometry.get("surface_remap", "")),
        surface_remap_count, surface_count)
    static_model_count = len(static_draws) // 40
    if len(static_draws) % 40 or len(static_instances) != static_model_count * 32:
        raise FormatError("invalid captured Xbox static-model arrays")
    have_static_models = static_model_count > 0 and static_model_pointers is not None
    if have_static_models and len(static_model_pointers) != static_model_count:
        raise FormatError("invalid relocated static-model pointer array")
    # PC 103D8960 allocates the four +584 visibility buffers from +276.
    # Enable placements and tree model indices together, never indices alone.
    include_static_models = have_static_models
    def validate_tree_indexes(tree):
        if tree is None:
            return
        indexes = bytes.fromhex(tree["indexes"])
        if len(indexes) % 4 or any(index >= static_model_count
                                 for (index,) in struct.iter_unpack(">I", indexes)):
            raise FormatError("GfxAabbTree static-model index exceeds placement array")
        for child in tree["children"]:
            validate_tree_indexes(child)
    if include_static_models:
        for cell in cells:
            validate_tree_indexes(cell["tree"])
    include_cell_trees = True
    include_empty_cell_trees = False

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
        material_pointer = (material_pointers[index]
                            if material_pointers is not None else INLINE)
        struct.pack_into("<I", pc_surfaces, target + 16, material_pointer)
        pc_surfaces[target + 20:target + 24] = source[44:48]
        # PC sub_1036E2E0 passes these corners to sub_1036E030, which
        # selects mins/maxs directly using the frustum-plane byte offsets.
        pc_surfaces[target + 24:target + 48] = _little_endian_words(
            source[48:72])

    pc_vertices = convert_world_vertices(xbox_vertices)
    pc_header = bytearray(728)
    struct.pack_into("<2I", pc_header, 0, INLINE, INLINE)
    struct.pack_into("<2I", pc_header, 8, len(planes) // 20, INLINE if planes else 0)
    struct.pack_into("<2I", pc_header, 16, len(nodes) // 2, INLINE if nodes else 0)
    struct.pack_into("<2I", pc_header, 24, len(indices) // 2, INLINE if indices else 0)
    struct.pack_into("<2I", pc_header, 32, surface_count, INLINE if surface_count else 0)
    # PC 103C7DE5/103C7DF6 pass these half-open ranges to 10391A10.
    # Xenon keeps the corresponding four scalars at +88..+100.
    draw_ranges = geometry.get("surface_draw_ranges", [0, surface_count,
                                                       surface_count, surface_count])
    if (len(draw_ranges) != 4 or not
            0 <= draw_ranges[0] <= draw_ranges[1] <= draw_ranges[2]
            <= draw_ranges[3] <= surface_count):
        raise FormatError("GfxWorld surface draw ranges exceed the surface array")
    struct.pack_into("<4I", pc_header, 44, *draw_ranges)
    struct.pack_into("<2I", pc_header, 60, len(sky_start_surfs) // 4,
                     INLINE if sky_start_surfs else 0)
    struct.pack_into("<2I", pc_header, 80, vertex_count, INLINE if vertex_count else 0)
    struct.pack_into("<2I", pc_header, 92, len(vertex_layers), INLINE if vertex_layers else 0)
    # The renderer dereferences this 68-byte world-sun record while bringing
    # up a map. Its final light-definition pointer remains null in the probe.
    struct.pack_into("<I", pc_header, 232, INLINE)
    struct.pack_into("<I", pc_header, 252, primary_light_count)
    struct.pack_into("<2I", pc_header, 264, len(reflection_probes),
                     INLINE if reflection_probes else 0)
    struct.pack_into("<3I", pc_header, 276,
                     static_model_count if include_static_models else 0,
                     INLINE if include_static_models else 0,
                     INLINE if include_static_models else 0)
    struct.pack_into("<3I", pc_header, 288, len(cells),
                     (len(cells) + 31) // 32, INLINE if cells else 0)
    struct.pack_into("<2I", pc_header, 300, len(lightmaps),
                     INLINE if lightmaps else 0)
    pc_header[308:360] = _pc_gfx_dpvs_planes_header(dpvs_planes)
    struct.pack_into("<2I", pc_header, 352, brush_model_count,
                     INLINE if brush_model_count else 0)
    struct.pack_into("<2I", pc_header, 360, dpvs_world_count,
                     INLINE if dpvs_world_count else 0)
    if runtime_tail:
        if len(runtime_tail) != 168:
            raise FormatError("invalid captured Xbox GfxWorld runtime tail")
        runtime_record_count = u32(runtime_tail, 156)
        runtime_record_pointer = u32(runtime_tail, 160)
        expected_runtime_bytes = (runtime_record_count * 24
                                  if runtime_record_pointer else 0)
        if len(runtime_records) != expected_runtime_bytes:
            raise FormatError(
                "captured Xbox GfxWorld runtime-record count mismatch")
    # The Xenon runtime tail is not layout-compatible with PC. Enabling its
    # markers made native DB_LoadXAsset stall inside the GfxWorld loader, and
    # its final record table uses a different surface-index representation.
    # Retain the conservative PC allocation layout that is known to load while
    # keeping the source tail captured for future field-by-field conversion.
    visibility_capacity = len(cells) + 1
    struct.pack_into("<2I", pc_header, 576,
                     visibility_capacity, visibility_capacity)
    struct.pack_into("<8I", pc_header, 584, *((INLINE,) * 8))
    struct.pack_into("<4I", pc_header, 616,
                     INLINE, INLINE, INLINE, INLINE)
    struct.pack_into("<2I", pc_header, 664, INLINE, INLINE)
    struct.pack_into("<2I", pc_header, 680, INLINE, INLINE)
    struct.pack_into("<I", pc_header, 696,
                     INLINE if surface_remap else 0)
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
    if material_pointers is not None and len(material_pointers) != surface_count:
        raise FormatError("material pointer count does not match world surfaces")
    if material_pointers is None:
        for _ in range(surface_count):
            payload.extend(_pc_external_material())
    payload.extend(_little_endian_words(sky_start_surfs))
    payload.extend(bytes(68))
    for probe in reflection_probes:
        source = bytes.fromhex(probe["raw"])
        if len(source) != 16:
            raise FormatError("invalid Xenon reflection-probe record")
        image_value = probe.get("image")
        pointer = (image_pointers.get(image_value.get("name"), 0)
                   if image_value else 0)
        payload.extend(_little_endian_words(source, 0, 12)[:12])
        payload.extend(struct.pack("<I", pointer))
    if include_static_models:
        payload.extend(convert_static_model_draws(
            static_draws, static_model_pointers))
        payload.extend(convert_static_model_instances(static_instances))
    for cell in cells:
        payload.extend(_pc_gfx_cell_header(
            cell, include_cell_trees, include_empty_cell_trees))
    for cell in cells:
        _write_pc_gfx_cell_nested(
            payload, cell, include_cell_trees, include_empty_cell_trees,
            include_static_models)
    for lightmap in lightmaps:
        pair = lightmap.get("images", [])
        if len(pair) != 2:
            raise FormatError("invalid Xenon lightmap image pair")
        for image_value in pair:
            pointer = (image_pointers.get(image_value.get("name"), 0)
                       if image_value else 0)
            payload.extend(struct.pack("<I", pointer))
    # QoS uses the same 52-byte DPVS-plane header, 168-byte brush-model bounds,
    # and 60-byte DPVS world records on PC and Xbox. These records own the
    # surface ranges used for world draw submission; emitting an empty record
    # leaves collision intact but prevents any map geometry from being drawn.
    _write_pc_gfx_dpvs_planes_nested(payload, dpvs_planes)
    payload.extend(_pc_dpvs_worlds(dpvs_worlds))
    payload.extend(pc_vertices)
    payload.extend(vertex_layers)
    payload.extend(surface_remap)
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
        # CollisionAabbTree stores Bounds (six floats), followed by two
        # halfwords and a child/partition index. PC sub_103E6780 reads the
        # child count at +26 and recursively indexes through the dword at +28.
        "aabb_trees": (32, (0, 4, 8, 12, 16, 20, 28), (24, 26)),
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
    edge_base = side_base + side_bytes + inline_side_bytes
    edge_bytes = len(bytes.fromhex(collision["brush_edges"]["data"]))
    edge_allocation_references = [
        value for value in edge_references
        if edge_base <= value < edge_base + edge_bytes
    ]
    # Some Xenon brush records alias the presumed edge field back into the
    # side allocation. The captured sequential stream still places edges
    # immediately after all sides, so ignore those lower aliases and require
    # at least one reference inside the actual edge allocation.
    if not edge_allocation_references:
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
        # baseAdjacentSide may legally alias a byte inside the side allocation;
        # _relocate_collision_pointer resolves those values by their actual
        # source region. Validate only pointers that target the edge array as
        # edge-array references.
        "brush_edges": (edge_allocation_references, 1),
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
    block2_cursor += _pc_clip_block2_cursor_bias(clip_name) + 324
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


def resolve_material_image_references(materials):
    """Resolve repeated Xenon image slots to previously decoded image content.

    Packed image values address an earlier 12-byte MaterialTextureDef image
    field. The definition prefix identifies the matching semantic/sampler slot;
    references are resolved only backward. Callers must include earlier XModel
    and top-level materials before GfxWorld materials in stream order.
    """
    decoded_slots = []
    resolved = {}
    external = set()
    visited = set()
    resolved_count = 0
    family_mappings = []

    # Canals comparison probe: these variants share the explicitly serialized
    # cobblestone texture family. Generic sampler hashes also match unrelated
    # wood-door slots, so they are not sufficient evidence of image identity.
    family_images = {}
    for material_value in materials:
        for texture in material_value.get("textures", []):
            if texture.get("name") is not None:
                family_images.setdefault(texture["name"], texture)
    canals_cobble_slots = {
        "0x40538ddd": "~gt_cobble_stonegrnd_02_s-g&$~3d30c20d",
        "0x40538de9": "gt_cobble_stonegrnd_02_n",
        "0x40538df5": "gt_cobble_stonegrnd_02_c",
    }
    if any(value.get("name") == "wc/gt_cobble_stonegrnd_02_out"
           for value in materials):
        for reference, name in canals_cobble_slots.items():
            if name in family_images:
                resolved[reference] = family_images[name]

    # Provisional family matching precedes the legacy sampler-only fallback.
    # Require complete underscore-delimited roots and reject conflicting names
    # for a shared packed pointer rather than choosing the last loaded image.
    family_candidates = {}
    for name, texture in family_images.items():
        definition = texture.get("definition")
        if not definition:
            continue
        root = name.lstrip("~").split("-g&", 1)[0]
        root = re.sub(r"_[cns]$", "", root)
        if len(root) < 8 or "_" not in root:
            continue
        prefix = bytes.fromhex(definition)[:8]
        family_candidates.setdefault(prefix, []).append((root, name, texture))
    proposals = {}
    seen_materials = set()
    for material_value in materials:
        if id(material_value) in seen_materials:
            continue
        seen_materials.add(id(material_value))
        material_name = material_value.get("name", "").split("/", 1)[-1]
        for texture in material_value.get("textures", []):
            reference, definition = texture.get("reference"), texture.get("definition")
            if reference is None or not definition:
                continue
            matches = [(root, name, value) for root, name, value in
                       family_candidates.get(bytes.fromhex(definition)[:8], [])
                       if material_name == root or material_name.startswith(root + "_")]
            if not matches:
                continue
            longest = max(len(root) for root, _, _ in matches)
            names = {name: value for root, name, value in matches if len(root) == longest}
            if len(names) == 1:
                proposals.setdefault(reference, {}).update(names)
    for reference, names in proposals.items():
        if len(names) != 1 or reference in resolved:
            continue
        name, target = next(iter(names.items()))
        resolved[reference] = target
        family_mappings.append({"reference": reference, "image": name,
                                "reason": "provisional_material_name_family"})

    for material_value in materials:
        identity = id(material_value)
        if identity in visited:
            continue
        visited.add(identity)
        textures = material_value.get("textures", [])
        updated = []
        for texture in textures:
            definition = texture.get("definition")
            prefix = bytes.fromhex(definition)[:8] if definition else None
            image_value = texture
            reference = texture.get("reference")
            if reference is not None:
                target = resolved.get(reference)
                if target is None and reference not in external and prefix is not None:
                    match = next((candidate for candidate_prefix, candidate
                                  in reversed(decoded_slots)
                                  if candidate_prefix == prefix), None)
                    if match is None:
                        external.add(reference)
                    else:
                        resolved[reference] = match
                        target = match
                if target is not None:
                    image_value = dict(target)
                    image_value["definition"] = definition
                    image_value["resolved_reference"] = reference
                    resolved_count += 1
            updated.append(image_value)
            if image_value.get("name") is not None and prefix is not None:
                decoded_slots.append((prefix, image_value))
        material_value["textures"] = updated

    return {
        "resolved_texture_slots": resolved_count,
        "resolved_image_references": len(resolved),
        "external_image_references": sorted(external),
        "provisional_family_mappings": family_mappings,
    }


def materials_preceding_gfx_world(assets, gfx_world):
    """Collect material image sources serialized before the GfxWorld root."""
    preceding = []
    for asset in assets:
        if asset is gfx_world:
            return preceding
        if asset["type"] == "xmodel":
            preceding.extend(asset.get("materials", []))
        elif asset["type"] == "material":
            preceding.append(asset)
    raise FormatError("GfxWorld is absent from its asset stream")


def _pc_map_assets(models, techset_names, images, materials, rawfiles,
                   include_materials):
    # QoS PC's image-pointer loader (0x103D45B0) immediately dereferences a
    # packed asset-table reference. Images must therefore be linked before the
    # materials that consume their table cells; forward references retain the
    # table's INLINE marker and render white.
    return ([(12, "clip")]
            + [(7, name) for name in techset_names]
            + [(8, image_value) for image_value in images]
            + ([(6, None)] + [(6, material_value)
                              for material_value in materials]
               if include_materials else [])
            + [(5, model) for model in models]
            + [(13, "com"), (17, "gfx"), (15, "game")]
            + [(32, rawfile) for rawfile in rawfiles])


def build_pc_map_probe(path, include_images=False, include_materials=False,
                       diagnostics=None, pc_material_donor=None,
                       flatten_world_culling=False, normal_slope_probe=False,
                       pc_donor_directory=None, pc_donor_exclude=()):
    """Build a reduced PC zone for testing map and world deserialization.

    QoS PC mp_barge.ff confirms that PC and Xenon both use version 470 and the
    same 28-byte header. PC stores all seven words little-endian and orders its
    map roots as ComWorld, GfxWorld, GameWorldMp, then clipMap.
    """
    report = inspect(path, True, True)
    if normal_slope_probe:
        def mark_normal_images(value):
            if isinstance(value, dict):
                decoded = value.get("pc_mip_chain") or value.get("pc_base_level")
                if isinstance(decoded, dict) and decoded.get("format") == "DXN":
                    value["pc_normal_slope_probe"] = True
                for child in value.values():
                    mark_normal_images(child)
            elif isinstance(value, list):
                for child in value:
                    mark_normal_images(child)
        mark_normal_images(report)
        if diagnostics is not None:
            diagnostics["normal_encoding_probe"] = "BC5 unit XY to QoS PC slopes"

    def one(kind):
        matches = [asset for asset in report["assets"] if asset["type"] == kind]
        if len(matches) != 1:
            raise FormatError(f"map probe requires exactly one {kind} asset")
        return matches[0]

    com_world = one("com_map")
    game_world = one("game_map_mp")
    clip_map = one("col_map_mp")
    gfx_world = one("gfx_map")
    if flatten_world_culling:
        cells = gfx_world["geometry"]["cells"]
        if len(cells) != 1 or not cells[0].get("tree"):
            raise FormatError("flattened culling probe requires one rooted GfxCell")
        cells[0]["tree"]["children"] = []
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
    preceding_materials = materials_preceding_gfx_world(
        report["assets"], gfx_world)
    model_materials = [value for model in models for value in model["materials"]
                       if "header" in value]
    model_material_ids = {id(value) for value in model_materials}
    material_values = gfx_world["geometry"]["surface_materials"] + model_materials
    material_image_report = resolve_material_image_references(
        preceding_materials + gfx_world["geometry"]["surface_materials"])
    material_image_report["preceding_material_count"] = len(preceding_materials)
    images_by_name = {}
    if include_images:
        def keep_image(image_value):
            if not image_value or not isinstance(image_value.get("name"), str):
                return
            name = image_value["name"]
            if name in PC_EXTERNAL_IMAGES:
                images_by_name.setdefault(name, {"name": name, "pc_external": True})
            elif ("pc_base_level" in image_value
                  and image_value["pc_base_level"]["format"] in PC_TEXTURE_FOURCC):
                images_by_name.setdefault(name, image_value)

        for material_value in material_values:
            for image_value in material_value.get("textures", []):
                if "pc_base_level" in image_value:
                    base = image_value["pc_base_level"]
                    if base["format"] in PC_TEXTURE_FOURCC:
                        definition = image_value.get("definition")
                        if definition:
                            image_value.setdefault(
                                "pc_semantic", bytes.fromhex(definition)[7])
                        keep_image(image_value)
                else:
                    keep_image(image_value)
        for probe in gfx_world["geometry"].get("reflection_probes", []):
            keep_image(probe.get("image"))
        for lightmap in gfx_world["geometry"].get("lightmaps", []):
            for image_value in lightmap.get("images", []):
                # Native QoS PC world lightmaps use IMG_SEMANTIC_1 and the
                # category/load tuple 2/(1, 2), unlike ordinary streamed
                # material images. These fields control renderer texture setup.
                configure_pc_lightmap_image(image_value)
                keep_image(image_value)
    images = list(images_by_name.values())
    convertible_materials = []
    unique_materials = []
    converted_by_source = {}
    techsets_by_name = {}
    techset_mappings = Counter()
    material_mappings = Counter()
    unverified_state_layouts = Counter()
    unverified_state_materials = []
    donor_materials = {}
    donor_templates = []
    donor_techset_names = []
    donor_rejections = Counter()
    incomplete_materials = Counter()
    incomplete_material_ids = set()
    donor_names = set()
    if include_materials and pc_material_donor is not None:
        donor_names = {
            value.get("name")
            for value in material_values
            if isinstance(value.get("name"), str)
        }
        donor_materials = read_pc_material_donor_zones(
            pc_material_donor, donor_names, donor_techset_names,
            donor_templates)
    directory_audit = None
    if pc_donor_directory is not None:
        names = {value.get("name") for value in material_values
                 if isinstance(value.get("name"), str)}
        directory_materials, directory_techsets, directory_audit = scan_pc_zone_directory(
            pc_donor_directory, images, names, pc_donor_exclude)
        available = {name.lstrip(",") for name in (*PC_COMMON_TECHSETS, *donor_techset_names)}
        directory_audit["catalogued_techset_count"] = len(set(directory_techsets))
        directory_audit["unavailable_material_techsets"] = []
        for name, records in directory_materials.items():
            for record in records:
                if record["techset_name"].lstrip(",") in available:
                    donor_materials.setdefault(name, []).append(record)
                else:
                    directory_audit["unavailable_material_techsets"].append({
                        "material": name, "techset": record["techset_name"],
                        "zone": record["donor_zone"]})
    pc_techset_candidates = tuple(dict.fromkeys(
        (*PC_COMMON_TECHSETS, *donor_techset_names)))
    if include_materials:
        for material_value in material_values:
            source_identity = id(material_value)
            if source_identity not in converted_by_source:
                source_techset_name = material_value.get("techset_name", "")
                pc_techset_name, mapping_reason = select_pc_material_techset(
                    material_value, pc_techset_candidates)
                if source_identity in model_material_ids:
                    source_name = material_value.get("techset_name")
                    pc_techset_name = (source_name if source_name in pc_techset_candidates
                                      else None)
                    mapping_reason = "exact_model_techset" if pc_techset_name else "unsupported_model_techset"
                textures = _complete_pc_material_textures(
                    material_value, images_by_name)
                if textures is None and material_value.get("textures"):
                    incomplete_materials[material_value.get("name", "<unnamed>")] += 1
                    incomplete_material_ids.add(source_identity)
                convertible = ("header" in material_value
                               and pc_techset_name is not None
                               and textures is not None)
                converted = None
                if convertible:
                    converted = dict(material_value)
                    converted["textures"] = textures
                    converted["pc_techset_name"] = pc_techset_name
                    donor_candidates = donor_materials.get(converted.get("name"), [])
                    if donor_candidates:
                        donor = select_pc_material_donor(textures, donor_candidates)
                        if donor is not None:
                            converted["pc_material_donor"] = donor
                            pc_techset_name = donor["techset_name"]
                            converted["pc_techset_name"] = pc_techset_name
                            mapping_reason = "native_pc_donor"
                        else:
                            donor_rejections[converted.get("name", "<unnamed>")] += 1
                    source_state_count = len(bytes.fromhex(
                        converted.get("state_bits", ""))) // 8
                    # Native PC Barge wc_l_sm_t0c0 has seven state records.
                    # Canals glass uses five on Xenon; generic expansion only
                    # makes six and misindexes this alpha-test technique family.
                    if ("pc_material_donor" not in converted
                            and requires_pc_state_template(
                                pc_techset_name, source_state_count)):
                        signature = _material_texture_signature_from_xenon(textures)
                        state_donor = next((candidate for candidate in donor_templates
                            if candidate["techset_name"].lstrip(",")
                                == pc_techset_name.lstrip(",")
                            and candidate["header"][67] == len(textures)
                            and _material_texture_signature_from_pc(candidate)
                                == signature), None)
                        state_template_reason = "native_pc_state_template"
                        if state_donor is None:
                            state_donor = next((candidate
                                for candidate in donor_templates
                                if candidate["techset_name"].lstrip(",")
                                    == pc_techset_name.lstrip(",")),
                                None)
                            state_template_reason = (
                                "native_pc_techset_state_template")
                        if (state_donor is None
                                and pc_techset_name.lstrip(",").endswith("p0")):
                            compatible_name = pc_techset_name.lstrip(",")[:-2]
                            state_donor = next((candidate
                                for candidate in donor_templates
                                if candidate["techset_name"].lstrip(",")
                                    == compatible_name), None)
                            state_template_reason = (
                                "native_pc_compatible_state_template")
                        if state_donor is not None:
                            converted["pc_state_donor"] = state_donor
                            mapping_reason = state_template_reason
                    unique_materials.append(converted)
                    techsets_by_name.setdefault(pc_techset_name, converted)
                if pc_techset_name is not None:
                    techset_mappings[(source_techset_name, pc_techset_name)] += 1
                    material_mappings[(material_value.get("name", "<unnamed>"),
                                       source_techset_name, pc_techset_name,
                                       mapping_reason)] += 1
                converted_by_source[source_identity] = converted
            converted = converted_by_source[source_identity]
        convertible_materials = [converted_by_source[id(value)]
                                 for value in gfx_world["geometry"]["surface_materials"]]
    gfx_world["geometry"]["material_image_resolution"] = material_image_report
    techset_names = list(techsets_by_name)
    assets = _pc_map_assets(models, techset_names, images, unique_materials,
                            rawfiles, include_materials)
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
    destination_indices = {model["manifest_index"]: index
                           for index, (kind, model) in enumerate(assets) if kind == 5}
    gfx_world["geometry"]["pc_static_model_pointers"] = [
        0x40000001 + asset_table_base + destination_indices[source_index] * 8
        for source_index in gfx_world["geometry"]["static_model_asset_indices"]
    ] if models else []

    block2_cursor = asset_table_base + len(assets) * 8
    clip_block2_end = write_pc_clip_map(
        payload, clip_map, block2_cursor, clip_name, entity_string, entity_name)

    for name in techset_names:
        payload.extend(_pc_external_techset(name))

    for image_value in images:
        write_pc_image(payload, image_value)

    def manifest_pointer(index):
        return 0x40000001 + asset_table_base + index * 8

    image_pointers = {
        value["name"]: manifest_pointer(index)
        for index, (kind, value) in enumerate(assets) if kind == 8
    }
    surface_material_pointers = None
    if include_materials:
        techset_pointers = {
            value: manifest_pointer(index)
            for index, (kind, value) in enumerate(assets) if kind == 7
        }
        material_entries = [
            (index, value) for index, (kind, value) in enumerate(assets)
            if kind == 6
        ]
        fallback_material_pointer = manifest_pointer(material_entries[0][0])
        material_pointers = {
            id(value): manifest_pointer(index)
            for index, value in material_entries[1:]
        }

        payload.extend(_pc_external_material())
        for material_value in unique_materials:
            write_pc_material(
                payload, material_value,
                techset_pointers[material_value["pc_techset_name"]],
                [image_pointers[image_value["name"]]
                 for image_value in material_value["textures"]])
            if not material_value.get("pc_state_layout_verified", False):
                source_count = len(bytes.fromhex(
                    material_value.get("state_bits", ""))) // 8
                unverified_state_layouts[source_count] += 1
                unverified_state_materials.append({
                    "material": material_value.get("name", "<unnamed>"),
                    "xenon_state_count": source_count,
                    "pc_techset": material_value.get("pc_techset_name"),
                })
        surface_material_pointers = [
            (material_pointers[id(material_value)]
             if material_value is not None else fallback_material_pointer)
            for material_value in convertible_materials
        ]

    for model in models:
        pointers = ([material_pointers.get(id(converted_by_source.get(id(value))),
                                         fallback_material_pointer)
                     for value in model["materials"]] if include_materials else None)
        write_pc_xmodel(payload, model, pointers)

    write_pc_com_world(payload, com_world)

    write_pc_gfx_world(payload, gfx_world, len(com_world["primary_lights"]),
                       surface_material_pointers, image_pointers)

    payload.extend(struct.pack("<I", INLINE))
    payload.extend(game_name.encode() + b"\0")

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
    # PC GfxWorld runtime-only buffers are allocated from XFile block 1. The
    # restored DPVS surface tables alone exceed the old 64 KiB probe reserve on
    # mp_canals, causing the loader to run past the block and clear the world
    # pointer before DB_LinkXAssetEntry. Native QoS PC multiplayer zones reserve
    # as much as ~1.3 MiB here, so retain conservative headroom for converted
    # worlds and scale further for unusually large surface/light counts.
    runtime_allocation = max(
        2 * 1024 * 1024,
        gfx_world["surface_count"] * 16
        + len(com_world["primary_lights"]) * 16384
        + 65536)
    result = bytearray(struct.pack("<7I", 470, len(payload), allocation,
                                   runtime_allocation, allocation, 0, 0))
    result.extend(zlib.compress(payload, 1))
    # Native PC and Xenon QoS fastfiles pad the zlib stream to 32 bytes.
    result.extend(bytes((-len(result)) % 32))
    if diagnostics is not None:
        if directory_audit is not None:
            diagnostics["pc_directory_audit"] = directory_audit
        diagnostics["material_image_resolution"] = material_image_report
        # Successful loading is not full asset coverage. Keep omissions visible
        # in the same report as material substitutions instead of hiding them.
        emitted_source_types = {
            "techset", "xmodel", "com_map", "gfx_map", "game_map_mp",
            "col_map_mp", "rawfile", "material",
        }
        diagnostics["completion_audit"] = {
            "world_surfaces": gfx_world["surface_count"],
            "static_model_placements": len(bytes.fromhex(
                gfx_world["geometry"].get("static_model_draws", ""))) // 40,
            "source_xmodels": len(models),
            "renderable_model_surfaces": sum(_renderable_rigid_surface(surface)
                for model in models for surface in model["surfaces"]),
            "model_material_slots": sum(len(model["materials"]) for model in models),
            "converted_model_material_slots": sum(
                converted_by_source.get(id(value)) is not None
                for model in models for value in model["materials"]),
            "fallback_model_materials": sorted({value.get("name", value.get("reference", "<unnamed>"))
                for model in models for value in model["materials"]
                if converted_by_source.get(id(value)) is None}),
            "layered_material_fallbacks": [
                {"material": material, "source_techset": source,
                 "pc_techset": pc, "reason": reason}
                for material, source, pc, reason in sorted(material_mappings)
                if material.startswith("*") and source != pc
            ],
            "non_emitted_asset_types": {
                kind: count for kind, count in report["asset_counts"].items()
                if kind not in emitted_source_types
            },
            "provisional_image_family_count": len(material_image_report.get(
                "provisional_family_mappings", [])),
            "vertex_layer_bytes": len(bytes.fromhex(
                gfx_world["geometry"].get("vertex_layers", ""))),
        }
        diagnostics["incomplete_texture_surface_count"] = sum(
            id(material_value) in incomplete_material_ids for material_value
            in gfx_world["geometry"]["surface_materials"])
        diagnostics["incomplete_texture_materials"] = [
            {"material": name, "definitions": count}
            for name, count in sorted(incomplete_materials.items())]
        diagnostics["techset_mappings"] = [
            {"source": source, "pc": pc, "exact": source == pc,
             "materials": count}
            for (source, pc), count in sorted(techset_mappings.items())
        ]
        diagnostics["material_mappings"] = [
            {"material": material, "source_techset": source,
             "pc_techset": pc, "reason": reason, "instances": count}
            for (material, source, pc, reason), count
            in sorted(material_mappings.items())
        ]
        diagnostics["unverified_state_layouts"] = [
            {"xenon_state_count": count, "materials": materials}
            for count, materials in sorted(unverified_state_layouts.items())
        ]
        diagnostics["unverified_state_materials"] = unverified_state_materials
        if pc_material_donor is not None:
            diagnostics["pc_material_donor"] = str(pc_material_donor)
            diagnostics["pc_material_donor_zones"] = [str(path) for path in (
                [pc_material_donor] if isinstance(pc_material_donor, (str, Path))
                else pc_material_donor)]
            diagnostics["pc_material_donor_sources"] = [
                {"material": value.get("name"),
                 "zone": value["pc_material_donor"].get("donor_zone"),
                 "techset": value["pc_techset_name"]}
                for value in unique_materials if "pc_material_donor" in value]
            diagnostics["pc_material_donor_matches"] = sum(
                "pc_material_donor" in value for value in unique_materials)
            diagnostics["pc_material_donor_candidates"] = len(donor_materials)
            diagnostics["pc_techset_donor_candidates"] = len(donor_techset_names)
            diagnostics["pc_state_template_candidates"] = len(donor_templates)
            diagnostics["pc_state_template_matches"] = sum(
                "pc_state_donor" in value for value in unique_materials)
            diagnostics["pc_material_donor_rejections"] = [
                {"material": name, "instances": count}
                for name, count in sorted(donor_rejections.items())
            ]
    return bytes(result)


def build_pc_load_zone(path):
    """Convert a self-contained Xenon map-load zone to the PC v470 layout.

    Load screens use only a shared 2D technique, materials, base-level images,
    and rawfiles. Unlike a GfxWorld they do not need any reduced-world probes,
    so this preserves their three native images and material bindings directly.
    """
    report = inspect(path, True, True)
    source_techsets = [asset for asset in report["assets"]
                       if asset["type"] == "techset"]
    if len(source_techsets) != 1:
        raise FormatError("load-zone conversion requires exactly one techset")
    techset_name = source_techsets[0].get("name")
    if techset_name != ",2d":
        raise FormatError(
            f"load-zone conversion requires the external ',2d' techset, got {techset_name!r}")

    materials = [dict(asset) for asset in report["assets"]
                 if asset["type"] == "material"]
    if not materials:
        raise FormatError("load-zone conversion requires at least one material")
    images_by_name = {}
    for material_value in materials:
        material_value["techset_name"] = techset_name
        for image_value in material_value.get("textures", []):
            name = image_value.get("name")
            if not isinstance(name, str) or "pc_base_level" not in image_value:
                raise FormatError(
                    f"load-zone material {material_value.get('name')!r} has no convertible inline image")
            images_by_name.setdefault(name, image_value)
    images = list(images_by_name.values())
    for image_value in images:
        image_value["pc_no_picmip"] = 1
        image_value["pc_semantic"] = 0
        levels = image_value.get("load_definition", {}).get("levels", 1)
        image_value["pc_load_flags"] = (1, 2) if levels == 1 else (0, 0)
    rawfiles = [asset for asset in report["assets"]
                if asset["type"] == "rawfile" and "data" in asset]

    # Native QoS PC map-load zones declare both 2D technique-set aliases and
    # serialize each image inline beneath its material rather than as a
    # top-level image asset.
    techset_names = [",sm2/2d", techset_name]
    assets = ([(7, name) for name in techset_names]
              + [(6, material_value) for material_value in materials]
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
    # With no script-string stream, native PC load zones encode the manifest
    # table at payload offset - 12. This gives 0x4000000D for asset index 1,
    # matching the stock QoS load zones' reference to the external ',2d' set.
    asset_table_base = asset_table_offset - 12

    def manifest_pointer(index):
        return 0x40000001 + asset_table_base + index * 8

    techset_pointer = manifest_pointer(1)
    for name in techset_names:
        payload.extend(_pc_external_techset(name))
    for material_value in materials:
        write_pc_material(payload, material_value, techset_pointer,
                          [INLINE] * len(material_value["textures"]), True)
    for rawfile in rawfiles:
        data = bytes.fromhex(rawfile["data"])
        if not data or not data.endswith(b"\0"):
            raise FormatError(f"rawfile {rawfile['name']!r} is not NUL-terminated")
        payload.extend(struct.pack("<3I", INLINE, len(data) - 1, INLINE))
        payload.extend(rawfile["name"].encode() + b"\0")
        payload.extend(data)

    allocation = len(payload) + 65536
    result = bytearray(struct.pack("<7I", 470, len(payload), allocation,
                                   65536, allocation, 0, 0))
    result.extend(zlib.compress(payload, 1))
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


def conversion_provenance(args, output, diagnostics):
    """Record content identities, not timestamps, for cross-PC regeneration.

    Preserve explicit donor order: the first compatible donor wins. Include
    every audited directory input, even rejected donors, since changing one
    can change which native records the converter accepts next time.
    """
    def identity(path):
        path = Path(path)
        digest = hashlib.sha256()
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        return {"path": str(path.resolve()), "bytes": path.stat().st_size,
                "sha256": digest.hexdigest()}

    return {
        "converter": identity(__file__),
        "source": identity(args.files[0]),
        "explicit_donors": [identity(path) for path in (args.pc_material_donor or [])],
        "directory_donors": [identity(value["zone"]) for value in
                             diagnostics.get("pc_directory_audit", {}).get("zones", [])
                             if not value.get("excluded")],
        "options": {"include_images": args.include_images or args.include_materials,
                    "include_materials": args.include_materials,
                    "flatten_world_culling": args.flatten_world_culling,
                    "normal_slope_probe": args.normal_slope_probe,
                    "pc_donor_exclude": args.pc_donor_exclude},
        "output": identity(output),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("files", nargs="+", type=Path)
    parser.add_argument("--details", action="store_true",
                        help="decode supported asset schemas and require exact stream consumption")
    parser.add_argument("--convert-map-probe", type=Path, metavar="OUTPUT",
                        help="emit a reduced PC v470 map zone for loader testing")
    parser.add_argument("--convert-load-zone", type=Path, metavar="OUTPUT",
                        help="emit a PC v470 2D map-load zone")
    parser.add_argument("--include-images", action="store_true",
                        help="include decoded base-level images in a converted map probe")
    parser.add_argument("--include-materials", action="store_true",
                        help="bind directly serialized world materials to converted images")
    parser.add_argument("--pc-material-donor", type=Path, action="append",
                        help="reuse native PC material data; repeat for multiple v470 zones")
    parser.add_argument("--pc-donor-directory", type=Path,
                        help="audit all PC zones for exact native image and material matches")
    parser.add_argument("--pc-donor-exclude", action="append", default=[],
                        help="exclude a generated zone filename from the native donor audit")
    parser.add_argument("--flatten-world-culling", action="store_true",
                        help="diagnostic: submit the one-cell world from its root AABB")
    parser.add_argument("--normal-slope-probe", action="store_true",
                        help="diagnostic: re-encode BC5 unit normals for the QoS PC slope decode")
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
            diagnostics = {}
            args.convert_map_probe.write_bytes(build_pc_map_probe(
                args.files[0], args.include_images, args.include_materials,
                diagnostics, args.pc_material_donor,
                args.flatten_world_culling, args.normal_slope_probe,
                args.pc_donor_directory, args.pc_donor_exclude))
            result = {"input": str(args.files[0]),
                      "output": str(args.convert_map_probe),
                      "bytes": args.convert_map_probe.stat().st_size,
                      **diagnostics}
            result["provenance"] = conversion_provenance(
                args, args.convert_map_probe, diagnostics)
            mappings_directory = Path(__file__).resolve().parents[1] / ".work" / "reports"
            mappings_directory.mkdir(parents=True, exist_ok=True)
            mappings_path = mappings_directory / (
                args.convert_map_probe.name + ".mappings.json")
            mappings_path.write_text(json.dumps(result, indent=2),
                                     encoding="utf-8")
            result["mappings_output"] = str(mappings_path)
            print(json.dumps(result, indent=2))
            return 0
        except (OSError, FormatError) as error:
            print(json.dumps({"file": str(args.files[0]), "error": str(error)}))
            return 1
    if args.convert_load_zone:
        if len(args.files) != 1:
            parser.error("--convert-load-zone requires exactly one input")
        try:
            args.convert_load_zone.write_bytes(build_pc_load_zone(args.files[0]))
            print(json.dumps({"input": str(args.files[0]),
                              "output": str(args.convert_load_zone),
                              "bytes": args.convert_load_zone.stat().st_size}))
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
