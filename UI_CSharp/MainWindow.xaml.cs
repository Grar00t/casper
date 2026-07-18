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
    private static readonly string EngineDir = AppContext.BaseDirectory;
    private static readonly string EnginePath = Path.Combine(EngineDir, "niyah_hybrid.exe");

    public MainWindow()
    {
        InitializeComponent();
        InitWebView();

        // Allow dragging the borderless window
        MouseLeftButtonDown += (_, e) => { if (e.ButtonState == System.Windows.Input.MouseButtonState.Pressed) DragMove(); };
    }

    private async void InitWebView()
    {
        string dataDir = Path.Combine(Path.GetTempPath(), "CasperWebView2");
        Directory.CreateDirectory(dataDir);

        var env = await CoreWebView2Environment.CreateAsync(null, dataDir);
        await WebView.EnsureCoreWebView2Async(env);

        WebView.CoreWebView2.Settings.IsStatusBarEnabled = false;
        WebView.CoreWebView2.Settings.AreDefaultContextMenusEnabled = false;
        WebView.CoreWebView2.Settings.IsZoomControlEnabled = false;

        // Register C# → JS bridge
        WebView.CoreWebView2.AddHostObjectToScript("casperBridge", new CasperBridge(this));

        // Load the HTML UI
        string htmlPath = Path.Combine(AppContext.BaseDirectory, "casper_workbench.html");
        if (File.Exists(htmlPath))
            WebView.CoreWebView2.Navigate(new Uri(htmlPath).AbsoluteUri);
        else
            WebView.CoreWebView2.NavigateToString(FallbackHtml());
    }

    private void WebView_NavigationCompleted(object sender, CoreWebView2NavigationCompletedEventArgs e)
    {
        // Inject bridge initialisation
        _ = WebView.CoreWebView2.ExecuteScriptAsync(
            "window.casper = window.chrome?.webview?.hostObjects?.casperBridge ?? null;");
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