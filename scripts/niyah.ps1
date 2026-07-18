[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [ValidateSet("build", "corpus", "train", "smoke", "bench", "save", "run", "all")]
    [string]$Action = "all",

    [Parameter(Position = 1)]
    [string]$DataPath = "Data_Training/sovereign_knowledge.txt",

    [Parameter(Position = 2)]
    [int]$Epochs = 3,

    [Parameter(Position = 3)]
    [double]$Lr = 0.001,
    [double]$MinLr = 0.0001,

    [string]$Prompt = "bismillah",
    [int]$Tokens = 64,
    [string]$Model = "niyah_tiny.bin",
    [string]$Size = "tiny",
    [int]$Steps = 200,
    [double]$Temp = 0.8,
    [double]$TopP = 0.9,
    [int]$Seed = 42
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$RepoRoot = (Get-Location).Path

function Invoke-Build {
    Write-Host "[niyah] build..."
    & powershell -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "scripts\build_msvc.ps1") -Config Release
}

function Ensure-Build {
    if (-not (Test-Path (Join-Path $RepoRoot "Core_CPP\niyah.exe"))) {
        Invoke-Build
    }
}

function Invoke-Corpus {
    Write-Host "[niyah] corpus..."
    $corpusScript = Join-Path $RepoRoot "scripts\build_corpus.ps1"
    if (Test-Path $corpusScript) {
        & powershell -ExecutionPolicy Bypass -File $corpusScript
    } else {
        Write-Host "[niyah] corpus script not found; skipping."
    }
}

function Invoke-Train {
    Write-Host "[niyah] train..."
    Ensure-Build
    $trainer = Join-Path $RepoRoot "niyah_train.exe"
    & $trainer $DataPath $Epochs $Lr $MinLr
}

function Invoke-Smoke {
    Write-Host "[niyah] smoke..."
    Ensure-Build
    $smoke = Join-Path $RepoRoot "Core_CPP\niyah_hybrid.exe"
    & $smoke --smoke
}

function Invoke-Bench {
    Write-Host "[niyah] bench..."
    Ensure-Build
    $bench = Join-Path $RepoRoot "Core_CPP\bench_niyah.exe"
    if (-not (Test-Path $bench)) {
        Write-Host "[niyah] building benchmark binary..."
        & powershell -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "scripts\build_msvc.ps1") -Config Release
    }
    & $bench
}

function Invoke-Save {
    Write-Host "[niyah] save..."
    Invoke-Train
    $saved = Join-Path $RepoRoot "niyah_trained.bin"
    if (Test-Path $saved) {
        Write-Host "[niyah] saved model to $saved"
    } else {
        Write-Host "[niyah] model output was not produced; check the trainer output."
    }
}

function Invoke-Run {
    Write-Host "[niyah] run..."
    Ensure-Build
    $hybrid = Join-Path $RepoRoot "Core_CPP\niyah_hybrid.exe"
    if (-not (Test-Path $hybrid)) {
        Write-Host "[niyah] building hybrid binary..."
        & powershell -ExecutionPolicy Bypass -File (Join-Path $RepoRoot "scripts\build_msvc.ps1") -Config Release
    }

    $inputText = @($Prompt, "quit") -join [Environment]::NewLine
    $inputText | & $hybrid --model $Model --interactive
}

switch ($Action) {
    "build" { Invoke-Build }
    "corpus" { Invoke-Corpus }
    "train" { Invoke-Train }
    "smoke" { Invoke-Smoke }
    "bench" { Invoke-Bench }
    "save" { Invoke-Save }
    "run" { Invoke-Run }
    "all" {
        Invoke-Build
        if (Test-Path (Join-Path $RepoRoot "Data_Training\sources")) {
            try { Invoke-Corpus } catch { Write-Host "[niyah] corpus skipped: $($_.Exception.Message)" }
        }
        Invoke-Train
        Invoke-Smoke
    }
}


