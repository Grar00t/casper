$ErrorActionPreference = "Stop"

Set-Location (Join-Path $PSScriptRoot "..")

$source = "Core_CPP\trainer.cpp"
$output = "Core_CPP\trainer.exe"

Write-Host "[Casper] Building trainer..."

if (Get-Command g++ -ErrorAction SilentlyContinue) {
    g++ $source -O2 -std=c++17 -o $output
    Write-Host "[Casper] Build OK via g++ -> $output"
    exit 0
}

if (Get-Command clang++ -ErrorAction SilentlyContinue) {
    clang++ $source -O2 -std=c++17 -o $output
    Write-Host "[Casper] Build OK via clang++ -> $output"
    exit 0
}

if (Get-Command cl.exe -ErrorAction SilentlyContinue) {
    cl.exe /nologo /EHsc /O2 /std:c++17 $source /Fe:$output
    if (Test-Path $output) {
        Write-Host "[Casper] Build OK via cl.exe -> $output"
        exit 0
    }
}

# Try Visual Studio Developer environment automatically (when cl.exe is not in PATH).
$vsDevCmd = "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
if (Test-Path $vsDevCmd) {
    Write-Host "[Casper] Trying MSVC via VsDevCmd..."
    cmd /c "`"$vsDevCmd`" -arch=arm64 && cl /nologo /EHsc /O2 /std:c++17 $source /Fe:$output"
    if (Test-Path $output) {
        Write-Host "[Casper] Build OK via MSVC (VsDevCmd) -> $output"
        exit 0
    }
}

Write-Error @"
[Casper] No C++ compiler found.
Install one of:
  1) LLVM/Clang (recommended): winget install -e --id LLVM.LLVM
  2) Visual Studio Build Tools (C++): winget install -e --id Microsoft.VisualStudio.2022.BuildTools

Then open a new PowerShell and rerun:
  .\scripts\build_trainer.ps1
"@

