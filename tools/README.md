# Zone Tools

- `zone-dumper/`: Export assets from PC or Xenon `.ff` files. See
  [its guide](zone-dumper/README.md). `dump-fastfile.bat` remains the drag and
  drop launcher.
- `xenon-converter/`: Inspect Xenon zones and generate experimental PC zones.
  Run `python -B tools/xenon-converter/xenon_ff.py --help` for options.
- `zone-analysis/`: Compare PC and Xenon material or image data and preview
  embedded PC images. These scripts are independent command-line tools.

Run the converter's tests with
`python -B -m unittest discover -s tools/xenon-converter -p test_xenon_ff.py`.
Keep generated diagnostics and temporary files under ignored `tools/.work/`.

Map conversion writes `tools/.work/reports/<output-name>.mappings.json`.
Its `provenance` section records SHA-256 hashes for the converter, Xbox source,
ordered explicit PC donors, every non-excluded directory donor, and output,
plus the rendering probe options. Compare these identities when moving between
PCs; an old output timestamp alone does not mean regeneration loses progress.
Different input content or options can produce a different candidate even with
the same converter. Absolute paths can differ between machines.
