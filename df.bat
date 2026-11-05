@echo off
rem windf - single-file df for Windows (batch + embedded PowerShell).
rem Targets Windows Vista/7+ using the built-in PowerShell. No installs.
setlocal
set "DF_SELF=%~f0"
set "DF_ARGS=%*"
set "DF_PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%DF_PS%" set "DF_PS=powershell"
"%DF_PS%" -NoProfile -ExecutionPolicy Bypass -Command "$m = '#__' + 'PS__'; $s = [string]::Join([char]10, (Get-Content -LiteralPath $env:DF_SELF)); $i = $s.IndexOf($m); Invoke-Expression $s.Substring($i + $m.Length)"
set "DF_EC=%ERRORLEVEL%"
endlocal & exit /b %DF_EC%
#__PS__
# windf.bat - single-file "df for Windows".
# Batch wrapper + embedded Windows PowerShell. Targets Windows Vista/7+
# with the built-in PowerShell 2.0. No .NET install, no external tools.
# The script below is intentionally PowerShell 2.0 compatible:
#   - Get-WmiObject (not Get-CimInstance)
#   - New-Object PSObject (not [pscustomobject])
#   - no Storage module (Get-Disk/Get-Volume/Get-Partition)

$ErrorActionPreference = 'SilentlyContinue'

function Show-DfHelp {
    Write-Host 'Usage: df [OPTION]...'
    Write-Host ''
    Write-Host 'Show disk space usage, grouped by physical disk.'
    Write-Host ''
    Write-Host '  -h, --human-readable   print sizes in powers of 1024 (default)'
    Write-Host '  -H, --si               print sizes in powers of 1000'
    Write-Host '  -k                     print sizes in 1024-byte blocks'
    Write-Host '  -T, --print-type       accepted for df compatibility (Type is always shown)'
    Write-Host '  -a, --all              include volumes without a drive letter'
    Write-Host '      --no-color         do not colorize output'
    Write-Host '      --help             display this help and exit'
    Write-Host ''
    Write-Host 'Install (df.exe only):'
    Write-Host '  df.exe --install       copy df into System32 and SysWOW64'
    Write-Host '  df.exe --install -r    overwrite an existing df'
    Write-Host ''
    Write-Host 'Output blocks:'
    Write-Host '  Each physical disk is printed as a block, with all of its logical'
    Write-Host '  volumes (drive letters) listed underneath it. Network and cloud'
    Write-Host '  mounts are printed in a separate block and marked [NET] or [CLOUD].'
    Write-Host ''
    Write-Host 'Columns:'
    Write-Host '  Volume       the drive letter (or ''-'' for a letterless volume)'
    Write-Host '  Type         the filesystem (NTFS, FAT32, exFAT, ...)'
    Write-Host '  Size/Used/Avail/Use%'
    Write-Host '               space in bytes, and the percentage in use'
    Write-Host '  Mounted on   the volume label, or the network/cloud provider'
}

function Format-DfSize {
    param([double]$Bytes, [bool]$Si, [bool]$Blocks)
    if ($Bytes -lt 0) { return 'N/A' }
    if ($Blocks) { return [string][long][math]::Ceiling($Bytes / 1024) }
    $base = 1024.0
    $units = @('B','K','M','G','T','P')
    if ($Si) { $base = 1000.0; $units = @('B','k','M','G','T','P') }
    $v = $Bytes
    $i = 0
    while ($v -ge $base -and $i -lt 5) { $v = $v / $base; $i++ }
    if ($i -eq 0) { return ('{0:0}{1}' -f $v, $units[$i]) }
    return ('{0:0.0}{1}' -f $v, $units[$i])
}

$cloudKeywords = @('mega','rclone','onedrive','dropbox','google drive','gdrive',
                   'yandex','webdav','nextcloud','icloud','pcloud','box')

function Get-DfClass {
    param($Vol)
    $combined = ('' + $Vol.Provider + ' ' + $Vol.Label).ToLower()
    foreach ($w in $cloudKeywords) { if ($combined.Contains($w)) { return 'CLOUD' } }
    if ($Vol.DriveType -eq 4 -or $Vol.Provider) { return 'NET' }
    return 'LOCAL'
}

# ---- option parsing (from DF_ARGS set by the batch wrapper) ----
$Si = $false; $Blocks = $false; $ShowType = $false; $All = $false; $NoColor = $false
$argList = @()
if ($env:DF_ARGS) { $argList = $env:DF_ARGS -split '\s+' }

