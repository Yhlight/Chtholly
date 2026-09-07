param(
    [Parameter(Mandatory = $true)]
    [string]$OutputPath
)

$ErrorActionPreference = "Stop"
$extensionRoot = Join-Path $PSScriptRoot "..\editors\vscode"
$resolvedRoot = (Resolve-Path -LiteralPath $extensionRoot).Path
$resolvedOutput = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $resolvedOutput
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null

Push-Location $resolvedRoot
try {
    & npm ci --ignore-scripts
    if ($LASTEXITCODE -ne 0) {
        throw "npm ci failed with exit code $LASTEXITCODE"
    }
    & npm run compile
    if ($LASTEXITCODE -ne 0) {
        throw "extension bundling failed with exit code $LASTEXITCODE"
    }
    & npx --no-install vsce package --out $resolvedOutput
    if ($LASTEXITCODE -ne 0) {
        throw "VSIX packaging failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
