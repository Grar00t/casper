using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace CasperUI;

/// <summary>
/// Host object exposed to JavaScript as window.casper.
/// Provides: Query (NIYAH search), PTY terminal, window controls.
/// </summary>
[ClassInterface(ClassInterfaceType.AutoDual)]
[ComVisible(true)]
public class CasperBridge(MainWindow owner)
{
    private readonly MainWindow _owner = owner;
    private PtyBridge? _pty;

    // ── NIYAH search ──────────────────────────────────────────────────────────
    public Task<string> Query(string text) => _owner.RunQuery(text);

    // ── Real Terminal (ConPTY) ────────────────────────────────────────────────
    /// <summary>
    /// Start a real PowerShell PTY session.
    /// xterm.js calls this once on load.
    /// </summary>
    public void TerminalStart(int cols = 120, int rows = 30)
    {
        _owner.Dispatcher.Invoke(() =>
        {
            _pty?.Dispose();
            _pty = new PtyBridge();
            _pty.Start(_owner.WebView.CoreWebView2, (short)cols, (short)rows);
        });
        // Note: _owner.WebView is the WebView2 control (x:Name="WebView" in XAML)
    }

    /// <summary>
    /// Send raw input from xterm.js → real shell.
    /// Called on every keystroke.
    /// </summary>
    public void TerminalInput(string data)
    {
        _pty?.SendInput(data);
    }

    /// <summary>Resize PTY when terminal window size changes.</summary>
    public void TerminalResize(int cols, int rows)
    {
        _pty?.Resize((short)cols, (short)rows);
    }

    // ── Window controls ────────────────────────────────────────────────────────
    public void Minimise() => _owner.Dispatcher.Invoke(() =>
        _owner.WindowState = System.Windows.WindowState.Minimized);

    public void ToggleMaximise() => _owner.Dispatcher.Invoke(() =>
        _owner.WindowState = _owner.WindowState == System.Windows.WindowState.Maximized
            ? System.Windows.WindowState.Normal
            : System.Windows.WindowState.Maximized);

    public void Close()
    {
        _pty?.Dispose();
        _owner.Dispatcher.Invoke(_owner.Close);
    }
}
