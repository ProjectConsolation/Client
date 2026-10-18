using System.IO;
using System.Text.Json;

namespace Consolation.Launcher;

public sealed record LauncherSettings
{
    public string GameDirectory { get; init; } = "";
    public bool CheckUpdatesBeforePlay { get; init; } = true;
    public bool SkipIntro { get; init; } = true;
    public bool DebugAssets { get; init; }
    public bool TraceActions { get; init; }
    public string PlayerName { get; init; } = "";
    public double? WindowLeft { get; init; }
    public double? WindowTop { get; init; }
    public double WindowWidth { get; init; } = 1080;
    public double WindowHeight { get; init; } = 700;
    public bool Maximized { get; init; }
}

internal static class SettingsStore
{
    private static readonly string FilePath = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "ProjectConsolation", "launcher.json");

    public static LauncherSettings Load()
    {
        if (!File.Exists(FilePath)) return new();
        return JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(FilePath))
            ?? throw new InvalidDataException("The launcher settings file is empty.");
    }

    public static void Save(LauncherSettings settings)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(FilePath)!);
        var temporary = FilePath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, JsonSerializer.Serialize(settings,
                new JsonSerializerOptions { WriteIndented = true }));
            File.Move(temporary, FilePath, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }
}
