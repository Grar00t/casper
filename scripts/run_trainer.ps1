$ErrorActionPreference = "Stop"

Set-Location (Join-Path $PSScriptRoot "..")

$exe = "Core_CPP\trainer.exe"
$data = "Data_Training\sovereign_knowledge.txt"

if (-not (Test-Path $exe)) {
    Write-Host "[Casper] trainer.exe not found, building first..."
    & (Join-Path $PSScriptRoot "build_trainer.ps1")
}

Write-Host "[Casper] Running trainer..."
& ".\$exe" $data

