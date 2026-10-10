"""Extract lossless QoS button sprites from exported console font atlases.

Inputs are an RGBA atlas and the serialized font-map JSON, not rendered text.
Requires Pillow. Refuses to overwrite existing assets unless --replace is given.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
from PIL import Image

BUTTONS = dict(zip(
    (1, 2, 3, 4, 5, 6, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23),
    ("a", "b", "x", "y", "lb", "rb", "start", "back", "ls", "rs",
     "lt", "rt", "up", "down", "left", "right"), strict=True))


def export(atlas_path, map_path, family, output, replace=False):
    data = json.loads(map_path.read_text(encoding="utf-8"))
    font = next(row for row in data["fonts"] if row["font"] == "fonts/bigFont")
    glyphs = {row["code"]: row for row in font["glyphs"]}
    atlas = Image.open(atlas_path).convert("RGBA")
    if atlas.size != (512, 1024):
        raise ValueError("expected a QoS 512x1024 console gamefonts atlas")
    results = []
    prepared = []
    for code, name in BUTTONS.items():
        glyph = glyphs[code]
        uv = glyph["uv"]
        if len(uv) != 4 or not all(math.isfinite(v) and 0 <= v <= 1 for v in uv):
            raise ValueError("invalid glyph UV")
        bounds = tuple(math.floor(value * extent) for value, extent in
                       zip(uv, (*atlas.size, *atlas.size), strict=True))
        sprite = atlas.crop(bounds)
        if sprite.size != (32, 32) or sprite.getbbox() is None:
            raise ValueError(f"invalid or empty button sprite {code}")
        destination = output / f"controller_{family}_{name}.png"
        if destination.exists() and not replace:
            raise FileExistsError(destination)
        prepared.append((destination, sprite))
        results.append({"file": destination.name, "font_code": code,
                        "bounds": bounds,
                        "pixel_sha256": hashlib.sha256(sprite.tobytes()).hexdigest()})
    output.mkdir(parents=True, exist_ok=True)
    for destination, sprite in prepared:
        sprite.save(destination, optimize=True)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--atlas", type=Path, required=True)
    parser.add_argument("--font-map", type=Path, required=True)
    parser.add_argument("--family", choices=("xbox", "ps3"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--replace", action="store_true")
    args = parser.parse_args()
    result = export(args.atlas, args.font_map, args.family, args.output, args.replace)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
