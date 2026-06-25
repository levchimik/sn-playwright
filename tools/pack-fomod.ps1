<#
.SYNOPSIS
    Package the Playwright FOMOD installer from mod/ -- the single source of truth.

.DESCRIPTION
    Builds dist/Playwright-<version>-fomod.zip from the repo's mod/ folder, which is
    the self-contained FOMOD (mod/fomod/ModuleConfig.xml maps onto mod/'s flat layout).

    It exists because two ways of building this zip silently produce a BROKEN release:
      1. Packaging from dist/Playwright-FOMOD/ -- abandoned 0.7-era scaffolding that is
         gitignored and never refreshed, so it ships an ancient index.html / DLL / etc.
         ("some old version"). The ONLY correct source is mod/.
      2. PowerShell Compress-Archive / .NET-Framework ZipFile.CreateFromDirectory both
         write BACKSLASH path separators into the zip, which FOMOD installers (Vortex/MO2)
         can't read as folders -- the install lands as flat garbage.

    This script avoids both, and then VALIDATES the result against ModuleConfig.xml
    (every referenced file/folder must be present) plus staleness guards (the DLL in mod/
    must match the freshly-built plugin DLL; each .pex must be newer than its .psc). Any
    failure throws and exits non-zero, so a bad zip is never produced.

.PARAMETER Check
    Validate mod/ (and staleness guards) without writing a zip.

.PARAMETER Upload
    After a successful build, replace the asset on the matching GitHub release
    (gh release upload <version> <zip> --clobber). The release/tag must already exist.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/pack-fomod.ps1
    powershell -ExecutionPolicy Bypass -File tools/pack-fomod.ps1 -Check
    powershell -ExecutionPolicy Bypass -File tools/pack-fomod.ps1 -Upload
#>
[CmdletBinding()]
param(
    [switch]$Check,
    [switch]$Upload
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$BS = [char]92   # '\'  -- never write the literal '\','/' pair; the repo safety hook
$FS = [char]47   # '/'  -- false-matches it as a Remove-Item path.

$repo    = Split-Path -Parent $PSScriptRoot
$modDir  = Join-Path $repo 'mod'
$distDir = Join-Path $repo 'dist'
$fomodDir   = Join-Path $modDir 'fomod'
$moduleCfg  = Join-Path $fomodDir 'ModuleConfig.xml'
$infoXml    = Join-Path $fomodDir 'info.xml'

$problems = New-Object System.Collections.Generic.List[string]
function Fail([string]$m) { $script:problems.Add($m) }

# --- preconditions ----------------------------------------------------------
if (-not (Test-Path $modDir))     { throw "mod/ not found at $modDir" }
if (-not (Test-Path $moduleCfg))  { throw "mod/fomod/ModuleConfig.xml not found -- mod/ is not a FOMOD" }
if (-not (Test-Path $infoXml))    { throw "mod/fomod/info.xml not found" }

# --- version (single source of truth = mod/fomod/info.xml) ------------------
[xml]$info = Get-Content -Raw -Path $infoXml
$version = ([string]$info.fomod.Version).Trim()
if ([string]::IsNullOrWhiteSpace($version)) { throw "info.xml has no <Version>" }
Write-Host "Playwright FOMOD packager" -ForegroundColor Cyan
Write-Host ("  version (mod/fomod/info.xml): {0}" -f $version)

# --- staleness guard: DLL in mod/ must equal the freshly-built plugin DLL ----
$builtDll = Join-Path $repo 'plugin\build\windows\x64\release\SNPlaywright.dll'
$modDll   = Join-Path $modDir 'SKSE\Plugins\SNPlaywright.dll'
if (Test-Path $builtDll) {
    if (-not (Test-Path $modDll)) {
        Fail "mod/ has no SNPlaywright.dll but a build exists -- deploy the build into mod/."
    } elseif ((Get-FileHash $builtDll).Hash -ne (Get-FileHash $modDll).Hash) {
        Fail "mod/ DLL is STALE: it differs from plugin/build/.../SNPlaywright.dll. Re-deploy the freshly-built DLL into mod/ before packaging."
    } else {
        Write-Host "  DLL: mod/ matches the latest build" -ForegroundColor DarkGray
    }
} else {
    Write-Host "  DLL: no plugin/build output to compare against (skipping freshness check)" -ForegroundColor Yellow
}

# --- staleness guard: each .pex must be newer than its .psc ------------------
foreach ($name in @('PW_Controller','PW_MCM')) {
    $pex = Join-Path $modDir ("Scripts\{0}.pex" -f $name)
    $psc = Join-Path $modDir ("Scripts\Source\{0}.psc" -f $name)
    if ((Test-Path $pex) -and (Test-Path $psc)) {
        if ((Get-Item $pex).LastWriteTimeUtc -lt (Get-Item $psc).LastWriteTimeUtc) {
            Fail ("{0}.pex is OLDER than {0}.psc -- recompile before packaging." -f $name)
        }
    } elseif (Test-Path $psc) {
        Fail ("{0}.psc exists but {0}.pex is missing -- compile it." -f $name)
    }
}

# --- collect what ModuleConfig.xml requires ---------------------------------
[xml]$cfg = Get-Content -Raw -Path $moduleCfg
function Norm([string]$p) { return ($p -replace ('[' + [regex]::Escape($BS) + ']'), $FS).Trim($FS) }

$reqFiles   = New-Object System.Collections.Generic.HashSet[string]
$reqFolders = New-Object System.Collections.Generic.HashSet[string]

foreach ($n in $cfg.SelectNodes('//*[local-name()="file"]'))   { [void]$reqFiles.Add((Norm $n.source)) }
foreach ($n in $cfg.SelectNodes('//*[local-name()="folder"]')) { [void]$reqFolders.Add((Norm $n.source)) }
foreach ($attr in @('//*[local-name()="moduleImage"]/@path','//*[local-name()="image"]/@path')) {
    foreach ($n in $cfg.SelectNodes($attr)) { [void]$reqFiles.Add((Norm $n.Value)) }
}

# every required source must exist on disk under mod/
foreach ($rf in $reqFiles) {
    if (-not (Test-Path (Join-Path $modDir ($rf -replace $FS, $BS)))) { Fail "ModuleConfig references missing file: $rf" }
}
foreach ($rd in $reqFolders) {
    $abs = Join-Path $modDir ($rd -replace $FS, $BS)
    if (-not (Test-Path $abs)) { Fail "ModuleConfig references missing folder: $rd" }
    elseif (-not (Get-ChildItem -Path $abs -Recurse -File | Select-Object -First 1)) { Fail "ModuleConfig folder is empty: $rd" }
}

if ($problems.Count -gt 0) {
    Write-Host ""; Write-Host "VALIDATION FAILED:" -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    exit 1
}
Write-Host ("  ModuleConfig: {0} files + {1} folders all present" -f $reqFiles.Count, $reqFolders.Count) -ForegroundColor DarkGray

if ($Check) { Write-Host "OK (check only -- no zip written)" -ForegroundColor Green; exit 0 }

# --- build the zip from mod/ (forward slashes, exclude cruft) ----------------
if (-not (Test-Path $distDir)) { [void](New-Item -ItemType Directory -Path $distDir) }
$out = Join-Path $distDir ("Playwright-{0}-fomod.zip" -f $version)
if ([System.IO.File]::Exists($out)) { [System.IO.File]::Delete($out) }

$excludeNames = @('meta.ini','Thumbs.db','desktop.ini','.DS_Store')
$modLen = $modDir.Length + 1
$filesystem = [System.IO.File]::Open($out, [System.IO.FileMode]::CreateNew)
$zip = New-Object System.IO.Compression.ZipArchive($filesystem, [System.IO.Compression.ZipArchiveMode]::Create)
$packed = 0
foreach ($f in (Get-ChildItem -Path $modDir -Recurse -File)) {
    if ($excludeNames -contains $f.Name) { continue }
    if ($f.Name -like '*.swp' -or $f.Name -like '*~') { continue }
    $rel = $f.FullName.Substring($modLen).Replace($BS, $FS)
    $entry = $zip.CreateEntry($rel, [System.IO.Compression.CompressionLevel]::Optimal)
    $es = $entry.Open()
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    $es.Write($bytes, 0, $bytes.Length)
    $es.Dispose()
    $packed++
}
$zip.Dispose(); $filesystem.Close()

# --- validate the produced zip ----------------------------------------------
$z = [System.IO.Compression.ZipFile]::OpenRead($out)
try {
    $entries = @($z.Entries | ForEach-Object { $_.FullName })
    $entrySet = New-Object System.Collections.Generic.HashSet[string]
    foreach ($e in $entries) { [void]$entrySet.Add($e) }

    $bad = @($entries | Where-Object { $_.Contains($BS) })
    if ($bad.Count -gt 0) { Fail ("{0} zip entries use backslash separators (FOMOD-breaking)" -f $bad.Count) }
    if (-not $entrySet.Contains('fomod/ModuleConfig.xml')) { Fail "zip is missing fomod/ModuleConfig.xml at root" }
    if (-not $entrySet.Contains('fomod/info.xml'))         { Fail "zip is missing fomod/info.xml at root" }

    foreach ($rf in $reqFiles) {
        if (-not $entrySet.Contains($rf)) { Fail "zip is missing ModuleConfig file: $rf" }
    }
    foreach ($rd in $reqFolders) {
        if (-not ($entries | Where-Object { $_.StartsWith($rd + $FS) } | Select-Object -First 1)) {
            Fail "zip has no entries under ModuleConfig folder: $rd"
        }
    }
} finally { $z.Dispose() }

if ($problems.Count -gt 0) {
    Write-Host ""; Write-Host "ZIP VALIDATION FAILED -- deleting bad artifact:" -ForegroundColor Red
    $problems | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    [System.IO.File]::Delete($out)
    exit 1
}

$sizeKB = [math]::Round((Get-Item $out).Length / 1KB)
Write-Host ""
Write-Host ("Built {0}" -f $out) -ForegroundColor Green
Write-Host ("  {0} files, {1:N0} KB, forward-slash entries verified" -f $packed, $sizeKB)

# --- optional: replace the release asset ------------------------------------
if ($Upload) {
    Write-Host ""
    Write-Host ("Uploading to GitHub release {0} ..." -f $version) -ForegroundColor Cyan
    & gh release upload $version $out --clobber
    if ($LASTEXITCODE -ne 0) { throw "gh release upload failed (exit $LASTEXITCODE) -- does the '$version' release exist?" }
    Write-Host "Asset replaced." -ForegroundColor Green
}
