# Windows local regression gate.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-windows.ps1
#   ... -Configuration Release
#   ... -Package          # also build and verify the Windows ZIPs
#   ... -SkipA2A / -SkipMcp
#
# Prerequisites are the same as scripts/build-windows.ps1. In addition, a2a
# tests need a cached googletest source tree:
#   <AIAGENT_DEPS_DIR>/googletest-1.15.2
#
# This script does not install dependencies. It runs every stage even if an
# earlier stage fails, prints a summary, and exits non-zero if any stage failed.

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [switch]$Package,
    [switch]$SkipA2A,
    [switch]$SkipMcp
)

# PowerShell 5.1 treats redirected native stderr as a terminating
# NativeCommandError when ErrorActionPreference=Stop. CMake/GTest routinely
# write warnings to stderr, so keep it at Continue and rely on exit codes.
$ErrorActionPreference = "Continue"

$repo = Split-Path -Parent $PSScriptRoot
$mingwRoot = if ($env:MINGW_ROOT) { $env:MINGW_ROOT } else { "D:\Tools\mingw64" }
$curlRoot = if ($env:CURL_ROOT) { $env:CURL_ROOT } else { "D:\Tools\curl-mingw" }
$depsRoot = if ($env:AIAGENT_DEPS_DIR) { $env:AIAGENT_DEPS_DIR } else { "D:\Tools\aiagent-deps" }
$logDir = Join-Path $repo "build-mingw\smoke"
$toolchain = Join-Path $repo "cmake\toolchains\mingw-w64.cmake"
$buildDirName = if ($Configuration -eq "Release") { "build-mingw-release" } else { "build-mingw" }
$buildDir = Join-Path $repo $buildDirName

New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$env:PATH = (Join-Path $mingwRoot "bin") + ";" + $env:PATH
$env:MINGW_ROOT = $mingwRoot
$env:CURL_ROOT = $curlRoot
$env:AIAGENT_DEPS_DIR = $depsRoot

function Require-File([string]$Path, [string]$Hint) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "missing $Path; $Hint"
    }
}

function Require-Command([string]$Name, [string]$Hint) {
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if (-not $command) {
        throw "missing command '$Name'; $Hint"
    }
    return $command.Source
}

function Write-LogLine([string]$Log, [object]$Line) {
    Add-Content -LiteralPath $Log -Value $Line -Encoding UTF8
}

function Invoke-Logged([string]$Label, [string]$Log, [scriptblock]$Action) {
    Write-Host "--- $Label"
    Write-LogLine $Log "`n### $Label"
    & $Action 2>&1 | ForEach-Object {
        Write-Host $_
        Write-LogLine $Log $_
    }
    $code = $LASTEXITCODE
    if ($null -ne $code -and $code -ne 0) {
        throw "$Label failed (exit=$code)"
    }
}

$cmake = Require-Command "cmake" "install CMake or add it to PATH"
$ctest = Join-Path (Split-Path -Parent $cmake) "ctest.exe"
Require-File $ctest "ctest should be installed next to cmake"
$ninja = Require-Command "ninja" "install Ninja or add it to PATH"
Require-File (Join-Path $mingwRoot "bin\g++.exe") "install MinGW-w64 or set MINGW_ROOT"
Require-File (Join-Path $curlRoot "include\curl\curl.h") "build static libcurl or set CURL_ROOT"
Require-File (Join-Path $depsRoot "cpp-httplib-0.18.3\httplib.h") "download cpp-httplib or set AIAGENT_DEPS_DIR"
Require-File (Join-Path $depsRoot "sqlite-amalgamation-3490100\sqlite3.c") "download sqlite amalgamation or set AIAGENT_DEPS_DIR"
Require-File $toolchain "toolchain file is missing"

$results = @()

function Add-Result([string]$Stage, [string]$Status, [string]$Detail, [string]$Log) {
    $script:results += [pscustomobject]@{
        Stage  = $Stage
        Status = $Status
        Detail = $Detail
        Log    = $Log
    }
}

