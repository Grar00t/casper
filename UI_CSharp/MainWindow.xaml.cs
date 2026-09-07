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
    private static readonly string EngineDir =
        Directory.Exists(Path.Combine(AppContext.BaseDirectory, "app"))
            ? Path.Combine(AppContext.BaseDirectory, "app")
            : AppContext.BaseDirectory;

    private static readonly string EnginePath = Path.Combine(EngineDir, "niyah_hybrid.exe");
    private Process? _agentProcess;
    private const int AgentPort = 3000;
    private static readonly string AgentUrl = $"http://127.0.0.1:{AgentPort}";

    public MainWindow()
    {
        InitializeComponent();
        StartNodeAgent();
        InitWebView();

        MouseLeftButtonDown += (_, e) =>
        {
            if (e.ButtonState == System.Windows.Input.MouseButtonState.Pressed) DragMove();
        };
        Closed += (_, _) => StopNodeAgent();
    }

    private void StartNodeAgent()
    {
        var candidates = new[]
        {
            Path.Combine(AppContext.BaseDirectory, "app", "niyah_engine", "server.js"),
            Path.Combine(AppContext.BaseDirectory, "niyah_engine_local", "server.js"),
            Path.Combine(AppContext.BaseDirectory, "..", "niyah_engine_local", "server.js"),
        };

        string? serverJs = null;
        foreach (var candidate in candidates)
        {
            if (File.Exists(candidate))
            {
                serverJs = Path.GetFullPath(candidate);
                break;
            }
        }

        if (serverJs == null) return;

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "node",
                Arguments = $"\"{serverJs}\"",
                WorkingDirectory = Path.GetDirectoryName(serverJs)!,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = false,
                RedirectStandardError = false,
            };
            psi.Environment["NIYAH_HYBRID_EXE"] = EnginePath;
            psi.Environment["PORT"] = AgentPort.ToString();
            psi.Environment["HOST"] = "127.0.0.1";

            _agentProcess = Process.Start(psi);
        }
        catch
        {
            _agentProcess = null;
        }
    }

    private void StopNodeAgent()
    {
        try { _agentProcess?.Kill(entireProcessTree: true); } catch { }
        _agentProcess?.Dispose();
        _agentProcess = null;
    }

    private async void InitWebView()
    {
        string dataDir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "CasperApp",
            "webview2_data");
        Directory.CreateDirectory(dataDir);

        var env = await CoreWebView2Environment.CreateAsync(null, dataDir);
        await WebView.EnsureCoreWebView2Async(env);

        WebView.CoreWebView2.Settings.IsStatusBarEnabled = false;
        WebView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
        WebView.CoreWebView2.Settings.IsZoomControlEnabled = false;
        WebView.CoreWebView2.AddHostObjectToScript("casperBridge", new CasperBridge(this));

        string appDir = Path.Combine(AppContext.BaseDirectory, "app");
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
        _ = WebView.CoreWebView2.ExecuteScriptAsync(
            "window.casper = window.chrome?.webview?.hostObjects?.casperBridge ?? null;" +
            $"window._agentUrl = '{AgentUrl}';");
    }

    internal async Task<string> RunQuery(string query)
    {
        if (!File.Exists(EnginePath))
            return JsonSerializer.Serialize(new { error = $"Engine not found: {EnginePath}" });

        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = EnginePath,
                Arguments = "--rag",
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                UseShellExecute = false,
                CreateNoWindow = true,
                WorkingDirectory = EngineDir,
                StandardInputEncoding = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                StandardOutputEncoding = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
                StandardErrorEncoding = new System.Text.UTF8Encoding(encoderShouldEmitUTF8Identifier: false),
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
                output = stdout,
                trace = stderr,
                exitCode = proc.ExitCode,
            });
        }
        catch (Exception ex)
        {
            return JsonSerializer.Serialize(new { error = ex.Message });
        }
    }

    private static string FallbackHtml() =>
        "<html><body style='font-family:monospace;padding:40px'>" +
        "<h2>Casper</h2><p>casper_workbench.html not found next to Casper.exe</p></body></html>";
}
