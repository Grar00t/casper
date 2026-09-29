param(
    [Parameter(Position = 0)]
    [ValidateSet("build", "train", "train-c", "smoke", "bench", "run", "all")]
    [string]$Action = "build",
    [Parameter(Position = 1)] [string]$DataPath = "",
    [Parameter(Position = 2)] [int]$Epochs = 12,
    [Parameter(Position = 3)] [double]$Lr = 0.0003,
    [double]$MinLr = 0.00003,
    [int]$BatchSize = 4,
    [int]$GradAccum = 4,
    [int]$CtxLen = 256,
    [int]$EmbedDim = 128,
    [int]$Layers = 4,
    [int]$Heads = 8,
    [int]$KvHeads = 4,
    [string]$Device = "auto",
    [string]$Checkpoint = "casper_training.pt",
    [string]$Prompt = "bismillah",
    [string]$Model = "casper_trained.bin",
    [switch]$Resume
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

function Assert-ProcessSuccess([string]$Name, [int]$ExitCode) {
    if ($ExitCode -ne 0) {
        throw "[niyah] $Name failed (exit $ExitCode)."
    }
}

function Assert-Bash {
    if (-not (Get-Command bash -ErrorAction SilentlyContinue)) {
        throw "[niyah] bash is required for C build/run and train-c actions."
    }
}

function Get-PythonCommand {
    if (Get-Command python -ErrorAction SilentlyContinue) {
        return @("python")
    }
    if (Get-Command py -ErrorAction SilentlyContinue) {
        return @("py", "-3")
    }
    throw "[niyah] Python 3 is required for full-model GPU training."
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
    if ([string]::IsNullOrWhiteSpace($DataPath)) {
        throw "[niyah] train requires a teacher JSONL path."
    }
    if (-not (Test-Path $DataPath)) {
        throw "[niyah] teacher JSONL missing: $DataPath"
    }
    if ($Device -notin @("auto", "cuda", "cpu")) {
        throw "[niyah] Device must be auto, cuda, or cpu."
    }

    $py = Get-PythonCommand
    $trainArgs = @(
        "tools/train_casper.py",
        $DataPath,
        "--epochs", [string]$Epochs,
        "--lr", [string]$Lr,
        "--batch-size", [string]$BatchSize,
        "--grad-accum", [string]$GradAccum,
        "--ctx-len", [string]$CtxLen,
        "--embed-dim", [string]$EmbedDim,
        "--layers", [string]$Layers,
        "--heads", [string]$Heads,
        "--kv-heads", [string]$KvHeads,
        "--device", $Device,
        "--output", $Model,
        "--checkpoint", $Checkpoint
    )
    if ($Resume) { $trainArgs += "--resume" }

    if ($py.Count -eq 1) {
        & $py[0] @trainArgs
    }
    else {
        & $py[0] $py[1] @trainArgs
    }
    Assert-ProcessSuccess "full-model train" $LASTEXITCODE
}

function Invoke-CTrain {
    Assert-Bash
    Invoke-Build
    $source = $DataPath
    if ([string]::IsNullOrWhiteSpace($source)) {
        $source = "Data_Training/sovereign_knowledge.txt"
    }
    if (-not (Test-Path $source)) {
        throw "[niyah] C training data missing: $source"
    }

    $env:NIYAH_DATA_PATH = $source
    $env:NIYAH_EPOCHS = [string]$Epochs
    $env:NIYAH_LR = [string]$Lr
    $env:NIYAH_MIN_LR = [string]$MinLr
    try {
        & bash -c './build/trainer "$NIYAH_DATA_PATH" "$NIYAH_EPOCHS" "$NIYAH_LR" "$NIYAH_MIN_LR"'
        Assert-ProcessSuccess "detached-KV C train" $LASTEXITCODE
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
    "build"   { Invoke-Build }
    "train"   { Invoke-FullTrain }
    "train-c" { Invoke-CTrain }
    "smoke"   { Invoke-Build -Smoke }
    "bench"   { Invoke-Build -Bench }
    "run"     { Invoke-Run }
    "all"     { Invoke-Build -Smoke; Invoke-FullTrain }
}
