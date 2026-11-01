# Windows 启动器端到端测试（source / package / installed 布局）。
# 用法：powershell -NoProfile -ExecutionPolicy Bypass -File tests/de_script_test.ps1

param(
    [string]$BackendExe = ""
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$backendSource = if ($BackendExe) { $BackendExe } else { Join-Path $repo "build-mingw\aiagent_app.exe" }
$temp = Join-Path $env:TEMP ("de-launcher-test-" + $PID)
$failures = 0

function Fail([string]$Message) {
    Write-Host "FAILED: $Message"
    $script:failures++
}

function Assert([bool]$Condition, [string]$Message) {
    if (-not $Condition) { Fail $Message }
}

function Assert-Exit([int]$Expected, [string]$Message) {
    if ($LASTEXITCODE -ne $Expected) {
        Fail "$Message (exit=$LASTEXITCODE, expected=$Expected)"
    }
}

function Copy-Launcher([string]$Destination) {
    New-Item -ItemType Directory -Force -Path $Destination | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "de.ps1") -Destination $Destination
    Copy-Item -LiteralPath (Join-Path $repo "de.cmd") -Destination $Destination
}

function Copy-Cli([string]$Destination) {
    Copy-Item -LiteralPath (Join-Path $repo "cli") -Destination $Destination -Recurse
}

function New-TestConfig([string]$Path, [string]$DataDir) {
    $slashData = $DataDir -replace "\\", "/"
    $slashRuntime = (Join-Path $DataDir "runtime") -replace "\\", "/"
    $content = @"
[deepseek]
api_key = launcher-test-key
base_url = http://127.0.0.1:9
model = launcher-test
reasoning_model = launcher-test
timeout_seconds = 5
max_retries = 0

[mcp]
binary =
plugins =

[storage]
data_dir = $slashData

[logging]
level = info
file = $slashRuntime/backend.log
console = false

[agents]
enabled = false
"@
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) | Out-Null
    # 用 ASCII 写，避免 PowerShell 5.1 的 UTF-8 BOM 干扰 INI 首行。
    Set-Content -LiteralPath $Path -Value $content -Encoding ASCII
}

function Choose-Ports {
    $listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
    $listener.Start()
    $port = $listener.LocalEndpoint.Port
    $listener.Stop()
    return @($port, ($port + 1))
}

