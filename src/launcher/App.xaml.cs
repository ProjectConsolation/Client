using System.Windows;
using System.Threading;

namespace Consolation.Launcher;

public partial class App : Application
{
    private Mutex? instance;

    protected override void OnStartup(StartupEventArgs e)
    {
        instance = new Mutex(true, @"Local\ProjectConsolation.Launcher", out var firstInstance);
        if (!firstInstance)
        {
            MessageBox.Show("The Consolation launcher is already open.", "Project: Consolation",
                MessageBoxButton.OK, MessageBoxImage.Information);
            instance.Dispose();
            instance = null;
            Shutdown();
            return;
        }
        base.OnStartup(e);
    }

    protected override void OnExit(ExitEventArgs e)
    {
        instance?.ReleaseMutex();
        instance?.Dispose();
        base.OnExit(e);
    }
}
