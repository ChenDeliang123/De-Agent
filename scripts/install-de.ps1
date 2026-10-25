# Add a de.cmd shim to the per-user PATH (no system-wide changes).
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/install-de.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/install-de.ps1 -Remove
#
# The shim only calls this repository's de.cmd; move/delete the repo and rerun.

param(
    [string]$InstallDir = (Join-Path $env:USERPROFILE "bin"),
    [switch]$Remove
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$deCmd = Join-Path $repoRoot "de.cmd"
$shim = Join-Path $InstallDir "de.cmd"

if ($Remove) {
    if (Test-Path -LiteralPath $shim) {
        Remove-Item -LiteralPath $shim -Force
        Write-Host "removed $shim"
    } else {
        Write-Host "shim not installed: $shim"
    }

    $userPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($userPath) {
        $parts = @($userPath -split ";" | Where-Object {
            $_ -and ($_.TrimEnd("\") -ine $InstallDir.TrimEnd("\"))
        })
        if ($parts.Count -ne ($userPath -split ";" | Where-Object { $_ }).Count) {
            [Environment]::SetEnvironmentVariable("Path", ($parts -join ";"), "User")
            Write-Host "removed $InstallDir from user PATH"
        }
    }
    exit 0
}

if (-not (Test-Path -LiteralPath $deCmd)) {
    throw "de.cmd not found: $deCmd"
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
$content = @"
@echo off
call "$deCmd" %*
exit /b %ERRORLEVEL%
"@
Set-Content -LiteralPath $shim -Value $content -Encoding ASCII
Write-Host "installed $shim"

$userPath = [Environment]::GetEnvironmentVariable("Path", "User")
$parts = @()
if ($userPath) {
    $parts = @($userPath -split ";" | Where-Object { $_ })
}
$already = $false
foreach ($part in $parts) {
    if ($part.TrimEnd("\") -ieq $InstallDir.TrimEnd("\")) {
        $already = $true
        break
    }
}
if (-not $already) {
    $parts += $InstallDir
    [Environment]::SetEnvironmentVariable("Path", ($parts -join ";"), "User")
    Write-Host "added $InstallDir to user PATH"
} else {
    Write-Host "$InstallDir already in user PATH"
}

Write-Host "open a new terminal, then run: de doctor"
