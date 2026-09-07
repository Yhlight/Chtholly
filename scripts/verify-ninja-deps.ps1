<#
.SYNOPSIS
Validates that a Ninja build directory has usable header dependencies.

.DESCRIPTION
This script is a local guard for the Chtholly Ninja + sccache + MSVC workflow.
It checks that Ninja's deps log contains non-empty dependencies and can also
require the sccache MSVC dependency-prefix stamp written by build.ps1.

.EXAMPLE
./scripts/verify-ninja-deps.ps1 -BuildDir build-ninja -RequireSccacheMsvcPrefix -RequireProjectHeader

.EXAMPLE
./scripts/verify-ninja-deps.ps1 -BuildDir build-ninja -ObjectPath src/sema/borrow_check/CMakeFiles/chtholly_sema_borrow_dataflow.dir/dataflow/BorrowDataflowBlockTransfer.cpp.obj
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$BuildDir = "build-ninja",
    [string]$ObjectPath = "",
    [int]$MinDeps = 1,
    [int]$MinObjectsWithDeps = 1,
    [switch]$RequireProjectHeader,
    [switch]$RequireSccacheMsvcPrefix,
    [string[]]$ProjectHeaderPatterns = @(
        "../src/",
        "..\src\",
        "../tests/",
        "..\tests\"
    )
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$script:RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))

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

