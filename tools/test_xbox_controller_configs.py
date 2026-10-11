"""Original controller config extraction and provenance regression checks."""
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import extract_xbox_controller_configs as exporter


class XboxControllerConfigsTests(unittest.TestCase):
    def temporary_output(self):
        work = Path(__file__).parent / '.work'
        work.mkdir(exist_ok=True)
        return tempfile.TemporaryDirectory(prefix='controller-config-test-', dir=work)

    def report(self, name='buttons_default.cfg', data=b'bind BUTTON_LTRIG "+speed"\r\n\0'):
        return {'source_sha256': 'source', 'partial_error': 'later unsupported asset',
                'assets': [{'type': 'rawfile', 'name': name, 'data': data.hex(), 'offset': 123}]}

    def test_exact_bytes_and_partial_provenance(self):
        with self.temporary_output() as directory:
            output = Path(directory)
            with patch.object(exporter, 'inspect', return_value=self.report()):
                manifest = exporter.extract(Path('test.ff'), output)
                self.assertEqual((output / 'buttons_default.cfg').read_bytes(), b'bind BUTTON_LTRIG "+speed"\r\n')
                self.assertEqual(manifest['partial_error'], 'later unsupported asset')
                self.assertEqual(exporter.extract(Path('test.ff'), output), manifest)
                (output / 'buttons_default.cfg').write_bytes(b'user modification')
                with self.assertRaises(ValueError):
                    exporter.extract(Path('test.ff'), output)

    def test_invalid_rawfiles_are_rejected(self):
        for report in (self.report('buttons_../bad.cfg'), self.report(data=b'no terminator')):
            with self.temporary_output() as directory, patch.object(exporter, 'inspect', return_value=report):
                with self.assertRaises(ValueError):
                    exporter.extract(Path('test.ff'), Path(directory))

    def test_preserved_real_configs_match_manifest(self):
        root = Path(__file__).resolve().parents[1] / 'consolation/controller_assets/native_xbox/code_post_gfx_mp'
        manifest = json.loads((root / 'source.json').read_text())
        self.assertEqual(len(manifest['files']), 11)
        for entry in manifest['files']:
            self.assertEqual(hashlib.sha256((root / entry['name']).read_bytes()).hexdigest(), entry['sha256'])
        default = (root / 'buttons_default.cfg').read_text()
        self.assertIn('buttons_default_mp.cfg', default)
        self.assertIn('+speed', default)
        self.assertIn('+usereload', default)


if __name__ == '__main__':
    unittest.main()