# ---------------------------------------------------------------------------
# 1. a2a-protocol: standalone build, ctest, install, external consumer
# ---------------------------------------------------------------------------
$stageLog = Join-Path $logDir "a2a.log"
try {
    if ($SkipA2A) {
        Add-Result "a2a" "SKIP" "requested by -SkipA2A" $stageLog
    } else {
        $gtest = Join-Path $depsRoot "googletest-1.15.2"
        Require-File (Join-Path $gtest "CMakeLists.txt") "download googletest 1.15.2 to $gtest"
        $a2aBuild = Join-Path $repo "build-a2a"
        $a2aInstall = Join-Path $a2aBuild "install"
        $consumerBuild = Join-Path $a2aBuild "consumer"
        Remove-Item -LiteralPath $a2aBuild -Recurse -Force -ErrorAction SilentlyContinue

        $a2aArgs = @(
            "-S", (Join-Path $repo "vendor/a2a-protocol"),
            "-B", $a2aBuild,
            "-G", "Ninja",
            "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DA2A_BUILD_TESTS=ON",
            "-DA2A_BUILD_HTTP_SERVER=ON",
            "-DA2A_BUILD_SQLITE_STORE=ON",
            "-DFETCHCONTENT_FULLY_DISCONNECTED=ON",
            "-DFETCHCONTENT_SOURCE_DIR_CPP_HTTPLIB=$($depsRoot)\cpp-httplib-0.18.3",
            "-DFETCHCONTENT_SOURCE_DIR_SQLITE3=$($depsRoot)\sqlite-amalgamation-3490100",
            "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=$gtest"
        )
        Invoke-Logged "a2a configure" $stageLog { & $cmake @a2aArgs }
        Invoke-Logged "a2a build" $stageLog { & $cmake --build $a2aBuild -j }
        Invoke-Logged "a2a ctest" $stageLog {
            & $ctest --test-dir $a2aBuild --output-on-failure
        }
        Invoke-Logged "a2a install" $stageLog { & $cmake --install $a2aBuild --prefix $a2aInstall }
        $consumerArgs = @(
            "-S", (Join-Path $repo "tests/a2a_consumer"),
            "-B", $consumerBuild,
            "-G", "Ninja",
            "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_PREFIX_PATH=$a2aInstall;$curlRoot",
            "-DCURL_ROOT=$curlRoot"
        )
        Invoke-Logged "a2a consumer configure" $stageLog { & $cmake @consumerArgs }
        Invoke-Logged "a2a consumer build" $stageLog { & $cmake --build $consumerBuild -j }
        Invoke-Logged "a2a consumer run" $stageLog {
            & (Join-Path $consumerBuild "a2a_consumer.exe")
        }
        Add-Result "a2a" "PASS" "ctest + install + consumer" $stageLog
    }
} catch {
    Add-Result "a2a" "FAIL" $_.Exception.Message $stageLog
}

# ---------------------------------------------------------------------------
# 2. Root project: configure/build/ctest through the existing entry point
# ---------------------------------------------------------------------------
$stageLog = Join-Path $logDir "root.log"
try {
    Invoke-Logged "root build + ctest" $stageLog {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass `
            -File (Join-Path $repo "scripts/build-windows.ps1") `
            -Configuration $Configuration
    }
    $testLog = Join-Path $buildDir "Testing\Temporary\LastTest.log"
    $skips = @()
    if (Test-Path -LiteralPath $testLog) {
        $skips = @(Select-String -LiteralPath $testLog -Pattern "SKIP:" |
            ForEach-Object { $_.Line.Trim() })
    }
    $skipText = if ($skips.Count -gt 0) { "$($skips.Count) explicit SKIP(s)" } else { "no SKIP lines" }
    Add-Result "root" "PASS" $skipText $stageLog
    foreach ($skip in $skips) {
        Write-Host "SKIP  $skip"
    }
} catch {
    Add-Result "root" "FAIL" $_.Exception.Message $stageLog
}

# ---------------------------------------------------------------------------
# 3. CLI tests
# ---------------------------------------------------------------------------
$stageLog = Join-Path $logDir "cli.log"
try {
    $node = Require-Command "node" "install Node.js >= 18 or add it to PATH"
    Invoke-Logged "cli node --test" $stageLog {
        Push-Location (Join-Path $repo "cli")
        try {
            & $node --test
        } finally {
            Pop-Location
        }
    }
    Add-Result "cli" "PASS" "node --test" $stageLog
} catch {
    Add-Result "cli" "FAIL" $_.Exception.Message $stageLog
}

