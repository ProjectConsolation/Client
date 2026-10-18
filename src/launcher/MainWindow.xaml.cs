using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Text;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Threading;
using Microsoft.Win32;

namespace Consolation.Launcher;

public partial class MainWindow : Window
{
    private bool busy;
    private bool closingAfterLaunch;
    private bool rollbackRequired;
    private bool syncingUpdatePreference;
    private bool preferencesLoaded;
    private EventWaitHandle? updateCancellation;
    private bool wasMaximized;
    private sealed record BackupChoice(string Id, string Label);
    private readonly DispatcherTimer steamRefresh = new() { Interval = TimeSpan.FromSeconds(3) };

    public MainWindow()
    {
        InitializeComponent();
        StateChanged += (_, _) => { if (WindowState != WindowState.Minimized) wasMaximized = WindowState == WindowState.Maximized; };
        PlayNavigation.IsChecked = true;
        try
        {
            var settings = SettingsStore.Load();
            GameDirectoryBox.Text = GameInstallation.Discover(settings.GameDirectory);
            CheckUpdatesBox.IsChecked = settings.CheckUpdatesBeforePlay;
            SkipIntroBox.IsChecked = settings.SkipIntro;
            DebugAssetsBox.IsChecked = settings.DebugAssets;
            TraceActionsBox.IsChecked = settings.TraceActions;
            PlayerNameBox.Text = settings.PlayerName ?? "";
            RestoreGeometry(settings);
            RefreshInstallation();
            RefreshBackups();
        }
        catch (Exception error) when (IsExpected(error))
        {
            SetStatus("Could not load settings. Select your game folder and save again. " + error.Message);
        }
        preferencesLoaded = true;
        steamRefresh.Tick += (_, _) => RefreshNameLock();
        Activated += (_, _) => RefreshNameLock();
        Closed += (_, _) => steamRefresh.Stop();
        RefreshNameLock();
        steamRefresh.Start();
    }

    private static bool IsExpected(Exception error) => error is IOException or UnauthorizedAccessException
        or InvalidOperationException or ArgumentException or NotSupportedException or Win32Exception or JsonException;

    private LauncherSettings ReadSettings() => new()
    {
        GameDirectory = GameDirectoryBox.Text,
        CheckUpdatesBeforePlay = CheckUpdatesBox.IsChecked == true,
        SkipIntro = SkipIntroBox.IsChecked == true,
        DebugAssets = DebugAssetsBox.IsChecked == true,
        TraceActions = TraceActionsBox.IsChecked == true,
        PlayerName = PlayerNameBox.Text.Trim(),
        WindowLeft = RestoreBounds.IsEmpty ? Left : RestoreBounds.Left,
        WindowTop = RestoreBounds.IsEmpty ? Top : RestoreBounds.Top,
        WindowWidth = RestoreBounds.IsEmpty ? Width : RestoreBounds.Width,
        WindowHeight = RestoreBounds.IsEmpty ? Height : RestoreBounds.Height,
        Maximized = WindowState == WindowState.Minimized ? wasMaximized : WindowState == WindowState.Maximized
    };

    private void RestoreGeometry(LauncherSettings settings)
    {
        // Clamp to the current primary work area so unplugged monitors or DPI
        // changes cannot strand the borderless title bar offscreen.
        var area = SystemParameters.WorkArea;
        Width = double.IsFinite(settings.WindowWidth) ? Math.Clamp(settings.WindowWidth, Math.Min(MinWidth, area.Width), area.Width) : 1080;
        Height = double.IsFinite(settings.WindowHeight) ? Math.Clamp(settings.WindowHeight, Math.Min(MinHeight, area.Height), area.Height) : 700;
        if (settings.WindowLeft is double left && settings.WindowTop is double top && double.IsFinite(left) && double.IsFinite(top))
        {
            WindowStartupLocation = WindowStartupLocation.Manual;
            Left = Math.Clamp(left, area.Left, Math.Max(area.Left, area.Right - Width));
            Top = Math.Clamp(top, area.Top, Math.Max(area.Top, area.Bottom - Height));
        }
        if (settings.Maximized) WindowState = WindowState.Maximized;
    }

