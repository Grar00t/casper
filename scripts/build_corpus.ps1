param(
    [string]$SourceRoot = "Data_Training/sources",
    [string]$Output = "Data_Training/sovereign_knowledge.txt"
)

$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")

$srcRoot = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $SourceRoot))
$outFile = [System.IO.Path]::GetFullPath((Join-Path (Get-Location) $Output))

if (-not (Test-Path $srcRoot -PathType Container)) {
    throw "[corpus] source directory not found: $srcRoot"
}

$files = @(Get-ChildItem -Path $srcRoot -Filter *.txt -File -Recurse | Sort-Object FullName)
if ($files.Count -eq 0) {
    throw "[corpus] no .txt source files found under $srcRoot"
}

$outDir = Split-Path -Parent $outFile
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$encoding = [System.Text.UTF8Encoding]::new($false)
$writer = [System.IO.StreamWriter]::new($outFile, $false, $encoding)
$written = 0
$skipped = 0

try {
    foreach ($file in $files) {
        foreach ($line in [System.IO.File]::ReadLines($file.FullName, $encoding)) {
            $trimmed = $line.Trim()
            if ($trimmed.Length -lt 2 -or $trimmed.StartsWith("#")) {
                $skipped++
                continue
            }
            if ($seen.Add($trimmed)) {
                $writer.WriteLine($trimmed)
                $written++
            } else {
                $skipped++
            }
        }
    }
}
finally {
    $writer.Dispose()
}

Write-Host "[corpus] files=$($files.Count) lines=$written skipped=$skipped bytes=$((Get-Item $outFile).Length)"