# ---------------------------------------------------------------------------
# 4. MCP: build the add-on server and run every Python integration test
# ---------------------------------------------------------------------------
$stageLog = Join-Path $logDir "mcp.log"
try {
    if ($SkipMcp) {
        Add-Result "mcp" "SKIP" "requested by -SkipMcp" $stageLog
    } else {
        $python = if ($env:PYTHON_BIN) { $env:PYTHON_BIN } else {
            (Require-Command "python" "install Python 3 or set PYTHON_BIN")
        }
        Require-File $python "install Python 3 or set PYTHON_BIN"
        $mcpBuild = Join-Path $repo "build-mcp-release"
        $mcpExe = Join-Path $mcpBuild "mcp_server\mcp_server.exe"
        if (-not (Test-Path -LiteralPath $mcpExe)) {
            $mcpArgs = @(
                "-S", (Join-Path $repo "vendor/mcp-extension-platform"),
                "-B", $mcpBuild,
                "-G", "Ninja",
                "-DCMAKE_TOOLCHAIN_FILE=$toolchain",
                "-DCMAKE_BUILD_TYPE=Release"
            )
            Invoke-Logged "mcp configure" $stageLog { & $cmake @mcpArgs }
            Invoke-Logged "mcp build" $stageLog { & $cmake --build $mcpBuild -j }
        }
        $env:MCP_SERVER_BIN = $mcpExe
        $env:MCP_PLUGIN_DIR = Join-Path $repo "vendor/mcp-extension-platform/mcp_server/plugins"
        $env:PYTHON_BIN = $python
        $tests = Get-ChildItem -LiteralPath `
            (Join-Path $repo "vendor/mcp-extension-platform/mcp_server/tests") `
            -Filter "test_*.py" | Sort-Object Name
        if ($tests.Count -eq 0) {
            throw "no MCP integration tests found"
        }
        $failed = @()
        foreach ($test in $tests) {
            Write-Host "--- mcp $($test.Name)"
            Write-LogLine $stageLog "`n### $($test.Name)"
            & $python $test.FullName 2>&1 | ForEach-Object {
                Write-Host $_
                Write-LogLine $stageLog $_
            }
            if ($LASTEXITCODE -ne 0) {
                $failed += $test.Name
            }
        }
        if ($failed.Count -gt 0) {
            throw "MCP tests failed: $($failed -join ', ')"
        }
        $mcpSkips = @(Select-String -LiteralPath $stageLog -Pattern "SKIP:" |
            ForEach-Object { $_.Line.Trim() } | Select-Object -Unique)
        foreach ($skip in $mcpSkips) {
            Write-Host "SKIP  $skip"
        }
        Add-Result "mcp" "PASS" "$($tests.Count) scripts, $($mcpSkips.Count) explicit SKIP(s)" $stageLog
    }
} catch {
    Add-Result "mcp" "FAIL" $_.Exception.Message $stageLog
}

# ---------------------------------------------------------------------------
# 5. Optional package gate
# ---------------------------------------------------------------------------
$stageLog = Join-Path $logDir "package.log"
if ($Package) {
    try {
        Invoke-Logged "package -WithMcp" $stageLog {
            & powershell.exe -NoProfile -ExecutionPolicy Bypass `
                -File (Join-Path $repo "scripts/package-windows.ps1") -WithMcp
        }
        Add-Result "package" "PASS" "-WithMcp" $stageLog
    } catch {
        Add-Result "package" "FAIL" $_.Exception.Message $stageLog
    }
} else {
    Add-Result "package" "SKIP" "use -Package to enable" $stageLog
}

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=== Windows regression summary ==="
$results | Format-Table -AutoSize | Out-String | Write-Host
$failures = @($results | Where-Object { $_.Status -eq "FAIL" })
if ($failures.Count -gt 0) {
    foreach ($failure in $failures) {
        Write-Host "FAILED: $($failure.Stage): $($failure.Detail)"
        Write-Host "log: $($failure.Log)"
    }
    exit 1
}
Write-Host "RESULT: PASS"
exit 0
