param(
    [string]$Version = "1.1.0"
)

$ErrorActionPreference = "Stop"
$Version = $Version.TrimStart('v', 'V')

$RootDir = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$ReleaseDir = Join-Path $RootDir "release"
$StageDir = Join-Path $ReleaseDir "_stage_portable"
$ZipName = "IncunaReader-v$Version-Windows-x64.zip"
$ZipPath = Join-Path $ReleaseDir $ZipName
$ShaPath = Join-Path $ReleaseDir "SHA256SUMS.txt"

Write-Host "============================================================"
Write-Host " Incuna Reader Cleanroom Release Packager (v$Version)"
Write-Host "============================================================"

$ExePath = Join-Path $RootDir "IncunaReader.exe"
$RuntimePath = Join-Path $RootDir "runtime"
if (-not (Test-Path $ExePath)) { throw "Missing IncunaReader.exe at $ExePath" }
if (-not (Test-Path $RuntimePath)) { throw "Missing runtime/ at $RuntimePath" }

if (-not (Test-Path $ReleaseDir)) {
    New-Item -ItemType Directory -Path $ReleaseDir | Out-Null
}

# Remove stale v<Version> artifacts and any leftover staging directory
Get-ChildItem -Path $ReleaseDir -Filter "IncunaReader-v$Version*" -ErrorAction SilentlyContinue | Remove-Item -Force -Recurse
if (Test-Path $StageDir) {
    Remove-Item -Path $StageDir -Force -Recurse
}
New-Item -ItemType Directory -Path $StageDir | Out-Null

try {
    Copy-Item -Path $ExePath -Destination (Join-Path $StageDir "IncunaReader.exe") -Force
    $StageRuntime = Join-Path $StageDir "runtime"
    New-Item -ItemType Directory -Path $StageRuntime | Out-Null
    foreach ($rtItem in Get-ChildItem -Path $RuntimePath) {
        if ($rtItem.Name -ieq "webview2_udf") { continue }
        Copy-Item -Path $rtItem.FullName -Destination (Join-Path $StageRuntime $rtItem.Name) -Recurse -Force
    }

    foreach ($doc in @("LICENSE", "THIRD_PARTY_NOTICES.md")) {
        $docPath = Join-Path $RootDir $doc
        if (Test-Path $docPath) {
            Copy-Item -Path $docPath -Destination (Join-Path $StageDir $doc) -Force
        }
    }

    # CRITICAL CLEANROOM GUARD: abort if any private, cache, or debug file is staged
    $allStaged = Get-ChildItem -Path $StageDir -Recurse -Force
    $forbiddenNames = @("data", "logs", "offline_cache", "settings.json", "cloud_tokens.json")
    $forbiddenExts = @(".db", ".db-wal", ".db-shm", ".pack", ".pdb")

    foreach ($item in $allStaged) {
        if ($forbiddenNames -contains $item.Name.ToLowerInvariant()) {
            throw "CLEANROOM VIOLATION: Forbidden item '$($item.FullName)' found in staging directory!"
        }
        if (-not $item.PSIsContainer -and ($forbiddenExts -contains $item.Extension.ToLowerInvariant())) {
            throw "CLEANROOM VIOLATION: Forbidden file extension '$($item.FullName)' found in staging directory!"
        }
    }

    Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -CompressionLevel Optimal -Force
}
finally {
    if (Test-Path $StageDir) {
        Remove-Item -Path $StageDir -Force -Recurse
    }
}

$hash = (Get-FileHash -Path $ZipPath -Algorithm SHA256).Hash.ToUpperInvariant()
$shaLine = "$hash  $ZipName"
Set-Content -Path $ShaPath -Value $shaLine -Encoding ascii

$zipMB = [math]::Round((Get-Item $ZipPath).Length / 1MB, 2)
Write-Host "ZIP Created : $ZipPath ($zipMB MB)"
Write-Host "SHA256SUMS  : $shaLine"
Write-Host ""
Write-Host "=== ZIP ENTRY AUDIT ==="

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
try {
    $entries = $zip.Entries | ForEach-Object { $_.FullName.Replace('\', '/') }
    $entries | ForEach-Object { Write-Host "  $_" }

    $bad = $entries | Where-Object {
        $_ -match '(^|/)data/' -or
        $_ -match '(^|/)webview2_udf/' -or
        $_ -match '\.(db|db-wal|db-shm|pack|pdb)$' -or
        $_ -match '(cloud_tokens\.json|settings\.json)'
    }
    if ($bad) {
        throw "CLEANROOM ZIP AUDIT FAILED: $($bad -join ', ')"
    }
    if (-not ($entries -contains "IncunaReader.exe")) { throw "Missing IncunaReader.exe in ZIP" }
    if (-not ($entries -contains "runtime/pdfium.dll")) { throw "Missing runtime/pdfium.dll in ZIP" }
    if (-not ($entries | Where-Object { $_ -like "runtime/dist/*" })) { throw "Missing runtime/dist/ in ZIP" }
    Write-Host "CLEANROOM AUDIT: PASSED ($($entries.Count) entries, 0 forbidden files)"
}
finally {
    $zip.Dispose()
}
