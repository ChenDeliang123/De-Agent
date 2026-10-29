# Build and package the Windows x64 portable ZIP.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-windows.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-windows.ps1 -WithMcp
#   ... -NoBuild        # reuse an existing release build
#
# Output:
#   dist/v<version>/de-agent-<version>-windows-x64.zip
#   dist/v<version>/de-agent-<version>-windows-x64.zip.sha256
#   dist/v<version>/de-agent-mcp-<version>-windows-x64.zip       # -WithMcp
#   dist/v<version>/SHA256SUMS
#   dist/v<version>/RELEASE.md                                   # generated if absent
#
# Layout inside the main ZIP:
#   de.cmd, de.ps1, bin/aiagent_app.exe, lib/de/cli, share/de/{conf,prompts}
# The optional MCP ZIP contains share/de/mcp/ and is extracted over the main
# package root.

param(
    [string]$BuildDir = "build-mingw-release",
    [string]$McpBuildDir = "build-mcp-release",
    [switch]$WithMcp,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$cmakeFile = Join-Path $repo "CMakeLists.txt"
$content = Get-Content -LiteralPath $cmakeFile -Raw
if ($content -notmatch 'project\(\s*ai-agent-cpp\s+VERSION\s+([0-9]+\.[0-9]+\.[0-9]+)') {
    throw "cannot determine version from CMakeLists.txt"
}
$version = $Matches[1]
$packageName = "de-agent-$version-windows-x64"
$outDir = Join-Path $repo "dist\v$version"
$stageRoot = Join-Path $outDir ".stage-$version"
$stage = Join-Path $stageRoot $packageName
$releaseDate = Get-Date -Format "yyyy-MM-dd"

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Invoke-Checked([string]$Label, [scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE -ne 0) {
        throw "$Label failed (exit=$LASTEXITCODE)"
    }
}

function Write-Utf8NoBom([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, (New-Object Text.UTF8Encoding($false)))
}

function Write-Checksum([string]$Path) {
    $hash = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    $name = Split-Path -Leaf $Path
    # '*' marks binary mode; Git/MSYS sha256sum -c otherwise opens files in text
    # mode and reports false mismatches for ZIPs.
    Write-Utf8NoBom "$Path.sha256" "$hash *$name`n"
}

# .NET Framework's CreateFromDirectory writes '\' in entry names, which is not
# the ZIP spec and makes unzip warn. Build entries explicitly with '/'.
function New-PortableZip([string]$SourceDir, [string]$ZipPath, [string]$RootPrefix) {
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $stream = [IO.File]::Open($ZipPath, [IO.FileMode]::Create)
    try {
        $archive = New-Object System.IO.Compression.ZipArchive(
            $stream, [System.IO.Compression.ZipArchiveMode]::Create)
        try {
            foreach ($file in Get-ChildItem -LiteralPath $SourceDir -Recurse -File) {
                $relative = $file.FullName.Substring($SourceDir.Length)
                $relative = $relative.TrimStart([char[]]@('\', '/')).Replace('\', '/')
                $entryName = if ($RootPrefix) { "$RootPrefix/$relative" } else { $relative }
                $entry = $archive.CreateEntry(
                    $entryName, [System.IO.Compression.CompressionLevel]::Optimal)
                $entryStream = $entry.Open()
                try {
                    $fileStream = [IO.File]::OpenRead($file.FullName)
                    try {
                        $fileStream.CopyTo($entryStream)
                    } finally {
                        $fileStream.Dispose()
                    }
                } finally {
                    $entryStream.Dispose()
                }
            }
        } finally {
            $archive.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
if (-not $NoBuild) {
    $buildPath = Join-Path $repo $BuildDir
    if (-not (Test-Path -LiteralPath (Join-Path $buildPath "CMakeCache.txt"))) {
        if ($BuildDir -ne "build-mingw-release") {
            throw "build dir $BuildDir is not configured; run cmake first or use the default preset"
        }
        Invoke-Checked "cmake configure" { & cmake --preset windows-mingw-release }
    }
    Invoke-Checked "cmake build" { & cmake --build $buildPath -j }
}

# ---------------------------------------------------------------------------
# Stage main package
# ---------------------------------------------------------------------------
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $stage | Out-Null

Invoke-Checked "cmake install (deagent)" {
    & cmake --install (Join-Path $repo $BuildDir) --prefix $stage --component deagent
}

# The launcher is installed in bin/; also expose it at the package root so
# double-click/PowerShell usage matches the documented layout.
Copy-Item -LiteralPath (Join-Path $stage "bin\de.cmd") -Destination (Join-Path $stage "de.cmd")
Copy-Item -LiteralPath (Join-Path $stage "bin\de.ps1") -Destination (Join-Path $stage "de.ps1")

# Never ship user secrets/tests/dependency caches.
$forbidden = Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Name -eq "conf.ini" -or $_.Name -eq "cli.ini" -or
    $_.Name -eq ".rag_index.json" -or
    $_.FullName -match "\\tests\\" -or
    $_.FullName -match "\\node_modules\\" -or
    $_.FullName -match "\\\.runtime\\"
}
if ($forbidden) {
    throw ("package contains forbidden files:`n" +
        (($forbidden | ForEach-Object { $_.FullName }) -join "`n"))
}

$zipPath = Join-Path $outDir "$packageName.zip"
if (Test-Path -LiteralPath $zipPath) {
    Remove-Item -LiteralPath $zipPath -Force
}
New-PortableZip $stage $zipPath $packageName
Write-Checksum $zipPath

# ---------------------------------------------------------------------------
# Optional MCP add-on
# ---------------------------------------------------------------------------
if ($WithMcp) {
    $mcpBuildPath = Join-Path $repo $McpBuildDir
    $mcpExe = Join-Path $mcpBuildPath "mcp_server\mcp_server.exe"
    if (-not (Test-Path -LiteralPath $mcpExe)) {
        if ($NoBuild) {
            throw "MCP binary not found: $mcpExe (remove -NoBuild or build it first)"
        }
        $mcpSource = Join-Path $repo "vendor\mcp-extension-platform"
        $mingwToolchain = Join-Path $repo "cmake\toolchains\mingw-w64.cmake"
        Invoke-Checked "cmake configure (MCP)" {
            & cmake -S $mcpSource -B $mcpBuildPath -G Ninja `
                "-DCMAKE_TOOLCHAIN_FILE=$mingwToolchain" `
                -DCMAKE_BUILD_TYPE=Release
        }
        Invoke-Checked "cmake build (MCP)" { & cmake --build $mcpBuildPath -j }
    }

    $mcpStage = Join-Path $outDir ".mcp-stage-$version"
    if (Test-Path -LiteralPath $mcpStage) {
        Remove-Item -LiteralPath $mcpStage -Recurse -Force
    }
    Invoke-Checked "cmake install (mcp)" {
        & cmake --install $mcpBuildPath --prefix $mcpStage --component mcp
    }
    $mcpPackageName = "de-agent-mcp-$version-windows-x64"
    $mcpZip = Join-Path $outDir "$mcpPackageName.zip"
    if (Test-Path -LiteralPath $mcpZip) {
        Remove-Item -LiteralPath $mcpZip -Force
    }
    # The MCP package root is share/ so it can be extracted over the main package.
    New-PortableZip $mcpStage $mcpZip ""
    Write-Checksum $mcpZip
    Remove-Item -LiteralPath $mcpStage -Recurse -Force
}

# ---------------------------------------------------------------------------
# RELEASE.md (generated once; do not overwrite human notes)
# ---------------------------------------------------------------------------
$releasePath = Join-Path $outDir "RELEASE.md"
if (-not (Test-Path -LiteralPath $releasePath)) {
    $artifactLine = "$packageName.zip"
    if ($WithMcp) {
        $artifactLine += "、de-agent-mcp-$version-windows-x64.zip"
    }
    $release = @"
# de agent v$version — Windows x64

- 发布日期：$releaseDate
- 产物：$artifactLine
- 变更摘要：Windows 原生支持（MinGW-w64 构建、PowerShell 启动器 `de.cmd`/`de.ps1`、
  MCP 原生服务与插件、文件/权限/命令策略适配）。
- 兼容性：Windows 10 1809+ / Windows 11 x64；数据目录默认为
  `%USERPROFILE%\.de`（`%LOCALAPPDATA%\de` 兜底）。
- 升级：解压新版本到新目录，用户数据目录保持不变；运行 `de.cmd doctor` 检查，
  必要时 `de.cmd restart`。
- 回滚：停止后端，解压上一版本 ZIP 并运行 `de.cmd doctor`。
- 已知限制：默认 shell 为 `cmd.exe`；`system_monitor` 在 Windows 返回
  unsupported；推荐 Windows Terminal；ARM64 暂不支持。
- 校验：同目录 `SHA256SUMS` / `*.sha256`。
"@
    Write-Utf8NoBom $releasePath $release
}

# ---------------------------------------------------------------------------
# Aggregated checksums for every artifact in this version directory.
# ---------------------------------------------------------------------------
$artifacts = Get-ChildItem -LiteralPath $outDir -File | Where-Object {
    $_.Name -match '^de-agent.*\.(zip|tar\.gz|deb|rpm)$'
} | Sort-Object Name

$lines = foreach ($artifact in $artifacts) {
    $hash = (Get-FileHash -LiteralPath $artifact.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash *$($artifact.Name)"
}
Write-Utf8NoBom (Join-Path $outDir "SHA256SUMS") (($lines -join "`n") + "`n")

# Layout/forbidden-file/checksum verification (shared with manual runs).
$verifyScript = Join-Path $repo "tests\package_windows_test.ps1"
$mcpZipToVerify = if ($WithMcp) {
    Join-Path $outDir "de-agent-mcp-$version-windows-x64.zip"
} else {
    ""
}
& $verifyScript -ZipPath $zipPath -McpZipPath $mcpZipToVerify

Remove-Item -LiteralPath $stageRoot -Recurse -Force

Write-Host "artifact: $zipPath"
if ($WithMcp) {
    Write-Host "mcp add-on: $(Join-Path $outDir "de-agent-mcp-$version-windows-x64.zip")"
}
Write-Host "checksums: $(Join-Path $outDir 'SHA256SUMS')"
Write-Host "release:   $releasePath"
