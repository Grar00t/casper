param(
    [Parameter(Position = 0)]
    [ValidateSet("build", "train", "train-head", "smoke", "bench", "run", "all")]
    [string]$Action = "build",
    [Parameter(Position = 1)] [string]$DataPath = "Data_Training/casper_teacher.jsonl",
    [Parameter(Position = 2)] [int]$Epochs = 12,
    [Parameter(Position = 3)] [double]$Lr = 0.0003,
    [double]$MinLr = 0.00003,
    [string]$Prompt = "bismillah",
    [string]$Model = "casper_trained.bin",
    [string]$Checkpoint = "casper_training.pt",
    [switch]$Resume
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

function Assert-ProcessSuccess([string]$Name, [int]$ExitCode) {
    if ($ExitCode -ne 0) {
        throw "[niyah] $Name failed (exit $ExitCode)."
    }
}

function Get-PythonCommand {
    if (Get-Command python -ErrorAction SilentlyContinue) {
        return @("python")
    }
    if (Get-Command py -ErrorAction SilentlyContinue) {
        return @("py", "-3")
    }
    throw "[niyah] Python 3 is required for full-model training."
}

function Assert-Bash {
    if (-not (Get-Command bash -ErrorAction SilentlyContinue)) {
        throw "[niyah] bash is required for C build/head-only compatibility training."
    }
}

function Invoke-Build([switch]$Smoke, [switch]$Bench) {
    Assert-Bash
    $buildArgs = @("scripts/build.sh", "--arch", "generic")
    if ($Smoke) { $buildArgs += "--smoke" }
    if ($Bench) { $buildArgs += "--bench" }
    & bash @buildArgs
    Assert-ProcessSuccess "build" $LASTEXITCODE
}

function Invoke-FullTrain {
    if (-not (Test-Path $DataPath)) {
        throw "[niyah] teacher JSONL missing: $DataPath"
    }
    $py = Get-PythonCommand
    $args = @(
        "tools/train_casper.py",
        $DataPath,
        "--epochs", [string]$Epochs,
        "--lr", [string]$Lr,
        "--output", $Model,
        "--checkpoint", $Checkpoint,
        "--device", "auto"
    )
    if ($Resume) { $args += "--resume" }

    if ($py.Count -eq 1) {
        & $py[0] @args
    }
    else {
        & $py[0] $py[1] @args
    }
    Assert-ProcessSuccess "full-model train" $LASTEXITCODE
}

function Invoke-HeadTrain {
    Assert-Bash
    Invoke-Build
    $env:NIYAH_DATA_PATH = $DataPath
    $env:NIYAH_EPOCHS = [string]$Epochs
    $env:NIYAH_LR = [string]$Lr
    $env:NIYAH_MIN_LR = [string]$MinLr
    try {
        & bash -c './build/trainer "$NIYAH_DATA_PATH" "$NIYAH_EPOCHS" "$NIYAH_LR" "$NIYAH_MIN_LR"'
        Assert-ProcessSuccess "head-only compatibility train" $LASTEXITCODE
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
    "build"      { Invoke-Build }
    "train"      { Invoke-FullTrain }
    "train-head" { Invoke-HeadTrain }
    "smoke"      { Invoke-Build -Smoke }
    "bench"      { Invoke-Build -Bench }
    "run"        { Invoke-Run }
    "all"        { Invoke-Build -Smoke; Invoke-FullTrain }
}
