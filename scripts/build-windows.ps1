# Windows 本机构建入口（MinGW-w64 + Ninja + 静态 libcurl）。
#
# 用法：
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-windows.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/build-windows.ps1 -Configuration Release
#
# 前置：
#   - MinGW-w64（默认 D:\Tools\mingw64，可用 MINGW_ROOT 覆盖）
#   - CMake >= 3.21、Ninja
#   - 静态 libcurl（默认 D:\Tools\curl-mingw，可用 CURL_ROOT 覆盖）
#   - cpp-httplib / sqlite3 源码（默认 D:\Tools\aiagent-deps，可用 AIAGENT_DEPS_DIR 覆盖）
#
# 离线依赖布局：
#   <deps>/cpp-httplib-0.18.3/httplib.h
#   <deps>/sqlite-amalgamation-3490100/sqlite3.c

param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",
    [switch]$NoTests
)

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$mingwRoot = if ($env:MINGW_ROOT) { $env:MINGW_ROOT } else { "D:\Tools\mingw64" }
$curlRoot = if ($env:CURL_ROOT) { $env:CURL_ROOT } else { "D:\Tools\curl-mingw" }
$depsRoot = if ($env:AIAGENT_DEPS_DIR) { $env:AIAGENT_DEPS_DIR } else { "D:\Tools\aiagent-deps" }

function Require-File([string]$Path, [string]$Hint) {
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "缺少 $Path；$Hint"
    }
}

Require-File (Join-Path $mingwRoot "bin\g++.exe") "安装 MinGW-w64 或设置 MINGW_ROOT"
Require-File (Join-Path $curlRoot "include\curl\curl.h") "先构建静态 libcurl 或设置 CURL_ROOT"
Require-File (Join-Path $depsRoot "cpp-httplib-0.18.3\httplib.h") "下载 cpp-httplib v0.18.3 或设置 AIAGENT_DEPS_DIR"
Require-File (Join-Path $depsRoot "sqlite-amalgamation-3490100\sqlite3.c") "下载 sqlite amalgamation 或设置 AIAGENT_DEPS_DIR"

$env:PATH = (Join-Path $mingwRoot "bin") + ";" + $env:PATH
$env:MINGW_ROOT = $mingwRoot
$env:CURL_ROOT = $curlRoot
$env:AIAGENT_DEPS_DIR = $depsRoot

$preset = if ($Configuration -eq "Release") { "windows-mingw-release" } else { "windows-mingw-debug" }
$buildDir = if ($Configuration -eq "Release") { "build-mingw-release" } else { "build-mingw" }

Push-Location $repoRoot
try {
    Write-Host "[1/3] configure ($preset)"
    & cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "cmake configure 失败" }

    Write-Host "[2/3] build"
    & cmake --build --preset $preset -j
    if ($LASTEXITCODE -ne 0) { throw "cmake build 失败" }

    if (-not $NoTests) {
        Write-Host "[3/3] ctest"
        & ctest --preset $preset
        if ($LASTEXITCODE -ne 0) { throw "ctest 失败" }
    } else {
        Write-Host "[3/3] 跳过 ctest（-NoTests）"
    }

    Write-Host "完成：$Configuration，产物目录 $buildDir"
} finally {
    Pop-Location
}