    private void RefreshNameLock()
    {
        var state = SteamActivityProbe.Read();
        OfflineNameReminder.Visibility = state == SteamActivity.NotRunning ? Visibility.Visible : Visibility.Collapsed;
        SteamStatus.Text = state switch
        {
            SteamActivity.Running => "Steam running · sign-in and game connection not verified",
            SteamActivity.Unknown => "Steam status unavailable · sign-in not verified",
            _ => "Steam not running · System Link available"
        };
        PlayerNameBox.IsEnabled = state == SteamActivity.NotRunning && !busy;
        PlayerNameStatus.Text = state switch
        {
            SteamActivity.Running => "Steam is running. Offline name is locked and will not be sent; the game manages your Steam name.",
            SteamActivity.Unknown => "Steam status could not be checked. Name override is disabled for safety.",
            _ => "Steam isn't running. Please set your player name here and save before playing, or use name \"YourName\" in the in-game console. Up to 15 ASCII characters; blank keeps your existing name."
        };
    }

    private void SkipUpdates_Changed(object sender, RoutedEventArgs e)
    {
        if (syncingUpdatePreference || CheckUpdatesBox is null) return;
        syncingUpdatePreference = true;
        try { CheckUpdatesBox.IsChecked = SkipUpdatesBox.IsChecked != true; }
        finally { syncingUpdatePreference = false; }
        SaveTogglePreferences();
    }

    private void CheckUpdates_Changed(object sender, RoutedEventArgs e)
    {
        if (syncingUpdatePreference || SkipUpdatesBox is null) return;
        syncingUpdatePreference = true;
        try { SkipUpdatesBox.IsChecked = CheckUpdatesBox.IsChecked != true; }
        finally { syncingUpdatePreference = false; }
        SaveTogglePreferences();
    }

    private void Preference_Changed(object sender, RoutedEventArgs e) => SaveTogglePreferences();

    private void SaveTogglePreferences()
    {
        // XAML defaults and loading persisted values also raise Changed events.
        // Never overwrite a saved preference with partially initialized controls.
        if (!preferencesLoaded) return;
        try
        {
            // Save only toggles: an invalid, unsubmitted name or a browsed folder
            // must not block persistence or get silently committed alongside it.
            SettingsStore.Save(SettingsStore.Load() with
            {
                CheckUpdatesBeforePlay = CheckUpdatesBox.IsChecked == true,
                SkipIntro = SkipIntroBox.IsChecked == true,
                DebugAssets = DebugAssetsBox.IsChecked == true,
                TraceActions = TraceActionsBox.IsChecked == true
            });
            SetStatus("Preferences saved.");
        }
        catch (Exception error) when (IsExpected(error))
        { ShowError("Could not save preferences. Your changes have not been saved. " + error.Message); }
    }

    private void SetStatus(string text)
    {
        StatusText.Text = text.ReplaceLineEndings(" ");
        StatusText.ToolTip = text;
    }

    private void RefreshInstallation()
    {
        if (string.IsNullOrWhiteSpace(GameDirectoryBox.Text))
        {
            InstallStatus.Text = "Choose your game folder in Settings.";
            BuildStatus.Text = "";
            return;
        }
        var directory = GameInstallation.Normalize(GameDirectoryBox.Text);
        var missing = GameInstallation.MissingFiles(directory);
        InstallStatus.Text = missing.Length == 0
            ? "Client files found · English/French 1.1 required"
            : $"{missing.Length} required file(s) missing · check Settings";
        InstallStatus.ToolTip = missing.Length == 0 ? directory : string.Join("\n", missing);
        BuildStatus.Text = InstallationInfo.ClientBuild(directory);
        if (File.Exists(Path.Combine(directory, "jb_mp_s.dll")))
        {
            using var stream = File.OpenRead(Path.Combine(directory, "jb_mp_s.dll"));
            if (!InstallationInfo.IsSupported(stream)) InstallStatus.Text = "Unsupported/damaged game binary · English/French 1.1 required";
        }
    }

    private void Navigate_Checked(object sender, RoutedEventArgs e)
    {
        // Called only after InitializeComponent has populated all pages.
        if (PlayPage is null || SettingsPage is null || CommunityPage is null) return;
        var page = (sender as RadioButton)?.Tag as string;
        PlayPage.Visibility = page == "Play" ? Visibility.Visible : Visibility.Collapsed;
        SettingsPage.Visibility = page == "Settings" ? Visibility.Visible : Visibility.Collapsed;
        CommunityPage.Visibility = page == "Community" ? Visibility.Visible : Visibility.Collapsed;
    }

