Set-StrictMode -Version Latest

function Get-ChthollyCMakeCacheEntry {
    param(
        [string]$CachePath,
        [string]$Name
    )

    if (-not (Test-Path -LiteralPath $CachePath -PathType Leaf)) {
        return $null
    }
    $escapedName = [regex]::Escape($Name)
    foreach ($line in (Get-Content -LiteralPath $CachePath)) {
        if ($line -match "^${escapedName}:([^=]*)=(.*)$") {
            return [pscustomobject]@{
                Type = [string]$Matches[1]
                Value = [string]$Matches[2]
            }
        }
    }
    return $null
}

function Test-ChthollyCachePathMatch {
    param(
        [AllowNull()][string]$Actual,
        [string]$Expected,
        [bool]$IsWindowsHost
    )

    if ([string]::IsNullOrWhiteSpace($Actual)) {
        return $false
    }
    $actualPath = $Actual.Trim().Replace('\', '/').TrimEnd('/')
    $expectedPath = $Expected.Trim().Replace('\', '/').TrimEnd('/')
    $comparison = if ($IsWindowsHost) {
        [System.StringComparison]::OrdinalIgnoreCase
    } else {
        [System.StringComparison]::Ordinal
    }
    return [string]::Equals($actualPath, $expectedPath, $comparison)
}

function Get-ChthollyVcpkgCacheIssues {
    param(
        [string]$CachePath,
        [string]$ExpectedToolchain,
        [string]$ExpectedManifestDir,
        [AllowEmptyString()][string]$ExpectedTriplet,
        [bool]$IsWindowsHost,
        [string]$RequiredPackageConfig = "unofficial-sodium"
    )

    $issues = @()
    $toolchain = Get-ChthollyCMakeCacheEntry -CachePath $CachePath -Name "CMAKE_TOOLCHAIN_FILE"
    if ($null -eq $toolchain) {
        $issues += "CMake cache is missing CMAKE_TOOLCHAIN_FILE"
    } elseif ($toolchain.Type -eq "UNINITIALIZED") {
        $issues += "CMAKE_TOOLCHAIN_FILE is still UNINITIALIZED"
    } elseif (-not (Test-ChthollyCachePathMatch -Actual $toolchain.Value -Expected $ExpectedToolchain -IsWindowsHost $IsWindowsHost)) {
        $issues += "cached vcpkg toolchain '$($toolchain.Value)' does not match '$ExpectedToolchain'"
    }

    $manifestMode = Get-ChthollyCMakeCacheEntry -CachePath $CachePath -Name "VCPKG_MANIFEST_MODE"
    if ($null -eq $manifestMode -or $manifestMode.Value -notin @("ON", "TRUE", "1")) {
        $issues += "VCPKG_MANIFEST_MODE is not enabled"
    }
    $manifestDir = Get-ChthollyCMakeCacheEntry -CachePath $CachePath -Name "VCPKG_MANIFEST_DIR"
    if ($null -eq $manifestDir -or
        -not (Test-ChthollyCachePathMatch -Actual $manifestDir.Value -Expected $ExpectedManifestDir -IsWindowsHost $IsWindowsHost)) {
        $issues += "VCPKG_MANIFEST_DIR does not match the project manifest directory"
    }

    if (-not [string]::IsNullOrWhiteSpace($ExpectedTriplet)) {
        $triplet = Get-ChthollyCMakeCacheEntry -CachePath $CachePath -Name "VCPKG_TARGET_TRIPLET"
        if ($null -eq $triplet -or $triplet.Type -eq "UNINITIALIZED" -or
            $triplet.Value -ne $ExpectedTriplet) {
            $issues += "VCPKG_TARGET_TRIPLET is not '$ExpectedTriplet'"
        }
    }

    $installed = Get-ChthollyCMakeCacheEntry -CachePath $CachePath -Name "VCPKG_INSTALLED_DIR"
    if ($null -eq $installed -or $installed.Type -eq "UNINITIALIZED" -or
        [string]::IsNullOrWhiteSpace($installed.Value) -or
        -not (Test-Path -LiteralPath $installed.Value -PathType Container)) {
        $issues += "VCPKG_INSTALLED_DIR is missing or does not exist"
    }

    $packageEntry = Get-ChthollyCMakeCacheEntry -CachePath $CachePath -Name "${RequiredPackageConfig}_DIR"
    if ($null -eq $packageEntry -or $packageEntry.Type -eq "UNINITIALIZED" -or
        [string]::IsNullOrWhiteSpace($packageEntry.Value) -or
        $packageEntry.Value.EndsWith("-NOTFOUND") -or
        -not (Test-Path -LiteralPath $packageEntry.Value -PathType Container)) {
        $issues += "${RequiredPackageConfig}_DIR is missing or unresolved"
    } else {
        $configFiles = @(Get-ChildItem -LiteralPath $packageEntry.Value -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name.EndsWith("config.cmake", [System.StringComparison]::OrdinalIgnoreCase) })
        if ($configFiles.Count -eq 0) {
            $issues += "${RequiredPackageConfig}_DIR contains no package config"
        }
    }

    return @($issues)
}
