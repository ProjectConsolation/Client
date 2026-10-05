"""Package a single image override directory as a classic ZIP/IWD archive."""
import argparse
from pathlib import Path
import zipfile

from dump_pc_images import safe_name


def package_images(source, destination):
    source = Path(source).resolve(strict=True)
    destination = Path(destination)
    if not source.is_dir():
        raise ValueError("source must be an image format directory")
    files = []
    names = set()
    for path in sorted(source.rglob("*")):
        if not path.is_file() or path.suffix.lower() not in (".iwi", ".dds", ".png"):
            continue
        if not path.resolve().is_relative_to(source):
            raise ValueError("image path escapes source directory")
        relative = path.relative_to(source).as_posix()
        name = "images/" + relative
        if not safe_name(relative) or not name.isascii() or len(name) >= 256:
            raise ValueError(f"unsafe or unsupported IWD path: {relative}")
        if name.lower() in names:
            raise ValueError(f"case-insensitive duplicate IWD path: {relative}")
        names.add(name.lower())
        if not 8 <= path.stat().st_size <= 64 * 1024 * 1024:
            raise ValueError(f"image size outside override limits: {relative}")
        files.append((path, name))
    if not files or len(files) > 65535:
        raise ValueError("classic IWD requires 1..65535 image entries")
    # Exclusive creation: do not overwrite an existing archive. Classic ZIP
    # avoids ZIP64 and newer compression methods unsupported by native QoS.
    with zipfile.ZipFile(destination, "x", compression=zipfile.ZIP_DEFLATED,
                         allowZip64=False) as archive:
        for path, name in files:
            entry = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_DEFLATED
            entry.external_attr = 0o100644 << 16
            archive.writestr(entry, path.read_bytes())
    with zipfile.ZipFile(destination) as archive:
        failed = archive.testzip()
        if failed:
            raise ValueError(f"archive CRC verification failed: {failed}")
    return len(files)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="directory containing override images, not the whole dump")
    parser.add_argument("destination", type=Path, help="new .iwd file (never overwritten)")
    args = parser.parse_args()
    try:
        count = package_images(args.source, args.destination)
    except (OSError, ValueError, zipfile.LargeZipFile) as error:
        parser.exit(1, f"IWD packaging failed: {error}\n")
    print(f"Packaged and CRC-verified {count} images: {args.destination}")


if __name__ == "__main__":
    main()
