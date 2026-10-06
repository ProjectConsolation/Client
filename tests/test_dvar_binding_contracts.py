"""Static ABI regressions for QoS PC 1.1; does not build or run the client.

Addresses/registers are checked against the jb_mp_s.dll IDA implementation.
Run with: python -m unittest discover -s tests -p test_dvar_binding_contracts.py
"""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]


def source(path):
    return (ROOT / path).read_text(encoding="utf-8")


def function_body(text, signature):
    start = text.index("{", text.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]


class DvarBindingContracts(unittest.TestCase):
    def test_lookup_uses_locked_native_cdecl_entry(self):
        game = function_body(source("src/game/game.cpp"), "dvar_s* Dvar_FindMalleableVar(")
        shim = function_body(source("src_xlive/xlive_shim.cpp"), "dvar_s* find_dvar(")
        self.assertIn("0x10276040", game)
        self.assertIn("__cdecl", game)
        self.assertIn("__cdecl", shim)
        self.assertIn("find_var(name)", shim)
        self.assertNotIn("generateHashValue", game)
        self.assertNotIn("generate_hash_value", shim)
        self.assertNotIn("symbol<int(char* dvar)> generateHashValue", source("src/game/symbols.hpp"))

    def test_variant_bridge_stack_layout(self):
        body = function_body(source("src/game/game.cpp"), "dvar_s* Dvar_RegisterVariant(")
        asm = body[body.index("__asm"):]
        pushes = re.findall(r"^\s*push (.+)$", asm, re.MULTILINE)
        self.assertEqual(pushes, [
            "unknown", "description", "dword ptr [eax + 4]", "dword ptr [eax]",
            "dword ptr [eax + 12]", "dword ptr [eax + 8]",
            "dword ptr [eax + 4]", "dword ptr [eax]", "native_flags", "native_type",
        ])
        self.assertIn("0x10278960", body)
        self.assertIn("mov edi, name", asm)
        self.assertIn("add esp, 40", asm)
        self.assertIn("sizeof(DvarValue) == 16", body)
        self.assertIn("sizeof(DvarLimits) == 8", body)

    def test_typed_registration_never_calls_allocate_only_entry(self):
        text = source("src/game/dvars.cpp")
        for kind in ("Float", "Vec4", "Bool", "Int", "String"):
            body = function_body(text, "game::dvar_s* Dvar_Register" + kind + "(")
            self.assertIn("game::Dvar_RegisterVariant(", body)
            self.assertNotIn("game::Dvar_RegisterNew(", body)
        self.assertNotIn('game::Dvar_RegisterNew("pm_movement_mode"', source("src/component/engine/patches/patches.cpp"))

    def test_timer_is_update_only(self):
        text = source("src/game/dvars.cpp")
        refresh = function_body(text, "void refresh_existing_override(")
        self.assertIn("replace_dvar(spec, false, false)", refresh)
        replace = function_body(text, "game::dvar_s* replace_dvar(")
        self.assertIn("if (!allow_registration)", replace)
        self.assertIn("allow_registration ? register_dvar(spec, log) : nullptr", replace)
        loop = text[text.index("scheduler::loop([]", text.index('"cg_overheadNamesSize"')):]
        loop = loop[:loop.index("250ms")]
        self.assertEqual(loop.count("refresh_existing_override("), 4)
        self.assertNotIn("replace_dvar(", loop)

    def test_shim_name_setter_is_rebased(self):
        body = function_body(source("src_xlive/xlive_shim.cpp"), "void sync_engine_name(")
        self.assertIn("qos_address<void*>(0x10278FD0)", body)
        self.assertNotIn("qos_base +", body)
        self.assertIn("mov eax, dvar_name", body)
        self.assertIn("mov edi, value", body)

    def test_boolean_adaptation_is_scoped_to_native_camera_write(self):
        body = function_body(source("src/component/engine/patches/patches.cpp"), "const char* __cdecl dvar_setvariant_stub(")
        self.assertIn("_ReturnAddress()", body)
        for token in ("0x10274C05", "0x104CAD94", "0x104CAD98", '"cg_thirdPerson"', "game::DVAR_TYPE_INT"):
            self.assertIn(token, body)
        self.assertIn("value.string == zero || value.string == one", body)
        self.assertIn("value.integer = enabled", body)
        self.assertIn("dvar_setvariant_hook.invoke<const char*>(dvar, value, source)", body)


if __name__ == "__main__":
    unittest.main()
