# Check that every PowerShell script in the repo is pure ASCII.
#
# Why (2026-11-06): an editing tool that rewrites a .ps1 silently drops the UTF-8
# BOM, and Windows PowerShell then decodes the file with the system ANSI code page
# (GBK / CP936 here) - the whole script fails to parse with garbled errors. Keeping
# the scripts ASCII-only removes the dependency on the BOM entirely (a stray non-ASCII
# byte anywhere would bring the problem back, hence this check).
#
# This file must stay ASCII-only too. Exit code: 0 = clean, 1 = offending bytes found.

$root = Split-Path -Parent $PSScriptRoot
$bad = @()

Get-ChildItem -Path (Join-Path $root 'scripts') -Filter *.ps1 -Recurse -File | ForEach-Object {
    $bytes = [IO.File]::ReadAllBytes($_.FullName)
    $off = 0
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) { $off = 3 }
    for ($i = $off; $i -lt $bytes.Length; $i++) {
        if ($bytes[$i] -gt 127) {
            $bad += ("{0} (first non-ASCII byte at offset {1})" -f $_.FullName, $i)
            break
        }
    }
}

if ($bad.Count -gt 0) {
    Write-Host '::error::non-ASCII bytes in PowerShell scripts:' -ForegroundColor Red
    foreach ($b in $bad) { Write-Host "  $b" }
    Write-Host 'Keep .ps1 files ASCII-only (Chinese comments break parsing without a BOM).'
    exit 1
}

Write-Host 'scripts/*.ps1: ASCII-only, OK' -ForegroundColor Green
exit 0
