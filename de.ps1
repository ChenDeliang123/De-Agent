# de.ps1 - Windows launcher, behavior aligned with the POSIX `de` script.
#
# Layouts:
#   source     <repo>\de.ps1, backend at build-mingw\aiagent_app.exe (fallback build\)
#   package    <pkg>\de.ps1,  backend at <pkg>\bin\aiagent_app.exe
#   installed  <prefix>\bin\de.ps1, backend at <prefix>\bin\aiagent_app.exe
#
# Usage: .\de.cmd [command] [options]   (or powershell -File de.ps1 ...)

$ErrorActionPreference = "Stop"

$SelfDir = $PSScriptRoot
$ScriptArgs = @($args)

function Resolve-DataDir {
    if (-not [string]::IsNullOrWhiteSpace($env:DE_HOME)) {
        return $env:DE_HOME.Trim()
    }
    if (-not [string]::IsNullOrWhiteSpace($env:XDG_DATA_HOME)) {
        return (Join-Path $env:XDG_DATA_HOME.Trim() "de")
    }
    if (-not [string]::IsNullOrWhiteSpace($env:USERPROFILE)) {
        return (Join-Path $env:USERPROFILE.Trim() ".de")
    }
    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        return (Join-Path $env:LOCALAPPDATA.Trim() "de")
    }
    return $null
}

$DataDir = Resolve-DataDir

# ---------------------------------------------------------------------------
# Layout detection
# ---------------------------------------------------------------------------
$BackendBin = $null
$Layout = $null
$Prefix = $null

$sourceBackend = $null
foreach ($candidate in @(
    (Join-Path $SelfDir "build-mingw\aiagent_app.exe"),
    (Join-Path $SelfDir "build\aiagent_app.exe")
)) {
    if (Test-Path -LiteralPath $candidate) {
        $sourceBackend = $candidate
        break
    }
}
if ($null -ne $sourceBackend) {
    $Layout = "source"
    $Prefix = $SelfDir
    $BackendBin = $sourceBackend
} elseif (Test-Path -LiteralPath (Join-Path $SelfDir "aiagent_app.exe")) {
    $Layout = "installed"
    $Prefix = Split-Path -Parent $SelfDir
    $BackendBin = Join-Path $SelfDir "aiagent_app.exe"
} elseif (Test-Path -LiteralPath (Join-Path $SelfDir "bin\aiagent_app.exe")) {
    $Layout = "package"
    $Prefix = $SelfDir
    $BackendBin = Join-Path $SelfDir "bin\aiagent_app.exe"
} elseif ((Test-Path -LiteralPath (Join-Path $SelfDir "conf")) -and
          (Test-Path -LiteralPath (Join-Path $SelfDir "cli"))) {
    $Layout = "source"
    $Prefix = $SelfDir
    $buildDir = if (Test-Path -LiteralPath (Join-Path $SelfDir "build-mingw")) { "build-mingw" } else { "build" }
    $BackendBin = Join-Path $SelfDir "$buildDir\aiagent_app.exe"
} elseif (Test-Path -LiteralPath (Join-Path $SelfDir "lib\de\cli\cli.mjs")) {
    $Layout = "installed"
    $Prefix = Split-Path -Parent $SelfDir
    $BackendBin = Join-Path $SelfDir "aiagent_app.exe"
} elseif (Test-Path -LiteralPath (Join-Path $SelfDir "bin")) {
    $Layout = "package"
    $Prefix = $SelfDir
    $BackendBin = Join-Path $SelfDir "bin\aiagent_app.exe"
} else {
    Write-Host "de: backend binary not found near $SelfDir`nlooked for build-mingw\aiagent_app.exe, build\aiagent_app.exe, bin\aiagent_app.exe, .\aiagent_app.exe"
    exit 1
}

