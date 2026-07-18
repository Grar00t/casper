using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace CasperUI;

/// <summary>
/// Host object exposed to JavaScript as window.casper.
/// JS calls: await window.casper.query("...") and receives a JSON string.
/// </summary>
[ClassInterface(ClassInterfaceType.AutoDual)]
[ComVisible(true)]
public class CasperBridge(MainWindow owner)
{
    private readonly MainWindow _owner = owner;

    /// <summary>
    /// Run a RAG query through niyah_hybrid.exe and return JSON result.
    /// Called from JavaScript: const result = await window.casper.query(text)
    /// </summary>
    public Task<string> Query(string text) => _owner.RunQuery(text);

    /// <summary>Minimise the window.</summary>
    public void Minimise() => _owner.Dispatcher.Invoke(() =>
        _owner.WindowState = System.Windows.WindowState.Minimized);

    /// <summary>Toggle maximise / restore.</summary>
    public void ToggleMaximise() => _owner.Dispatcher.Invoke(() =>
        _owner.WindowState = _owner.WindowState == System.Windows.WindowState.Maximized
            ? System.Windows.WindowState.Normal
            : System.Windows.WindowState.Maximized);

    /// <summary>Close the application.</summary>
    public void Close() => _owner.Dispatcher.Invoke(_owner.Close);
}