    private void NameSettings_Click(object sender, RoutedEventArgs e)
    {
        SettingsNavigation.IsChecked = true;
        PlayerNameBox.BringIntoView();
        PlayerNameBox.Focus();
    }

    private void Browse_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new OpenFolderDialog { Title = "Select your Quantum of Solace game folder", Multiselect = false };
        if (Directory.Exists(GameDirectoryBox.Text)) dialog.InitialDirectory = GameDirectoryBox.Text;
        if (dialog.ShowDialog(this) != true) return;
        try
        {
            var directory = GameInstallation.Normalize(dialog.FolderName);
            if (!File.Exists(Path.Combine(directory, GameInstallation.Executable)) && !File.Exists(Path.Combine(directory, "jb_mp_s.dll")))
                throw new InvalidOperationException("Select the folder containing JB_Launcher_s.exe or jb_mp_s.dll.");
            GameDirectoryBox.Text = directory;
            RefreshInstallation();
            RefreshBackups();
            SetStatus("Game directory selected. Save settings to remember it.");
        }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
    }

    private void Save_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            GameDirectoryBox.Text = GameInstallation.Normalize(GameDirectoryBox.Text);
            LaunchOptions.ValidatePlayerName(PlayerNameBox.Text);
            SettingsStore.Save(ReadSettings());
            RefreshInstallation();
            SetStatus("Settings saved.");
        }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
    }

    private void SetBusy(bool value)
    {
        busy = value;
        SkipUpdatesBox.IsEnabled = !value;
        PlayButton.IsEnabled = UpdateButton.IsEnabled = SettingsPage.IsEnabled = RepairButton.IsEnabled = !value;
        if (!value) { UpdateProgress.IsIndeterminate = false; UpdateProgress.Visibility = Visibility.Collapsed; CancelUpdateButton.Visibility = Visibility.Collapsed; }
        RefreshNameLock();
    }

    private async Task ReadUpdateOutputAsync(StreamReader reader, StringBuilder report)
    {
        while (await reader.ReadLineAsync() is { } line)
        {
            const string prefix = "[consolation-progress]";
            if (!line.StartsWith(prefix, StringComparison.Ordinal))
            {
                if (report.Length < 65536) report.AppendLine(line);
                continue;
            }
            try
            {
                using var json = JsonDocument.Parse(line[prefix.Length..]);
                var root = json.RootElement;
                var stage = root.GetProperty("stage").GetString() ?? "Working";
                var completed = root.GetProperty("completed").GetInt64();
                var total = root.GetProperty("total").GetInt64();
                var detail = root.GetProperty("detail").GetString();
                CancelUpdateButton.IsEnabled = root.TryGetProperty("canCancel", out var canCancel) && canCancel.GetBoolean() && updateCancellation is not null;
                CancelUpdateButton.Content = CancelUpdateButton.IsEnabled ? "Cancel update" : "Finishing safely…";
                if (stage == "Nightly selected")
                {
                    NightlyStatus.Text = "Target nightly: " + detail;
                    if (report.Length < 65536) report.AppendLine(NightlyStatus.Text);
                }
                UpdateProgress.IsIndeterminate = total <= 0;
                UpdateProgress.Value = total > 0 ? Math.Clamp(100.0 * completed / total, 0, 100) : 0;
                var amount = total > 0 ? stage == "Downloading"
                    ? $" · {completed / 1048576.0:F1} / {total / 1048576.0:F1} MB"
                    : $" · {completed}/{total}" : "";
                SetStatus(stage + amount + (string.IsNullOrWhiteSpace(detail) ? "" : " · " + detail));
            }
            catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException or FormatException)
            {
                // A malformed progress record must not break reading or bypass
                // the updater's exit-code and rollback checks.
                if (report.Length < 65536) report.AppendLine(line);
            }
        }
    }

    private async Task<bool> RunUpdateAsync(string directory, bool repair = false, string? restoreBackup = null)
    {
        GameInstallation.RequireGameClosed();
        if (restoreBackup is null) NightlyStatus.Text = "";
        SetStatus("Checking client updates… Confirm any update in the updater's dialog.");
        UpdateProgress.Visibility = Visibility.Visible;
        UpdateProgress.IsIndeterminate = true;
        var eventName = @"Local\ConsolationUpdate-" + Guid.NewGuid().ToString("N");
        using var cancellation = new EventWaitHandle(false, EventResetMode.ManualReset, eventName);
        updateCancellation = cancellation;
        CancelUpdateButton.Visibility = Visibility.Visible;
        CancelUpdateButton.IsEnabled = true;
        using var process = Process.Start(GameInstallation.CreateUpdate(directory, repair, eventName, restoreBackup))
            ?? throw new InvalidOperationException("The updater could not be started.");
        var outputText = new StringBuilder();
        var output = ReadUpdateOutputAsync(process.StandardOutput, outputText);
        var errors = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        await output;
        var report = (outputText + "\n" + (await errors)).Trim();
        if (process.ExitCode == 2)
        {
            rollbackRequired = true;
            throw new InvalidOperationException("The updater could not restore its rollback copies. " +
                "Do not start the game until you have restored the backup identified below.\n\n" + report);
        }
        if (process.ExitCode != 0)
            throw new InvalidOperationException("The updater did not complete successfully.\n\n" + report);
        // Existing updater may return zero after a network warning or a skipped
        // debug build. Show its actual report; never label that 'up to date'.
        if (string.IsNullOrWhiteSpace(report)) report = "Updater finished. No status was returned.";
        SetStatus(report);
        OperationReport.Show(this, "Consolation updater", report);
        RefreshInstallation();
        RefreshBackups();
        return true;
    }

    private async void Play_Click(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        var launched = false;
        SetBusy(true);
        try
        {
            if (rollbackRequired)
                throw new InvalidOperationException("Manual update restoration is required before launching. " +
                    "Restore the reported rollback copies, then reopen the launcher.");
            var settings = ReadSettings();
            var directory = GameInstallation.Normalize(settings.GameDirectory);
            GameInstallation.RequireGameClosed();
            if (settings.CheckUpdatesBeforePlay) await RunUpdateAsync(directory);
            GameInstallation.RequireGameClosed();
            var start = GameInstallation.CreateLaunch(directory, settings);
            // Save before starting so a save failure never masquerades as a
            // launch failure after the game process has already been created.
            SettingsStore.Save(settings);
            using var process = Process.Start(start)
                ?? throw new InvalidOperationException("The game launcher could not be started.");
            launched = true;
        }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
        finally
        {
            updateCancellation = null;
            if (launched)
            {
                // Settings (including geometry) were saved before Process.Start.
                // Shut down WPF, not just the window: release the dispatcher,
                // timer and single-instance mutex without terminating the game.
                closingAfterLaunch = true;
                busy = false;
                steamRefresh.Stop();
                Application.Current.Shutdown(0);
            }
            else SetBusy(false); // Failed/skipped launches remain available to fix.
        }
    }

    private async void Update_Click(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        SetBusy(true);
        try
        {
            if (rollbackRequired) throw new InvalidOperationException("Restore the update's rollback copies before continuing.");
            await RunUpdateAsync(GameInstallation.Normalize(GameDirectoryBox.Text));
        }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
        finally { updateCancellation = null; SetBusy(false); }
    }

    private async void Repair_Click(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        SetBusy(true);
        try
        {
            if (rollbackRequired) throw new InvalidOperationException("Restore the update's rollback copies before continuing.");
            // The bundled updater owns the warning and defaults to No, before
            // downloading or modifying anything. Repair never launches the game.
            await RunUpdateAsync(GameInstallation.Normalize(GameDirectoryBox.Text), repair: true);
        }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
        finally { updateCancellation = null; SetBusy(false); }
    }

    private void CancelUpdate_Click(object sender, RoutedEventArgs e)
    {
        updateCancellation?.Set();
        CancelUpdateButton.IsEnabled = false;
        SetStatus("Cancellation requested. Waiting for a safe checkpoint; installation, if already started, will finish safely.");
    }

    private void RefreshBackups()
    {
        BackupList.Items.Clear();
        if (string.IsNullOrWhiteSpace(GameDirectoryBox.Text)) return;
        var root = Path.Combine(GameInstallation.Normalize(GameDirectoryBox.Text), "consolation-update-backups");
        if (!Directory.Exists(root)) return;
        foreach (var path in Directory.EnumerateDirectories(root).OrderByDescending(Directory.GetCreationTimeUtc))
        {
            var id = Path.GetFileName(path);
            if (!System.Text.RegularExpressions.Regex.IsMatch(id, "^[0-9a-f]{32}$")) continue;
            var manifest = Path.Combine(path, "backup.json");
            try
            {
                if (!File.Exists(manifest) || new FileInfo(manifest).Length > 2097152) continue;
                using var json = JsonDocument.Parse(File.ReadAllText(manifest));
                var data = json.RootElement;
                if (data.GetProperty("state").GetString() != "complete" || data.GetProperty("schema").GetInt32() != 1) continue;
                BackupList.Items.Add(new BackupChoice(id, $"{data.GetProperty("createdUtc").GetString()} · before {data.GetProperty("release").GetString()}"));
            }
            catch (Exception error) when (IsExpected(error) || error is KeyNotFoundException) { /* Unsupported backups remain on disk for manual inspection. */ }
        }
        if (BackupList.Items.Count > 0) BackupList.SelectedIndex = 0;
    }

    private void Backups_Click(object sender, RoutedEventArgs e)
    {
        try { RefreshBackups(); }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
    }

    private async void Restore_Click(object sender, RoutedEventArgs e)
    {
        if (busy) return;
        if (BackupList.SelectedItem is not BackupChoice backup) { ShowError("Select a complete client backup first."); return; }
        SetBusy(true);
        try
        {
            if (rollbackRequired) throw new InvalidOperationException("Resolve the failed update's manual restoration before continuing.");
            await RunUpdateAsync(GameInstallation.Normalize(GameDirectoryBox.Text), restoreBackup: backup.Id);
        }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
        finally { updateCancellation = null; SetBusy(false); }
    }

    private void Logs_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            Directory.CreateDirectory(OperationReport.LogDirectory);
            using var process = Process.Start(new ProcessStartInfo(OperationReport.LogDirectory) { UseShellExecute = true });
        }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
    }

    private void Link_Click(object sender, RoutedEventArgs e)
    {
        var url = ((sender as Button)?.Tag as string) switch
        {
            "Devlog" => "https://github.com/ProjectConsolation/Client/wiki/Fastfile-Devlog",
            "Wiki" => "https://github.com/ProjectConsolation/Client/wiki",
            "InstallGuide" => "https://github.com/ProjectConsolation/Client/wiki/How-To-Obtain-And-Install",
            "Donate" => "https://buymeacoffee.com/consolation",
            "Source" => "https://github.com/ProjectConsolation/Client",
            "Discord" => "https://discord.gg/XSrTvXJcsw",
            _ => null
        };
        if (url is null) return;
        try { using var process = Process.Start(new ProcessStartInfo(url) { UseShellExecute = true }); }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
    }

    private void ShowError(string message)
    {
        SetStatus(message);
        OperationReport.Show(this, "Project: Consolation", message);
    }

    private void GameInfo_Click(object sender, RoutedEventArgs e)
    {
        try
        {
            using var process = Process.Start(new ProcessStartInfo(
                "https://en.wikipedia.org/wiki/007:_Quantum_of_Solace") { UseShellExecute = true });
        }
        catch (Exception error) when (IsExpected(error)) { ShowError(error.Message); }
    }

    private void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    private void Close_Click(object sender, RoutedEventArgs e) => Close();
    private void Window_Closing(object? sender, CancelEventArgs e)
    {
        if (closingAfterLaunch) return;
        if (!busy)
        {
            try
            {
                // Persist geometry only here. Unsaved edits remain unsaved.
                var geometry = ReadSettings();
                SettingsStore.Save(SettingsStore.Load() with { WindowLeft = geometry.WindowLeft, WindowTop = geometry.WindowTop,
                    WindowWidth = geometry.WindowWidth, WindowHeight = geometry.WindowHeight, Maximized = geometry.Maximized });
            }
            catch (Exception error) when (IsExpected(error)) { ShowError("Could not save window position: " + error.Message); }
            return;
        }
        e.Cancel = true;
        SetStatus("Wait for the current launch/update operation to finish before closing.");
    }
}