if ($Layout -eq "source") {
    $CliDir = Join-Path $Prefix "cli"
    $DefaultConfigFile = Join-Path $Prefix "conf\conf.ini"
    $CliConfigFile = Join-Path $Prefix "conf\cli.ini"
    $buildDbDir = if (Test-Path -LiteralPath (Join-Path $Prefix "build-mingw")) { "build-mingw\db" } else { "build\db" }
    $DefaultDbPath = Join-Path $Prefix "$buildDbDir\aiagent_tasks.db"
    $DefaultRuntimeDir = Join-Path $Prefix ".runtime"
    $ShareDir = Join-Path $Prefix "share\de"
    $ExampleDir = Join-Path $Prefix "conf"
    $LaunchCwd = $Prefix
} else {
    $CliDir = Join-Path $Prefix "lib\de\cli"
    $DefaultConfigFile = Join-Path $DataDir "conf\conf.ini"
    $CliConfigFile = Join-Path $DataDir "conf\cli.ini"
    $DefaultDbPath = Join-Path $DataDir "aiagent_tasks.db"
    $DefaultRuntimeDir = Join-Path $DataDir "runtime"
    $ShareDir = Join-Path $Prefix "share\de"
    $ExampleDir = Join-Path $ShareDir "conf"
    $LaunchCwd = if ($DataDir) { $DataDir } else { $Prefix }
}

$RuntimeDir = if ($env:AIAGENT_RUNTIME_DIR) { $env:AIAGENT_RUNTIME_DIR } else { $DefaultRuntimeDir }
$PidFile = Join-Path $RuntimeDir "backend.pid"
$LogFile = Join-Path $RuntimeDir "backend.log"
$ConsoleLogFile = Join-Path $RuntimeDir "backend-console.log"
$ConsoleErrFile = Join-Path $RuntimeDir "backend-console.err.log"

$A2aPort = if ($env:AIAGENT_PORT) { [int]$env:AIAGENT_PORT } else { 18080 }
$BackendPort = if ($env:AIAGENT_BACKEND_PORT) {
    [int]$env:AIAGENT_BACKEND_PORT
} elseif ($env:AIAGENT_WEB_PORT) {
    [int]$env:AIAGENT_WEB_PORT
} else {
    18081
}
$ConfigFile = if ($env:AIAGENT_CONFIG) { $env:AIAGENT_CONFIG } else { $DefaultConfigFile }
$DbPath = if ($env:AIAGENT_DB) { $env:AIAGENT_DB } else { $DefaultDbPath }
$LogLevel = ""
$LogFileArg = ""

$Action = "cli"
$NoBackend = $false
$CliArgs = New-Object System.Collections.Generic.List[string]

for ($i = 0; $i -lt $ScriptArgs.Count; $i++) {
    $arg = [string]$ScriptArgs[$i]
    if ($arg -eq "review") {
        $Action = "review"
        for ($j = $i + 1; $j -lt $ScriptArgs.Count; $j++) {
            $CliArgs.Add([string]$ScriptArgs[$j])
        }
        break
    }
    switch -Regex ($arg) {
        '^(stop)$'            { $Action = "stop"; continue }
        '^(status)$'          { $Action = "status"; continue }
        '^(restart)$'         { $Action = "restart"; continue }
        '^(logs)$'            { $Action = "logs"; continue }
        '^(init)$'            { $Action = "init"; continue }
        '^(doctor|check)$'    { $Action = "doctor"; continue }
        '^(purge)$'           { $Action = "purge"; continue }
        '^(help|--help|-h)$'  { $Action = "help"; continue }
        '^(--no-backend)$'    { $NoBackend = $true; continue }
        '^(--a2a-port)$'      { $i++; $A2aPort = [int]$ScriptArgs[$i]; continue }
        '^(--backend-port|--web-port)$' { $i++; $BackendPort = [int]$ScriptArgs[$i]; continue }
        '^(--config)$'        { $i++; $ConfigFile = [string]$ScriptArgs[$i]; continue }
        '^(--db)$'            { $i++; $DbPath = [string]$ScriptArgs[$i]; continue }
        '^(--log-level)$'     { $i++; $LogLevel = [string]$ScriptArgs[$i]; continue }
        '^(--log-file)$'      { $i++; $LogFileArg = [string]$ScriptArgs[$i]; continue }
        '^(--)$' {
            for ($j = $i + 1; $j -lt $ScriptArgs.Count; $j++) {
                $CliArgs.Add([string]$ScriptArgs[$j])
            }
            $i = $ScriptArgs.Count
            continue
        }
        default { $CliArgs.Add($arg) }
    }
}

