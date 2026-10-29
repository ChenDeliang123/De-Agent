# Verify a generated Windows ZIP: layout, forbidden files, checksums.
# Called by scripts/package-windows.ps1; can also be run manually:
#   powershell -File tests/package_windows_test.ps1 -ZipPath dist/...zip [-McpZipPath ...]

param(
    [Parameter(Mandatory = $true)][string]$ZipPath,
    [string]$McpZipPath = ""
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) {
        throw "package test failed: $Message"
    }
}

function Get-ZipEntries([string]$Path) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        return @($archive.Entries | ForEach-Object {
            $_.FullName.Replace("\", "/")
        })
    } finally {
        $archive.Dispose()
    }
}

$pkgName = [IO.Path]::GetFileNameWithoutExtension($ZipPath)
$entries = Get-ZipEntries $ZipPath
$required = @(
    "$pkgName/de.cmd",
    "$pkgName/de.ps1",
    "$pkgName/bin/aiagent_app.exe",
    "$pkgName/bin/de.cmd",
    "$pkgName/bin/de.ps1",
    "$pkgName/lib/de/cli/cli.mjs",
    "$pkgName/share/de/conf/app.ini.example",
    "$pkgName/share/de/conf/cli.ini.example"
)
foreach ($item in $required) {
    Assert ($entries -contains $item) "missing $item"
}

$forbidden = $entries | Where-Object {
    $_ -match '(^|/)conf\.ini$' -or
    $_ -match '(^|/)cli\.ini$' -or
    $_ -match '/tests/' -or
    $_ -match '/node_modules/' -or
    $_ -match '/\.runtime/' -or
    $_ -match '/\.rag_index\.json$'
}
Assert (-not $forbidden) ("forbidden entries: " + ($forbidden -join ", "))

if ($McpZipPath) {
    Assert (Test-Path -LiteralPath $McpZipPath) "MCP zip not found: $McpZipPath"
    $mcpEntries = Get-ZipEntries $McpZipPath
    Assert ($mcpEntries -contains "share/de/mcp/mcp_server.exe") "MCP zip missing mcp_server.exe"
    Assert (($mcpEntries | Where-Object { $_ -match '^share/de/mcp/plugins/native/.*\.dll$' }).Count -gt 0) `
        "MCP zip missing native .dll"
    Assert (($mcpEntries | Where-Object { $_ -match '^share/de/mcp/plugins/python/.*\.py$' }).Count -gt 0) `
        "MCP zip missing Python plugins"
    $badMcp = $mcpEntries | Where-Object {
        $_ -match '\.rag_index\.json$' -or $_ -match '__pycache__' -or $_ -match '\.pyc$'
    }
    Assert (-not $badMcp) ("MCP zip has generated files: " + ($badMcp -join ", "))
}

# Verify SHA256SUMS next to the main zip with .NET (Git/MSYS sha256sum -c has a
# large-file bug on Windows).
$outDir = Split-Path -Parent $ZipPath
$sums = Join-Path $outDir "SHA256SUMS"
Assert (Test-Path -LiteralPath $sums) "SHA256SUMS not found"
foreach ($line in Get-Content -LiteralPath $sums) {
    if ($line -notmatch '^\s*([0-9a-fA-F]+)\s+\*?(.+?)\s*$') {
        continue
    }
    $expected = $Matches[1].ToLowerInvariant()
    $file = Join-Path $outDir $Matches[2]
    Assert (Test-Path -LiteralPath $file) "checksum target missing: $file"
    $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    Assert ($actual -eq $expected) "sha256 mismatch: $file"
}

Write-Host "package test passed: $ZipPath"
