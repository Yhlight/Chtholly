<#
.SYNOPSIS
Configures, builds, and tests the Chtholly compiler.

.DESCRIPTION
The default path is tuned for Chtholly development on Windows:
Ninja + RelWithDebInfo + sccache + MSVC.  When this combination is used,
the script probes sccache's real MSVC /showIncludes prefix and patches the
generated Ninja rules so Ninja and sccache can share correct header deps.

.EXAMPLE
./scripts/build.ps1

.EXAMPLE
./scripts/build.ps1 -Target chtholly_borrow_tests -SkipTests

.EXAMPLE
./scripts/build.ps1 -Config Release -CleanFirst -ShowSccacheStats

.EXAMPLE
./scripts/build.ps1 -ReportBuildArtifacts -BuildArtifactReportPath artifacts/build-artifacts.json
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$SourceDir = "",
    [string]$BuildDir = "build-ninja",
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Config = "RelWithDebInfo",
    [string]$Target = "",
    [string]$Generator = "Ninja",
    [string]$CompilerLauncher = "sccache",
    [ValidateSet("", "address", "address,undefined", "thread")]
    [string]$Sanitizer = "",
    [string]$VcpkgRoot = "",
    [string]$VcVarsPath = "",
    [switch]$SkipVSImport,
    [switch]$ConfigureOnly,
    [switch]$SkipBuild,
    [switch]$SkipTests,
    [switch]$NoResetStaleBuildDir,
    [switch]$CleanFirst,
    [switch]$VerboseBuild,
    [switch]$FullDebugArtifacts,
    [switch]$WarningsAsErrors,
    [switch]$ReportBuildArtifacts,
    [string]$BuildArtifactReportPath = "",
    [switch]$ShowSccacheStats,
    [switch]$VerifyNinjaDeps,
    [string]$VerifyNinjaDepsObject = "",
    [int]$Parallel = 0,
    [int]$TestParallel = 0,
    [switch]$SkipSccacheMsvcDepsPrefixFix,
    [int]$NinjaRetryCount = 2,
    [int]$NinjaRetryDelaySeconds = 2,
    [string[]]$CMakeArgs = @(),
    [string[]]$BuildArgs = @(),
    [string[]]$AdditionalTestArgs = @()
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
. (Join-Path $PSScriptRoot "build-cache-contract.ps1")

$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
$script:IsWindowsHost = if (Test-Path variable:IsWindows) { $IsWindows } else { $env:OS -eq "Windows_NT" }
$script:BuildLockStream = $null
$script:BuildLockPath = ""

function Resolve-ChthollyPath {
    param(
        [AllowEmptyString()][string]$Path,
        [string]$BasePath,
        [switch]$DefaultToBase
    )

    if ([string]::IsNullOrWhiteSpace($Path)) {
        if ($DefaultToBase) {
            return [System.IO.Path]::GetFullPath($BasePath)
        }
        throw "Expected a path value."
    }

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Test-NinjaGenerator {
    param([string]$ResolvedGenerator)

    return $ResolvedGenerator -like "Ninja*"
}

function Test-SingleConfigGenerator {
    param([string]$ResolvedGenerator)

    return $ResolvedGenerator -in @("Ninja", "NMake Makefiles", "Unix Makefiles", "MinGW Makefiles")
}

function Resolve-CompilerLauncher {
    param([AllowNull()][string]$Launcher)

    if ([string]::IsNullOrWhiteSpace($Launcher)) {
        return ""
    }

    $trimmed = $Launcher.Trim()
    if ($trimmed.ToLowerInvariant() -in @("none", "off", "false", "disabled")) {
        return ""
    }

    return $trimmed
}

function Test-SccacheLauncher {
    param([AllowNull()][string]$Launcher)

    if ([string]::IsNullOrWhiteSpace($Launcher)) {
        return $false
    }

    $launcherName = [System.IO.Path]::GetFileNameWithoutExtension($Launcher.Trim())
    return $launcherName -ieq "sccache"
}

function Resolve-CommandSource {
    param([string]$Name)

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $command) {
        return $null
    }

    if (-not [string]::IsNullOrWhiteSpace($command.Source)) {
        return $command.Source
    }

    return $command.Path
}

function Assert-Tool {
    param(
        [string]$Name,
        [string]$ErrorMessage
    )

    $source = Resolve-CommandSource -Name $Name
    if ([string]::IsNullOrWhiteSpace($source)) {
        throw $ErrorMessage
    }

    return $source
}

function Resolve-VcpkgRoot {
    $candidates = @()
    # Prefer the repository-pinned checkout when present. Environment-level
    # VCPKG_ROOT values may point at an unrelated installation (for example
    # the Visual Studio bundled checkout) and can invalidate the manifest
    # baseline or the CMake cache. An explicit -VcpkgRoot still wins.
    if (-not [string]::IsNullOrWhiteSpace($VcpkgRoot)) {
        $candidates += Resolve-ChthollyPath -Path $VcpkgRoot -BasePath $script:ResolvedSourceDir
    }
    $candidates += Join-Path $script:ResolvedSourceDir ".vcpkg"
    if (-not [string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
        $candidates += Resolve-ChthollyPath -Path $env:VCPKG_ROOT -BasePath $script:ResolvedSourceDir
    }
    $candidates += Join-Path $script:ResolvedSourceDir "third_party/vcpkg"
    foreach ($candidate in $candidates) {
        $toolchain = Join-Path $candidate "scripts/buildsystems/vcpkg.cmake"
        if (Test-Path -LiteralPath $toolchain -PathType Leaf) {
            return [System.IO.Path]::GetFullPath($candidate)
        }
    }
    throw "vcpkg was not found. Set VCPKG_ROOT, pass -VcpkgRoot, or bootstrap the pinned vcpkg checkout at third_party/vcpkg."
}

function Get-LastNativeExitCode {
    $variable = Get-Variable -Name LASTEXITCODE -ErrorAction SilentlyContinue
    if ($null -eq $variable) {
        return 0
    }

    return [int]$variable.Value
}

function Invoke-NativeStep {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$WorkingDirectory = $script:ResolvedSourceDir
    )

    Write-Host "==> $Name"
    Push-Location $WorkingDirectory
    try {
        & $FilePath @Arguments
        $exitCode = Get-LastNativeExitCode
        if ($exitCode -ne 0) {
            throw "$Name failed with exit code $exitCode"
        }
    } finally {
        Pop-Location
    }
}

function Test-RetryableWindowsBuildFailure {
    param([string]$Message)

    if (-not $script:IsWindowsHost) {
        return $false
    }

    if ([string]::IsNullOrWhiteSpace($Message)) {
        return $false
    }

    $normalized = $Message.ToLowerInvariant()
    return $normalized.Contains("permission denied") -or
        $normalized.Contains("access is denied") -or
        $normalized.Contains("拒绝访问") -or
        $normalized.Contains("cannot open compiler generated file") -or
        $normalized.Contains("c1083") -or
        $normalized.Contains("c1041")
}

function Invoke-RetryableBuildStep {
    param([string[]]$BuildStepArgs)

    $attempt = 0
    $maxAttempts = [Math]::Max(1, $NinjaRetryCount + 1)
    $lastFailure = $null

    while ($attempt -lt $maxAttempts) {
        ++$attempt
        Write-Host "==> Build"
        Push-Location $script:ResolvedSourceDir
        try {
            $result = Invoke-CapturedProcess -FilePath $script:CMakePath -Arguments $BuildStepArgs
            if ($result.ExitCode -eq 0) {
                if (-not [string]::IsNullOrEmpty($result.Stdout)) {
                    Write-Host $result.Stdout.TrimEnd()
                }
                if (-not [string]::IsNullOrEmpty($result.Stderr)) {
                    Write-Host $result.Stderr.TrimEnd()
                }
                return
            }

            $combined = ($result.Stdout + "`n" + $result.Stderr).Trim()
            $lastFailure = "Build failed with exit code $($result.ExitCode)`n$combined"

            if ($attempt -lt $maxAttempts -and (Test-RetryableWindowsBuildFailure -Message $combined)) {
                Write-Warning "Build hit a retryable MSVC/Ninja file access failure; retrying in $NinjaRetryDelaySeconds second(s) (attempt $attempt of $maxAttempts)."
                Start-Sleep -Seconds $NinjaRetryDelaySeconds
                continue
            }

            throw $lastFailure
        } finally {
            Pop-Location
        }
    }

    throw $lastFailure
}