function Show-Usage {
    @'
usage: de [command] [options]

commands:
  (no command)     ensure backend daemon is running, then start CLI
  stop             stop the daemonized backend
  status           show backend status
  restart          stop then start backend
  logs [target]    tail log: backend (default) | cli
  init             create user config from examples (non-destructive)
  doctor           check runtime environment and resolved paths
  purge            delete user data dir (asks to confirm; --yes skips)
  review prepare   create a review pack without entering the CLI
  review <pack>    start an independent reviewer session for a pack
  review --background <pack>   submit a background review job
  review status [job_id]       list/query background review jobs
  review cancel <job_id>       cancel a background review job
  review attach <job_id>       show job state and report
  review --resume <review_id>  resume a review from its checkpoint
  help             show this help

options:
  --no-backend     only run CLI, do not start backend
  --a2a-port N     A2A port (default 18080)
  --backend-port N local backend API port for CLI (default 18081)
  --web-port N     alias of --backend-port (deprecated)
  --config FILE    DeepSeek config file
  --db FILE        SQLite database path
  --log-level N    backend log level (trace/debug/info/warn/error/fatal)
  --log-file FILE  backend structured log path
  --               pass remaining arguments to cli.mjs

environment:
  AIAGENT_PORT / AIAGENT_BACKEND_PORT / AIAGENT_CONFIG / AIAGENT_DB
  AIAGENT_WEB_PORT (deprecated alias of AIAGENT_BACKEND_PORT)
  AIAGENT_RUNTIME_DIR
  DE_HOME / XDG_DATA_HOME / USERPROFILE (user data directory)

layout: source | package | installed (auto-detected next to this script)
'@ | Write-Host
}

function Ensure-RuntimeDir {
    New-Item -ItemType Directory -Force -Path $RuntimeDir | Out-Null
}

function Test-Health {
    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($null -ne $curl) {
        & $curl.Source -sf "http://127.0.0.1:$BackendPort/health" 2>$null | Out-Null
        return ($LASTEXITCODE -eq 0)
    }
    try {
        $response = Invoke-WebRequest -Uri "http://127.0.0.1:$BackendPort/health" -UseBasicParsing -TimeoutSec 2
        return ($response.StatusCode -eq 200)
    } catch {
        return $false
    }
}

function Test-PortInUse([int]$Port) {
    $client = New-Object System.Net.Sockets.TcpClient
    try {
        $async = $client.BeginConnect("127.0.0.1", $Port, $null, $null)
        if (-not $async.AsyncWaitHandle.WaitOne(200)) {
            return $false
        }
        $client.EndConnect($async)
        return $true
    } catch {
        return $false
    } finally {
        $client.Close()
    }
}

function Get-ManagedProcess {
    if (-not (Test-Path -LiteralPath $PidFile)) {
        return $null
    }
    $pidText = (Get-Content -LiteralPath $PidFile -ErrorAction SilentlyContinue | Select-Object -First 1)
    if ([string]::IsNullOrWhiteSpace($pidText)) {
        return $null
    }
    $candidate = Get-Process -Id ([int]$pidText.Trim()) -ErrorAction SilentlyContinue
    if ($null -eq $candidate) {
        return $null
    }
    try {
        if ($candidate.Path -and (Test-Path -LiteralPath $BackendBin)) {
            $expected = (Resolve-Path -LiteralPath $BackendBin).Path
            $actual = (Resolve-Path -LiteralPath $candidate.Path).Path
            if ($actual -ine $expected) {
                return $null
            }
        }
    } catch {
        # Path can be inaccessible for another user; keep the PID as best effort.
    }
    return $candidate
}

