<#
.SYNOPSIS
    Prints the NRO header, the embedded NACP (name/author/version), icon and romfs of an .nro.

.EXAMPLE
    ./tools/inspect-nro.ps1                     # build/out/karafin.nro
    ./tools/inspect-nro.ps1 path/to/app.nro
#>
param([string]$Path = (Join-Path $PSScriptRoot '../build/out/karafin.nro'))

$ErrorActionPreference = 'Stop'
$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $Path))
function U32([int]$o) { [BitConverter]::ToUInt32($bytes, $o) }
function U64([int]$o) { [BitConverter]::ToUInt64($bytes, $o) }
function Ascii([int]$o, [int]$n) { [Text.Encoding]::ASCII.GetString($bytes, $o, $n) }
function Utf8Z([int]$o, [int]$n) { ([Text.Encoding]::UTF8.GetString($bytes, $o, $n) -split "`0")[0] }

$ok = $true
function Check([bool]$cond, [string]$what) {
    if ($cond) { Write-Host "  ok    $what" -ForegroundColor Green }
    else { Write-Host "  FAIL  $what" -ForegroundColor Red; $script:ok = $false }
}

# NRO header: NroStart (0x10 bytes) then NroHeader at 0x10.
$magic = Ascii 0x10 4
$nroSize = U32 0x18
Write-Host ("file   {0} ({1:N0} bytes, {2:N2} MiB)" -f $Path, $bytes.Length, ($bytes.Length / 1MB))
Check ($magic -eq 'NRO0') "NRO magic ($magic)"
Check ($nroSize -le $bytes.Length) ("NRO image size {0:N0}" -f $nroSize)

# Asset section follows the NRO image: "ASET", version, then icon/nacp/romfs (offset, size) pairs.
$aset = [int]$nroSize
Check ((Ascii $aset 4) -eq 'ASET') 'asset section magic'
$iconOff = U64 ($aset + 0x08); $iconSize = U64 ($aset + 0x10)
$nacpOff = U64 ($aset + 0x18); $nacpSize = U64 ($aset + 0x20)
$romfsOff = U64 ($aset + 0x28); $romfsSize = U64 ($aset + 0x30)

$icon = $aset + [int]$iconOff
Check ($iconSize -gt 0 -and $bytes[$icon] -eq 0xFF -and $bytes[$icon + 1] -eq 0xD8) ("icon JPEG ({0:N0} bytes)" -f $iconSize)

Check ($nacpSize -eq 0x4000) ("NACP size 0x{0:X}" -f $nacpSize)
if ($nacpSize -eq 0x4000) {
    $nacp = $aset + [int]$nacpOff
    # Language entry 0 (American English): name[0x200], author[0x100]. Display version at 0x3060.
    $name = Utf8Z $nacp 0x200
    $author = Utf8Z ($nacp + 0x200) 0x100
    $version = Utf8Z ($nacp + 0x3060) 0x10
    Write-Host "  nacp  name='$name' author='$author' version='$version'"
    Check ($name -eq 'Karafin') 'NACP name'
    Check ($version -ne '') 'NACP version'
}

Check ($romfsSize -gt 0) ("romfs ({0:N0} bytes)" -f $romfsSize)
if ($romfsSize -gt 0) {
    # RomFS header: header size (0x50), then table offsets; the file data section starts at 0x48.
    $romfs = $aset + [int]$romfsOff
    Check ((U64 $romfs) -eq 0x50) 'romfs header size 0x50'
    $fileTableOff = U64 ($romfs + 0x38); $fileTableSize = U64 ($romfs + 0x40)
    $names = @()
    $p = 0
    while ($p -lt $fileTableSize) {
        $e = $romfs + [int]$fileTableOff + $p
        $nameLen = U32 ($e + 0x1C)
        $names += (Ascii ($e + 0x20) $nameLen)
        $p += 0x20 + [int](([math]::Ceiling($nameLen / 4.0)) * 4)
    }
    Write-Host ("  romfs {0} files: {1}" -f $names.Count, ($names -join ', '))
    Check ($names -contains 'cacert.pem') 'romfs has certs/cacert.pem'
}

if (-not $ok) { exit 1 }