foreach ($a in $argList) {
    if ($a -eq '') { continue }
    if ($a -ceq '--help' -or $a -ceq '-?') { Show-DfHelp; exit 0 }
    elseif ($a -ceq '-h' -or $a -ceq '--human-readable') { }
    elseif ($a -ceq '-H' -or $a -ceq '--si') { $Si = $true; $Blocks = $false }
    elseif ($a -ceq '-k') { $Blocks = $true; $Si = $false }
    elseif ($a -ceq '-T' -or $a -ceq '--print-type') { $ShowType = $true }
    elseif ($a -ceq '-a' -or $a -ceq '--all') { $All = $true }
    elseif ($a -ceq '--no-color') { $NoColor = $true }
    elseif ($a -ceq '--install' -or $a -ceq '-i' -or
            $a -ceq '--replace' -or $a -ceq '-r' -or
            $a -cmatch '^-[ir]+$') {
        Write-Host 'df: --install is only available in the df.exe build.'
        Write-Host '   Run:  df.exe --install'
        exit 2
    }
    elseif ($a -cmatch '^-[hHkTa]+$') {
        foreach ($c in $a.Substring(1).ToCharArray()) {
            if ($c -ceq 'h') { }
            elseif ($c -ceq 'H') { $Si = $true; $Blocks = $false }
            elseif ($c -ceq 'k') { $Blocks = $true; $Si = $false }
            elseif ($c -ceq 'T') { $ShowType = $true }
            elseif ($c -ceq 'a') { $All = $true }
        }
    }
    elseif ($a.StartsWith('-')) {
        Write-Host ("df: unrecognized option '{0}'" -f $a)
        Write-Host "Try 'df --help' for more information."
        exit 2
    }
}

# ---- gather physical disks (WMI) ----
$disks = @()
foreach ($d in Get-WmiObject -Class Win32_DiskDrive) {
    $media = 'disk'
    if ('' + $d.MediaType -match 'fixed') { $media = 'Fixed' }
    if ('' + $d.Model -match 'SSD') { $media = 'SSD' }
    $disks += New-Object PSObject -Property @{
        Number = [int]$d.Index
        Name   = [string]$d.Model
        Serial = ([string]$d.SerialNumber).Trim()
        Size   = [double]$d.Size
        Bus    = [string]$d.InterfaceType
        Media  = $media
    }
}

# ---- map partition device id -> disk number ----
$partToDisk = @{}
foreach ($a in Get-WmiObject -Class Win32_DiskDriveToDiskPartition) {
    $m = [regex]::Match([string]$a.Antecedent.DeviceID, 'PHYSICALDRIVE(\d+)')
    if ($m.Success) { $partToDisk[[string]$a.Dependent.DeviceID] = [int]$m.Groups[1].Value }
}

$letterToDisk = @{}
foreach ($a in Get-WmiObject -Class Win32_LogicalDiskToPartition) {
    $letter = ([string]$a.Dependent.DeviceID).TrimEnd(':')
    $partId = [string]$a.Antecedent.DeviceID
    if ($partToDisk.ContainsKey($partId)) { $letterToDisk[$letter] = $partToDisk[$partId] }
}

# ---- gather logical volumes ----
$vols = @()
foreach ($v in Get-WmiObject -Class Win32_LogicalDisk) {
    $letter = ([string]$v.DeviceID).TrimEnd(':')
    $size = 0.0; if ($v.Size) { $size = [double]$v.Size }
    $free = 0.0; if ($v.FreeSpace) { $free = [double]$v.FreeSpace }
    $dn = -1
    if ($letterToDisk.ContainsKey($letter)) { $dn = $letterToDisk[$letter] }
    $vols += New-Object PSObject -Property @{
        Letter    = $letter
        Label     = [string]$v.VolumeName
        FS        = [string]$v.FileSystem
        Size      = $size
        Free      = $free
        DriveType = [int]$v.DriveType
        Provider  = [string]$v.ProviderName
        Disk      = $dn
    }
}

# ---- letterless volumes (only with -a) ----
if ($All) {
    foreach ($v in Get-WmiObject -Class Win32_Volume) {
        if ($v.DriveLetter) { continue }
        $size = 0.0; if ($v.Capacity) { $size = [double]$v.Capacity }
        $free = 0.0; if ($v.FreeSpace) { $free = [double]$v.FreeSpace }
        $vols += New-Object PSObject -Property @{
            Letter    = ''
            Label     = [string]$v.Label
            FS        = [string]$v.FileSystem
            Size      = $size
            Free      = $free
            DriveType = 3
            Provider  = ''
            Disk      = -1
        }
    }
}

