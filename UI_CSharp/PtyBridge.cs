using System;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.Web.WebView2.Core;

namespace CasperUI;

/// <summary>
/// Real PTY (Pseudo-Terminal) via Windows ConPTY API.
/// Bridges PowerShell ↔ xterm.js in WebView2.
/// No fake terminal — real shell process.
/// </summary>
public sealed class PtyBridge : IDisposable
{
    // ── ConPTY P/Invoke ──────────────────────────────────────────────────────
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CreatePipe(out nint hRead, out nint hWrite,
        nint lpAttr, uint nSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(nint h);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern int CreatePseudoConsole(COORD size, nint hIn,
        nint hOut, uint flags, out nint hPty);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern int ResizePseudoConsole(nint hPty, COORD size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern void ClosePseudoConsole(nint hPty);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CreateProcess(string? app, string? cmd,
        nint pAttr, nint tAttr, bool inherit, uint flags,
        nint env, string? dir, ref STARTUPINFOEX si, out PROCESS_INFO pi);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool InitializeProcThreadAttributeList(
        nint list, int count, uint flags, ref nint size);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool UpdateProcThreadAttribute(
        nint list, uint flags, nint attr, nint val, nint size,
        nint prev, nint retSize);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern nint HeapAlloc(nint heap, uint flags, nint bytes);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool HeapFree(nint heap, uint flags, nint mem);

    [DllImport("kernel32.dll")]
    private static extern nint GetProcessHeap();

    [StructLayout(LayoutKind.Sequential)]
    private struct COORD { public short X, Y; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFOEX
    {
        public STARTUPINFO StartupInfo;
        public nint lpAttributeList;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFO
    {
        public int cb;
        public string? lpReserved, lpDesktop, lpTitle;
        public uint dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars,
                    dwFillAttribute, dwFlags;
        public ushort wShowWindow, cbReserved2;
        public nint lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFO
    {
        public nint hProcess, hThread;
        public uint dwProcessId, dwThreadId;
    }

    private const uint EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
    private const nint PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE = 0x00020016;

    // ── State ────────────────────────────────────────────────────────────────
    private nint _hPty = nint.Zero;
    private nint _hProcess = nint.Zero;
    private Stream? _ptyIn;   // we write here → shell reads
    private Stream? _ptyOut;  // shell writes here → we read
    private CancellationTokenSource _cts = new();
    private CoreWebView2? _webView;
    private bool _disposed;

    public bool IsRunning => _hProcess != nint.Zero;

    /// <summary>Start a real PowerShell PTY session.</summary>
    public void Start(CoreWebView2? webView, short cols = 120, short rows = 30)
    {
        _webView = webView;

        // Create pipe pairs: ptyIn (we→shell), ptyOut (shell→we)
        CreatePipe(out var inRead,  out var inWrite,  nint.Zero, 0);
        CreatePipe(out var outRead, out var outWrite, nint.Zero, 0);

        // Create ConPTY
        var size = new COORD { X = cols, Y = rows };
        int hr = CreatePseudoConsole(size, inRead, outWrite, 0, out _hPty);
        if (hr != 0) throw new InvalidOperationException($"CreatePseudoConsole failed: 0x{hr:X}");

        CloseHandle(inRead);
        CloseHandle(outWrite);

        // Wrap handles as streams
        _ptyIn  = new FileStream(new Microsoft.Win32.SafeHandles.SafeFileHandle(inWrite,  true), FileAccess.Write);
        _ptyOut = new FileStream(new Microsoft.Win32.SafeHandles.SafeFileHandle(outRead, true), FileAccess.Read);

        // Build STARTUPINFOEX with ConPTY attribute
        nint attrListSize = nint.Zero;
        InitializeProcThreadAttributeList(nint.Zero, 1, 0, ref attrListSize);
        nint attrList = HeapAlloc(GetProcessHeap(), 0, attrListSize);
        InitializeProcThreadAttributeList(attrList, 1, 0, ref attrListSize);

        nint hPtyLocal = _hPty;
        UpdateProcThreadAttribute(attrList, 0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, hPtyLocal,
            Marshal.SizeOf<nint>(), nint.Zero, nint.Zero);

        var si = new STARTUPINFOEX
        {
            StartupInfo = new STARTUPINFO { cb = Marshal.SizeOf<STARTUPINFOEX>() },
            lpAttributeList = attrList,
        };

        // Launch PowerShell
        string shell = Environment.GetEnvironmentVariable("COMSPEC") ?? "powershell.exe";
        if (!shell.Contains("powershell", StringComparison.OrdinalIgnoreCase))
            shell = "powershell.exe";

        CreateProcess(null, shell, nint.Zero, nint.Zero, false,
            EXTENDED_STARTUPINFO_PRESENT, nint.Zero, null,
            ref si, out var pi);

        _hProcess = pi.hProcess;
        HeapFree(GetProcessHeap(), 0, attrList);

        // Start reading loop → send to xterm.js
        Task.Run(() => ReadLoop(_cts.Token));
    }

    /// <summary>Send keystrokes/input from xterm.js → shell.</summary>
    public void SendInput(string data)
    {
        if (_ptyIn == null || data.Length == 0) return;
        var bytes = Encoding.UTF8.GetBytes(data);
        _ptyIn.Write(bytes, 0, bytes.Length);
        _ptyIn.Flush();
    }

    /// <summary>Resize terminal (called when window resizes).</summary>
    public void Resize(short cols, short rows)
    {
        if (_hPty != nint.Zero)
            ResizePseudoConsole(_hPty, new COORD { X = cols, Y = rows });
    }

    private async Task ReadLoop(CancellationToken ct)
    {
        var buf = new byte[4096];
        while (!ct.IsCancellationRequested && _ptyOut != null)
        {
            try
            {
                int n = await _ptyOut.ReadAsync(buf, ct);
                if (n <= 0) break;

                // Send raw bytes to xterm.js as base64
                var b64 = Convert.ToBase64String(buf, 0, n);
                // Must dispatch to UI thread
                await System.Windows.Application.Current.Dispatcher.InvokeAsync(() =>
                {
                    _webView?.ExecuteScriptAsync($"window.__ptyData && window.__ptyData('{b64}')");
                });
            }
            catch (OperationCanceledException) { break; }
            catch { break; }
        }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        _cts.Cancel();
        _ptyIn?.Dispose();
        _ptyOut?.Dispose();
        if (_hPty != nint.Zero) ClosePseudoConsole(_hPty);
        if (_hProcess != nint.Zero) CloseHandle(_hProcess);
    }
}