function Init-Config {
    New-Item -ItemType Directory -Force -Path $RuntimeDir | Out-Null
    if ($DataDir) {
        New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    }
    $failed = $false

    if (Test-Path -LiteralPath $ConfigFile) {
        Write-Host "config exists: $ConfigFile"
    } else {
        $configDir = Split-Path -Parent $ConfigFile
        New-Item -ItemType Directory -Force -Path $configDir | Out-Null
        $example = Join-Path $ExampleDir "app.ini.example"
        if (Test-Path -LiteralPath $example) {
            Copy-Item -LiteralPath $example -Destination $ConfigFile
            Write-Host "created $ConfigFile"
            Write-Host "  -> set [deepseek] api_key (or export DEEPSEEK_API_KEY)"
        } else {
            Write-Host "de: example config not found: $example"
            $failed = $true
        }
    }

    if (Test-Path -LiteralPath $CliConfigFile) {
        Write-Host "cli config exists: $CliConfigFile"
    } else {
        $cliConfigDir = Split-Path -Parent $CliConfigFile
        New-Item -ItemType Directory -Force -Path $cliConfigDir | Out-Null
        $example = Join-Path $ExampleDir "cli.ini.example"
        if (Test-Path -LiteralPath $example) {
            Copy-Item -LiteralPath $example -Destination $CliConfigFile
            Write-Host "created $CliConfigFile"
        }
    }

    Write-Host ""
    Write-Host "data dir:    $(if ($DataDir) { $DataDir } else { '<none>' })"
    Write-Host "runtime dir: $RuntimeDir"
    Write-Host "next: fill api_key, then run 'de'"
    if ($failed) { return 1 }
    return 0
}

$script:DoctorFailures = 0
function Doctor-Check([string]$Label, [scriptblock]$Check) {
    try {
        if (& $Check) {
            Write-Host "  ok    $Label"
            return
        }
    } catch {
    }
    Write-Host "  FAIL  $Label"
    $script:DoctorFailures++
}

function Invoke-Doctor {
    $script:DoctorFailures = 0
    Write-Host "de doctor"
    Write-Host "  layout:      $Layout"
    Write-Host "  prefix:      $Prefix"
    Write-Host "  backend:     $BackendBin"
    Write-Host "  cli dir:     $CliDir"
    Write-Host "  share dir:   $ShareDir"
    Write-Host "  data dir:    $(if ($DataDir) { $DataDir } else { '<none>' })"
    Write-Host "  runtime dir: $RuntimeDir"
    Write-Host "  config:      $ConfigFile"
    Write-Host "  db:          $DbPath"
    Write-Host ""

    Doctor-Check "backend binary" { Test-Path -LiteralPath $BackendBin }
    Doctor-Check "cli entry" { Test-Path -LiteralPath (Join-Path $CliDir "cli.mjs") }
    Doctor-Check "config file" { Test-Path -LiteralPath $ConfigFile }

    $node = Get-Command node -ErrorAction SilentlyContinue
    if ($null -ne $node) {
        Write-Host "  ok    node: $(& $node.Source --version)"
    } else {
        Write-Host "  FAIL  node not found (>= 18 required)"
        $script:DoctorFailures++
    }

    $curl = Get-Command curl.exe -ErrorAction SilentlyContinue
    if ($null -ne $curl) {
        Write-Host "  ok    curl.exe"
    } else {
        Write-Host "  warn  curl.exe not found; health checks use Invoke-WebRequest"
    }

    Doctor-Check "data dir writable" {
        $probeDir = if ($DataDir) { $DataDir } else { $Prefix }
        New-Item -ItemType Directory -Force -Path $probeDir | Out-Null
        $probe = Join-Path $probeDir ".de-write-test"
        Set-Content -LiteralPath $probe -Value "ok" -Encoding ASCII
        Remove-Item -LiteralPath $probe -Force
        return $true
    }

    foreach ($port in @($A2aPort, $BackendPort)) {
        if (Test-PortInUse $port) {
            Write-Host "  warn  port $port already in use"
        } else {
            Write-Host "  ok    port $port free"
        }
    }

    Write-Host ""
    if ($script:DoctorFailures -gt 0) {
        Write-Host "doctor: $($script:DoctorFailures) critical problem(s) found"
        return 1
    }
    Write-Host "doctor: all critical checks passed"
    return 0
}

