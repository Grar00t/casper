<#
.SYNOPSIS
    Build NIYAH-CORE using MSVC (cl.exe).
#>
[CmdletBinding()]
param(
    [ValidateSet('x64','arm64','auto')]
    [string]$Arch = 'auto',

    [ValidateSet('Debug','Release')]
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
Set-Location (Join-Path $PSScriptRoot '..')
$Root = (Get-Location).Path

if ($Arch -eq 'auto') {
    $Arch = if ([System.Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -eq [System.Runtime.InteropServices.Architecture]::Arm64) { 'arm64' } else { 'x64' }
}
Write-Host "[build_msvc] Target arch : $Arch"
Write-Host "[build_msvc] Config      : $Config"

function Find-VsDevCmd {
    $vswhere = "$env:ProgramFiles(x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) {
        $vswhere = "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe"
    }
    if (Test-Path $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'Common7\Tools\VsDevCmd.bat'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $fallbacks = @(
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\Common7\Tools\VsDevCmd.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat'
    )
    foreach ($fb in $fallbacks) {
        if (Test-Path $fb) { return $fb }
    }
    return $null
}

function Test-ArchFlag([string]$flag) {
    $tmp = [System.IO.Path]::GetTempFileName() + '.c'
    'int main(void){return 0;}' | Set-Content $tmp
    $out = & cmd /c "cl.exe /nologo $flag $tmp /Fe:nul 2>&1"
    Remove-Item $tmp -ErrorAction SilentlyContinue
    return ($LASTEXITCODE -eq 0 -and ($out -notmatch 'D9002'))
}

if ($Arch -eq 'arm64') {
    $archFlag = ''
} elseif ($Config -eq 'Release' -and (Test-ArchFlag '/arch:AVX2')) {
    $archFlag = '/arch:AVX2'
} else {
    $archFlag = ''
}

if ($archFlag) {
    Write-Host "[build_msvc] SIMD flag   : $archFlag"
} else {
    Write-Host '[build_msvc] SIMD flag   : (none — scalar fallback)'
}

$commonFlags = @('/nologo', '/W4', '/WX', '/wd4996', '/EHsc')
if ($archFlag) { $commonFlags += $archFlag }
$configFlags = if ($Config -eq 'Release') { @('/O2', '/GL', '/DNDEBUG') } else { @('/Od', '/Zi', '/RTC1') }

function Invoke-ClBuild {
    param([string[]]$Sources, [string]$Out, [string[]]$ExtraFlags = @())
    $allFlags = @($commonFlags + $configFlags + @('/I.', '/ICore_CPP', '/Iinclude') + $ExtraFlags)
    $flagStr = $allFlags -join ' '
    $srcStr = ($Sources | ForEach-Object { '"' + $_ + '"' }) -join ' '
    $cmd = "cl.exe $flagStr $srcStr /Fe:`"$Out`""
    Write-Host "`n[build_msvc] Compiling: $Out"
    Write-Host "  cmd> $cmd"
    $result = & cmd /c "$cmd 2>&1"
    foreach ($line in $result) { Write-Host "  $line" }
    if ($LASTEXITCODE -ne 0) { throw "[build_msvc] FAILED (exit $LASTEXITCODE): $Out" }
    if (Test-Path $Out) {
        $sz = (Get-Item $Out).Length
        Write-Host "[build_msvc] OK  $Out  ($([math]::Round($sz/1KB,1)) KB)"
    }
}

$clInPath = Get-Command cl.exe -ErrorAction SilentlyContinue
if (-not $clInPath) {
    $vsDevCmd = Find-VsDevCmd
    if (-not $vsDevCmd) { throw '[build_msvc] cl.exe not found and VsDevCmd.bat not located. Install Visual Studio Build Tools (C++ workload).' }
    Write-Host "[build_msvc] Bootstrapping MSVC environment from:`n  $vsDevCmd"
    $scriptPath = $MyInvocation.MyCommand.Path
    $archArg = "-Arch $Arch"
    $cfgArg = "-Config $Config"
    $inner = "powershell -NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" $archArg $cfgArg"
    & cmd /c "`"$vsDevCmd`" -arch=$Arch -no_logo && $inner"
    exit $LASTEXITCODE
}

$niyahSrc = @("$Root\Core_CPP\niyah_core.c", "$Root\Core_CPP\niyah_main.c")
$trainerSrc = @("$Root\Core_CPP\trainer.cpp")
$niyahTrainSrc = @("$Root\Core_CPP\niyah_train.c", "$Root\Core_CPP\niyah_core.c", "$Root\tokenizer.c")
$hybridSrc = @(
    "$Root\Core_CPP\niyah_hybrid_main.c",
    "$Root\Core_CPP\niyah_core.c",
    "$Root\Core_CPP\hybrid_reasoner.c",
    "$Root\Core_CPP\constraint_solver.c",
    "$Root\Core_CPP\rule_parser.c",
    "$Root\Core_CPP\proof_generator.c",
    "$Root\Core_CPP\khz_q_svd.c",
    "$Root\tokenizer.c"
)
$benchSrc = @("$Root\Core_CPP\bench_niyah.c", "$Root\Core_CPP\niyah_core.c")

Invoke-ClBuild -Sources $niyahSrc -Out "$Root\Core_CPP\niyah.exe" -ExtraFlags @('/std:c17')
Invoke-ClBuild -Sources $trainerSrc -Out "$Root\Core_CPP\trainer.exe" -ExtraFlags @('/std:c++17')
Invoke-ClBuild -Sources $niyahTrainSrc -Out "$Root\niyah_train.exe" -ExtraFlags @('/std:c17')
Invoke-ClBuild -Sources $hybridSrc -Out "$Root\Core_CPP\niyah_hybrid.exe" -ExtraFlags @('/std:c17')
Invoke-ClBuild -Sources $benchSrc -Out "$Root\Core_CPP\bench_niyah.exe" -ExtraFlags @('/std:c17')

Write-Host "`n[build_msvc] Artifact checksums (SHA256):"
foreach ($artifact in @(
    "$Root\Core_CPP\niyah.exe",
    "$Root\Core_CPP\trainer.exe",
    "$Root\niyah_train.exe",
    "$Root\Core_CPP\niyah_hybrid.exe",
    "$Root\Core_CPP\bench_niyah.exe"
)) {
    if (Test-Path $artifact) {
        $hash = (Get-FileHash $artifact -Algorithm SHA256).Hash
        $sz = [math]::Round((Get-Item $artifact).Length / 1KB, 1)
        Write-Host "  $hash  $((Split-Path $artifact -Leaf))  (${sz} KB)"
    }
}

Write-Host "`n[build_msvc] Build complete."
