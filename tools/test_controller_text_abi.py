"""Regression-test the actual prompt-width adapter without building the client.

Set QOS_X86_VCVARS to an installed vcvars32.bat to run the x86 ABI probe.
The probe extracts the production stub, calls a deliberately SSE-clobbering
stand-in, and checks all eight registers, stack balance, arguments and EAX.
"""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/component/gamepad/controller/engine/icons.cpp"


def width_stub():
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index("__declspec(naked) void text_width_stub()")
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class TextABI(unittest.TestCase):
    def test_installed_adapter_and_localization(self):
        source = SOURCE.read_text(encoding="utf-8")
        self.assertIn("0x1037CFA0), text_width_stub)", source)
        self.assertIn("0x103C9F20), localized_key)", source)
        self.assertIn("text && text_icon::is_binding_token(text)", source)
        self.assertIn("localized_key_hook.invoke<const char*>(text)", source)

    def test_save_restore_contract(self):
        stub = width_stub()
        for i in range(8):
            address = "[esp]" if i == 0 else f"[esp + {i * 16}]"
            self.assertIn(f"movups {address}, xmm{i}", stub)
            self.assertIn(f"movups xmm{i}, {address}", stub)
        self.assertIn("push [ebp + 16]\n        push [ebp + 12]\n        push [ebp + 8]", stub)
        self.assertIn("call text_width\n        add esp, 12", stub)
        self.assertIn("mov esp, ebp\n        pop ebp\n        ret", stub)

    @unittest.skipUnless(os.environ.get("QOS_X86_VCVARS"), "x86 MSVC environment not selected")
    def test_native_x86_call(self):
        clobber = "\n".join(f"xorps xmm{i}, xmm{i}" for i in range(8))
        load = "\n".join(f"movups xmm{i}, [ecx + {i * 16}]" for i in range(8))
        save = "\n".join(f"movups [ecx + {i * 16}], xmm{i}" for i in range(8))
        harness = r'''
#include <cstring>
#include <cstdio>
int __cdecl text_width(const char* text, int max_chars, void* font)
{
    int result = text == reinterpret_cast<const char*>(0x12345)
        && max_chars == 47 && font == reinterpret_cast<void*>(0x42) ? 123 : -1;
    __asm { CLOBBER }
    return result;
}
STUB
int main()
{
    unsigned expected[32], actual[32] = {};
    for (unsigned i = 0; i < 32; ++i) expected[i] = 0x3f000000 + i * 101;
    int before, after, result;
    __asm {
        lea ecx, expected
        LOAD
        mov before, esp
        push 42h
        push 47
        push 12345h
        call text_width_stub
        add esp, 12
        mov result, eax
        mov after, esp
        lea ecx, actual
        SAVE
    }
    if (before != after || result != 123 || std::memcmp(expected, actual, 128)) return 1;
    std::puts("Prompt-width x86 ABI probe passed");
}
'''.replace("CLOBBER", clobber).replace("STUB", width_stub()).replace("LOAD", load).replace("SAVE", save)
        work = ROOT / "tools/.work"
        work.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="text-abi-", dir=work) as directory:
            folder = Path(directory)
            (folder / "probe.cpp").write_text(harness, encoding="utf-8")
            vcvars = os.environ["QOS_X86_VCVARS"]
            for optimization in ("/Od", "/O2"):
                command = f'call "{vcvars}" >nul && cl /nologo /EHsc {optimization} probe.cpp /Fe:probe.exe'
                built = subprocess.run(f'cmd.exe /d /s /c "{command}"',
                                       cwd=folder, capture_output=True, text=True)
                self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
                subprocess.run([str(folder / "probe.exe")], cwd=folder, check=True)
            command = (f'call "{vcvars}" >nul && cl /nologo /EHsc /std:c++17 '
                       f'"{ROOT / "tools/test_controller_icon_tokens.cpp"}" /Fe:tokens.exe /Fo:tokens.obj')
            built = subprocess.run(f'cmd.exe /d /s /c "{command}"',
                                   cwd=folder, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            subprocess.run([str(folder / "tokens.exe")], cwd=folder, check=True)


if __name__ == "__main__":
    unittest.main()
