using System.Diagnostics;
using System.IO;

namespace Consolation.Launcher;

internal static class GameInstallation
{
    public const string Executable = "JB_Launcher_s.exe";

    public static readonly string[] RequiredFiles =
    [
        Executable,
        "jb_mp_s.dll",
        "d3d9.dll",
        "xlive.dll",
        "consolation/zone/common_consolation.ff"
    ];

    public static string Discover(string savedDirectory)
    {
        // Prefer an explicit saved selection, then a launcher beside the game.
        // Never use the process working directory (e.g. a shortcut's Start In).
        if (!string.IsNullOrWhiteSpace(savedDirectory))
        {
            if (!Path.IsPathFullyQualified(savedDirectory))
                throw new InvalidDataException("The saved game directory is not an absolute path.");
            return Path.GetFullPath(savedDirectory);
        }
        foreach (var candidate in new[]
        {
            AppContext.BaseDirectory,
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86),
                "Activision", "Quantum of Solace(TM)")
        })
        {
            if (File.Exists(Path.Combine(candidate, Executable))) return Path.GetFullPath(candidate);
        }
        return "";
    }

    public static string Normalize(string directory)
    {
        if (string.IsNullOrWhiteSpace(directory))
            throw new InvalidOperationException("Choose your game directory in Settings first.");
        if (!Path.IsPathFullyQualified(directory))
            throw new InvalidOperationException("Choose an absolute game-directory path.");
        var fullPath = Path.GetFullPath(directory);
        if (!Directory.Exists(fullPath) || string.Equals(Path.TrimEndingDirectorySeparator(fullPath),
            Path.TrimEndingDirectorySeparator(Path.GetPathRoot(fullPath)!), StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException("Select the game installation folder, not a drive root.");
        return fullPath;
    }

    public static string[] MissingFiles(string directory) => RequiredFiles
        .Where(file => !File.Exists(Path.Combine(directory, file))).ToArray();

    public static void RequireGameClosed()
    {
        foreach (var name in new[] { "JB_Launcher_s", "JB_LiveEngine_s" })
        {
            var processes = Process.GetProcessesByName(name);
            try
            {
                if (processes.Length != 0)
                    throw new InvalidOperationException("Close Quantum of Solace before launching or updating.");
            }
            finally
            {
                foreach (var process in processes) process.Dispose();
            }
        }
    }

    public static ProcessStartInfo CreateLaunch(string directory, LauncherSettings settings)
    {
        directory = Normalize(directory);
        var missing = MissingFiles(directory);
        if (missing.Length != 0)
            throw new InvalidOperationException("The installation is missing:\n\n" +
                string.Join("\n", missing) + "\n\nInstall the complete Consolation package in the game folder.");
        InstallationInfo.RequireSupported(directory);
        var start = new ProcessStartInfo(Path.Combine(directory, Executable))
        {
            WorkingDirectory = directory,
            UseShellExecute = false
        };
        // Fresh detection after any updater delay, not a cached UI value.
        foreach (var argument in LaunchOptions.Arguments(settings, SteamActivityProbe.Read()))
            start.ArgumentList.Add(argument);
        return start;
    }

    public static ProcessStartInfo CreateUpdate(string directory, bool repair = false, string? cancelEvent = null, string? restoreBackup = null)
    {
        directory = Normalize(directory);
        if (!File.Exists(Path.Combine(directory, Executable)) &&
            !(repair && File.Exists(Path.Combine(directory, "jb_mp_s.dll"))))
            throw new InvalidOperationException("JB_Launcher_s.exe was not found in the selected folder.");
        // Always use the matching launcher-bundled script. An old/missing game
        // copy may not understand repair or the progress protocol.
        var updater = Path.Combine(AppContext.BaseDirectory, "Support", "Check Consolation Updates.ps1");
        if (!File.Exists(updater))
            throw new FileNotFoundException("The bundled updater is missing. Install the complete client package.", updater);
        // Reuse the existing verified-package and rollback implementation,
        // including its debug-build protection and user confirmation dialog.
        var start = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(
            Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"))
        {
            WorkingDirectory = directory,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true
        };
        foreach (var argument in new[]
        {
            "-NoProfile", "-STA", "-ExecutionPolicy", "Bypass",
            "-File", updater, "-GameDirectory", directory
        }) start.ArgumentList.Add(argument);
        start.ArgumentList.Add("-ProgressOutput");
        if (repair) start.ArgumentList.Add("-Repair");
        if (cancelEvent is not null) { start.ArgumentList.Add("-CancelEventName"); start.ArgumentList.Add(cancelEvent); }
        if (restoreBackup is not null) { start.ArgumentList.Add("-RestoreBackup"); start.ArgumentList.Add(restoreBackup); }
        return start;
    }
}
