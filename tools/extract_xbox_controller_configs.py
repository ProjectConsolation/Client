"""Export original controller rawfiles from a QoS Xenon zone, without executing them."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent / 'xenon-converter'))
from xenon_ff import inspect


def extract(zone, output):
    report = inspect(zone, details=True, capture_map=True, allow_partial=True)
    assets = [a for a in report.get('assets', []) if a.get('type') == 'rawfile'
              and a.get('name', '').endswith('.cfg')
              and a['name'].startswith(('buttons_', 'thumbstick_', 'viewsensitivity_'))]
    if not assets:
        raise ValueError('No parsed controller config rawfiles found')
    output.mkdir(parents=True, exist_ok=True)
    manifest = {'zone': Path(zone).name, 'source_sha256': report['source_sha256'],
                'partial_error': report.get('partial_error'), 'files': []}
    for asset in assets:
        name = asset['name']
        if Path(name).name != name or '/' in name or '\\' in name:
            raise ValueError('Non-flat config name')
        data = bytes.fromhex(asset['data'])
        if not data.endswith(b'\0'):
            raise ValueError('Rawfile lacks its serialized terminator')
        data = data[:-1]
        target = output / name
        if target.exists() and target.read_bytes() != data:
            raise ValueError(f'Refusing to overwrite changed config {target}')
        target.write_bytes(data)
        manifest['files'].append({'name': name, 'sha256': hashlib.sha256(data).hexdigest(),
                                  'stream_offset': asset.get('offset')})
    (output / 'source.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('zone', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    result = extract(args.zone, args.output)
    print(f"Exported {len(result['files'])} controller configs; original bytes retained.")
