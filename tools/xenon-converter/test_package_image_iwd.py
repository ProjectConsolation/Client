from pathlib import Path
import tempfile
import unittest
import zipfile

from package_image_iwd import package_images


class ImageIwdTests(unittest.TestCase):
    def test_paths_bytes_and_exclusive_creation(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            source = root / "iwi"
            (source / "custom").mkdir(parents=True)
            payload = b"IWi\x06" + bytes(28)
            (source / "custom/brick.iwi").write_bytes(payload)
            (source / "%2Alightmap.iwi").write_bytes(payload)
            (source / "manifest.json").write_text("{}")
            output = root / "csl_canals.iwd"
            self.assertEqual(package_images(source, output), 2)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(set(archive.namelist()), {"images/custom/brick.iwi", "images/%2Alightmap.iwi"})
                self.assertEqual(archive.read("images/custom/brick.iwi"), payload)
                self.assertIsNone(archive.testzip())
            with self.assertRaises(FileExistsError):
                package_images(source, output)

    def test_empty_directory_is_rejected_before_writing(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp) / "empty.iwd"
            with self.assertRaises(ValueError):
                package_images(temp, output)
            self.assertFalse(output.exists())

    def test_windows_device_path_is_rejected(self):
        from dump_pc_images import safe_name
        self.assertFalse(safe_name("nested/CON.iwi"))
        self.assertFalse(safe_name("../image.iwi"))


if __name__ == "__main__":
    unittest.main()
