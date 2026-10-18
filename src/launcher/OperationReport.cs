using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;

namespace Consolation.Launcher;

internal static class OperationReport
{
    internal static readonly string LogDirectory = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "ProjectConsolation", "logs");

    internal static void Show(Window owner, string title, string message)
    {
        string? log = null;
        try
        {
            Directory.CreateDirectory(LogDirectory);
            log = Path.Combine(LogDirectory, DateTime.UtcNow.ToString("yyyyMMdd-HHmmss") + "-" + Guid.NewGuid().ToString("N") + ".log");
            File.WriteAllText(log, title + "\n" + message);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        var details = title + "\n\n" + message + (log is null ? "\n\nCould not save a local log." : "\n\nLog: " + log);
        var window = new Window { Owner = owner, Title = title, Width = 720, Height = 440,
            MinWidth = 480, MinHeight = 300, WindowStartupLocation = WindowStartupLocation.CenterOwner,
            Background = owner.Background, Foreground = owner.Foreground };
        var grid = new DockPanel { Margin = new Thickness(20) };
        var buttons = new WrapPanel { Margin = new Thickness(0, 12, 0, 0), HorizontalAlignment = HorizontalAlignment.Right };
        DockPanel.SetDock(buttons, Dock.Bottom);
        void Add(string text, Action action)
        {
            var button = new Button { Content = text, Margin = new Thickness(5), Padding = new Thickness(12, 8, 12, 8) };
            button.Click += (_, _) =>
            {
                try { action(); }
                catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception or System.Runtime.InteropServices.COMException)
                { MessageBox.Show(window, error.Message, title); }
            };
            buttons.Children.Add(button);
        }
        Add("Copy details", () => Clipboard.SetText(details));
        Add("Open logs", () => { Directory.CreateDirectory(LogDirectory); Process.Start(new ProcessStartInfo(LogDirectory) { UseShellExecute = true })?.Dispose(); });
        Add("Troubleshooting", () => Process.Start(new ProcessStartInfo("https://github.com/ProjectConsolation/Client/wiki") { UseShellExecute = true })?.Dispose());
        Add("Close", () => window.Close());
        grid.Children.Add(buttons);
        grid.Children.Add(new TextBox { Text = details, IsReadOnly = true, TextWrapping = TextWrapping.Wrap,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto });
        window.Content = grid;
        window.ShowDialog();
    }
}
