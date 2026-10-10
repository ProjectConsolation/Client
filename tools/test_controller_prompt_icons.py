"""Check shipped controller sprite completeness and lossless atlas extraction."""
import json
import tempfile
import unittest
from pathlib import Path
from PIL import Image
from export_controller_prompt_icons import BUTTONS, export

ROOT = Path(__file__).resolve().parents[1]


class ControllerIcons(unittest.TestCase):
    def test_packaged_sprites(self):
        for family in ("xbox", "ps3"):
            for name in BUTTONS.values():
                with self.subTest(family=family, button=name):
                    with Image.open(ROOT / "required_files/consolation/images" /
                                    f"controller_{family}_{name}.png") as sprite:
                        self.assertEqual(sprite.size, (32, 32))
                        self.assertEqual(sprite.mode, "RGBA")
                        self.assertIsNotNone(sprite.getbbox())
                        self.assertEqual(sprite.getextrema()[3][0], 0)

    def test_lossless_and_no_overwrite(self):
        work = ROOT / "tools/.work"
        work.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as directory:
            folder = Path(directory)
            atlas = Image.new("RGBA", (512, 1024), (33, 66, 99, 255))
            atlas.save(folder / "atlas.png")
            glyphs = [{"code": code, "uv": [0, 0, 32/512, 32/1024]}
                      for code in BUTTONS]
            (folder / "map.json").write_text(json.dumps({"fonts": [
                {"font": "fonts/bigFont", "glyphs": glyphs}]}), encoding="utf-8")
            result = export(folder / "atlas.png", folder / "map.json", "xbox", folder / "out")
            self.assertEqual(len(result), 16)
            with Image.open(folder / "out/controller_xbox_a.png") as sprite:
                self.assertEqual(sprite.tobytes(), atlas.crop((0, 0, 32, 32)).tobytes())
            with self.assertRaises(FileExistsError):
                export(folder / "atlas.png", folder / "map.json", "xbox", folder / "out")


if __name__ == "__main__":
    unittest.main()
