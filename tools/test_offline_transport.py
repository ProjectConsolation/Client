"""Standalone x86 tests; does not build either game DLL."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class OfflineTransport(unittest.TestCase):
    def test_abi_and_opt_in_contract(self):
        source = (ROOT / 'src_xlive/xlive_shim.cpp').read_text()
        self.assertNotIn('if (!offline_test_requested()) return recvfrom', source)
        self.assertIn('if (discovery) memcpy(&game_port, sender.data() + 8', source)
        self.assertIn('if (offline_test_requested()) return; // Never import', source)
        self.assertIn('root / "debug-net" / offline_test_run_id()', source)
        self.assertIn('valid_recv_address_length(fromlen)', source)
        self.assertIn('protocol == xnet_protocol_vdp', source)
        self.assertIn('if (const auto bound = offline_bound_port.load()) port = bound;', source)
        exports = (ROOT / 'src_xlive/xlive.def').read_text()
        self.assertIn('xlive_XNetInAddrToXnAddr @60 NONAME', exports)
        self.assertIn('ConsolationOfflineNetStatus @6001', exports)
        self.assertIn('ConsolationOfflineTransportVersion @6002', exports)
        command = (ROOT / 'src/component/engine/console/command.cpp').read_text()
        self.assertIn('lower.starts_with("offline_test_port=")', command)
        connect = (ROOT / 'src/component/engine/console/direct_connect.cpp').read_text()
        self.assertIn('address.data()) + 8, &source.sin_port', connect)
        self.assertIn('std::string(2,', connect)
        self.assertIn('offline_transport::decode', connect)
        self.assertIn('dedicated->latched.integer = 1', connect)
        self.assertIn('0x102F7084', connect)
        self.assertIn('lower == "dedicated"', command)
        self.assertIn('scheduler::once(forward_launch_commands, scheduler::main)', command)
        self.assertNotIn('0x115F60BC)) =', connect)
        self.assertIn('utils::flags::has_flag("dedicated")', (ROOT / 'src/component/utils/scheduler.cpp').read_text())

    @unittest.skipUnless(os.environ.get('QOS_X86_VCVARS'), 'Select x86 MSVC for native unit test')
    def test_native_transport(self):
        work = ROOT / 'tools/.work'
        work.mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix='offline-transport-', dir=work) as directory:
            folder = Path(directory)
            command = (f'call "{os.environ["QOS_X86_VCVARS"]}" >nul && cl /nologo /EHsc '
                       f'/std:c++20 /W4 /WX /O2 "{ROOT / "tools/test_offline_transport.cpp"}" '
                       '/Fe:probe.exe /Fo:probe.obj')
            result = subprocess.run(f'cmd.exe /d /s /c "{command}"', cwd=folder, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            subprocess.run([str(folder / 'probe.exe')], cwd=folder, check=True)


if __name__ == '__main__':
    unittest.main()
