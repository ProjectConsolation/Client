import tempfile
from pathlib import Path
import unittest
import package_controller_resources as bundle


class ControllerResources(unittest.TestCase):
    def test_roundtrip_preserves_art_and_embeds_exact_resources(self):
        with tempfile.TemporaryDirectory(dir=bundle.ROOT / 'tools/.work') as directory:
            output = Path(directory) / 'common.ff'
            bundle.package(bundle.ROOT / 'required_files/consolation/zone/common_consolation.ff', output)
            payload = bundle.common.x._read_pc_zone_payload(output)
            _, _, entries, images, files = bundle.split(payload)
            self.assertEqual(len(images), 67)
            self.assertEqual(len(entries) + len(files), 93)
            self.assertEqual(files, bundle.resources())
            again = Path(directory) / 'again.ff'
            bundle.package(output, again)
            self.assertEqual(output.read_bytes(), again.read_bytes())
            with self.assertRaises(ValueError):
                bundle.package(output, again)


if __name__ == '__main__':
    unittest.main()
