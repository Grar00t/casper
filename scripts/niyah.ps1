param(
    [Parameter(Position = 0)]
    [ValidateSet("build", "train", "smoke", "bench", "run", "all")]
    [string]$Action = "build",
    [Parameter(Position = 1)] [string]$DataPath = "Data_Training/sovereign_knowledge.txt",
    [Parameter(Position = 2)] [int]$Epochs = 3,
    [Parameter(Position = 3)] [double]$Lr = 0.001,
    [double]$MinLr = 0.0001,
    [string]$Prompt = "bismillah",
    [string]$Model = "niyah_trained.bin"
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

if (-not (Get-Command bash -ErrorAction SilentlyContinue)) {
    throw "[niyah] bash is required. Use Git Bash, WSL, or another bash environment with gcc/clang available."
}

function Assert-ProcessSuccess([string]$Name, [int]$ExitCode) {
    if ($ExitCode -ne 0) {
        throw "[niyah] $Name failed (exit $ExitCode)."
    }
}

function Invoke-Build([switch]$Smoke, [switch]$Bench) {
    $buildArgs = @("scripts/build.sh", "--arch", "generic")
    if ($Smoke) { $buildArgs += "--smoke" }
    if ($Bench) { $buildArgs += "--bench" }
    & bash @buildArgs
    Assert-ProcessSuccess "build" $LASTEXITCODE
}

function Invoke-Train {
    Invoke-Build
    $env:NIYAH_DATA_PATH = $DataPath
    $env:NIYAH_EPOCHS = [string]$Epochs
    $env:NIYAH_LR = [string]$Lr
    $env:NIYAH_MIN_LR = [string]$MinLr
    try {
        & bash -c './build/trainer "$NIYAH_DATA_PATH" "$NIYAH_EPOCHS" "$NIYAH_LR" "$NIYAH_MIN_LR"'
        Assert-ProcessSuccess "train" $LASTEXITCODE
    }
    finally {
        Remove-Item Env:NIYAH_DATA_PATH, Env:NIYAH_EPOCHS, Env:NIYAH_LR, Env:NIYAH_MIN_LR -ErrorAction SilentlyContinue
    }
}

function Invoke-Run {
    Invoke-Build
    if (-not (Test-Path $Model)) {
        throw "[niyah] model missing: $Model"
    }

    $env:NIYAH_MODEL_PATH = $Model
    try {
        $inputText = @($Prompt, "quit") -join [Environment]::NewLine
        $inputText | & bash -c './build/niyah_hybrid --model "$NIYAH_MODEL_PATH" --interactive'
        Assert-ProcessSuccess "run" $LASTEXITCODE
    }
    finally {
        Remove-Item Env:NIYAH_MODEL_PATH -ErrorAction SilentlyContinue
    }
}

switch ($Action) {
    "build" { Invoke-Build }
    "train" { Invoke-Train }
    "smoke" { Invoke-Build -Smoke }
    "bench" { Invoke-Build -Bench }
    "run" { Invoke-Run }
    "all" { Invoke-Build -Smoke; Invoke-Train }
}
