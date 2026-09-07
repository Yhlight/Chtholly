[CmdletBinding()]
param(
  [string]$Distribution = "Ubuntu-24.04",
  [string]$SourceDir = (Get-Location).Path,
  [string]$RemoteRoot = "",
  [ValidateSet("linux", "asan", "tsan", "all")]
  [string]$BuildKind = "all",
  [switch]$FullSuite,
  [switch]$KeepRemote,
  [switch]$LongSoak,
  [string]$EvidenceDir = "build-wsl-evidence"
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$source = (Resolve-Path -LiteralPath $SourceDir).Path
$commit = (git -C $source rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw "Cannot identify source commit" }
function Quote-Bash([string]$Value) {
  return "'" + $Value.Replace("'", "'" + '"' + "'" + '"' + "'") + "'"
}
function Convert-ToWslPath([string]$Path) {
  $slash = $Path.Replace('\', '/')
  if ($slash -match '^([A-Za-z]):/(.*)$') {
    return "/mnt/$($Matches[1].ToLowerInvariant())/$($Matches[2])"
  }
  throw "Expected an absolute drive path: $Path"
}
if ([string]::IsNullOrWhiteSpace($RemoteRoot)) {
  $RemoteRoot = "/tmp/chtholly-native/$commit-$([Guid]::NewGuid().ToString('N'))"
}
if ($RemoteRoot -notmatch '^/tmp/chtholly-native/[A-Za-z0-9_-]+$') {
  throw "RemoteRoot must be one named child of /tmp/chtholly-native"
}
$remote = Quote-Bash $RemoteRoot
$sourceWsl = Quote-Bash (Convert-ToWslPath $source)
$evidence = [IO.Path]::GetFullPath($EvidenceDir)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$copy = "set -euo pipefail; mkdir -p /tmp/chtholly-native; " +
  "test `"`$(realpath /tmp/chtholly-native)`" = /tmp/chtholly-native; " +
  "test ! -e $remote; mkdir $remote; " +
  "git -C $sourceWsl archive $commit | tar -C $remote -xf -"
# A committed archive is immutable while Windows builds or later stages run.
# Evidence names this exact commit; uncommitted files are intentionally excluded.
& wsl.exe -d $Distribution -- bash -lc $copy
if ($LASTEXITCODE -ne 0) { throw "WSL source copy failed; inspect $RemoteRoot" }
$success = $false
try {
  $kinds = if ($BuildKind -eq "all") { @("linux", "asan", "tsan") } else { @($BuildKind) }
  foreach ($kind in $kinds) {
    $build = "build-$kind-abi2"
    $san = if ($kind -eq "linux") { "" } elseif ($kind -eq "asan") { "address" } else { "thread" }
    $configure = "cmake -S . -B $build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo " +
      "-DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18 " +
      "-DCMAKE_PREFIX_PATH=/usr/lib/llvm-18 -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm " +
      "-DClang_DIR=/usr/lib/llvm-18/lib/cmake/clang -DCHTHOLLY_SOURCE_COMMIT=$commit " +
      "-DCHTHOLLY_SANITIZER=$san"
    # Native Linux dependencies are resolved by CMake, never from Windows archives.
    $command = "set -euo pipefail; cd $remote; $configure; " +
      "cmake --build $build --target chtholly-test-suite --parallel 4; "
    $environment = "export ASAN_OPTIONS=halt_on_error=1:detect_leaks=1; export TSAN_OPTIONS=halt_on_error=1; "
    if ($kind -eq "asan") {
      $environment += "export LD_LIBRARY_PATH=`"`$(clang-18 -print-runtime-dir):`$`{LD_LIBRARY_PATH:-}`"; "
    }
    $selection = if ($FullSuite) { "" } else { "--label abi-2" }
    $command += $environment + "$build/tools/chtholly-test/chtholly-test run " +
      "--manifest $build/tests/chtholly-tests.generated.toml $selection --jobs 4 " +
      "--artifact-dir $build/evidence --format json"
    if ($LongSoak) {
      foreach ($seed in @(1,17,101)) {
        $command += "; python3 scripts/abi2-evidence.py --soak $build/tests/chtholly_component_abi2_unload_reload_soak " +
          "--provider $build/tests/libchtholly_component_abi2_pending_fixture.so --target x86_64-unknown-linux-gnu " +
          "--output $build/evidence/abi2-soak-$seed.json --seed $seed --min-cycles 100 --min-seconds 60 --gate"
      }
    }
    $dest = Join-Path $evidence $kind
    New-Item -ItemType Directory -Path $dest -Force | Out-Null
    & wsl.exe -d $Distribution -- bash -lc $command 2>&1 |
      Tee-Object -FilePath (Join-Path $dest "build-and-test.log")
    $status = $LASTEXITCODE
    $destWsl = Quote-Bash (Convert-ToWslPath $dest)
    & wsl.exe -d $Distribution -- bash -lc "set -eu; if test -d $remote/$build/evidence; then cp -a $remote/$build/evidence $destWsl/; fi"
    if ($status -ne 0) { throw "$kind failed; source and build retained at $RemoteRoot" }
  }
  $success = $true
} finally {
  if ($success -and -not $KeepRemote) {
    & wsl.exe -d $Distribution -- bash -lc "set -eu; test `"`$(realpath $remote)`" = $remote; rm -rf -- $remote"
    if ($LASTEXITCODE -ne 0) { throw "Could not remove verified temporary source directory" }
  } else {
    Write-Host "WSL source/build retained: $RemoteRoot"
  }
}
