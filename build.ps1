<#
.SYNOPSIS
    Builds build/out/karafin.nro inside the karafin-builder container.

.DESCRIPTION
    1. Downloads the pinned packages (docker/packages.lock) into build/pkgs and verifies SHA-256.
    2. Fetches vendored headers into build/third_party.
    3. Builds the karafin-builder image (skipped when the inputs are unchanged).
    4. Runs CMake + Ninja in the container with the repo mounted at /src.
    5. Leaves the result at build/out/karafin.nro.

    Everything downloaded or generated lives under ./build. Container image layers live in the
    container engine's own storage.

.PARAMETER BuildDir
    Repo-relative CMake build directory (default build/switch). Only the default build dir copies
    the result to build/out/karafin.nro; other build dirs leave it at <BuildDir>/karafin.nro, so
    parallel builds in separate dirs do not clash.

.PARAMETER Clean
    Removes the build dir and build/out (keeps the package/header caches) before building.

.PARAMETER Purge
    Like -Clean, but also deletes build/pkgs and build/third_party and forces an image rebuild.

.PARAMETER Engine
    auto (default): use Docker if the daemon answers, otherwise rootful podman inside the
    podman-machine WSL distro. Force with 'docker' or 'podman-wsl'.

.EXAMPLE
    ./build.ps1
    ./build.ps1 -Clean -BuildType Debug
    ./build.ps1 -BuildDir build/switch-jf
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$Purge,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$BuildType = 'Release',
    [ValidateSet('auto', 'docker', 'podman-wsl')]
    [string]$Engine = 'auto',
    [string]$WslDistro = 'podman-machine-default',
    [int]$Jobs = 0,
    [switch]$RebuildImage,
    [switch]$VerboseBuild,
    [string]$BuildDir = 'build/switch'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Repo = $PSScriptRoot
$BuildRoot = Join-Path $Repo 'build'
$PkgDir = Join-Path $BuildRoot 'pkgs'
$ThirdPartyDir = Join-Path $BuildRoot 'third_party'
$OutDir = Join-Path $BuildRoot 'out'

$CmakeDirRel = ($BuildDir -replace '\\', '/').Trim('/')
if ($CmakeDirRel -eq '' -or [System.IO.Path]::IsPathRooted($CmakeDirRel) -or $CmakeDirRel -match '(^|/)\.\.(/|$)') {
    throw "-BuildDir must be a path inside the repo (e.g. build/switch-jf): $BuildDir"
}
$CmakeDir = Join-Path $Repo $CmakeDirRel
$CopyOut = $CmakeDirRel -eq 'build/switch'
$LockFile = Join-Path $Repo 'docker/packages.lock'
$ImageName = 'karafin-builder'

function Write-Step([string]$msg) { Write-Host "==> $msg" -ForegroundColor Cyan }

function Read-Lock {
    $entries = @()
    foreach ($line in Get-Content $LockFile) {
        $t = $line.Trim()
        if ($t -eq '' -or $t.StartsWith('#')) { continue }
        $f = $t -split '\s+'
        $entries += [pscustomobject]@{
            Kind = $f[0]; Name = $f[1]
            Version = if ($f.Count -gt 2) { $f[2] } else { $null }
            File = if ($f.Count -gt 3) { $f[3] } else { $null }
            Sha256 = if ($f.Count -gt 4) { $f[4] } else { $null }
            Url = if ($f.Count -gt 5) { $f[5] } else { $null }
        }
    }
    return $entries
}

function Get-Sha256([string]$path) { (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant() }

function Get-Pinned([string]$dir, $entry) {
    $dest = Join-Path $dir $entry.File
    if ((Test-Path -LiteralPath $dest) -and (Get-Sha256 $dest) -eq $entry.Sha256) { return $false }
    Write-Host "    downloading $($entry.File)"
    $tmp = "$dest.download"
    & curl.exe -fsSL --retry 3 --connect-timeout 30 -o $tmp $entry.Url
    if ($LASTEXITCODE -ne 0) { throw "download failed: $($entry.Url)" }
    $got = Get-Sha256 $tmp
    if ($got -ne $entry.Sha256) {
        Remove-Item -LiteralPath $tmp -Force
        throw "checksum mismatch for $($entry.File): expected $($entry.Sha256), got $got"
    }
    Move-Item -LiteralPath $tmp -Destination $dest -Force
    return $true
}

# ---------------------------------------------------------------------------------------------
# Container engine abstraction

function Test-Docker {
    if (-not (Get-Command docker -ErrorAction SilentlyContinue)) { return $false }
    # `docker info` exits 0 even when the daemon is unreachable; require a server version.
    $ver = & docker version --format '{{.Server.Version}}' 2> $null
    return ($LASTEXITCODE -eq 0) -and -not [string]::IsNullOrWhiteSpace("$ver")
}

function Test-PodmanWsl {
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return $false }
    & wsl.exe -d $WslDistro -u root -e podman version --format '{{.Client.Version}}' *> $null
    return $LASTEXITCODE -eq 0
}

function Resolve-Engine {
    switch ($Engine) {
        'docker' { return 'docker' }
        'podman-wsl' { return 'podman-wsl' }
    }
    if (Test-Docker) { return 'docker' }
    if (Test-PodmanWsl) { return 'podman-wsl' }
    throw "No container engine available. Start Docker Desktop, or install Podman (WSL machine '$WslDistro')."
}

function ConvertTo-EnginePath([string]$winPath) {
    $full = [System.IO.Path]::GetFullPath($winPath)
    if ($script:EngineKind -eq 'docker') { return $full }
    if ($full -notmatch '^([A-Za-z]):\\(.*)$') { throw "cannot map path to WSL: $full" }
    return '/mnt/' + $Matches[1].ToLowerInvariant() + '/' + ($Matches[2] -replace '\\', '/')
}