function Stop-Backend {
    Ensure-RuntimeDir
    $process = Get-ManagedProcess
    if ($null -ne $process) {
        Write-Host "stopping backend (pid $($process.Id))..."
        # cmd.exe 吞掉 taskkill 的 stderr：PowerShell 会把原生命令 stderr 变成
        # ErrorRecord，在 $ErrorActionPreference=Stop 下会中断后续 /F 兜底。
        & cmd.exe /c "taskkill /PID $($process.Id) /T >NUL 2>&1" | Out-Null
        for ($i = 0; $i -lt 40; $i++) {
            try { $process.Refresh() } catch { }
            if ($process.HasExited) { break }
            Start-Sleep -Milliseconds 250
        }
        try { $process.Refresh() } catch { }
        if (-not $process.HasExited) {
            & cmd.exe /c "taskkill /PID $($process.Id) /T /F >NUL 2>&1" | Out-Null
            Start-Sleep -Milliseconds 500
        }
    } elseif (Test-Path -LiteralPath $PidFile) {
        Write-Host "removing stale pid file"
    } elseif (Test-Health) {
        Write-Host "backend is running but not managed by de; stop it in its own terminal"
        return
    } else {
        Write-Host "backend is not running"
        return
    }
    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
}

function Start-Backend {
    Ensure-RuntimeDir
    if (-not (Test-Path -LiteralPath $ConfigFile)) {
        Write-Host "de: config file not found: $ConfigFile`ncopy $ExampleDir\app.ini.example to $ConfigFile and fill api_key"
        return 1
    }
    if (Test-Health) {
        return 0
    }

    $managed = Get-ManagedProcess
    if ($null -ne $managed) {
        Write-Host "waiting for backend to become healthy..."
        for ($i = 0; $i -lt 40; $i++) {
            if (Test-Health) { return 0 }
            Start-Sleep -Milliseconds 250
        }
        Write-Host "backend pid is alive but not healthy; restarting it" -ForegroundColor Yellow
        Stop-Backend
    }

    if (-not (Test-Path -LiteralPath $BackendBin)) {
        Write-Host "backend binary not found: $BackendBin`nbuild it first (cmake --build build-mingw -j)"
        return 1
    }

    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $LaunchCwd | Out-Null
    $dbDir = Split-Path -Parent $DbPath
    if ($dbDir) {
        New-Item -ItemType Directory -Force -Path $dbDir | Out-Null
    }

    $effectiveLog = if ($LogFileArg) { $LogFileArg } else { $LogFile }
    $effectiveLogDir = Split-Path -Parent $effectiveLog
    if ($effectiveLogDir) {
        New-Item -ItemType Directory -Force -Path $effectiveLogDir | Out-Null
    }
    $argParts = @(
        "--config `"$ConfigFile`"",
        "--db `"$DbPath`"",
        "--port $A2aPort",
        "--web-port $BackendPort",
        "--log-file `"$effectiveLog`""
    )
    if ($LogLevel) {
        $argParts += "--log-level $LogLevel"
    }
    $argLine = $argParts -join " "

    Write-Host "starting backend daemon (console log: $ConsoleLogFile)..."
    try {
        $process = Start-Process -FilePath $BackendBin -ArgumentList $argLine `
            -WorkingDirectory $LaunchCwd -WindowStyle Hidden `
            -RedirectStandardOutput $ConsoleLogFile `
            -RedirectStandardError $ConsoleErrFile -PassThru
    } catch {
        Write-Host "failed to start backend: $($_.Exception.Message)"
        return 1
    }
    Set-Content -LiteralPath $PidFile -Value $process.Id -Encoding ASCII

    for ($i = 0; $i -lt 120; $i++) {
        if (Test-Health) {
            Write-Host "backend ready on http://127.0.0.1:$BackendPort"
            if (-not [Console]::IsOutputRedirected -and (Test-Path -LiteralPath $ConsoleLogFile)) {
                Get-Content -LiteralPath $ConsoleLogFile -Tail 20 -Encoding UTF8 -ErrorAction SilentlyContinue |
                    Write-Host
            }
            return 0
        }
        $process.Refresh()
        if ($process.HasExited) { break }
        Start-Sleep -Milliseconds 500
    }

    Remove-Item -LiteralPath $PidFile -Force -ErrorAction SilentlyContinue
    Write-Host "backend failed to become healthy; last console log:"
    if (Test-Path -LiteralPath $ConsoleLogFile) {
        Get-Content -LiteralPath $ConsoleLogFile -Tail 20 -Encoding UTF8 | Write-Host
    }
    if (Test-Path -LiteralPath $ConsoleErrFile) {
        Get-Content -LiteralPath $ConsoleErrFile -Tail 20 -Encoding UTF8 | Write-Host
    }
    if (Test-Path -LiteralPath $effectiveLog) {
        Get-Content -LiteralPath $effectiveLog -Tail 20 -Encoding UTF8 | Write-Host
    }
    return 1
}

