param(
    [Parameter(Position = 0)]
    [ValidateSet("build", "corpus", "train", "smoke", "bench", "run", "all")]
    [string]$Action = "build",
    [Parameter(Position = 1)] [string]$DataPath = "Data_Training/sovereign_knowledge.txt",
    [Parameter(Position = 2)] [int]$Epochs = 3,
    [Parameter(Position = 3)] [double]$Lr = 0.001,
    [double]$MinLr = 0.0001,
    [string]$Prompt = "hello",
    [string]$Model = "niyah_trained.bin"
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
$RepoRoot = (Get-Location).Path

function Assert-Exit([string]$Name) {
    if ($LASTEXITCODE -ne 0) { throw "[niyah] $Name failed with exit code $LASTEXITCODE" }
}

function Resolve-BuildArtifact([string]$Name) {
    $candidates = @(
        (Join-Path $RepoRoot "build\$Name"),
        (Join-Path $RepoRoot "build\$Name.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path $candidate -PathType Leaf) { return $candidate }
    }
    return $null
}

function Invoke-Build {
    $script = Join-Path $RepoRoot "scripts\build.sh"
    if (-not (Test-Path $script)) { throw "[niyah] build script missing: $script" }
    if (-not (Get-Command bash -ErrorAction SilentlyContinue)) { throw "[niyah] bash is required" }
    & bash $script --release --arch generic
    Assert-Exit "build"
}

function Require-Artifact([string]$Name) {
    $artifact = Resolve-BuildArtifact $Name
    if (-not $artifact) {
        Invoke-Build
        $artifact = Resolve-BuildArtifact $Name
    }
    if (-not $artifact) { throw "[niyah] build artifact missing: $Name" }
    return $artifact
}

function Invoke-Corpus {
    & (Join-Path $PSScriptRoot "build_corpus.ps1")
    Assert-Exit "corpus"
}

function Invoke-Train {
    $trainer = Require-Artifact "trainer"
    if (-not (Test-Path $DataPath -PathType Leaf)) { throw "[niyah] data file missing: $DataPath" }
    & $trainer $DataPath $Epochs $Lr $MinLr
    Assert-Exit "train"
}

function Invoke-Smoke {
    $hybrid = Require-Artifact "niyah_hybrid"
    & $hybrid --smoke
    Assert-Exit "smoke"
}

function Invoke-Bench {
    $bench = Require-Artifact "bench_niyah"
    & $bench
    Assert-Exit "bench"
}

function Invoke-Run {
    $hybrid = Require-Artifact "niyah_hybrid"
    if (-not (Test-Path $Model -PathType Leaf)) { throw "[niyah] model missing: $Model" }
    @($Prompt, "quit") | & $hybrid --model $Model
    Assert-Exit "run"
}

switch ($Action) {
    "build"  { Invoke-Build }
    "corpus" { Invoke-Corpus }
    "train"  { Invoke-Train }
    "smoke"  { Invoke-Smoke }
    "bench"  { Invoke-Bench }
    "run"    { Invoke-Run }
    "all"    { Invoke-Build; Invoke-Smoke }
}