function Invoke-CapturedProcess {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$WorkingDirectory = $script:RepoRoot
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $FilePath
    $startInfo.WorkingDirectory = $WorkingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false, $false)
    $startInfo.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false, $false)
    foreach ($argument in $Arguments) {
        [void]$startInfo.ArgumentList.Add($argument)
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

function Get-NinjaMsvcDepsPrefix {
    param([string]$ResolvedBuildDir)

    $rulesPath = Join-Path $ResolvedBuildDir "CMakeFiles\rules.ninja"
    if (-not (Test-Path -LiteralPath $rulesPath)) {
        return $null
    }

    $match = Select-String -LiteralPath $rulesPath -Pattern "^msvc_deps_prefix = (.*)$" |
        Select-Object -First 1
    if ($null -eq $match) {
        return $null
    }

    return $match.Matches[0].Groups[1].Value
}

function Test-ProjectHeaderDependency {
    param([string]$Dependency)

    foreach ($pattern in $ProjectHeaderPatterns) {
        if ($Dependency.IndexOf($pattern, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            return $true
        }
    }

    return $false
}

function Get-SccacheNinjaMsvcDepsStatusStampPath {
    return Join-Path $resolvedBuildDir ".chtholly-sccache-msvc-deps-status"
}

function Get-SccacheNinjaMsvcDepsStatus {
    $stampPath = Get-SccacheNinjaMsvcDepsStatusStampPath
    if (-not (Test-Path -LiteralPath $stampPath)) {
        return $null
    }

    $content = Get-Content -LiteralPath $stampPath -Raw -Encoding UTF8
    if ([string]::IsNullOrWhiteSpace($content)) {
        return [pscustomobject]@{
            Status = ""
            Detail = ""
        }
    }

    $lines = $content -split "\r?\n", 2
    return [pscustomobject]@{
        Status = $lines[0].Trim()
        Detail = if ($lines.Count -gt 1) { $lines[1].Trim() } else { "" }
    }
}

$resolvedBuildDir = Resolve-ChthollyPath -Path $BuildDir -BasePath $script:RepoRoot
$buildNinjaPath = Join-Path $resolvedBuildDir "build.ninja"
if (-not (Test-Path -LiteralPath $buildNinjaPath)) {
    throw "Ninja build file was not found: $buildNinjaPath"
}

$buildStatus = Get-SccacheNinjaMsvcDepsStatus
if ($RequireSccacheMsvcPrefix) {
    if ($null -ne $buildStatus -and $buildStatus.Status -eq "degraded") {
        throw "sccache MSVC prefix verification cannot be required for degraded build state at '$resolvedBuildDir'. Detail: $($buildStatus.Detail)"
    }

    $stampPath = Join-Path $resolvedBuildDir ".chtholly-sccache-msvc-deps-prefix"
    if (-not (Test-Path -LiteralPath $stampPath)) {
        $statusText = if ($null -eq $buildStatus) { "unknown" } else { $buildStatus.Status }
        $detailText = if ($null -eq $buildStatus) { "" } else { $buildStatus.Detail }
        throw "Missing sccache MSVC dependency-prefix stamp: $stampPath`nBuild status: $statusText`n$detailText"
    }

    $rulesPrefix = Get-NinjaMsvcDepsPrefix -ResolvedBuildDir $resolvedBuildDir
    if ([string]::IsNullOrWhiteSpace($rulesPrefix)) {
        throw "CMakeFiles/rules.ninja does not contain a usable msvc_deps_prefix."
    }

    $stampPrefix = Get-Content -LiteralPath $stampPath -Raw -Encoding UTF8
    if ($rulesPrefix -ne $stampPrefix) {
        throw "sccache MSVC dependency-prefix stamp does not match CMakeFiles/rules.ninja."
    }
}

$ninjaPath = Assert-Tool -Name "ninja" -ErrorMessage "Ninja was not found in PATH."
$ninjaArgs = @("-C", $resolvedBuildDir, "-t", "deps")
if (-not [string]::IsNullOrWhiteSpace($ObjectPath)) {
    $ninjaArgs += $ObjectPath
}

$result = Invoke-CapturedProcess -FilePath $ninjaPath -Arguments $ninjaArgs
if ($result.ExitCode -ne 0) {
    throw "ninja -t deps failed with exit code $($result.ExitCode): $($result.Stderr)"
}

$objects = @()
$current = $null
foreach ($line in ($result.Stdout -split "\r?\n")) {
    if ($line -match "^(?<object>.+): #deps (?<count>\d+),") {
        if ($null -ne $current) {
            $objects += $current
        }

        $current = [pscustomobject]@{
            Object = $Matches["object"]
            Count = [int]$Matches["count"]
            HasProjectHeader = $false
            ProjectHeaderSample = ""
        }
        continue
    }

    if ($null -ne $current -and $line -match "^\s+(?<dep>.+)$") {
        $dependency = $Matches["dep"]
        if (-not $current.HasProjectHeader -and (Test-ProjectHeaderDependency -Dependency $dependency)) {
            $current.HasProjectHeader = $true
            $current.ProjectHeaderSample = $dependency
        }
    }
}

if ($null -ne $current) {
    $objects += $current
}

if ($objects.Count -eq 0) {
    throw "ninja -t deps did not report any dependency entries."
}

$objectsWithDeps = @($objects | Where-Object { $_.Count -ge $MinDeps })
if ($objectsWithDeps.Count -lt $MinObjectsWithDeps) {
    throw "Ninja dependency log is not usable: $($objectsWithDeps.Count) object(s) have at least $MinDeps deps, expected $MinObjectsWithDeps."
}

$objectsWithProjectHeader = @($objects | Where-Object { $_.HasProjectHeader })
if ($RequireProjectHeader -and $objectsWithProjectHeader.Count -eq 0) {
    throw "Ninja dependency log has no project header dependency matching the configured patterns."
}

Write-Host "==> Ninja dependency summary"
Write-Host "Build:              $resolvedBuildDir"
Write-Host "Objects checked:    $($objects.Count)"
Write-Host "Objects with deps:  $($objectsWithDeps.Count)"
Write-Host "Project headers:    $($objectsWithProjectHeader.Count)"
if ($null -ne $buildStatus) {
    Write-Host "Build status:        $($buildStatus.Status)"
    if (-not [string]::IsNullOrWhiteSpace($buildStatus.Detail)) {
        Write-Host "Status detail:       $($buildStatus.Detail)"
    }
}
if ($RequireSccacheMsvcPrefix) {
    Write-Host "sccache MSVC prefix: verified"
}