$a2a = 0
$web = 0
try {
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    $env:DE_HOME = Join-Path $temp "data"
    $env:AIAGENT_RUNTIME_DIR = Join-Path $temp "runtime"
    Remove-Item Env:AIAGENT_CONFIG -ErrorAction SilentlyContinue
    Remove-Item Env:AIAGENT_DB -ErrorAction SilentlyContinue
    Remove-Item Env:AIAGENT_PORT -ErrorAction SilentlyContinue
    Remove-Item Env:AIAGENT_BACKEND_PORT -ErrorAction SilentlyContinue
    Remove-Item Env:AIAGENT_WEB_PORT -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path (Join-Path $temp "build-mingw") | Out-Null

    # ---- source layout ----
    Copy-Launcher $temp
    Copy-Cli (Join-Path $temp "cli")
    Copy-Item -LiteralPath (Join-Path $repo "conf") -Destination $temp -Recurse
    Copy-Item -LiteralPath $backendSource `
        -Destination (Join-Path $temp "build-mingw\aiagent_app.exe")

    $de = Join-Path $temp "de.cmd"
    & $de init *> (Join-Path $temp "init.out")
    Assert-Exit 0 "source init"
    Assert (Test-Path -LiteralPath (Join-Path $temp "conf\conf.ini")) "source init creates conf.ini"
    Assert (Test-Path -LiteralPath (Join-Path $temp "conf\cli.ini")) "source init creates cli.ini"

    $config = Join-Path $temp "conf\conf.ini"
    $db = Join-Path $temp "data\aiagent_tasks.db"
    $log = Join-Path $temp "runtime\backend.log"
    $runtime = Join-Path $temp "runtime"
    New-TestConfig $config (Join-Path $temp "data")
    $ports = Choose-Ports
    $a2a = $ports[0]
    $web = $ports[1]

    $doctorOut = (& $de doctor --config $config --db $db --a2a-port $a2a --backend-port $web *>&1 | Out-String)
    Assert-Exit 0 "source doctor"
    Assert ($doctorOut -match "layout:\s+source") "doctor reports source layout"

    # 从其它 cwd 调用也必须能找到脚本旁边的布局。
    $otherCwd = Split-Path -Parent $temp
    Push-Location $otherCwd
    try {
        $otherOut = (& $de doctor --config $config --db $db --a2a-port $a2a --backend-port $web *>&1 | Out-String)
        Assert-Exit 0 "doctor from another cwd"
        if ($LASTEXITCODE -ne 0) { Write-Host $otherOut }
    } finally {
        Pop-Location
    }

    # 不要用 PowerShell 的 *> 重定向包住 restart：backend 会继承调用方的输出句柄，
    # PowerShell 5.1 会等待该句柄关闭，表现为 restart 永不返回。
    & $de restart --config $config --db $db --a2a-port $a2a --backend-port $web `
        --log-file $log
    Assert-Exit 0 "source restart"
    Assert (Test-Path -LiteralPath (Join-Path $runtime "backend.pid")) "pid file written"
    $status = (& $de status --a2a-port $a2a --backend-port $web *>&1 | Out-String)
    Assert ($status -match "running") "status reports running"

    & $de stop --a2a-port $a2a --backend-port $web *> (Join-Path $temp "stop.out")
    Assert-Exit 0 "source stop"
    Assert (-not (Test-Path -LiteralPath (Join-Path $runtime "backend.pid"))) "pid file removed after stop"

    $helpOut = (& $de --no-backend -- --help *>&1 | Out-String)
    Assert-Exit 0 "CLI passthrough --help"
    Assert ($helpOut -match "usage: node cli.mjs") "CLI help reached through launcher"

    $launcherHelp = (& $de help *>&1 | Out-String)
    Assert-Exit 0 "launcher help"
    Assert ($launcherHelp -match "review --background") "launcher help lists review jobs"
    Assert ($launcherHelp -match "review prepare") "launcher help lists review prepare"

    & $de purge --yes *> (Join-Path $temp "purge.out")
    Assert-Exit 0 "source purge"
    Assert (-not (Test-Path -LiteralPath $env:DE_HOME)) "purge removes data dir"

    # ---- package layout ----
    $pkg = Join-Path $temp "pkg"
    New-Item -ItemType Directory -Force -Path (Join-Path $pkg "bin") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $pkg "lib\de") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $pkg "share\de") | Out-Null
    Copy-Launcher $pkg
    Copy-Item -LiteralPath $backendSource `
        -Destination (Join-Path $pkg "bin\aiagent_app.exe")
    Copy-Cli (Join-Path $pkg "lib\de\cli")
    Copy-Item -LiteralPath (Join-Path $repo "conf") -Destination (Join-Path $pkg "share\de") -Recurse
    $dePkg = Join-Path $pkg "de.cmd"
    $pkgData = Join-Path $temp "pkgdata"
    $env:DE_HOME = $pkgData
    & $dePkg init *> (Join-Path $temp "pkg-init.out")
    Assert-Exit 0 "package init"
    Assert (Test-Path -LiteralPath (Join-Path $pkgData "conf\conf.ini")) "package init writes data conf"
    $pkgDoctor = (& $dePkg doctor --config (Join-Path $pkgData "conf\conf.ini") `
        --a2a-port $a2a --backend-port $web *>&1 | Out-String)
    Assert-Exit 0 "package doctor"
    Assert ($pkgDoctor -match "layout:\s+package") "doctor reports package layout"

    # ---- installed layout ----
    $installed = Join-Path $temp "installed"
    New-Item -ItemType Directory -Force -Path (Join-Path $installed "bin") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $installed "lib\de") | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $installed "share\de") | Out-Null
    Copy-Launcher (Join-Path $installed "bin")
    Copy-Item -LiteralPath $backendSource `
        -Destination (Join-Path $installed "bin\aiagent_app.exe")
    Copy-Cli (Join-Path $installed "lib\de\cli")
    Copy-Item -LiteralPath (Join-Path $repo "conf") -Destination (Join-Path $installed "share\de") -Recurse
    $deInstalled = Join-Path $installed "bin\de.cmd"
    $instData = Join-Path $temp "instdata"
    $env:DE_HOME = $instData
    & $deInstalled init *> (Join-Path $temp "inst-init.out")
    Assert-Exit 0 "installed init"
    $instDoctor = (& $deInstalled doctor --config (Join-Path $instData "conf\conf.ini") `
        --a2a-port $a2a --backend-port $web *>&1 | Out-String)
    Assert-Exit 0 "installed doctor"
    Assert ($instDoctor -match "layout:\s+installed") "doctor reports installed layout"
} catch {
    Write-Host "FAILED: exception: $($_.Exception.Message)"
    Write-Host $_.ScriptStackTrace
    $failures++
} finally {
    # 失败时也尽量清掉后端。
    if ($null -ne $de -and (Test-Path -LiteralPath $de)) {
        try {
            & $de stop --a2a-port $a2a --backend-port $web *> (Join-Path $temp "cleanup-stop.out")
        } catch {
        }
    }
    Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures -gt 0) {
    Write-Host "de launcher test failed with $failures failure(s)"
    exit 1
}
Write-Host "de launcher test passed"
exit 0