# ---- rendering ----
function Write-DfLine {
    param([string]$Text, [string]$Color)
    if ($NoColor -or $Color -eq '') { Write-Host $Text; return }
    # Set only the foreground via .NET so the console background is preserved.
    # Write-Host -ForegroundColor pulls the background from RawUI.BackgroundColor,
    # which is often Black even when the window itself is blue.
    try {
        $prev = [Console]::ForegroundColor
        [Console]::ForegroundColor = [ConsoleColor]$Color
        Write-Host $Text
        [Console]::ForegroundColor = $prev
    } catch {
        Write-Host $Text
    }
}

function Get-DfHeader {
    return '  {0,-8} {1,-8} {2,9} {3,9} {4,9} {5,5}  {6}' -f 'Volume','Type','Size','Used','Avail','Use%','Mounted on'
}

function Get-DfRow {
    param($Vol)
    $sizeS = Format-DfSize $Vol.Size $Si $Blocks
    $used = $Vol.Size - $Vol.Free
    if ($used -lt 0) { $used = 0 }
    $usedS = Format-DfSize $used $Si $Blocks
    $availS = Format-DfSize $Vol.Free $Si $Blocks
    $pct = '-'
    if ($Vol.Size -gt 0) { $pct = [string][int][math]::Round(($used / $Vol.Size) * 100) }
    $volname = '-'
    if ($Vol.Letter) { $volname = $Vol.Letter + ':' }
    $mount = $Vol.Label
    if (-not $mount) { if ($Vol.Provider) { $mount = $Vol.Provider } else { $mount = '-' } }
    $cls = Get-DfClass $Vol
    if ($cls -eq 'CLOUD') { $mount = $mount + ' [CLOUD]' }
    elseif ($cls -eq 'NET') { $mount = $mount + ' [NET]' }
    $fst = $Vol.FS
    if (-not $fst) { $fst = '-' }
    return '  {0,-8} {1,-8} {2,9} {3,9} {4,9} {5,5}  {6}' -f $volname,$fst,$sizeS,$usedS,$availS,$pct,$mount
}

$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm'
Write-DfLine '' ''
Write-DfLine ("df for Windows  -  {0}" -f $stamp) ''

$placed = @{}
$rendered = 0
$header = Get-DfHeader

foreach ($disk in ($disks | Sort-Object Number)) {
    $mine = @()
    foreach ($v in $vols) {
        if ($v.Letter -and $v.Disk -eq $disk.Number) { $mine += $v }
    }
    if ($mine.Count -eq 0) { continue }
    Write-DfLine '' ''
    Write-DfLine ("Physical disk {0}: {1}" -f $disk.Number,$disk.Name) 'Cyan'
    if ($disk.Serial) { Write-DfLine ("  Serial: {0}" -f $disk.Serial) '' }
    Write-DfLine $header ''
    foreach ($v in ($mine | Sort-Object Letter)) {
        Write-DfLine (Get-DfRow $v) ''
        $placed[$v.Letter] = $true
    }
    $rendered++
}

# other local volumes (letterless or unplaced local)
$othersLocal = @()
foreach ($v in $vols) {
    if ($v.Letter -and $placed.ContainsKey($v.Letter)) { continue }
    if ((Get-DfClass $v) -eq 'LOCAL') { $othersLocal += $v }
}
if ($othersLocal.Count -gt 0) {
    Write-DfLine '' ''
    Write-DfLine 'Other local volumes:' 'Cyan'
    Write-DfLine $header ''
    foreach ($v in ($othersLocal | Sort-Object Letter)) { Write-DfLine (Get-DfRow $v) '' }
    $rendered++
}

# network and cloud mounts
$othersNet = @()
foreach ($v in $vols) {
    if ($v.Letter -and $placed.ContainsKey($v.Letter)) { continue }
    if ((Get-DfClass $v) -ne 'LOCAL') { $othersNet += $v }
}
if ($othersNet.Count -gt 0) {
    Write-DfLine '' ''
    Write-DfLine 'Network and cloud mounts:' 'Yellow'
    Write-DfLine $header ''
    foreach ($v in ($othersNet | Sort-Object Letter)) { Write-DfLine (Get-DfRow $v) '' }
    $rendered++
}

if ($rendered -eq 0) { Write-DfLine 'No disks or volumes found.' 'Red' }
Write-DfLine '' ''
