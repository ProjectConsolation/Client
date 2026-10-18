using System.Diagnostics;
using System.IO;
using System.Reflection.PortableExecutable;
using System.Text;

namespace Consolation.Launcher;

internal static class InstallationInfo
{
    // Same disk-image compatibility marker as src/main.cpp. This does not
    // authenticate a game installation or establish its language/region.
    internal const int MultiplayerMarkerRva = 0x0050502C;

    internal static bool IsSupported(Stream stream)
    {
        try
        {
            using var pe = new PEReader(stream, PEStreamOptions.LeaveOpen);
            if (pe.PEHeaders.CoffHeader.Machine != Machine.I386 ||
                pe.PEHeaders.PEHeader?.Magic != PEMagic.PE32) return false;
            var bytes = pe.GetSectionData(MultiplayerMarkerRva).GetContent(0, 12);
            return Encoding.ASCII.GetString(bytes.AsSpan()).Equals("multiplayer\0", StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception error) when (error is BadImageFormatException or IOException or ArgumentException)
        { return false; }
    }

    internal static void RequireSupported(string directory)
    {
        using var stream = File.OpenRead(Path.Combine(directory, "jb_mp_s.dll"));
        if (!IsSupported(stream)) throw new InvalidOperationException(
            "Unsupported or damaged multiplayer game binary. Install the supported English/French 1.1 patch before playing. " +
            "Repair client files cannot replace the original jb_mp_s.dll.");
    }

    internal static string ClientBuild(string directory)
    {
        var path = Path.Combine(directory, "d3d9.dll");
        if (!File.Exists(path)) return "Client not installed";
        var version = FileVersionInfo.GetVersionInfo(path);
        var label = version.ProductVersion ?? version.FileVersion;
        return "Installed client: " + (string.IsNullOrWhiteSpace(label) ? "version unavailable" : label) +
            (version.IsDebug ? " · debug" : "");
    }
}
