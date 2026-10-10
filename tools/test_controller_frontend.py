"""Native controller settings/footer adapter contracts (no client build)."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET
import zlib
from patch_controller_frontend import patch_menu, patch_footer, patch_platform, adapt_master

ROOT = Path(__file__).resolve().parents[1]


class ControllerFrontend(unittest.TestCase):
    def test_existing_settings_preserved_on_console_screen(self):
        source = '''class T {
         _global.menus.cmoptcntlr = new C([new as.shared.datatypes.ChangeSubMenu(null,"CONTROLLER",_global.NORMAL,[oldSetting,{help:"[escaped]"}],{})]);
}'''
        result = patch_menu(source)
        self.assertIn('[oldSetting,{help:"[escaped]"}]', result)
        self.assertIn('"ADVANCED CONTROLLER"', result)
        self.assertIn('[_global.menus.cslcontroller]', result)
        self.assertNotIn('run_game_script', result)

    def test_footer_restores_pc_keys_and_uses_resolved_family(self):
        result = patch_footer('   function initMenu()\n{this.refreshBackCaption();}')
        self.assertIn('ui_controllerIconFamily', result)
        self.assertIn('setImageSubstitutions', result)
        self.assertIn('"ENTER" : "ESCAPE"', result)
        self.assertIn('gpad_in_use', result)
        with self.assertRaises(ValueError): patch_footer(result)

    def test_glyphs_exported_before_first_frame(self):
        tags = '<item type="DefineEditTextTag" characterID="1" initialText="KEY_ENTER"/>'
        tags += '<item type="DefineSpriteTag" spriteId="2"><subTags><item name="txtField" characterId="1"/></subTags></item>'
        tags += '<item type="PlaceObject2Tag" characterId="2"/>' * 4
        tree = ET.ElementTree(ET.fromstring('<swf><tags>' + tags + '<item type="ShowFrameTag"/></tags></swf>'))
        patch_platform(tree, ROOT / 'consolation/controller_assets/glyphs')
        tags = tree.getroot().find('tags')
        exports = tags.find("item[@type='ExportAssetsTag']")
        self.assertEqual(len(exports.find('names')), 32)
        self.assertLess(list(tags).index(exports), next(i for i,t in enumerate(tags) if t.get('type') == 'ShowFrameTag'))

    def test_packaged_console_screens_use_pc_library(self):
        for name in ('cslcontroller', 'cslcontrolleradvanced'):
            data = (ROOT / 'required_files/consolation/scaleform' / (name + '.gfx')).read_bytes()
            body = zlib.decompress(data[8:]) if data[:1] == b'C' else data[8:]
            self.assertIn(b'pcsharedlibrary.swf', body)
            self.assertNotIn(b'cmsharedlibrary.swf', body)
            self.assertIn(b'cslcontrolleradvanced', body)

    def test_nightly_requires_complete_controller_bundle(self):
        packaging = (ROOT / 'tools/package-nightly.ps1').read_text()
        for name in ('CmOptCntlrBtns', 'CmOptCntlrStks', 'cslcontroller', 'cslcontrolleradvanced'):
            self.assertIn('consolation/scaleform/' + name + '.gfx', packaging)
        self.assertIn("$stockMenuFiles + @('consolation/zone/common_consolation.ff')", packaging)


if __name__ == '__main__': unittest.main()
