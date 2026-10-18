using System.ComponentModel;
using System.Diagnostics;

namespace Consolation.Launcher;

internal enum SteamActivity { NotRunning, Running, Unknown }

internal static class SteamActivityProbe
{
    public static SteamActivity Read()
    {
        // Read-only, current-session process check; do not initialize the
        // Steam SDK in a launcher (it can register the launcher as the game).
        // Running is a conservative name lock, NOT proof of BLoggedOn or of
        // successful game SDK initialization. The client remains authoritative.
        try
        {
            using var current = Process.GetCurrentProcess();
            var session = current.SessionId;
            var processes = Process.GetProcessesByName("steam");
            try
            {
                return processes.Any(process => !process.HasExited && process.SessionId == session)
                    ? SteamActivity.Running : SteamActivity.NotRunning;
            }
            finally { foreach (var process in processes) process.Dispose(); }
        }
        catch (Exception error) when (error is Win32Exception or InvalidOperationException
            or UnauthorizedAccessException or System.Security.SecurityException or NotSupportedException)
        {
            return SteamActivity.Unknown; // Fail closed for name overrides.
        }
    }
}
