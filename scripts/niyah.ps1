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

function ConvertTo-BashLiteral([string]$Value) {
    return "'" + $Value.Replace("'", "'\"'\"'") + "'"
}

function Invoke-Build([switch]$Smoke, [switch]$Bench) {
    $args = @("scripts/build.sh", "--arch", "generic")
    if ($Smoke) { $args += "--smoke" }
    if ($Bench) { $args += "--bench" }
    & bash @args
    Assert-ProcessSuccess "build" $LASTEXITCODE
}

function Invoke-Train {
    Invoke-Build
    $data = ConvertTo-BashLiteral $DataPath
    $command = "./build/trainer $data $Epochs $Lr $MinLr"
    & bash -c $command
    Assert-ProcessSuccess "train" $LASTEXITCODE
}

function Invoke-Run {
    Invoke-Build
    if (-not (Test-Path $Model)) {
        throw "[niyah] model missing: $Model"
    }

    $modelArg = ConvertTo-BashLiteral $Model
    $inputText = @($Prompt, "quit") -join [Environment]::NewLine
    $inputText | & bash -c "./build/niyah_hybrid --model $modelArg --interactive"
    Assert-ProcessSuccess "run" $LASTEXITCODE
}

switch ($Action) {
    "build" { Invoke-Build }
    "train" { Invoke-Train }
    "smoke" { Invoke-Build -Smoke }
    "bench" { Invoke-Build -Bench }
    "run" { Invoke-Run }
    "all" { Invoke-Build -Smoke; Invoke-Train }
}
