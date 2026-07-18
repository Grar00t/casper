using System;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Threading.Tasks;
using System.Windows;
using Microsoft.Web.WebView2.Core;

namespace CasperUI;

public partial class MainWindow : Window
{
    // Path to the C11 engine — same directory as this exe
    // Engine lives in app/ subfolder; fall back to exe directory for dev runs
    private static readonly string EngineDir =
        Directory.Exists(Path.Combine(AppContext.BaseDirectory, "app"))
            ? Path.Combine(AppContext.BaseDirectory, "app")
            : AppContext.BaseDirectory;
    private static readonly string EnginePath = Path.Combine(EngineDir, "niyah_hybrid.exe");

    // Node.js agent process (started in background, killed on exit)
    private Process? _agentProcess;
    private const int AgentPort = 3000;
    private string _agentUrl = "http://20.91.208.59"; // updated after StartNodeAgent()

    // WebView is x:Name in XAML — directly accessible as 'WebView' property
    // CasperBridge accesses _owner.WebView.CoreWebView2 for PTY output

    public MainWindow()
    {
        InitializeComponent();
        StartNodeAgent();
        InitWebView();

        // Allow dragging the borderless window
        MouseLeftButtonDown += (_, e) => { if (e.ButtonState == System.Windows.Input.MouseButtonState.Pressed) DragMove(); };

        // Kill agent on close
        Closed += (_, _) => StopNodeAgent();
    }

    private void StartNodeAgent()
    {
        // Look for niyah_engine server.js next to exe (app/niyah_engine/server.js)
        // or in dev: niyah_engine_local/server.js from repo root
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "app", "niyah_engine", "server.js"),
            Path.Combine(AppContext.BaseDirectory, "niyah_engine_local", "server.js"),
            Path.Combine(AppContext.BaseDirectory, "..", "niyah_engine_local", "server.js"),
        };

        string? serverJs = null;
        foreach (var c in candidates)
            if (File.Exists(c)) { serverJs = Path.GetFullPath(c); break; }

        if (serverJs == null) return; // no Node engine found, use Azure fallback

        // Find node.exe
        string node = "node"; // must be on PATH

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = node,
                Arguments = $"\"{serverJs}\"",
                WorkingDirectory = Path.GetDirectoryName(serverJs)!,
                UseShellExecute = false,
                CreateNoWindow  = true,
                RedirectStandardOutput = false,
                RedirectStandardError  = false,
            };
            // Pass C11 exe path so agent can find it
            psi.Environment["NIYAH_HYBRID_EXE"] = EnginePath;
            psi.Environment["PORT"] = AgentPort.ToString();

            _agentProcess = Process.Start(psi);
            // Give it 1.5s to bind port
            System.Threading.Thread.Sleep(1500);
        }
        catch
        {
            // node not found or failed — fall through to Azure agent
            _agentProcess = null;
        }
    }

    private void StopNodeAgent()
    {
        try { _agentProcess?.Kill(entireProcessTree: true); } catch { }
        _agentProcess = null;
    }

    private async void InitWebView()
    {
        // Store WebView2 user data in %LOCALAPPDATA% so it works from
        // Program Files and other protected directories (no write permission issues).
        string dataDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CasperApp", "webview2_data");
        Directory.CreateDirectory(dataDir);

        var env = await CoreWebView2Environment.CreateAsync(null, dataDir);
        await WebView.EnsureCoreWebView2Async(env);

        WebView.CoreWebView2.Settings.IsStatusBarEnabled = false;
        WebView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
        WebView.CoreWebView2.Settings.IsZoomControlEnabled = false;

        // Register C# host object — exposed to JS as window.casper
        WebView.CoreWebView2.AddHostObjectToScript("casperBridge", new CasperBridge(this));

        // AreHostObjectsAllowed is true by default, no need to set

        // Look for HTML in app/ subfolder first, then fallback to exe directory
        string appDir  = Path.Combine(AppContext.BaseDirectory, "app");
        string htmlPath = File.Exists(Path.Combine(appDir, "casper_workbench.html"))
            ? Path.Combine(appDir, "casper_workbench.html")
            : Path.Combine(AppContext.BaseDirectory, "casper_workbench.html");

        if (File.Exists(htmlPath))
            WebView.CoreWebView2.Navigate(new Uri(htmlPath).AbsoluteUri);
        else
            WebView.CoreWebView2.NavigateToString(FallbackHtml());
    }

    private void WebView_NavigationCompleted(object sender, CoreWebView2NavigationCompletedEventArgs e)
    {
        // Determine agent URL: local Node.js if running, else Azure VM
        _agentUrl = (_agentProcess != null && !_agentProcess.HasExited)
            ? $"http://127.0.0.1:{AgentPort}"
            : "http://20.91.208.59";

        // Inject bridge + agent URL
        _ = WebView.CoreWebView2.ExecuteScriptAsync(
            $"window.casper = window.chrome?.webview?.hostObjects?.casperBridge ?? null;" +
            $"window._agentUrl = '{_agentUrl}';");
    }

    // Called from JS: window.casper.query("...")
    internal async Task<string> RunQuery(string query)
    {
        if (!File.Exists(EnginePath))
            return JsonSerializer.Serialize(new { error = $"Engine not found: {EnginePath}" });

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = EnginePath,
                Arguments = $"--rag",
                RedirectStandardInput  = true,
                RedirectStandardOutput = true,
                RedirectStandardError  = true,
                UseShellExecute = false,
                CreateNoWindow  = true,
                WorkingDirectory = EngineDir,
                // UTF-8 without BOM — BOM causes %EF%BB%BF prefix in DDG queries
                StandardInputEncoding  = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                StandardOutputEncoding = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                StandardErrorEncoding  = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
            };

            using var proc = Process.Start(psi)!;
            await proc.StandardInput.WriteLineAsync(query);
            await proc.StandardInput.WriteLineAsync("quit");
            proc.StandardInput.Close();

            string stdout = await proc.StandardOutput.ReadToEndAsync();
            string stderr = await proc.StandardError.ReadToEndAsync();
            await proc.WaitForExitAsync();

            return JsonSerializer.Serialize(new
            {
                output   = stdout,
                trace    = stderr,
                exitCode = proc.ExitCode,
            });
        }
        catch (Exception ex)
        {
            return JsonSerializer.Serialize(new { error = ex.Message });
        }
    }

    // Minimal fallback if HTML file is missing
    private static string FallbackHtml() =>
        "<html><body style='background:#040706;color:#2CF2DA;font-family:monospace;padding:40px'>" +
        "<h2>Casper</h2><p>casper_workbench.html not found next to Casper.exe</p></body></html>";
}