function ConvertTo-ProcessArgumentString {
    param([string[]]$Arguments)

    $quotedArguments = @()
    foreach ($argument in $Arguments) {
        if ($null -eq $argument) {
            $argument = ""
        }

        if ($argument.Length -gt 0 -and $argument -notmatch '[\s"]') {
            $quotedArguments += $argument
            continue
        }

        $builder = [System.Text.StringBuilder]::new()
        [void]$builder.Append('"')
        $backslashes = 0
        foreach ($character in $argument.ToCharArray()) {
            if ($character -eq '\') {
                ++$backslashes
                continue
            }

            if ($character -eq '"') {
                if ($backslashes -gt 0) {
                    [void]$builder.Append('\' * ($backslashes * 2))
                    $backslashes = 0
                }
                [void]$builder.Append('\"')
                continue
            }

            if ($backslashes -gt 0) {
                [void]$builder.Append('\' * $backslashes)
                $backslashes = 0
            }
            [void]$builder.Append($character)
        }

        if ($backslashes -gt 0) {
            [void]$builder.Append('\' * ($backslashes * 2))
        }
        [void]$builder.Append('"')
        $quotedArguments += $builder.ToString()
    }

    return ($quotedArguments -join ' ')
}

function Write-Utf8NoBomFile {
    param(
        [string]$Path,
        [AllowNull()][string]$Value
    )

    if ($null -eq $Value) {
        $Value = ""
    }
    [System.IO.File]::WriteAllText($Path, $Value, [System.Text.UTF8Encoding]::new($false))
}

function Invoke-CapturedProcess {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$WorkingDirectory = $script:ResolvedSourceDir,
        [hashtable]$EnvironmentOverrides = @{}
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false, $false)
    $startInfo.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false, $false)
    if ($null -ne $startInfo.GetType().GetProperty("ArgumentList")) {
        foreach ($argument in $Arguments) {
            [void]$startInfo.ArgumentList.Add($argument)
        }
    } else {
        $startInfo.Arguments = ConvertTo-ProcessArgumentString -Arguments $Arguments
    }
    foreach ($entry in $EnvironmentOverrides.GetEnumerator()) {
        $startInfo.Environment[$entry.Key] = [string]$entry.Value
    }

    $process = [System.Diagnostics.Process]::Start($startInfo)
    try {
        $stdout = $process.StandardOutput.ReadToEnd()
        $stderr = $process.StandardError.ReadToEnd()
        $process.WaitForExit()

        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdout
            Stderr = $stderr
        }
    } finally {
        if ($null -ne $process) {
            $process.Dispose()
        }
    }
}

function Get-ChthollyBuildLockPath {
    return Join-Path $script:ResolvedBuildDir ".chtholly-build.lock"
}

function Close-ChthollyBuildLock {
    if ($null -ne $script:BuildLockStream) {
        try {
            $script:BuildLockStream.Dispose()
        } catch {
        }
        $script:BuildLockStream = $null
    }

    if (-not [string]::IsNullOrWhiteSpace($script:BuildLockPath)) {
        Remove-Item -LiteralPath $script:BuildLockPath -Force -ErrorAction SilentlyContinue
        $script:BuildLockPath = ""
    }
}

function Get-CommandLineFromProcess {
    param([uint32]$ProcessId)

    try {
        $process = Get-CimInstance Win32_Process -Filter "ProcessId = $ProcessId" -ErrorAction Stop
        return [string]$process.CommandLine
    } catch {
        return ""
    }
}

function Stop-ChthollyBuildDirResidualProcesses {
    if (-not $script:IsWindowsHost) {
        return
    }

    $targetExecutables = @("ninja", "cmake", "cl", "link", "rc", "mt", "sccache")
    $buildDirNeedle = $script:ResolvedBuildDir.ToLowerInvariant()
    $killed = @()

    foreach ($process in Get-Process -ErrorAction SilentlyContinue) {
        try {
            if ($process.Id -eq $PID) {
                continue
            }

            if ($targetExecutables -notcontains $process.ProcessName.ToLowerInvariant()) {
                continue
            }

            $commandLine = Get-CommandLineFromProcess -ProcessId ([uint32]$process.Id)
            if ([string]::IsNullOrWhiteSpace($commandLine)) {
                continue
            }

            if ($commandLine.ToLowerInvariant().Contains($buildDirNeedle)) {
                Stop-Process -Id $process.Id -Force -ErrorAction Stop
                $killed += [pscustomobject]@{
                    Id = $process.Id
                    Name = $process.ProcessName
                    CommandLine = $commandLine
                }
            }
        } catch {
        }
    }

    if ($killed.Count -gt 0) {
        Write-Host "==> Cleared residual build processes"
        foreach ($entry in $killed) {
            $summary = $entry.CommandLine
            if ($summary.Length -gt 160) {
                $summary = $summary.Substring(0, 160) + "..."
            }
            Write-Host ("PID {0} [{1}] {2}" -f $entry.Id, $entry.Name, $summary)
        }
    }
}

function Acquire-ChthollyBuildLock {
    if (-not (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator)) {
        return
    }

    New-Item -ItemType Directory -Path $script:ResolvedBuildDir -Force | Out-Null
    $script:BuildLockPath = Get-ChthollyBuildLockPath
    $lockTimeout = [TimeSpan]::FromSeconds(15)
    $poll = [TimeSpan]::FromMilliseconds(250)
    $deadline = [DateTime]::UtcNow.Add($lockTimeout)

    while ($true) {
        try {
            $script:BuildLockStream = [System.IO.File]::Open(
                $script:BuildLockPath,
                [System.IO.FileMode]::OpenOrCreate,
                [System.IO.FileAccess]::ReadWrite,
                [System.IO.FileShare]::None)
            break
        } catch {
            if ([DateTime]::UtcNow -ge $deadline) {
                throw "Timed out waiting for build lock for '$script:ResolvedBuildDir'. Another build may still be running. Wait for that process to finish or use a different build directory."
            }
            Start-Sleep -Milliseconds $poll.TotalMilliseconds
        }
    }
}

function Reset-EmptyBuildDirectoryIfNeeded {
    if (-not (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator)) {
        return
    }

    if (-not (Test-Path -LiteralPath $script:ResolvedBuildDir)) {
        return
    }

    $cachePath = Join-Path $script:ResolvedBuildDir "CMakeCache.txt"
    $hasCache = Test-Path -LiteralPath $cachePath
    $buildNinjaPath = Join-Path $script:ResolvedBuildDir "build.ninja"
    $rulesNinjaPath = Join-Path $script:ResolvedBuildDir "CMakeFiles\rules.ninja"
    $configureLooksIncomplete = -not $hasCache

    if ($hasCache) {
        $compilerWorks = Get-CMakeCacheValue -Name "CMAKE_CXX_COMPILER_WORKS"
        $configureLooksIncomplete = ($compilerWorks -eq "FALSE") -or
            -not (Test-Path -LiteralPath $buildNinjaPath) -or
            -not (Test-Path -LiteralPath $rulesNinjaPath)
    }

    if (-not $configureLooksIncomplete) {
        return
    }

    $entries = @(Get-ChildItem -LiteralPath $script:ResolvedBuildDir -Force -ErrorAction SilentlyContinue)
    $nonLockEntries = @(
        $entries | Where-Object { $_.Name -ne ".chtholly-build.lock" }
    )
    if ($nonLockEntries.Count -eq 0) {
        return
    }

    Write-Host "==> Recreate incomplete Ninja build directory"
    foreach ($entry in $nonLockEntries) {
        Remove-Item -LiteralPath $entry.FullName -Recurse -Force
    }
}