function Invoke-Engine([string[]]$EngineArgs) {
    if ($script:EngineKind -eq 'docker') {
        & docker @EngineArgs
    } else {
        & wsl.exe -d $WslDistro -u root -e podman @EngineArgs
    }
    if ($LASTEXITCODE -ne 0) { throw "$($script:EngineKind) $($EngineArgs[0]) failed (exit $LASTEXITCODE)" }
}

function Test-EngineImage([string]$ref) {
    if ($script:EngineKind -eq 'docker') {
        & docker image inspect $ref *> $null
    } else {
        & wsl.exe -d $WslDistro -u root -e podman image exists $ref *> $null
    }
    return $LASTEXITCODE -eq 0
}

# ---------------------------------------------------------------------------------------------

$sw = [System.Diagnostics.Stopwatch]::StartNew()

if ($Purge) {
    Write-Step 'Purging build/ caches'
    foreach ($d in @($PkgDir, $ThirdPartyDir)) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
    $Clean = $true
    $RebuildImage = $true
}
if ($Clean) {
    Write-Step "Cleaning $CmakeDirRel$(if ($CopyOut) { ' and build/out' })"
    $toClean = @($CmakeDir)
    if ($CopyOut) { $toClean += $OutDir }
    foreach ($d in $toClean) { if (Test-Path $d) { Remove-Item -Recurse -Force $d } }
}

New-Item -ItemType Directory -Force -Path $PkgDir, $ThirdPartyDir, $OutDir | Out-Null
$lock = Read-Lock

Write-Step 'Fetching pinned packages into build/pkgs'
foreach ($p in $lock | Where-Object Kind -eq 'pkg') { [void](Get-Pinned $PkgDir $p) }

Write-Step 'Fetching vendored headers into build/third_party'
foreach ($v in $lock | Where-Object Kind -eq 'vendor') { [void](Get-Pinned $ThirdPartyDir $v) }

# Stage the docker build context (build/pkgs) with the manifest + installer script, forcing LF
# so a CRLF checkout (core.autocrlf=true) cannot break the Linux side.
function Copy-Lf([string]$from, [string]$to) {
    $text = [System.IO.File]::ReadAllText($from) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($to, $text, [System.Text.UTF8Encoding]::new($false))
}
Copy-Lf $LockFile (Join-Path $PkgDir 'packages.lock')
Copy-Lf (Join-Path $Repo 'docker/install-packages.sh') (Join-Path $PkgDir 'install-packages.sh')
Copy-Lf (Join-Path $Repo 'docker/Dockerfile') (Join-Path $PkgDir 'Dockerfile')

$script:EngineKind = Resolve-Engine
Write-Step "Container engine: $script:EngineKind"

$baseImage = ($lock | Where-Object Kind -eq 'image' | Select-Object -First 1).Name
$hashInput = (Get-Content -Raw $LockFile) + (Get-Content -Raw (Join-Path $Repo 'docker/Dockerfile')) +
             (Get-Content -Raw (Join-Path $Repo 'docker/install-packages.sh'))
$sha = [System.Security.Cryptography.SHA256]::Create()
$imageTag = -join ($sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes(($hashInput -replace "`r", ''))) |
        Select-Object -First 6 | ForEach-Object { $_.ToString('x2') })
$imageRef = "localhost/${ImageName}:$imageTag"

if ($RebuildImage -or -not (Test-EngineImage $imageRef)) {
    Write-Step "Building image $imageRef"
    $ctx = ConvertTo-EnginePath $PkgDir
    Invoke-Engine @('build', '--build-arg', "BASE_IMAGE=$baseImage", '-t', $imageRef,
        '-t', "localhost/${ImageName}:latest", '-f', "$ctx/Dockerfile", $ctx)
} else {
    Write-Step "Image $imageRef is up to date"
}

Write-Step "Building karafin ($BuildType)"
$src = ConvertTo-EnginePath $Repo
$runArgs = @('run', '--rm', '-v', "${src}:/src", '-w', '/src',
    '-e', "KARAFIN_BUILD_TYPE=$BuildType",
    '-e', "KARAFIN_VERBOSE=$([int][bool]$VerboseBuild)",
    '-e', "KARAFIN_BUILD_DIR=$CmakeDirRel",
    '-e', "KARAFIN_COPY_OUT=$([int]$CopyOut)")
if ($Jobs -gt 0) { $runArgs += @('-e', "KARAFIN_JOBS=$Jobs") }
$runArgs += @($imageRef, 'sh', '-c', "tr -d '\r' < /src/docker/container-build.sh | sh")
# Builds in separate -BuildDir dirs may be started concurrently, but several full parallel builds at
# once exhaust the WSL VM's memory; take turns.
$buildMutex = [System.Threading.Mutex]::new($false, 'Local\karafin-container-build')
try {
    try {
        if (-not $buildMutex.WaitOne(0)) {
            Write-Step 'Waiting for another karafin container build to finish'
            [void]$buildMutex.WaitOne()
        }
    } catch [System.Threading.AbandonedMutexException] {}
    Invoke-Engine $runArgs
} finally {
    $buildMutex.ReleaseMutex()
    $buildMutex.Dispose()
}

$nroRel = if ($CopyOut) { 'build/out/karafin.nro' } else { "$CmakeDirRel/karafin.nro" }
$nro = Join-Path $Repo $nroRel
if (-not (Test-Path $nro)) { throw "build finished but $nro is missing" }
$size = (Get-Item $nro).Length
Write-Step ("Done in {0:n1}s: {1} ({2:n0} bytes, {3:n2} MiB)" -f $sw.Elapsed.TotalSeconds, $nroRel, $size, ($size / 1MB))
