namespace Consolation.Launcher;

internal static class LaunchOptions
{
    public static string ValidatePlayerName(string? value)
    {
        var name = (value ?? "").Trim();
        // QoS caches 15 bytes + NUL; the shim's command-line reader uses
        // CP_ACP. Restrict this input to printable ASCII for a lossless handoff.
        if (name.Length > 15 || name.Any(ch => ch < 32 || ch > 126 || "\"\\;^+".Contains(ch))
            || name.StartsWith('-'))
            throw new ArgumentException("Use up to 15 ASCII characters for the offline name. " +
                "Quotes, backslashes, semicolons, colour codes, plus signs and leading dashes are not allowed.");
        return name;
    }

    public static IReadOnlyList<string> Arguments(LauncherSettings settings, SteamActivity steam)
    {
        var result = new List<string> { "-multiplayer", "-seta", "g_gametype", "dm" };
        void Set(string key, string value) => result.AddRange(["-set", key, value]);
        Set("cin_firstRunDone", settings.SkipIntro ? "1" : "0");
        Set("cin_skipAllMovies", settings.SkipIntro ? "1" : "0");
        if (!settings.CheckUpdatesBeforePlay) result.Add("-noupdate");
        if (settings.DebugAssets) result.Add("-debug_xasset");
        if (settings.TraceActions) result.Add("-scaleform-actions");
        // A disabled textbox is not a security/behavior boundary. Reapply the
        // policy here so a saved name is never sent while Steam is detected.
        if (steam == SteamActivity.NotRunning)
        {
            var name = ValidatePlayerName(settings.PlayerName);
            if (name.Length != 0) Set("name", name);
        }
        return result;
    }
}