function Invoke-Purge {
    $assumeYes = $CliArgs.Contains("--yes")
    if ([string]::IsNullOrWhiteSpace($DataDir)) {
        Write-Host "data dir is empty; nothing to purge"
        return 0
    }
    Write-Host "purge will remove user data:"
    Write-Host "  $DataDir"
    Write-Host "  (sessions, permissions, bash policy, trust, logs, conf.ini with api key)"
    if (-not $assumeYes) {
        $answer = Read-Host "type 'yes' to confirm"
        if ($answer -ne "yes") {
            Write-Host "aborted"
            return 1
        }
    }
    Remove-Item -LiteralPath $DataDir -Recurse -Force
    Write-Host "removed $DataDir"
    if ($RuntimeDir -and (Test-Path -LiteralPath $RuntimeDir) -and
        -not $RuntimeDir.StartsWith($DataDir, [System.StringComparison]::OrdinalIgnoreCase)) {
        Write-Host "note: runtime dir is outside data dir and was kept: $RuntimeDir"
    }
    return 0
}

switch ($Action) {
    "help" { Show-Usage; exit 0 }
    "status" {
        Ensure-RuntimeDir
        $managed = Get-ManagedProcess
        if (Test-Health) {
            if ($null -ne $managed) {
                Write-Host "running (pid $($managed.Id)) on http://127.0.0.1:$BackendPort"
            } else {
                Write-Host "running (external) on http://127.0.0.1:$BackendPort"
            }
        } elseif ($null -ne $managed) {
            Write-Host "not healthy (pid $($managed.Id))"
        } else {
            Write-Host "stopped"
        }
        exit 0
    }
    "stop" { Stop-Backend; exit 0 }
    "restart" {
        Stop-Backend
        $code = Start-Backend
        exit $code
    }
    "init" { exit (Init-Config) }
    "doctor" { exit (Invoke-Doctor) }
    "purge" { exit (Invoke-Purge) }
    "logs" {
        Ensure-RuntimeDir
        $target = if ($CliArgs.Count -gt 0) { $CliArgs[0] } else { "backend" }
        if ($target -eq "backend") {
            $logPath = if ($LogFileArg) { $LogFileArg } else { $LogFile }
        } elseif ($target -eq "cli") {
            $logPath = Join-Path $RuntimeDir "cli.log"
        } else {
            Write-Host "unknown log target: $target; use backend|cli"
            exit 1
        }
        if (-not (Test-Path -LiteralPath $logPath)) {
            New-Item -ItemType File -Force -Path $logPath | Out-Null
        }
        Get-Content -LiteralPath $logPath -Wait -Tail 50 -Encoding UTF8
        exit 0
    }
    "review" {
        if (-not $NoBackend) {
            $code = Start-Backend
            if ($code -ne 0) { exit $code }
        }
        $node = Get-Command node -ErrorAction SilentlyContinue
        if ($null -eq $node) {
            Write-Host "node not found; install Node.js >= 18"
            exit 1
        }
        try {
            $reviewConfig = Invoke-RestMethod -Uri "http://127.0.0.1:$BackendPort/api/v1/reviews/config" -UseBasicParsing
        } catch {
            Write-Host "review config unavailable: $($_.Exception.Message)"
            exit 1
        }
        if (-not $reviewConfig.enabled) {
            Write-Host "review is disabled ([review] enabled=false)"
            exit 1
        }
        $oldRuntime = $env:AIAGENT_RUNTIME_DIR
        $oldShare = $env:AIAGENT_SHARE_DIR
        $env:AIAGENT_RUNTIME_DIR = $RuntimeDir
        $env:AIAGENT_SHARE_DIR = $ShareDir
        try {
            if ($CliArgs.Count -gt 0 -and $CliArgs[0] -eq "prepare") {
                $nodeArgs = @((Join-Path $CliDir "src\app\review-prepare.mjs"),
                               "--backend", "http://127.0.0.1:$BackendPort")
                for ($j = 1; $j -lt $CliArgs.Count; $j++) { $nodeArgs += $CliArgs[$j] }
                & $node.Source @nodeArgs
                exit $LASTEXITCODE
            }
            if ($CliArgs.Count -gt 0 -and $CliArgs[0] -eq "--background") {
                if ($CliArgs.Count -lt 2) {
                    Write-Host "de review --background requires a pack path"
                    exit 2
                }
                $nodeArgs = @((Join-Path $CliDir "src\app\review-job.mjs"),
                               "--backend", "http://127.0.0.1:$BackendPort",
                               "submit", $CliArgs[1])
                for ($j = 2; $j -lt $CliArgs.Count; $j++) { $nodeArgs += $CliArgs[$j] }
                & $node.Source @nodeArgs
                exit $LASTEXITCODE
            }
            if ($CliArgs.Count -gt 0 -and
                @("status", "jobs", "cancel", "attach") -contains $CliArgs[0]) {
                $nodeArgs = @((Join-Path $CliDir "src\app\review-job.mjs"),
                               "--backend", "http://127.0.0.1:$BackendPort")
                foreach ($item in $CliArgs) { $nodeArgs += $item }
                & $node.Source @nodeArgs
                exit $LASTEXITCODE
            }
            $cliArgsOut = @((Join-Path $CliDir "cli.mjs"),
                            "--backend", "http://127.0.0.1:$BackendPort")
            if (Test-Path -LiteralPath $CliConfigFile) {
                $cliArgsOut += @("--cli-config", $CliConfigFile)
            }
            if ($CliArgs.Count -eq 0) {
                $cliArgsOut += @("--role", "reviewer")
            } elseif ($CliArgs[0] -eq "--resume") {
                if ($CliArgs.Count -lt 2) {
                    Write-Host "de review --resume requires a review id"
                    exit 2
                }
                $cliArgsOut += @("--review-resume", $CliArgs[1])
            } else {
                $cliArgsOut += @("--review-pack", $CliArgs[0])
            }
            & $node.Source @cliArgsOut
            $cliStatus = $LASTEXITCODE
        } finally {
            $env:AIAGENT_RUNTIME_DIR = $oldRuntime
            $env:AIAGENT_SHARE_DIR = $oldShare
        }
        Write-Host ""
        Write-Host "Reviewer CLI exited. Backend is still running; use 'de stop' to stop it."
        exit $cliStatus
    }
    "cli" {
        if (-not $NoBackend) {
            $code = Start-Backend
            if ($code -ne 0) { exit $code }
        }

        $node = Get-Command node -ErrorAction SilentlyContinue
        if ($null -eq $node) {
            Write-Host "node not found; install Node.js >= 18"
            exit 1
        }
        $nodeArgs = @(
            (Join-Path $CliDir "cli.mjs"),
            "--backend", "http://127.0.0.1:$BackendPort"
        )
        if (Test-Path -LiteralPath $CliConfigFile) {
            $nodeArgs += @("--cli-config", $CliConfigFile)
        }
        foreach ($item in $CliArgs) {
            $nodeArgs += $item
        }

        $oldRuntime = $env:AIAGENT_RUNTIME_DIR
        $oldShare = $env:AIAGENT_SHARE_DIR
        $env:AIAGENT_RUNTIME_DIR = $RuntimeDir
        $env:AIAGENT_SHARE_DIR = $ShareDir
        try {
            & $node.Source @nodeArgs
            $cliStatus = $LASTEXITCODE
        } finally {
            $env:AIAGENT_RUNTIME_DIR = $oldRuntime
            $env:AIAGENT_SHARE_DIR = $oldShare
        }

        Write-Host ""
        Write-Host "CLI exited. Backend is still running; use 'de stop' to stop it."
        exit $cliStatus
    }
}
