param(
  [string]$Distribution = "Ubuntu-24.04",
  [string]$SourceDir = (Get-Location).Path,
  [string]$Evidence = "build-ninja/telemetry-wsl-evidence.json"
)

# Linux telemetry evidence runs on WSL ext4 rather than /mnt to avoid the
# known cross-filesystem slowdown. This script is opt-in and does not run in
# the Windows preview manifest.
$source = (Resolve-Path -LiteralPath $SourceDir).Path
$commit = (git -C $source rev-parse HEAD).Trim()
$remote = "/tmp/chtholly-telemetry-$commit"
$linuxSource = ($source -replace '\\', '/')
$command = @"
set -eu
rm -rf '$remote'
mkdir -p '$remote'
cp -a '$linuxSource/.' '$remote/'
cd '$remote'
cmake --preset linux-wsl-clang-ninja
cmake --build build-wsl --target chtholly-test-suite --parallel 4
python3 scripts/telemetry-wsl-evidence.py --source-dir '$remote' --output '$remote/evidence.json'
cat '$remote/evidence.json'
"@
$output = & wsl.exe -d $Distribution -- bash -lc $command
if ($LASTEXITCODE -ne 0) {
  throw "WSL telemetry validation failed for $commit"
}
$parent = Split-Path -Parent $Evidence
New-Item -ItemType Directory -Path $parent -Force | Out-Null
$evidencePath = [IO.Path]::GetFullPath($Evidence)
[IO.File]::WriteAllText($evidencePath, ($output -join [Environment]::NewLine))
