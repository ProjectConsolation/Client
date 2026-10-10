"""Extend the adapted QoS menu bundle with original controller settings and glyphs.

Requires FFDec 26.3 and the output of patch_controller_diagrams.py. Uses existing
PC menu callbacks, original Xbox timelines, and the same glyph PNGs as the native
common_consolation zone. Never builds the client or installs into the game.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zlib


FOOTER = '''   function refreshControllerPrompts()
   {
      var active = Number(as.shared.FSCommands.get_dvar("gpad_in_use")) == 1;
      var family = as.shared.FSCommands.get_dvar("ui_controllerIconFamily") == "ps3" ? "ps3" : "xbox";
      var mode = active ? family : "pc";
      var fields = [this.ReferenceItem0.cslAcceptKey.txtField,this.ReferenceItem1.cslBackKey.txtField];
      var names = ["a","b"];
      for(var i = 0; i < fields.length; i++)
      {
         var field = fields[i];
         if(field == undefined) { continue; }
         var marker = "[CSL_" + names[i] + "]";
         var label = i == 0 ? "ENTER" : "ESCAPE";
         if(field.cslPromptMode == mode && field.text == (active ? marker : label)) { continue; }
         field.setImageSubstitutions(null);
         if(active)
         {
            var image = flash.display.BitmapData.loadBitmap("csl_controller_" + family + "_" + names[i]);
            if(image == undefined) { continue; }
            field.setImageSubstitutions([{subString:marker,image:image,width:20,height:20}]);
            field.text = marker;
         }
         else { field.text = label; }
         field.cslPromptMode = mode;
      }
   }
   function onEnterFrame()
   {
      if(getTimer() < this.cslPromptNext) { return; }
      this.cslPromptNext = getTimer() + 100;
      this.refreshControllerPrompts();
   }
'''


def patch_footer(text):
    anchor = '   function initMenu()\n'
    if text.count(anchor) != 1 or 'refreshControllerPrompts' in text:
        raise ValueError('Expected unpatched native ReferenceList')
    text = text.replace(anchor, FOOTER + anchor)
    return text.replace('this.refreshBackCaption();',
                        'this.refreshBackCaption();\n      this.refreshControllerPrompts();')


def patch_menu(text):
    """Separate original console settings screen; retain PC keyboard Controls tabs."""
    anchor = 'new as.shared.datatypes.ChangeSubMenu(null,"CONTROLLER",_global.NORMAL,'
    start = text.find(anchor)
    if start < 0 or text.find(anchor, start + 1) >= 0:
        raise ValueError('Expected one PC controller tab')
    array_start = start + len(anchor)
    if text[array_start] != '[': raise ValueError('Expected controller settings array')
    depth, quote, escaped = 0, False, False
    for end in range(array_start, len(text)):
        char = text[end]
        if quote:
            if escaped: escaped = False
            elif char == '\\': escaped = True
            elif char == '"': quote = False
        elif char == '"': quote = True
        elif char == '[': depth += 1
        elif char == ']':
            depth -= 1
            if depth == 0: break
    else: raise ValueError('Unterminated controller settings')
    settings = text[array_start:end + 1]
    # Keep old settings available, while the top-level console screen stays compact.
    definition = '''      _global.menus.cslcontrolleradvanced = new as.shared.datatypes.OptionsScreen("cslcontrolleradvanced","ADVANCED CONTROLLER",_global.NORMAL,SETTINGS,{});
      _global.menus.cslcontroller = new as.shared.datatypes.OptionsScreen("cslcontroller","CONTROLLER",_global.NORMAL,[
         _global.menus.cmoptcntlrbtns,_global.menus.cmoptcntlrstks,
         new as.shared.datatypes.Option(null,"LOOK SENSITIVITY",_global.NORMAL,["1","2","3","4","5","6","7","8","9","10"],[1,2,3,4,5,6,7,8,9,10],{onCloseSetDvar:"gpad_sensitivity",toggle:1,helpText:"Controller look sensitivity."}),
         new as.shared.datatypes.Option(null,"INVERT LOOK",_global.NORMAL,["NO","YES"],[0,1],{onCloseSetDvar:"input_invertPitch",toggle:1,helpText:"Invert controller pitch only."}),
         new as.shared.datatypes.Option(null,"CONTROLLER ICONS",_global.NORMAL,["AUTO","XBOX","PS3"],["auto","xbox","ps3"],{onCloseSetDvar:"ui_controllerIcons",toggle:1,helpText:"AUTO follows the active controller. Override artwork without changing bindings."}),
         _global.menus.cslcontrolleradvanced],{helpText:"Original QoS controller layouts and settings."});
'''.replace('SETTINGS', settings)
    text = text[:array_start] + '[_global.menus.cslcontroller]' + text[end + 1:]
    init = '         _global.menus.cmoptcntlr = '
    if init not in text: raise ValueError('Missing PC Controls definition')
    return text.replace(init, definition + init, 1)


def patch_platform(tree, glyphs):
    from PIL import Image
    tags = tree.getroot().find('tags')
    texts = {tag.get('characterID'): ('cslAcceptKey' if 'KEY_ENTER' in tag.get('initialText', '') else 'cslBackKey')
             for tag in tags if tag.get('type') == 'DefineEditTextTag'
             and any(key in tag.get('initialText', '') for key in ('KEY_ENTER', 'KEY_ESCAPE'))}
    sprites = {tag.get('spriteId'): texts[child.get('characterId')]
               for tag in tags if tag.get('type') == 'DefineSpriteTag'
               for child in tag.findall("./subTags/item[@name='txtField']")
               if child.get('characterId') in texts}
    named = 0
    for tag in tags.iter('item'):
        if tag.get('type') == 'PlaceObject2Tag' and tag.get('characterId') in sprites:
            tag.set('name', sprites[tag.get('characterId')])
            tag.set('placeFlagHasName', 'true')
            named += 1
    if named < 4: raise ValueError('Missing native keyboard footer sprites')
    exports = ET.Element('item', type='ExportAssetsTag', forceWriteAsLong='true')
    ids, names = ET.SubElement(exports, 'tags'), ET.SubElement(exports, 'names')
    insertion = next(i for i, tag in enumerate(tags) if tag.get('type') == 'ShowFrameTag')
    for index, source in enumerate(sorted(glyphs.glob('controller_*.png'))):
        image = Image.open(source).convert('RGBA')
        raw = bytearray()
        for r, g, b, a in image.getdata():
            raw.extend((a, (r*a+127)//255, (g*a+127)//255, (b*a+127)//255))
        cid = 30000 + index
        if any(tag.get('characterID') == str(cid) for tag in tags):
            raise ValueError('Glyph character ID occupied')
        bitmap = ET.Element('item', type='DefineBitsLossless2Tag', characterID=str(cid),
                            bitmapFormat='5', bitmapWidth=str(image.width), bitmapHeight=str(image.height),
                            forceWriteAsLong='true', zlibBitmapData=zlib.compress(raw).hex())
        tags.insert(insertion, bitmap)
        insertion += 1
        ET.SubElement(ids, 'item').text = str(cid)
        ET.SubElement(names, 'item').text = 'csl_' + source.stem
    if len(ids) != 32: raise ValueError('Expected complete Xbox and PS3 glyph families')
    tags.insert(insertion, exports)
    return named


def adapt_master(tree, name):
    tags = tree.getroot().find('tags')
    tags.find("item[@type='ExporterInfo']").set('swfName', name)
    for item in tags.findall("item[@type='ImportAssets2Tag']"):
        if item.get('url') == 'cmsharedlibrary.swf': item.set('url', 'pcsharedlibrary.swf')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ffdec', type=Path, required=True)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--xbox-master', type=Path, required=True)
    parser.add_argument('--glyphs', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists(): parser.error('Output must be a new directory')
    args.output.mkdir(parents=True)
    def run(*argv):
        result = subprocess.run([str(args.ffdec.resolve()), *map(str, argv)],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if result.returncode: raise RuntimeError(result.stdout)
    with tempfile.TemporaryDirectory(prefix='controller-frontend-', dir=args.output.parent) as directory:
        work = Path(directory)
        menu_source = None
        for source in args.bundle.glob('*.gfx'):
            if source.stem.lower() in ('cmoptcntlrbtns', 'cmoptcntlrstks'):
                (args.output / source.name).write_bytes(source.read_bytes())
                continue
            export = work / source.stem
            run('-export', 'script', export, source)
            root = export / 'scripts/__Packages'
            menu = patch_menu((root / 'as/frontend/menuData.as').read_text(encoding='utf-8'))
            menu_source = menu
            patch = work / (source.stem + '-patch')
            target = patch / '__Packages/as/frontend/menuData.as'
            target.parent.mkdir(parents=True)
            target.write_text(menu, encoding='utf-8')
            if source.stem.lower() == 'cmsharedplatform':
                target = patch / '__Packages/as/shared/ReferenceList.as'
                target.parent.mkdir(parents=True)
                target.write_text(patch_footer((root / 'as/shared/ReferenceList.as').read_text(encoding='utf-8')), encoding='utf-8')
            compiled = work / (source.stem + '-compiled.gfx')
            run('-onerror', 'abort', '-importScript', source, compiled, patch)
            if source.stem.lower() == 'cmsharedplatform':
                xml = work / 'platform.xml'
                run('-swf2xml', compiled, xml)
                tree = ET.parse(xml)
                patch_platform(tree, args.glyphs)
                tree.write(xml, encoding='utf-8', xml_declaration=True)
                run('-xml2swf', xml, args.output / source.name)
            else: (args.output / source.name).write_bytes(compiled.read_bytes())
        # Original console OptionsScreen timeline + current PC menu definitions.
        for name in ('cslcontroller', 'cslcontrolleradvanced'):
            xml = work / (name + '.xml')
            run('-swf2xml', args.xbox_master, xml)
            tree = ET.parse(xml)
            adapt_master(tree, name)
            tree.write(xml, encoding='utf-8', xml_declaration=True)
            raw = work / (name + '.gfx')
            run('-xml2swf', xml, raw)
            patch = work / (name + '-patch')
            target = patch / '__Packages/as/frontend/menuData.as'
            target.parent.mkdir(parents=True)
            target.write_text(menu_source, encoding='utf-8')
            run('-onerror', 'abort', '-importScript', raw, args.output / (name + '.gfx'), patch)
    print('Patched 13 native menu movies, controller screens and 32 embedded glyphs')


if __name__ == '__main__': main()