function Get-VcVars64Candidates {
    $candidates = @()

    if (-not [string]::IsNullOrWhiteSpace($VcVarsPath)) {
        $candidates += $VcVarsPath
    }

    if (-not [string]::IsNullOrWhiteSpace($env:CHTHOLLY_VCVARS_PATH)) {
        $candidates += $env:CHTHOLLY_VCVARS_PATH
    }

    $vsWhereCandidates = @(
        (Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFilesX86)) "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)) "Microsoft Visual Studio\Installer\vswhere.exe")
    )

    foreach ($vsWhere in $vsWhereCandidates) {
        if (-not (Test-Path -LiteralPath $vsWhere)) {
            continue
        }

        $installPath = & $vsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null |
            Select-Object -First 1
        if ((Get-LastNativeExitCode) -eq 0 -and -not [string]::IsNullOrWhiteSpace($installPath)) {
            $candidates += (Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat")
        }
    }

    $programFiles = [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
    $candidates += @(
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"),
        (Join-Path $programFiles "Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
    )

    return $candidates | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Select-Object -Unique
}

function Resolve-VcVars64Path {
    foreach ($candidate in (Get-VcVars64Candidates)) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    return $null
}

function Import-VS64 {
    param([AllowNull()][string]$Path = $script:ResolvedVcVarsPath)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        $Path = Resolve-VcVars64Path
    }

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path)) {
        throw "Could not find vcvars64.bat; install MSVC Build Tools, set CHTHOLLY_VCVARS_PATH, or pass -VcVarsPath."
    }

    Write-Host "==> Import MSVC environment"
    $vcvarsCommand = 'call "' + $Path + '" >nul && set'
    & cmd.exe /d /s /c $vcvarsCommand | ForEach-Object {
        if ($_ -match "=") {
            $name, $value = $_ -split "=", 2
            Set-Item "env:$name" $value
        }
    }
}

function Test-MSVCEnvironmentReady {
    if ($null -eq (Get-Command "cl.exe" -ErrorAction SilentlyContinue)) {
        return $false
    }

    if ([string]::IsNullOrWhiteSpace($env:INCLUDE)) {
        return $false
    }

    return $env:INCLUDE -like "*VC\Tools\MSVC*\include*"
}

function Test-PreloadedVisualStudioSession {
    if (-not (Test-MSVCEnvironmentReady)) {
        return $false
    }

    return -not [string]::IsNullOrWhiteSpace($env:VSCMD_VER) -or
        -not [string]::IsNullOrWhiteSpace($env:VSINSTALLDIR) -or
        -not [string]::IsNullOrWhiteSpace($env:VisualStudioVersion)
}

function Write-PreloadedVisualStudioSessionWarning {
    if (-not $script:IsWindowsHost) {
        return
    }

    if (-not (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator)) {
        return
    }

    if (-not (Test-PreloadedVisualStudioSession)) {
        return
    }

    Write-Warning "Detected a preloaded Visual Studio/MSVC environment in the current PowerShell session (possibly inherited from a parent shell or primed via 'vs64'). Reusing a preloaded session can leave build directories in a half-configured state after failures. Prefer a fresh shell and let build.ps1 import MSVC itself."
}

function Get-CMakeCacheValue {
    param([string]$Name)

    $cachePath = Join-Path $script:ResolvedBuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath)) {
        return $null
    }

    $escapedName = [regex]::Escape($Name)
    foreach ($line in (Get-Content -LiteralPath $cachePath)) {
        if ($line -match "^${escapedName}:[^=]*=(.*)$") {
            return $Matches[1]
        }
    }

    return $null
}

function Get-CMakeDefinitionOverride {
    param([string]$Name)

    $escapedName = [regex]::Escape($Name)
    foreach ($argument in $CMakeArgs) {
        if ($argument -match "^-D${escapedName}(?::[^=]+)?=(.*)$") {
            return [string]$Matches[1]
        }
    }
    return $null
}

