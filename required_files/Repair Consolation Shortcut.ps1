param(
    [string]$ShortcutPath = (Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\Launch Consolation.lnk'),
    [string]$GameDirectory = 'C:\Program Files (x86)\Activision\Quantum of Solace(TM)'
)
$ErrorActionPreference = 'Stop'
# Set the Shell identity on the shortcut as well as the client window.
# Existing launch arguments (including offline identity) are preserved.
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ConsolationShortcutIdentity {
    [StructLayout(LayoutKind.Sequential)] public struct Key { public Guid format; public uint id; }
    [StructLayout(LayoutKind.Explicit, Size=24)] public struct Value {
        [FieldOffset(0)] public ushort type;
        [FieldOffset(8)] public IntPtr text;
    }
    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface Store {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int GetAt(uint index, out Key key);
        [PreserveSig] int GetValue(ref Key key, out Value value);
        [PreserveSig] int SetValue(ref Key key, ref Value value);
        [PreserveSig] int Commit();
    }
    [DllImport("shell32.dll", CharSet=CharSet.Unicode, PreserveSig=true)]
    static extern int SHGetPropertyStoreFromParsingName(string path, IntPtr bind, uint flags, ref Guid iid, out Store store);
    public static void Set(string path) {
        var iid = new Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99");
        Store store;
        Marshal.ThrowExceptionForHR(SHGetPropertyStoreFromParsingName(path, IntPtr.Zero, 2, ref iid, out store));
        var key = new Key { format = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3"), id = 5 };
        var value = new Value { type = 31, text = Marshal.StringToCoTaskMemUni("ProjectConsolation.Client.Multiplayer") };
        try {
            Marshal.ThrowExceptionForHR(store.SetValue(ref key, ref value));
            Marshal.ThrowExceptionForHR(store.Commit());
        } finally { Marshal.FreeCoTaskMem(value.text); Marshal.ReleaseComObject(store); }
    }
}
'@
if (-not (Test-Path -LiteralPath (Join-Path $GameDirectory 'icon.ico'))) { throw 'Install icon.ico in the game directory first.' }
$shell = New-Object -ComObject WScript.Shell
$exists = Test-Path -LiteralPath $ShortcutPath
if ($exists) {
    $backup = $ShortcutPath + '.before-consolation-' + [Guid]::NewGuid().ToString('N') + '.bak'
    Copy-Item -LiteralPath $ShortcutPath -Destination $backup
}
$shortcut = $shell.CreateShortcut($ShortcutPath)
if (-not $exists) {
    $shortcut.TargetPath = Join-Path $GameDirectory 'JB_Launcher_s.exe'
    $shortcut.Arguments = '-multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1'
    $shortcut.WorkingDirectory = $GameDirectory
}
$shortcut.IconLocation = (Join-Path $GameDirectory 'icon.ico') + ',0'
$shortcut.Save()
[ConsolationShortcutIdentity]::Set($ShortcutPath)
Write-Output "Updated shortcut icon and application identity: $ShortcutPath"
if ($exists) { Write-Output "Rollback copy: $backup" }
Write-Output 'If Explorer retains the old cached icon, unpin and pin this shortcut again. Launch arguments were preserved.'
