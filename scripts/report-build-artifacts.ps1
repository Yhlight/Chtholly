<#
.SYNOPSIS
Reports build artifact counts and sizes without modifying the build tree.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [string]$JsonOut = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$reporter = Join-Path $PSScriptRoot "report-build-artifacts.py"
if (-not (Test-Path -LiteralPath $reporter -PathType Leaf)) {
    [Console]::Error.WriteLine("stable-reason=build-artifact-report-script")
    [Console]::Error.WriteLine("Build artifact reporter was not found: $reporter")
    exit 1
}

$python = Get-Command python -ErrorAction SilentlyContinue
if ($null -eq $python) {
    [Console]::Error.WriteLine("stable-reason=build-artifact-report-python")
    [Console]::Error.WriteLine("Python was not found in PATH.")
    exit 1
}

$arguments = @($reporter, "--build-dir", $BuildDir)
if (-not [string]::IsNullOrWhiteSpace($JsonOut)) {
    $arguments += @("--json-out", $JsonOut)
}

& $python.Source @arguments
exit $LASTEXITCODE