function Normalize-CMakeBuildStatePath {
    param([AllowNull()][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }

    $normalized = $Path.Trim().Replace('\', '/')
    while ($normalized.Length -gt 1 -and $normalized.EndsWith('/')) {
        $normalized = $normalized.Substring(0, $normalized.Length - 1)
    }
    return $normalized
}

function Test-WindowsDriveCachePath {
    param([AllowNull()][string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return $false
    }

    return $Path -match '^[A-Za-z]:[\\/]'
}

function Test-CMakeBuildStatePathMatch {
    param(
        [AllowNull()][string]$CachedPath,
        [string]$ExpectedPath
    )

    $cached = Normalize-CMakeBuildStatePath -Path $CachedPath
    $expected = Normalize-CMakeBuildStatePath -Path $ExpectedPath
    if ($script:IsWindowsHost) {
        return [string]::Equals($cached, $expected, [System.StringComparison]::OrdinalIgnoreCase)
    }

    return [string]::Equals($cached, $expected, [System.StringComparison]::Ordinal)
}

function Test-CMakeBuildDirectoryContainsSource {
    $source = Normalize-CMakeBuildStatePath -Path $script:ResolvedSourceDir
    $build = Normalize-CMakeBuildStatePath -Path $script:ResolvedBuildDir
    if ($script:IsWindowsHost) {
        $source = $source.ToLowerInvariant()
        $build = $build.ToLowerInvariant()
    }

    $prefix = $build
    if (-not $prefix.EndsWith('/')) {
        $prefix += '/'
    }

    return [string]::Equals($source, $build, [System.StringComparison]::Ordinal) -or
        $source.StartsWith($prefix, [System.StringComparison]::Ordinal)
}

function Reset-StaleCMakeBuildDirectoryIfNeeded {
    $cachePath = Join-Path $script:ResolvedBuildDir "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
        return
    }

    $cachedSource = Get-CMakeCacheValue -Name "CMAKE_HOME_DIRECTORY"
    $cachedBuild = Get-CMakeCacheValue -Name "CMAKE_CACHEFILE_DIR"
    $reasons = @()

    if ([string]::IsNullOrWhiteSpace($cachedSource)) {
        $reasons += "CMake cache is missing CMAKE_HOME_DIRECTORY"
    } elseif (-not $script:IsWindowsHost -and (Test-WindowsDriveCachePath -Path $cachedSource)) {
        $reasons += "cached source directory uses a Windows path on this host: $cachedSource"
    } elseif (-not (Test-CMakeBuildStatePathMatch -CachedPath $cachedSource -ExpectedPath $script:ResolvedSourceDir)) {
        $reasons += "cached source directory '$cachedSource' does not match current source '$script:ResolvedSourceDir'"
    }

    if ([string]::IsNullOrWhiteSpace($cachedBuild)) {
        $reasons += "CMake cache is missing CMAKE_CACHEFILE_DIR"
    } elseif (-not $script:IsWindowsHost -and (Test-WindowsDriveCachePath -Path $cachedBuild)) {
        $reasons += "cached build directory uses a Windows path on this host: $cachedBuild"
    } elseif (-not (Test-CMakeBuildStatePathMatch -CachedPath $cachedBuild -ExpectedPath $script:ResolvedBuildDir)) {
        $reasons += "cached build directory '$cachedBuild' does not match current build '$script:ResolvedBuildDir'"
    }

    if (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator) {
        $expectedToolchain = Get-CMakeDefinitionOverride -Name "CMAKE_TOOLCHAIN_FILE"
        if ([string]::IsNullOrWhiteSpace($expectedToolchain)) {
            $expectedToolchain = Join-Path $script:ResolvedVcpkgRoot "scripts/buildsystems/vcpkg.cmake"
        } elseif (-not [System.IO.Path]::IsPathRooted($expectedToolchain)) {
            $expectedToolchain = Resolve-ChthollyPath -Path $expectedToolchain -BasePath $script:ResolvedSourceDir
        }
        $expectedManifest = Get-CMakeDefinitionOverride -Name "VCPKG_MANIFEST_DIR"
        if ([string]::IsNullOrWhiteSpace($expectedManifest)) {
            $expectedManifest = $script:ResolvedSourceDir
        } elseif (-not [System.IO.Path]::IsPathRooted($expectedManifest)) {
            $expectedManifest = Resolve-ChthollyPath -Path $expectedManifest -BasePath $script:ResolvedSourceDir
        }
        $expectedTriplet = Get-CMakeDefinitionOverride -Name "VCPKG_TARGET_TRIPLET"
        if ([string]::IsNullOrWhiteSpace($expectedTriplet) -and $script:IsWindowsHost) {
            $expectedTriplet = "x64-windows-static"
        }
        $reasons += @(Get-ChthollyVcpkgCacheIssues `
            -CachePath $cachePath `
            -ExpectedToolchain $expectedToolchain `
            -ExpectedManifestDir $expectedManifest `
            -ExpectedTriplet $expectedTriplet `
            -IsWindowsHost $script:IsWindowsHost)
    }

    if ($reasons.Count -eq 0) {
        return
    }

    if ($NoResetStaleBuildDir) {
        throw "Stale CMake build directory detected:`n  - $($reasons -join "`n  - ")`nRerun without -NoResetStaleBuildDir or use a fresh -BuildDir."
    }

    if (Test-CMakeBuildDirectoryContainsSource) {
        throw "Refusing to reset build directory because it contains the source directory: $script:ResolvedBuildDir"
    }

    Write-Host "==> Recreate stale CMake build directory"
    foreach ($reason in $reasons) {
        Write-Host "  - $reason"
    }

    $entries = @(Get-ChildItem -LiteralPath $script:ResolvedBuildDir -Force -ErrorAction SilentlyContinue)
    foreach ($entry in $entries) {
        if ($entry.Name -eq ".chtholly-build.lock") {
            continue
        }
        Remove-Item -LiteralPath $entry.FullName -Recurse -Force
    }
}

function Get-CMakeCxxCompilerId {
    $cmakeFilesDir = Join-Path $script:ResolvedBuildDir "CMakeFiles"
    if (-not (Test-Path -LiteralPath $cmakeFilesDir)) {
        return $null
    }

    $compilerFile = Get-ChildItem -LiteralPath $cmakeFilesDir -Recurse -Filter "CMakeCXXCompiler.cmake" -File |
        Select-Object -First 1
    if ($null -eq $compilerFile) {
        return $null
    }

    $content = Get-Content -LiteralPath $compilerFile.FullName -Raw
    if ($content -match 'set\(CMAKE_CXX_COMPILER_ID\s+"([^"]+)"\)') {
        return $Matches[1]
    }

    return $null
}

function Get-CMakeCxxShowIncludesPrefix {
    $cmakeFilesDir = Join-Path $script:ResolvedBuildDir "CMakeFiles"
    if (-not (Test-Path -LiteralPath $cmakeFilesDir)) {
        return $null
    }

    $compilerFile = Get-ChildItem -LiteralPath $cmakeFilesDir -Recurse -Filter "CMakeCXXCompiler.cmake" -File |
        Select-Object -First 1
    if ($null -eq $compilerFile) {
        return $null
    }

    $content = Get-Content -LiteralPath $compilerFile.FullName -Raw
    if ($content -match 'set\(CMAKE_CXX_CL_SHOWINCLUDES_PREFIX "([^"]*)"\)') {
        return $Matches[1]
    }

    return $null
}

function Test-SccacheNinjaMsvcDepsPrefixFixNeeded {
    return $script:IsWindowsHost -and
        (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator) -and
        (Test-SccacheLauncher -Launcher $script:ResolvedCompilerLauncher) -and
        -not $SkipSccacheMsvcDepsPrefixFix
}

function Test-NinjaMsvcDepsPrefixFixNeeded {
    if (-not $script:IsWindowsHost) {
        return $false
    }

    if (-not (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator)) {
        return $false
    }

    return -not ((Test-SccacheLauncher -Launcher $script:ResolvedCompilerLauncher) -and
        $SkipSccacheMsvcDepsPrefixFix)
}

function Get-StableBuildToken {
    param([string]$Text)

    $sha256 = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        $hash = $sha256.ComputeHash($bytes)
        $hex = [System.BitConverter]::ToString($hash).Replace("-", "").ToLowerInvariant()
        return $hex.Substring(0, 12)
    } finally {
        $sha256.Dispose()
    }
}

function Get-SccacheMsvcShowIncludesPrefix {
    param(
        [string]$LauncherPath,
        [string]$CompilerPath
    )

    $probeDir = Join-Path $script:ResolvedBuildDir "CMakeFiles\sccache-msvc-deps-prefix-probe"
    New-Item -ItemType Directory -Path $probeDir -Force | Out-Null
    $probeDir = (Resolve-Path -LiteralPath $probeDir).Path

    $buildToken = Get-StableBuildToken -Text $script:ResolvedBuildDir
    $headerName = "chtholly_sccache_msvc_deps_probe_$buildToken.h"
    $sourceName = "chtholly_sccache_msvc_deps_probe_$buildToken.cpp"
    $objectName = "chtholly_sccache_msvc_deps_probe_$buildToken.obj"
    $headerPath = Join-Path $probeDir $headerName
    $sourcePath = Join-Path $probeDir $sourceName
    $objectPath = Join-Path $probeDir $objectName

    Set-Content -LiteralPath $headerPath -Value "#pragma once`n#define CHTHOLLY_SCCACHE_MSVC_DEPS_PROBE 1`n" -Encoding ASCII -NoNewline
    Set-Content -LiteralPath $sourcePath -Value "#include `"$headerName`"`nint chtholly_sccache_msvc_deps_probe() { return CHTHOLLY_SCCACHE_MSVC_DEPS_PROBE; }`n" -Encoding ASCII -NoNewline
    Remove-Item -LiteralPath $objectPath -Force -ErrorAction SilentlyContinue

    $cmakePrefix = Get-CMakeCxxShowIncludesPrefix
    $result = Invoke-CapturedProcess `
        -FilePath $LauncherPath `
        -WorkingDirectory $probeDir `
        -EnvironmentOverrides @{
            SCCACHE_RECACHE = "1"
            SCCACHE_DIRECT = "false"
        } `
        -Arguments @(
            $CompilerPath,
            "/nologo",
            "/TP",
            "/std:c++20",
            "/showIncludes",
            "/c",
            $sourceName,
            "/Fo$objectName"
        )

    if ($result.ExitCode -ne 0) {
        if (Test-RetryableWindowsBuildFailure -Message ($result.Stdout + "`n" + $result.Stderr)) {
            Write-Warning "sccache MSVC /showIncludes probe failed because the temporary probe file could not be persisted. Skipping the prefix patch for this invocation."
            return $null
        }
        throw "sccache MSVC /showIncludes probe failed with exit code $($result.ExitCode): $($result.Stderr)"
    }

    $strictPathCandidates = @(
        $headerPath,
        $headerPath.Replace('\', '/')
    ) | Sort-Object Length -Descending
    $fallbackNameCandidates = @(
        $headerName,
        ".\$headerName"
    ) | Sort-Object Length -Descending

    foreach ($line in ($result.Stdout -split "\r?\n")) {
        foreach ($pathCandidate in $strictPathCandidates) {
            $index = $line.IndexOf($pathCandidate, [System.StringComparison]::OrdinalIgnoreCase)
            if ($index -gt 0) {
                $prefix = $line.Substring(0, $index)
                if ($prefix.IndexOf('\', [System.StringComparison]::Ordinal) -lt 0 -and
                    $prefix.IndexOf('/', [System.StringComparison]::Ordinal) -lt 0) {
                    return $prefix
                }
            }
        }

        foreach ($nameCandidate in $fallbackNameCandidates) {
            $index = $line.IndexOf($nameCandidate, [System.StringComparison]::OrdinalIgnoreCase)
            if ($index -le 0) {
                continue
            }

            $previousChar = $line[$index - 1]
            if ($previousChar -eq '\' -or $previousChar -eq '/' -or $previousChar -eq ':') {
                continue
            }

            $prefix = $line.Substring(0, $index)
            if ($prefix.IndexOf('\', [System.StringComparison]::Ordinal) -lt 0 -and
                $prefix.IndexOf('/', [System.StringComparison]::Ordinal) -lt 0) {
                return $prefix
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($cmakePrefix)) {
        Write-Warning "sccache MSVC /showIncludes probe returned an implausible prefix; falling back to the CMake-detected prefix."
        return $cmakePrefix
    }

    throw "sccache MSVC /showIncludes probe did not produce a detectable include prefix."
}

function Get-NativeMsvcShowIncludesPrefix {
    param([string]$CompilerPath)

    $probeDir = Join-Path $script:ResolvedBuildDir "CMakeFiles\native-msvc-deps-prefix-probe"
    New-Item -ItemType Directory -Path $probeDir -Force | Out-Null
    $probeDir = (Resolve-Path -LiteralPath $probeDir).Path

    $buildToken = Get-StableBuildToken -Text $script:ResolvedBuildDir
    $headerName = "chtholly_native_msvc_deps_probe_$buildToken.h"
    $sourceName = "chtholly_native_msvc_deps_probe_$buildToken.cpp"
    $objectName = "chtholly_native_msvc_deps_probe_$buildToken.obj"
    $headerPath = Join-Path $probeDir $headerName
    $sourcePath = Join-Path $probeDir $sourceName
    $objectPath = Join-Path $probeDir $objectName

    Set-Content -LiteralPath $headerPath -Value "#pragma once`n#define CHTHOLLY_NATIVE_MSVC_DEPS_PROBE 1`n" -Encoding ASCII -NoNewline
    Set-Content -LiteralPath $sourcePath -Value "#include `"$headerName`"`nint chtholly_native_msvc_deps_probe() { return CHTHOLLY_NATIVE_MSVC_DEPS_PROBE; }`n" -Encoding ASCII -NoNewline
    Remove-Item -LiteralPath $objectPath -Force -ErrorAction SilentlyContinue

    $cmakePrefix = Get-CMakeCxxShowIncludesPrefix
    $result = Invoke-CapturedProcess `
        -FilePath $CompilerPath `
        -WorkingDirectory $probeDir `
        -Arguments @(
            "/nologo",
            "/TP",
            "/std:c++20",
            "/showIncludes",
            "/c",
            $sourceName,
            "/Fo$objectName"
        )

    if ($result.ExitCode -ne 0) {
        throw "MSVC /showIncludes probe failed with exit code $($result.ExitCode): $($result.Stderr)"
    }

    $strictPathCandidates = @(
        $headerPath,
        $headerPath.Replace('\', '/')
    ) | Sort-Object Length -Descending
    $fallbackNameCandidates = @(
        $headerName,
        ".\$headerName"
    ) | Sort-Object Length -Descending

    foreach ($line in ($result.Stdout -split "\r?\n")) {
        foreach ($pathCandidate in $strictPathCandidates) {
            $index = $line.IndexOf($pathCandidate, [System.StringComparison]::OrdinalIgnoreCase)
            if ($index -gt 0) {
                $prefix = $line.Substring(0, $index)
                if ($prefix.IndexOf('\', [System.StringComparison]::Ordinal) -lt 0 -and
                    $prefix.IndexOf('/', [System.StringComparison]::Ordinal) -lt 0) {
                    return $prefix
                }
            }
        }

        foreach ($nameCandidate in $fallbackNameCandidates) {
            $index = $line.IndexOf($nameCandidate, [System.StringComparison]::OrdinalIgnoreCase)
            if ($index -le 0) {
                continue
            }

            $previousChar = $line[$index - 1]
            if ($previousChar -eq '\' -or $previousChar -eq '/' -or $previousChar -eq ':') {
                continue
            }

            $prefix = $line.Substring(0, $index)
            if ($prefix.IndexOf('\', [System.StringComparison]::Ordinal) -lt 0 -and
                $prefix.IndexOf('/', [System.StringComparison]::Ordinal) -lt 0) {
                return $prefix
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($cmakePrefix)) {
        Write-Warning "MSVC /showIncludes probe returned an implausible prefix; falling back to the CMake-detected prefix."
        return $cmakePrefix
    }

    throw "MSVC /showIncludes probe did not produce a detectable include prefix."
}

function Set-NinjaMsvcDepsPrefix {
    param([string]$Prefix)

    $rulesPath = Join-Path $script:ResolvedBuildDir "CMakeFiles\rules.ninja"
    if (-not (Test-Path -LiteralPath $rulesPath)) {
        return $false
    }

    $content = Get-Content -LiteralPath $rulesPath -Raw -Encoding UTF8
    $replacement = "msvc_deps_prefix = $Prefix"
    $updated = [System.Text.RegularExpressions.Regex]::Replace(
        $content,
        "(?m)^msvc_deps_prefix = .*$",
        { param($match) $replacement }
    )

    if ($updated -eq $content) {
        return $false
    }

    Write-Utf8NoBomFile -Path $rulesPath -Value $updated
    return $true
}

function Remove-NinjaDepsLog {
    $depsPath = Join-Path $script:ResolvedBuildDir ".ninja_deps"
    if (Test-Path -LiteralPath $depsPath) {
        Remove-Item -LiteralPath $depsPath -Force
    }
}

function Get-SccacheMsvcDepsPrefixStampPath {
    return Join-Path $script:ResolvedBuildDir ".chtholly-sccache-msvc-deps-prefix"
}

function Get-SccacheMsvcDepsPrefixCacheKeyStampPath {
    return Join-Path $script:ResolvedBuildDir ".chtholly-sccache-msvc-deps-prefix-key"
}

function Get-FileIdentityForCacheKey {
    param([string]$Path)

    if ([string]::IsNullOrWhiteSpace($Path) -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return $Path
    }

    $file = Get-Item -LiteralPath $Path
    return "$($file.FullName)|$($file.Length)|$($file.LastWriteTimeUtc.Ticks)"
}

function Get-SccacheMsvcDepsPrefixCacheKey {
    param(
        [string]$LauncherPath,
        [string]$CompilerPath
    )

    $keyText = @(
        "launcher=$((Get-FileIdentityForCacheKey -Path $LauncherPath))",
        "compiler=$((Get-FileIdentityForCacheKey -Path $CompilerPath))",
        "cmake-prefix=$(Get-CMakeCxxShowIncludesPrefix)",
        "generator=$script:ResolvedGenerator",
        "build-dir=$script:ResolvedBuildDir"
    ) -join "`n"
    return Get-StableBuildToken -Text $keyText
}

function Get-CachedSccacheMsvcDepsPrefix {
    param([string]$CacheKey)

    $prefixPath = Get-SccacheMsvcDepsPrefixStampPath
    $keyPath = Get-SccacheMsvcDepsPrefixCacheKeyStampPath
    if ([string]::IsNullOrWhiteSpace($CacheKey) -or
        -not (Test-Path -LiteralPath $prefixPath) -or
        -not (Test-Path -LiteralPath $keyPath)) {
        return $null
    }

    $storedKey = (Get-Content -LiteralPath $keyPath -Raw -Encoding UTF8).Trim()
    if ($storedKey -ne $CacheKey) {
        return $null
    }

    return Normalize-SccacheMsvcDepsPrefix (Get-Content -LiteralPath $prefixPath -Raw -Encoding UTF8)
}

function Remove-SccacheMsvcDepsPrefixCacheKeyStamp {
    Remove-Item -LiteralPath (Get-SccacheMsvcDepsPrefixCacheKeyStampPath) -Force -ErrorAction SilentlyContinue
}

function Save-SccacheMsvcDepsPrefixCacheKey {
    param([AllowNull()][string]$CacheKey)

    if ([string]::IsNullOrWhiteSpace($CacheKey)) {
        Remove-SccacheMsvcDepsPrefixCacheKeyStamp
        return
    }

    Write-Utf8NoBomFile -Path (Get-SccacheMsvcDepsPrefixCacheKeyStampPath) -Value $CacheKey
}

function Remove-SccacheNinjaMsvcDepsPrefixStamp {
    $stampPath = Get-SccacheMsvcDepsPrefixStampPath
    Remove-Item -LiteralPath $stampPath -Force -ErrorAction SilentlyContinue
    Remove-SccacheMsvcDepsPrefixCacheKeyStamp
}

function Get-SccacheNinjaMsvcDepsStatusStampPath {
    return Join-Path $script:ResolvedBuildDir ".chtholly-sccache-msvc-deps-status"
}

function Save-SccacheNinjaMsvcDepsStatus {
    param(
        [string]$Status,
        [AllowEmptyString()][string]$Detail = ""
    )

    $stampPath = Get-SccacheNinjaMsvcDepsStatusStampPath
    $content = if ([string]::IsNullOrWhiteSpace($Detail)) {
        $Status
    } else {
        "$Status`n$Detail"
    }
    Write-Utf8NoBomFile -Path $stampPath -Value $content
}

function Write-SccacheNinjaMsvcDepsStatus {
    if ([string]::IsNullOrWhiteSpace($script:SccacheMsvcDepsStatus)) {
        return
    }

    Write-Host "==> Ninja MSVC deps status"
    Write-Host "Status:            $script:SccacheMsvcDepsStatus"
    if (-not [string]::IsNullOrWhiteSpace($script:SccacheMsvcDepsStatusDetail)) {
        Write-Host "Detail:            $script:SccacheMsvcDepsStatusDetail"
    }
}

function Format-SccachePrefixDetail {
    param([AllowEmptyString()][string]$Prefix)

    if ([string]::IsNullOrWhiteSpace($Prefix)) {
        return ""
    }

    $trimmed = $Prefix.Trim()
    if ([string]::IsNullOrWhiteSpace($trimmed)) {
        return "detected a non-empty /showIncludes prefix"
    }

    return "detected a /showIncludes prefix ($($trimmed.Length) chars)"
}

function Normalize-NinjaMsvcDepsPrefix {
    param([AllowNull()][object]$Prefix)

    if ($null -eq $Prefix) {
        return $null
    }

    $lines = @()
    foreach ($item in @($Prefix)) {
        if ($null -eq $item) {
            continue
        }
        foreach ($line in ([string]$item -split "\r?\n")) {
            if (-not [string]::IsNullOrWhiteSpace($line)) {
                $lines += $line
            }
        }
    }

    if ($lines.Count -eq 0) {
        return $null
    }

    foreach ($line in $lines) {
        if ($line.IndexOf('\', [System.StringComparison]::Ordinal) -ge 0 -or
            $line.IndexOf('/', [System.StringComparison]::Ordinal) -ge 0) {
            continue
        }
        if ($line.Contains("Cleaning")) {
            continue
        }
        return $line
    }

    return $lines[-1]
}

function Normalize-SccacheMsvcDepsPrefix {
    param([AllowNull()][object]$Prefix)

    return Normalize-NinjaMsvcDepsPrefix $Prefix
}

function Invoke-SccacheNinjaMsvcDepsPrefixFix {
    if (-not (Test-SccacheNinjaMsvcDepsPrefixFixNeeded)) {
        $script:SccacheMsvcDepsStatus = "not-applicable"
        $script:SccacheMsvcDepsStatusDetail = "prefix fix disabled or launcher/generator combination does not require it"
        return $null
    }

    $compilerId = Get-CMakeCxxCompilerId
    if ($compilerId -ne "MSVC") {
        $script:SccacheMsvcDepsStatus = "not-applicable"
        $script:SccacheMsvcDepsStatusDetail = "configured compiler is '$compilerId', not MSVC"
        return $null
    }

    $compilerPath = Get-CMakeCacheValue -Name "CMAKE_CXX_COMPILER"
    if ([string]::IsNullOrWhiteSpace($compilerPath)) {
        $compilerPath = Assert-Tool -Name "cl.exe" -ErrorMessage "CMake selected MSVC, but cl.exe was not found in PATH."
    }

    $launcherPath = if ([string]::IsNullOrWhiteSpace($script:ResolvedCompilerLauncherPath)) {
        Assert-Tool -Name $script:ResolvedCompilerLauncher -ErrorMessage "'sccache' requested but was not found in PATH."
    } else {
        $script:ResolvedCompilerLauncherPath
    }
    $script:SccacheMsvcDepsPrefixCacheKey = Get-SccacheMsvcDepsPrefixCacheKey -LauncherPath $launcherPath -CompilerPath $compilerPath
    $cachedPrefix = Get-CachedSccacheMsvcDepsPrefix -CacheKey $script:SccacheMsvcDepsPrefixCacheKey
    if (-not [string]::IsNullOrWhiteSpace($cachedPrefix)) {
        Set-NinjaMsvcDepsPrefix -Prefix $cachedPrefix | Out-Null
        $script:SccacheMsvcDepsStatus = "active-prefix-cached"
        $script:SccacheMsvcDepsStatusDetail = (Format-SccachePrefixDetail -Prefix $cachedPrefix) + "; reused cached sccache/MSVC prefix"
        return $cachedPrefix
    }
    $prefix = Normalize-SccacheMsvcDepsPrefix (
        Get-SccacheMsvcShowIncludesPrefix -LauncherPath $launcherPath -CompilerPath $compilerPath
    )
    if ([string]::IsNullOrWhiteSpace($prefix)) {
        Remove-SccacheNinjaMsvcDepsPrefixStamp
        Write-Warning "sccache MSVC /showIncludes probe reported a transient file-access failure; clearing residual build processes for this build directory and retrying once."
        Stop-ChthollyBuildDirResidualProcesses
        $retryPrefix = Normalize-SccacheMsvcDepsPrefix (
            Get-SccacheMsvcShowIncludesPrefix -LauncherPath $launcherPath -CompilerPath $compilerPath
        )
        if ([string]::IsNullOrWhiteSpace($retryPrefix)) {
            $script:SccacheMsvcDepsStatus = "degraded"
            $script:SccacheMsvcDepsStatusDetail = "transient file-access failure during /showIncludes probe; continuing without prefix patch or prefix-stamp verification"
            return $null
        }
        $prefix = $retryPrefix
    }
    Set-NinjaMsvcDepsPrefix -Prefix $prefix | Out-Null

    $stampPath = Get-SccacheMsvcDepsPrefixStampPath
    $previousPrefix = if (Test-Path -LiteralPath $stampPath) {
        Get-Content -LiteralPath $stampPath -Raw -Encoding UTF8
    } else {
        ""
    }

    $hasExistingNinjaDeps = Test-Path -LiteralPath (Join-Path $script:ResolvedBuildDir ".ninja_deps")
    $needsDepsMigration = $script:BuildWillRun -and
        $previousPrefix -ne $prefix -and
        ($previousPrefix -ne "" -or $hasExistingNinjaDeps)

    if ($needsDepsMigration) {
        Write-Host "==> Ninja+sccache MSVC dependency prefix"
        if ($previousPrefix -eq "") {
            Write-Host "Existing Ninja build has no sccache MSVC dependency-prefix stamp; cleaning once to repopulate Ninja deps."
        } else {
            Write-Host "sccache MSVC /showIncludes prefix changed; cleaning once to repopulate Ninja deps."
        }

        Remove-NinjaDepsLog
        Invoke-NativeStep -Name "Clean stale Ninja deps" -FilePath $script:CMakePath -Arguments @("--build", $script:ResolvedBuildDir, "--target", "clean") | Out-Null
    }

    $script:SccacheMsvcDepsStatus = "active-prefix"
    $script:SccacheMsvcDepsStatusDetail = Format-SccachePrefixDetail -Prefix $prefix
    return $prefix
}

function Invoke-NinjaMsvcDepsPrefixFix {
    if (-not (Test-NinjaMsvcDepsPrefixFixNeeded)) {
        $script:SccacheMsvcDepsStatus = "not-applicable"
        $script:SccacheMsvcDepsStatusDetail = "prefix fix disabled or generator/compiler-launcher combination does not require it"
        return $null
    }

    $compilerId = Get-CMakeCxxCompilerId
    if ($compilerId -ne "MSVC") {
        $script:SccacheMsvcDepsStatus = "not-applicable"
        $script:SccacheMsvcDepsStatusDetail = "configured compiler is '$compilerId', not MSVC"
        return $null
    }

    if (Test-SccacheLauncher -Launcher $script:ResolvedCompilerLauncher) {
        return Invoke-SccacheNinjaMsvcDepsPrefixFix
    }

    $compilerPath = Get-CMakeCacheValue -Name "CMAKE_CXX_COMPILER"
    if ([string]::IsNullOrWhiteSpace($compilerPath)) {
        $compilerPath = Assert-Tool -Name "cl.exe" -ErrorMessage "CMake selected MSVC, but cl.exe was not found in PATH."
    }

    $prefix = Normalize-SccacheMsvcDepsPrefix (
        Get-NativeMsvcShowIncludesPrefix -CompilerPath $compilerPath
    )
    if ([string]::IsNullOrWhiteSpace($prefix)) {
        $script:SccacheMsvcDepsStatus = "degraded"
        $script:SccacheMsvcDepsStatusDetail = "native MSVC /showIncludes probe did not produce a usable prefix"
        return $null
    }

    Set-NinjaMsvcDepsPrefix -Prefix $prefix | Out-Null

    $stampPath = Get-SccacheMsvcDepsPrefixStampPath
    $previousPrefix = if (Test-Path -LiteralPath $stampPath) {
        Get-Content -LiteralPath $stampPath -Raw -Encoding UTF8
    } else {
        ""
    }

    $hasExistingNinjaDeps = Test-Path -LiteralPath (Join-Path $script:ResolvedBuildDir ".ninja_deps")
    $needsDepsMigration = $script:BuildWillRun -and
        $previousPrefix -ne $prefix -and
        ($previousPrefix -ne "" -or $hasExistingNinjaDeps)

    if ($needsDepsMigration) {
        Write-Host "==> Ninja MSVC dependency prefix"
        if ($previousPrefix -eq "") {
            Write-Host "Existing Ninja build has no MSVC dependency-prefix stamp; cleaning once to repopulate Ninja deps."
        } else {
            Write-Host "MSVC /showIncludes prefix changed; cleaning once to repopulate Ninja deps."
        }

        Remove-NinjaDepsLog
        Invoke-NativeStep -Name "Clean stale Ninja deps" -FilePath $script:CMakePath -Arguments @("--build", $script:ResolvedBuildDir, "--target", "clean") | Out-Null
    }

    $script:SccacheMsvcDepsStatus = "active-native-prefix"
    $script:SccacheMsvcDepsStatusDetail = Format-SccachePrefixDetail -Prefix $prefix
    return $prefix
}

function Save-SccacheNinjaMsvcDepsPrefix {
    param([AllowNull()][string]$Prefix)

    if ([string]::IsNullOrWhiteSpace($Prefix)) {
        return
    }

    $stampPath = Get-SccacheMsvcDepsPrefixStampPath
    $normalizedPrefix = Normalize-SccacheMsvcDepsPrefix $Prefix
    if ([string]::IsNullOrWhiteSpace($normalizedPrefix)) {
        return
    }

    Write-Utf8NoBomFile -Path $stampPath -Value $normalizedPrefix
}

function Write-BuildSummary {
    Write-Host "==> Build configuration"
    Write-Host "Source:           $script:ResolvedSourceDir"
    Write-Host "Build:            $script:ResolvedBuildDir"
    Write-Host "Generator:        $script:ResolvedGenerator"
    Write-Host "Config:           $Config"
    Write-Host "Target:           $(if ([string]::IsNullOrWhiteSpace($Target)) { '<default>' } else { $Target })"
    Write-Host "Compiler launcher:$(if ([string]::IsNullOrWhiteSpace($script:ResolvedCompilerLauncher)) { ' <none>' } else { " $script:ResolvedCompilerLauncher" })"
    Write-Host "Warnings as errors:$(if ($WarningsAsErrors) { ' on' } else { ' off' })"
}

function Invoke-NinjaDepsVerification {
    if (-not (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator)) {
        throw "-VerifyNinjaDeps requires a Ninja generator."
    }

    $verifyScript = Join-Path $PSScriptRoot "verify-ninja-deps.ps1"
    if (-not (Test-Path -LiteralPath $verifyScript)) {
        throw "Ninja deps verification script was not found: $verifyScript"
    }

    Write-Host "==> Verify Ninja deps"
    $verifyArgs = @{
        BuildDir = $script:ResolvedBuildDir
        RequireProjectHeader = $true
    }
    if (Test-SccacheNinjaMsvcDepsPrefixFixNeeded -and $script:SccacheMsvcDepsStatus -eq "active-prefix" -and -not [string]::IsNullOrWhiteSpace($script:SccacheMsvcDepsPrefix)) {
        $verifyArgs.RequireSccacheMsvcPrefix = $true
    }
    if (-not [string]::IsNullOrWhiteSpace($VerifyNinjaDepsObject)) {
        $verifyArgs.ObjectPath = $VerifyNinjaDepsObject
    }

    & $verifyScript @verifyArgs
}

$script:ResolvedSourceDir = Resolve-ChthollyPath -Path $SourceDir -BasePath $script:RepoRoot -DefaultToBase
$script:ResolvedBuildDir = Resolve-ChthollyPath -Path $BuildDir -BasePath $script:ResolvedSourceDir
$script:ResolvedGenerator = $Generator.Trim()
$script:ResolvedCompilerLauncher = Resolve-CompilerLauncher -Launcher $CompilerLauncher
$script:ResolvedVcpkgRoot = Resolve-VcpkgRoot
$script:ResolvedCompilerLauncherPath = ""
$script:SccacheMsvcDepsPrefixCacheKey = ""
$script:IsSingleConfig = Test-SingleConfigGenerator -ResolvedGenerator $script:ResolvedGenerator
$script:BuildWillRun = -not $ConfigureOnly -and -not $SkipBuild
if ([string]::IsNullOrWhiteSpace($env:ChthollyMSVCPath)) {
    throw "ChthollyMSVCPath is not set. Please point it to vcvars64.bat or MSVC environment script."
}

$script:ResolvedVcVarsPath = $env:ChthollyMSVCPath
$script:SccacheMsvcDepsStatus = ""
$script:SccacheMsvcDepsStatusDetail = ""

$script:CMakePath = Assert-Tool -Name "cmake" -ErrorMessage "CMake was not found in PATH."

if (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator) {
    Assert-Tool -Name "ninja" -ErrorMessage "Ninja generator requested but 'ninja' was not found in PATH." | Out-Null
}

if (Test-SccacheLauncher -Launcher $script:ResolvedCompilerLauncher) {
    $script:ResolvedCompilerLauncherPath = Assert-Tool -Name $script:ResolvedCompilerLauncher -ErrorMessage "'sccache' requested but was not found in PATH."
}

if ($script:IsWindowsHost -and (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator) -and -not (Test-MSVCEnvironmentReady)) {
    if ($SkipVSImport) {
        Write-Host "==> Skipping MSVC environment import"
    } else {
        Import-VS64 -Path $script:ResolvedVcVarsPath
    }
}

if ($script:IsWindowsHost -and (Test-NinjaGenerator -ResolvedGenerator $script:ResolvedGenerator) -and -not $SkipVSImport -and -not (Test-MSVCEnvironmentReady)) {
    throw "Ninja + MSVC requires an MSVC environment, but automatic vs64 import failed."
}

Write-PreloadedVisualStudioSessionWarning

Acquire-ChthollyBuildLock
try {
    Stop-ChthollyBuildDirResidualProcesses
    Reset-EmptyBuildDirectoryIfNeeded
    Reset-StaleCMakeBuildDirectoryIfNeeded
    Write-BuildSummary

    $configureArgs = @("-S", $script:ResolvedSourceDir, "-B", $script:ResolvedBuildDir)
    $hasToolchainOverride = $null -ne (Get-CMakeDefinitionOverride -Name "CMAKE_TOOLCHAIN_FILE")
    if (-not $hasToolchainOverride) {
        $configureArgs += "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=$(Join-Path $script:ResolvedVcpkgRoot 'scripts/buildsystems/vcpkg.cmake')"
    }
    $configureArgs += "-DVCPKG_MANIFEST_DIR:PATH=$script:ResolvedSourceDir"
    if ($script:IsWindowsHost -and
        $null -eq (Get-CMakeDefinitionOverride -Name "VCPKG_TARGET_TRIPLET")) {
        $configureArgs += "-DVCPKG_TARGET_TRIPLET:STRING=x64-windows-static"
    }
    if (-not [string]::IsNullOrWhiteSpace($script:ResolvedGenerator)) {
        $configureArgs += @("-G", $script:ResolvedGenerator)
    }
    if ([string]::IsNullOrWhiteSpace($script:ResolvedCompilerLauncher)) {
        $configureArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER:STRING="
    } elseif (-not [string]::IsNullOrWhiteSpace($script:ResolvedCompilerLauncherPath)) {
        $configureArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=$script:ResolvedCompilerLauncherPath"
    } else {
        $configureArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=$script:ResolvedCompilerLauncher"
    }
    if ($script:IsSingleConfig) {
        $configureArgs += "-DCMAKE_BUILD_TYPE=$Config"
    }
    if ($FullDebugArtifacts) {
        $configureArgs += "-DCHTHOLLY_FULL_DEBUG_ARTIFACTS:BOOL=ON"
    } else {
        $configureArgs += "-DCHTHOLLY_FULL_DEBUG_ARTIFACTS:BOOL=OFF"
    }
    if ($WarningsAsErrors) {
        $configureArgs += "-DCHTHOLLY_WARNINGS_AS_ERRORS:BOOL=ON"
    } else {
        $configureArgs += "-DCHTHOLLY_WARNINGS_AS_ERRORS:BOOL=OFF"
    }
    $configureArgs += "-DCHTHOLLY_SANITIZER:STRING=$Sanitizer"
    $configureArgs += $CMakeArgs

    Invoke-NativeStep -Name "Configure" -FilePath $script:CMakePath -Arguments $configureArgs

    $script:SccacheMsvcDepsPrefix = Invoke-NinjaMsvcDepsPrefixFix
    Save-SccacheNinjaMsvcDepsStatus -Status $script:SccacheMsvcDepsStatus -Detail $script:SccacheMsvcDepsStatusDetail
    Write-SccacheNinjaMsvcDepsStatus

    if ($script:BuildWillRun) {
        $buildStepArgs = @("--build", $script:ResolvedBuildDir)
        if (-not $script:IsSingleConfig) {
            $buildStepArgs += @("--config", $Config)
        }
        if (-not [string]::IsNullOrWhiteSpace($Target)) {
            $buildStepArgs += @("--target", $Target)
        }
        if ($CleanFirst) {
            $buildStepArgs += "--clean-first"
        }
        if ($Parallel -gt 0) {
            $buildStepArgs += @("--parallel", $Parallel)
        }
        if ($VerboseBuild) {
            $buildStepArgs += "--verbose"
        }
        $buildStepArgs += $BuildArgs

        Invoke-RetryableBuildStep -BuildStepArgs $buildStepArgs
        Save-SccacheNinjaMsvcDepsPrefix -Prefix $script:SccacheMsvcDepsPrefix
        Save-SccacheMsvcDepsPrefixCacheKey -CacheKey $script:SccacheMsvcDepsPrefixCacheKey
    }

    if ($VerifyNinjaDeps) {
        Invoke-NinjaDepsVerification
    }

    if (-not $SkipTests -and -not $ConfigureOnly) {
        $resolvedTestParallel = if ($TestParallel -gt 0) {
            $TestParallel
        } elseif ($Parallel -gt 0) {
            $Parallel
        } else {
            [Math]::Min([Environment]::ProcessorCount, 12)
        }
        $runnerName = if ($script:IsWindowsHost) { "chtholly-test.exe" } else { "chtholly-test" }
        $runnerRelativePath = if ($script:IsSingleConfig) {
            Join-Path "tools" "chtholly-test\$runnerName"
        } else {
            Join-Path "tools" "chtholly-test\$Config\$runnerName"
        }
        $runnerPath = Join-Path $script:ResolvedBuildDir $runnerRelativePath
        $manifestPath = Join-Path $script:ResolvedBuildDir "tests\chtholly-tests.generated.toml"
        if (-not (Test-Path -LiteralPath $runnerPath -PathType Leaf)) {
            throw "Chtholly test runner was not built: $runnerPath"
        }
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
            throw "Generated Chtholly test manifest was not found: $manifestPath"
        }
        $runnerArguments = @(
            "run",
            "--manifest", $manifestPath,
            "--jobs", [Math]::Max(1, $resolvedTestParallel),
            "--format", "text",
            "--progress"
        )
        $runnerArguments += $AdditionalTestArgs

        Invoke-NativeStep -Name "Test" -FilePath $runnerPath -Arguments $runnerArguments
    }

    if ($ShowSccacheStats -and (Test-SccacheLauncher -Launcher $script:ResolvedCompilerLauncher)) {
        $sccachePath = if ([string]::IsNullOrWhiteSpace($script:ResolvedCompilerLauncherPath)) {
            Assert-Tool -Name $script:ResolvedCompilerLauncher -ErrorMessage "'sccache' requested but was not found in PATH."
        } else {
            $script:ResolvedCompilerLauncherPath
        }
        Invoke-NativeStep -Name "sccache stats" -FilePath $sccachePath -Arguments @("--show-stats")
    }

    if ($ReportBuildArtifacts -or -not [string]::IsNullOrWhiteSpace($BuildArtifactReportPath)) {
        $reportScript = Join-Path $PSScriptRoot "report-build-artifacts.ps1"
        if (-not (Test-Path -LiteralPath $reportScript -PathType Leaf)) {
            throw "Build artifact report script was not found: $reportScript"
        }
        $reportArgs = @{
            BuildDir = $script:ResolvedBuildDir
        }
        if (-not [string]::IsNullOrWhiteSpace($BuildArtifactReportPath)) {
            $reportArgs.JsonOut = Resolve-ChthollyPath -Path $BuildArtifactReportPath -BasePath $script:ResolvedSourceDir
        }
        & $reportScript @reportArgs
        if ($LASTEXITCODE -ne 0) {
            throw "Build artifact report failed with exit code $LASTEXITCODE"
        }
    }
} finally {
    Close-ChthollyBuildLock
}
