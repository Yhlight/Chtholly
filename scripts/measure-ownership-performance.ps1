[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Compiler,
    [string]$AsanCompiler = "",
    [string]$AsanResourceDir = "build-ninja/share/chtholly",
    [string]$Workspace = "examples/telemetry-pipeline",
    [int]$Repetitions = 5,
    [int]$AsanRepetitions = 3,
    [string]$OutputPath = "build-ninja/ownership-performance.json"
)

$ErrorActionPreference = "Stop"
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$workspacePath = if ([System.IO.Path]::IsPathRooted($Workspace)) {
    [System.IO.Path]::GetFullPath($Workspace)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repo $Workspace))
}
$output = if ([System.IO.Path]::IsPathRooted($OutputPath)) {
    [System.IO.Path]::GetFullPath($OutputPath)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repo $OutputPath))
}
$asanResourcePath = if ([System.IO.Path]::IsPathRooted($AsanResourceDir)) {
    [System.IO.Path]::GetFullPath($AsanResourceDir)
} else {
    [System.IO.Path]::GetFullPath((Join-Path $repo $AsanResourceDir))
}

function Invoke-Sample {
    param([string]$Executable, [int]$Count, [string]$Label,
          [ValidateSet("cold", "warm")][string]$Mode,
          [string]$ResourceDir = "")
    $samples = @()
    $temp = Join-Path ([System.IO.Path]::GetTempPath()) (
        "chtholly-ownership-" + [Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $temp | Out-Null
    try {
        Copy-Item -LiteralPath $workspacePath -Destination $temp -Recurse
        $project = Join-Path $temp (Split-Path $workspacePath -Leaf)
        $cache = Join-Path $project ".chtholly"
        $native = Join-Path $temp "native"
        $resourceArgs = if ([string]::IsNullOrWhiteSpace($ResourceDir)) {
            @()
        } else {
            @("--resource-dir", $ResourceDir)
        }
        if ($Mode -eq "warm") {
            $seedMetrics = Join-Path $temp "metrics-seed.json"
            & $Executable build --workspace $project --out-dir $native `
                --dump-analysis-metrics $seedMetrics @resourceArgs |
                Out-Null
            if ($LASTEXITCODE -ne 0) {
                throw "$Label compiler warm-up failed with exit code $LASTEXITCODE"
            }
        }
        for ($index = 0; $index -lt $Count; ++$index) {
            if ($Mode -eq "cold" -and (Test-Path -LiteralPath $cache)) {
                Remove-Item -LiteralPath $cache -Recurse -Force
            }
            $metrics = Join-Path $temp ("metrics-" + $index + ".json")
            $watch = [System.Diagnostics.Stopwatch]::StartNew()
            & $Executable build --workspace $project --out-dir $native `
                --dump-analysis-metrics $metrics @resourceArgs | Out-Null
            $exitCode = $LASTEXITCODE
            $watch.Stop()
            if ($exitCode -ne 0) {
                throw "$Label compiler build failed with exit code $exitCode"
            }
            $samples += [ordered]@{
                wall_ms = $watch.Elapsed.TotalMilliseconds
                analysis = Get-Content -LiteralPath $metrics -Raw |
                    ConvertFrom-Json -AsHashtable
            }
        }
    } finally {
        if (Test-Path -LiteralPath $temp) {
            Remove-Item -LiteralPath $temp -Recurse -Force
        }
    }
    $ordered = @($samples.wall_ms | Sort-Object)
    return [ordered]@{
        compiler = (& $Executable --version | Out-String).Trim()
        mode = $Mode
        repetitions = $Count
        median_wall_ms = $ordered[[int][Math]::Floor($ordered.Count / 2)]
        samples = $samples
    }
}

$report = [ordered]@{
    schema = "chtholly-ownership-performance-v1"
    workspace = $workspacePath
    normal = [ordered]@{
        cold = Invoke-Sample -Executable $Compiler -Count $Repetitions `
            -Label "normal" -Mode "cold"
        warm = Invoke-Sample -Executable $Compiler -Count $Repetitions `
            -Label "normal" -Mode "warm"
    }
}
if (-not [string]::IsNullOrWhiteSpace($AsanCompiler)) {
    if (-not (Test-Path -LiteralPath $asanResourcePath -PathType Container)) {
        throw "ASan target resource directory does not exist: $asanResourcePath"
    }
    $report.asan = [ordered]@{
        cold = Invoke-Sample -Executable $AsanCompiler `
            -Count $AsanRepetitions -Label "asan" -Mode "cold" `
            -ResourceDir $asanResourcePath
        warm = Invoke-Sample -Executable $AsanCompiler `
            -Count $AsanRepetitions -Label "asan" -Mode "warm" `
            -ResourceDir $asanResourcePath
    }
}

$parent = Split-Path -Parent $output
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$report | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $output -Encoding utf8
Write-Output $output
