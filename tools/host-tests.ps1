<#
.SYNOPSIS
    Builds and runs the host-side self tests (jellyfin::selfTest, downloads::selfTest) in a Linux
    GCC 14 container. Uses Docker if the daemon answers, otherwise rootful podman in the
    podman-machine WSL distro (same as build.ps1).

.EXAMPLE
    ./tools/host-tests.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('auto', 'docker', 'podman-wsl')]
    [string]$Engine = 'auto',
    [string]$WslDistro = 'podman-machine-default',
    [string]$Image = 'docker.io/library/gcc:14'
)

$ErrorActionPreference = 'Stop'
$Repo = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not (Test-Path (Join-Path $Repo 'build/third_party/json.hpp'))) {
    throw 'build/third_party/json.hpp is missing; run ./build.ps1 once to fetch vendored headers'
}

function Test-Docker {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { return $false }
    $ver = & docker version --format '{{.Server.Version}}' 2> $null
    return ($LASTEXITCODE -eq 0) -and -not [string]::IsNullOrWhiteSpace("$ver")
}

$kind = $Engine
if ($kind -eq 'auto') { $kind = if (Test-Docker) { 'docker' } else { 'podman-wsl' } }

if ($kind -eq 'docker') {
    $src = $Repo
} else {
    if ($Repo -notmatch '^([A-Za-z]):\\(.*)$') { throw "cannot map path to WSL: $Repo" }
    $src = '/mnt/' + $Matches[1].ToLowerInvariant() + '/' + ($Matches[2] -replace '\\', '/')
}

$runArgs = @('run', '--rm', '-v', "${src}:/src", '-w', '/src', $Image, 'sh', '-c',
    "tr -d '\r' < /src/tests/host/run.sh | sh")
Write-Host "==> host tests ($kind, $Image)" -ForegroundColor Cyan
# Shares build.ps1's lock so a test run never competes with a full build for the VM's memory.
$buildMutex = [System.Threading.Mutex]::new($false, 'Local\karafin-container-build')
try {
    try { [void]$buildMutex.WaitOne() } catch [System.Threading.AbandonedMutexException] {}
    if ($kind -eq 'docker') { & docker @runArgs 2>&1 | Out-Host }
    else { & wsl.exe -d $WslDistro -u root -e podman @runArgs 2>&1 | Out-Host }
    $code = $LASTEXITCODE
} finally {
    $buildMutex.ReleaseMutex()
    $buildMutex.Dispose()
}
if ($code -ne 0) { throw "host tests failed (exit $code)" }
Write-Host '==> host tests passed' -ForegroundColor Green
