<#
.SYNOPSIS
    Re-run the .github/workflows/windows.yml build pipeline locally on Windows.

.DESCRIPTION
    In CI the deps come from the aqt / manual Qt repo + the runner-provided vcpkg + a self-built libunrar;
    locally all of it happens inside the workspace (default .local/), nothing touches system dirs:

        .local/
          dl/           download cache (Qt .7z, unrar source, vcpkg zip)
          tools/7zr.exe unpacks the Qt .7z
          qt/6.12.0/msvc2022_64/
          vcpkg/        vcpkg + installed/x64-windows
          unrar/        lib/unrar.lib + include/unrar/dll.hpp

    The only real differences from CI:
      * no actions/cache - once installed the deps stay in .local/, so the script skips what already exists;
      * no upload-artifact - the artifact is dist/photoset-reader-windows-x64.zip, same name and shape as CI.
    Everything else (MSVC env, configure args, HLSL compile check, running every suite, windeployqt + PE dep closure,
    ICU / VC runtime top-up, post-package smoke) mirrors the workflow step by step, so the workflow itself can be verified locally.

.PARAMETER Step
    all (default) / deps / configure / build / hlsl / test / package

.PARAMETER DepsRoot
    Dependency root, defaults to <repo>/.local.

.PARAMETER BuildDir
    Build directory, defaults to <repo>/build.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1

.EXAMPLE
    # rebuild + run tests only: no dependency install, no packaging
    powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1 -Step all -SkipPackage
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'deps', 'configure', 'build', 'hlsl', 'test', 'package', 'check', 'run')]
    [string]$Step = 'all',

    [string]$DepsRoot = '',
    [string]$BuildDir = '',
    [string]$Config = 'Release',

    #   the whole argument line passed by -Step run (e.g. '"H:\Dragon Ball" --log-level debug';
    #   NOTE: a string, not an array - powershell -File joins an array with commas into a single arg)
    [string]$RunArgs = '',

    [string]$QtVersion = '6.12.0',
    [string]$QtArch = 'msvc2022_64',
    [string]$UnrarVersion = '7.1.6',

    [switch]$SkipTests,
    [switch]$SkipHlsl,
    [switch]$SkipPackage,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $DepsRoot) { $DepsRoot = Join-Path $RepoRoot '.local' }
$DepsRoot = [IO.Path]::GetFullPath($DepsRoot)
if (-not $BuildDir) { $BuildDir = Join-Path $RepoRoot 'build' }
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

$QtRoot   = Join-Path (Join-Path $DepsRoot 'qt') (Join-Path $QtVersion $QtArch)
$VcpkgRoot = Join-Path $DepsRoot 'vcpkg'
$VcpkgInstalled = Join-Path $VcpkgRoot 'installed\x64-windows'
$UnrarPrefix = Join-Path $DepsRoot 'unrar'
$DlDir    = Join-Path $DepsRoot 'dl'

# -- output: step-wise and readable like CI; any failure throws --------------------
function Write-Head([string]$text) {
    Write-Host ''
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
}
function Write-Note([string]$text) { Write-Host "  - $text" -ForegroundColor DarkGray }

# -- versions: mirrors the env block of windows.yml 1:1 ---------------------------
Write-Head "photoset-reader / local Windows CI"
Write-Note "repo      = $RepoRoot"
Write-Note "deps      = $DepsRoot"
Write-Note "build     = $BuildDir"
Write-Note "Qt        = $QtVersion ($QtArch)"
Write-Note "unrar     = $UnrarVersion"
Write-Note "config    = $Config"
Write-Note "step      = $Step"

# == helpers ======================================================================

function Get-NodePath {
    $n = Get-Command node -ErrorAction SilentlyContinue
    if ($n) { return $n.Source }
    return $null
}

#   In restricted environments Schannel (PowerShell/.NET) cannot get credentials (SEC_E_NO_CREDENTIALS),
#   while Node ships OpenSSL, so downloads go through node first and only then fall back to Invoke-WebRequest.
function Get-RemoteFile([string]$Url, [string]$OutFile) {
    if (Test-Path $OutFile) {
        Write-Note "skip (exists): $([IO.Path]::GetFileName($OutFile))"
        return
    }
    $dir = Split-Path -Parent $OutFile
    if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

    $node = Get-NodePath
    $dljs = Join-Path $DepsRoot 'tools\dl.mjs'
    if ($node -and (Test-Path $dljs)) {
        Write-Note "GET (node): $Url"
        & $node $dljs $Url $OutFile
        if ($LASTEXITCODE -ne 0) { throw "download failed: $Url" }
        return
    }
    Write-Note "GET (IWR): $Url"
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $Url -OutFile $OutFile -UseBasicParsing
}

function Invoke-Native([string]$Exe, [string[]]$ArgList, [string]$What = '') {
    if ($What) { Write-Note $What }
    #   !! PS 5.1 turns native stderr into an ErrorRecord; with $ErrorActionPreference = 'Stop'
    #      a single compiler/tool warning aborts the whole step (observed with vcpkg remove). Only exit codes matter here.
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Exe @ArgList
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $eap
    }
    if ($code -ne 0) { throw "command failed ($code): $Exe $($ArgList -join ' ')" }
}

# -- MSVC environment: equivalent to ilammy/msvc-dev-cmd@v1 in CI -------------------
function Import-MsvcEnvironment {
    if ($env:VSCMD_VER) { Write-Note "MSVC env already present (VSCMD_VER=$env:VSCMD_VER)"; return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere not found: $vswhere" }

    #   !! -latest is not enough: this machine has both BuildTools (newer, but without the VS CMake component)
    #      and Community (with CMake) => pick the one that has vcvars AND CMake.
    $candidates = @(& $vswhere -all -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if (-not $candidates) { throw 'MSVC toolset not found (VS 2022 with C++ workload is required)' }
    $vsPath = $null
    $fallback = $null
    foreach ($c in $candidates) {
        if (-not (Test-Path (Join-Path $c 'VC\Auxiliary\Build\vcvars64.bat'))) { continue }
        if (-not $fallback) { $fallback = $c }
        if (Test-Path (Join-Path $c 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')) {
            $vsPath = $c
            break
        }
    }
    if (-not $vsPath) { $vsPath = $fallback }
    $script:VsInstallPath = $vsPath
    Write-Note "VS = $vsPath"

    $vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { throw "vcvars64.bat not found: $vcvars" }

    #   copy the env vars produced by vcvars into this process (cmd /c "call ... && set")
    $lines = & cmd.exe /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'vcvars64.bat failed' }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("env:" + $matches[1]) -Value $matches[2]
        }
    }
    Write-Note "MSVC = $env:VCToolsVersion"

    #   VS ships cmake / ninja but they are not on PATH (CI has its own installs; locally use the VS ones)
    $vsCmakeDir = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    if (Test-Path (Join-Path $vsCmakeDir 'cmake.exe')) {
        $env:PATH = $vsCmakeDir + ';' + (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
    }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw 'cmake not found (this VS instance lacks the CMake component; install it or put cmake on PATH)'
    }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        throw 'ninja not found (installed together with the CMake component of VS)'
    }
}

function Add-EnvPath([string]$Path) {
    if ($Path -and (Test-Path $Path)) { $env:PATH = $Path + ';' + $env:PATH }
}

# == runtime dependency check (avoids the Windows "cannot find xxx.dll" modal box) ===
#   When a DLL is missing at load time Windows shows a system error box and the process hangs. A dev-tree
#   photoset-reader.exe does not have the Qt / vcpkg / unrar DLLs next to it,
#   so double-clicking it pops that box. This script checks first: it prints what is missing and fails,
#   instead of leaving the user to click it away.
function Get-PeDependents([string]$File) {
    if (-not (Get-Command dumpbin -ErrorAction SilentlyContinue)) { throw 'dumpbin is not on PATH (import the MSVC environment first)' }
    $deps = (& dumpbin /nologo /dependents $File 2>$null) |
            Select-String '^\s+(\S+\.dll)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    return @($deps)
}

function Get-MissingRuntimeDeps([string]$Exe, [string[]]$SearchDirs) {
    $system32 = Join-Path $env:SystemRoot 'System32'
    $queue = New-Object System.Collections.Queue
    $queue.Enqueue($Exe)
    $seen = @{}
    $missing = New-Object System.Collections.Generic.List[string]
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        if ($seen.ContainsKey($file)) { continue }
        $seen[$file] = $true
        foreach ($d in (Get-PeDependents $file)) {
            if ($d -match '^(api-ms-|ext-ms-)') { continue }
            $src = $null
            foreach ($dir in $SearchDirs) {
                $cand = Join-Path $dir $d
                if (Test-Path $cand) { $src = $cand; break }
            }
            if ($src) {
                if (-not $seen.ContainsKey($src)) { $queue.Enqueue($src) }
            } elseif (-not (Test-Path (Join-Path $system32 $d))) {
                $missing.Add(("{0} (referenced by {1})" -f $d, [IO.Path]::GetFileName($file)))
            }
        }
    }
    return $missing
}

function Get-RuntimeSearchDirs([string]$ExtraDir) {
    $dirs = @()
    if ($ExtraDir) { $dirs += $ExtraDir }
    $dirs += (Join-Path $VcpkgInstalled 'bin')
    $dirs += (Join-Path $UnrarPrefix 'lib')
    $dirs += (Join-Path $QtRoot 'bin')
    return $dirs
}

function Invoke-Check {
    Write-Head 'Runtime dependency check'
    $exe = Join-Path $BuildDir 'photoset-reader.exe'
    if (-not (Test-Path $exe)) { throw "not found: $exe (run -Step build first)" }
    $missing = @(Get-MissingRuntimeDeps $exe (Get-RuntimeSearchDirs $BuildDir))
    if ($missing.Count -gt 0) {
        foreach ($m in $missing) { Write-Host "  missing: $m" -ForegroundColor Yellow }
        throw ("runtime dependencies incomplete ({0}). The dev exe needs the Qt/vcpkg/unrar DLLs - " -f $missing.Count) +
              'use -Step run, or -Step package to produce a self-contained stage and double-click that.'
    }
    Write-Host '  deps OK: Qt / vcpkg / libunrar all resolve' -ForegroundColor Green
}

function Invoke-Run {
    Write-Head 'Run'
    $exe = Join-Path $BuildDir 'photoset-reader.exe'
    if (-not (Test-Path $exe)) { throw "not found: $exe (run -Step build first)" }
    $dirs = Get-RuntimeSearchDirs $BuildDir
    $missing = @(Get-MissingRuntimeDeps $exe $dirs)
    if ($missing.Count -gt 0) {
        foreach ($m in $missing) { Write-Host "  missing: $m" -ForegroundColor Yellow }
        throw 'deps incomplete - refusing to start (otherwise Windows pops the "cannot find xxx.dll" box)'
    }
    foreach ($d in $dirs) { Add-EnvPath $d }
    $argLine = $RunArgs.Trim()
    #   add --log-file when absent (a GUI subsystem has no console; the log file is all you get)
    if ($argLine -notmatch '--log-file') {
        $argLine = ($argLine + ' --log-file "' + (Join-Path $BuildDir 'run.log') + '"').Trim()
    }
    Write-Note ("launch: {0} {1}" -f $exe, $argLine)
    #   !! MULTI-INSTANCE app (since 2026-11-06; it used to forward to a running instance): a new exe opens its own window,
    #      it no longer hands its path to an older process. Still worth warning that an old one is running -
    #      the window you are looking at may be old code; "my change did nothing" is often exactly this.
    $running = @(Get-Process photoset-reader -ErrorAction SilentlyContinue)
    if ($running.Count -gt 0) {
        foreach ($p in $running) {
            $path = ''
            $started = ''
            try { $path = $p.Path; $started = $p.StartTime.ToString('HH:mm:ss') } catch { }
            Write-Warning ("instance already running: pid={0} started {1} path={2}" -f $p.Id, $started, $path)
        }
        Write-Warning 'this launch opens another window (multi-instance); look at the new one, or close the old one first'
    }
    #   !! --version / --help: a GUI subsystem has no console, so Qt turns them into a MESSAGE BOX
    #      (the user saw an "app name + version" box on 2026-11-05). Here the output is redirected and printed.
    if ($argLine -match '(^|\s)(--version|-v|--help|-h)(\s|$)') {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $exe
        $psi.Arguments = $argLine
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.WorkingDirectory = $BuildDir
        $proc = New-Object System.Diagnostics.Process
        $proc.StartInfo = $psi
        $null = $proc.Start()
        $out = $proc.StandardOutput.ReadToEnd()
        $err = $proc.StandardError.ReadToEnd()
        if (-not $proc.WaitForExit(30000)) { try { $proc.Kill() } catch { } }
        if ($out) { Write-Host $out.TrimEnd() }
        if ($err) { Write-Host $err.TrimEnd() }
        Write-Note ("exit={0}" -f $proc.ExitCode)
        return
    }
    Start-Process -FilePath $exe -ArgumentList $argLine -WorkingDirectory $BuildDir
}

# == deps: Qt / vcpkg / libunrar (mirrors Restore Qt -> vcpkg -> libunrar) ===========

function Install-SevenZip {
    $sevenZip = Join-Path $DepsRoot 'tools\7zr.exe'
    Get-RemoteFile 'https://www.7-zip.org/a/7zr.exe' $sevenZip
    return $sevenZip
}

function Install-Qt {
    if (Test-Path (Join-Path $QtRoot 'bin\Qt6Core.dll')) {
        Write-Note "Qt already installed: $QtRoot"
        return
    }
    Write-Note "Qt missing, installing into $QtRoot"

    $sevenZip = Install-SevenZip
    $repo = "https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/qt6_6120/qt6_6120_$QtArch"
    $updates = Join-Path $DlDir 'qt\Updates.xml'
    Get-RemoteFile "$repo/Updates.xml" $updates
    [xml]$xml = Get-Content $updates

    #   the same three packages as the workflow; no qtdoc / qttranslations (big, unused)
    $want = @(
        "qt.qt6.6120.win64_$QtArch"
        "qt.qt6.6120.addons.qtshadertools.win64_$QtArch"
        "qt.qt6.6120.addons.qtimageformats.win64_$QtArch"
    )
    New-Item -ItemType Directory -Force -Path $QtRoot | Out-Null
    foreach ($p in $xml.SelectNodes('//PackageUpdate')) {
        if ($want -notcontains $p.Name) { continue }
        $archives = ($p.DownloadableArchives -split ',') |
            ForEach-Object { $_.Trim() } |
            Where-Object { $_ -and $_ -notmatch 'qtdoc|qttranslations' }
        foreach ($a in $archives) {
            #   file name = <Version in package> + <DownloadableArchives> (same as the workflow)
            $url = "$repo/$($p.Name)/$($p.Version)$a"
            $file = Join-Path $DlDir ('qt\' + ($a -replace '/', '_'))
            Get-RemoteFile $url $file
            Write-Note "extract: $([IO.Path]::GetFileName($file))"
            & $sevenZip x -y "-o$QtRoot" $file | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "7zr failed: $file" }
        }
    }
    if (-not (Test-Path (Join-Path $QtRoot 'bin\Qt6Core.dll'))) { throw "Qt install incomplete: $QtRoot" }
}

function Install-Vcpkg {
    if (-not (Test-Path (Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake'))) {
        $zip = Join-Path $DlDir 'vcpkg-master.zip'
        Get-RemoteFile 'https://github.com/microsoft/vcpkg/archive/refs/heads/master.zip' $zip
        Write-Note 'extract vcpkg'
        $tmp = Join-Path $DepsRoot 'dl\vcpkg-extract'
        if (Test-Path $tmp) { Remove-Item $tmp -Recurse -Force }
        Expand-Archive -Path $zip -DestinationPath $tmp -Force
        $inner = Get-ChildItem $tmp -Directory | Select-Object -First 1
        Move-Item $inner.FullName $VcpkgRoot
        Remove-Item $tmp -Recurse -Force
    }

    $vcpkgExe = Join-Path $VcpkgRoot 'vcpkg.exe'
    if (-not (Test-Path $vcpkgExe)) {
        Write-Note 'bootstrap vcpkg (downloads vcpkg-tool)'
        & cmd.exe /c "`"$(Join-Path $VcpkgRoot 'bootstrap-vcpkg.bat')`" -disableMetrics"
        if ($LASTEXITCODE -ne 0 -or -not (Test-Path $vcpkgExe)) { throw 'vcpkg bootstrap failed' }
    }

    #   same packages and features as the workflow:
    #     * libheif with hevc off (AVIF goes through dav1d; we never ship an HEVC decoder - design 21 s8)
    #     * libarchive with crypto / xar off (unused, only adds DLLs)
    #   !! pin the VS instance: this machine has two, vcpkg picks the newest by default, which may not match
    #      the imported vcvars environment ($script:VsInstallPath).
    if ($script:VsInstallPath) { $env:VCPKG_VISUAL_STUDIO_PATH = $script:VsInstallPath }
    Write-Note 'vcpkg install dependencies'
    & $vcpkgExe install 'libarchive[core,bzip2,lz4,lzma,zstd]' libexif liblzma zlib --triplet x64-windows --recurse
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg install failed' }

    #   if an HEVC provider is still in the tree (old cache / pulled in by another package), remove it right after install.
    #   !! PowerShell 5.1: when a native command writes to stderr, 2>$null becomes an ErrorRecord
    #      and with $ErrorActionPreference = 'Stop' it kills the script (observed: this line aborted the deps step).
    #      => relax temporarily here; only look at the exit code.
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $vcpkgExe remove libde265 libx265 2>&1 | Out-Null
    $ErrorActionPreference = $eap
}

function Install-Unrar {
    $marker = Join-Path $UnrarPrefix '.nosmp'
    if ((Test-Path (Join-Path $UnrarPrefix 'lib\unrar.lib')) -and
        (Test-Path (Join-Path $UnrarPrefix 'include\unrar\dll.hpp')) -and
        (Test-Path $marker)) {
        Write-Note "libunrar already built (no-SMP): $UnrarPrefix"
        return
    }
    $tar = Join-Path $DlDir "unrarsrc-$UnrarVersion.tar.gz"
    Get-RemoteFile "https://www.rarlab.com/rar/unrarsrc-$UnrarVersion.tar.gz" $tar

    $srcRoot = Join-Path $DepsRoot 'unrar-src'
    if (Test-Path $srcRoot) { Remove-Item $srcRoot -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $srcRoot | Out-Null
    Write-Note 'extract unrar source'
    $eap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    tar -xzf $tar -C $srcRoot
    $tarCode = $LASTEXITCODE
    $ErrorActionPreference = $eap
    if ($tarCode -ne 0) { throw 'tar failed (unrar source)' }
    $srcDir = Get-ChildItem $srcRoot -Directory | Select-Object -First 1
    if (-not $srcDir) { throw "unrar source not found in $srcRoot" }

    #   !! disable unrar SMP (thread pool): with 7.1.6 in DLL mode and our "read one item, close any time" usage
    #      RARCloseArchive waits forever for a pool worker (real machine 2026-11-05: an encrypted solid archive,
    #      around item 43 the whole window froze; the thread stack sat in RARCloseArchive -> WaitForSingleObject).
    #      Single-threaded unpacking is only slower, it never hangs. See docs/windows/pitfalls.md pit-15.
    $osHpp = Join-Path $srcDir.FullName 'os.hpp'
    $osText = [IO.File]::ReadAllText($osHpp)
    $patched = $osText -replace '#if !defined\(ZIPSFX\)\s*\r?\n#define RAR_SMP\s*\r?\n#endif',
        "// [photoset] RAR_SMP disabled (dll-mode deadlock); see docs/windows/pitfalls.md`r`n#if 0`r`n#define RAR_SMP`r`n#endif"
    if ($patched -eq $osText) {
        Write-Warning 'unrar: the RAR_SMP patch did not apply (source version changed?) - it may still hang'
    } else {
        [IO.File]::WriteAllText($osHpp, $patched, (New-Object System.Text.UTF8Encoding($false)))
        Write-Note 'patched: RAR_SMP disabled'
    }

    #   !! same as the workflow: no msbuild (the vcxproj pins an old toolset / Windows SDK 8.1),
    #      the source list is read from the vcxproj and its five macros are honoured.
    [xml]$proj = Get-Content (Join-Path $srcDir.FullName 'UnRARDll.vcxproj')
    #   !! use GetAttribute: some <ClCompile> children have no Include (property access throws under StrictMode)
    $srcs = @($proj.SelectNodes("//*[local-name()='ClCompile']") |
              ForEach-Object { $_.GetAttribute('Include') } |
              Where-Object { $_ })
    Write-Note "libunrar: $($srcs.Count) source files"

    Push-Location $srcDir.FullName
    try {
        #   same as Invoke-Native: native stderr becomes an ErrorRecord on 5.1; do not let it abort the build
        $eap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        #   !! /utf-8 is mandatory: unrar sources contain non-ASCII comments; under a Chinese locale (CP936)
        #      GBK decoding swallows the code after those comments (observed: cmddata.cpp reported Flags undeclared
        #      plus C4819). An English CI runner never shows this.
        & cl /nologo /utf-8 /MP /O2 /EHsc /MT /DRARDLL /DUNRAR /DSILENT $srcs /link /DLL /DEF:dll.def /OUT:unrar.dll /IMPLIB:unrar.lib kernel32.lib user32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib
        $code = $LASTEXITCODE
        $ErrorActionPreference = $eap
        if ($code -ne 0) { throw 'libunrar build failed' }

        New-Item -ItemType Directory -Force -Path (Join-Path $UnrarPrefix 'lib'), (Join-Path $UnrarPrefix 'include\unrar') | Out-Null
        Copy-Item 'unrar.lib' (Join-Path $UnrarPrefix 'lib\unrar.lib') -Force
        Copy-Item 'unrar.dll' (Join-Path $UnrarPrefix 'lib\unrar.dll') -Force
        Copy-Item 'dll.hpp'  (Join-Path $UnrarPrefix 'include\unrar\dll.hpp') -Force
        #   marker: this unrar was built with SMP off (see the RAR_SMP patch above); skip the rebuild next time
        New-Item -ItemType File -Force -Path $marker | Out-Null
    } finally {
        Pop-Location
    }
}

function Invoke-Deps {
    Install-Qt
    Install-Vcpkg
    Install-Unrar
}

# == configure / build =============================================================

function Invoke-Configure {
    Write-Head 'Configure'
    if ($Clean -and (Test-Path $BuildDir)) { Remove-Item $BuildDir -Recurse -Force }

    $configureArgs = @(
        '-S', $RepoRoot,
        '-B', $BuildDir,
        '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DCMAKE_TOOLCHAIN_FILE=$(Join-Path $VcpkgRoot 'scripts\buildsystems\vcpkg.cmake')",
        '-DVCPKG_TARGET_TRIPLET=x64-windows',
        "-DCMAKE_PREFIX_PATH=$QtRoot",
        "-DUNRAR_LIBRARY=$(Join-Path $UnrarPrefix 'lib\unrar.lib')",
        "-DUNRAR_INCLUDE_DIR=$(Join-Path $UnrarPrefix 'include')"
    )
    Invoke-Native 'cmake' $configureArgs
}

function Invoke-Build {
    Write-Head 'Build'
    #   !! pit-18 guard (hit a SECOND time on 2026-11-06): local ninja occasionally misses "header -> some TU"
    #      deps - after editing a class header with members, an incremental build leaves .obj files built against the OLD layout.
    #      The symptom is nasty: the app starts, but "new ViewerModel" (= launch with a path) dies with heap corruption 0xC0000374.
    #      Here the newest header mtime under src/ is compared with the last successful build and we warn LOUDLY when stale.
    $stamp = Join-Path $BuildDir '.headers.stamp'
    $hdrNewest = Get-ChildItem (Join-Path $RepoRoot 'src') -Recurse -Include *.h -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($hdrNewest -and (Test-Path $stamp)) {
        $stampTime = (Get-Item $stamp).LastWriteTime
        if ($hdrNewest.LastWriteTime -gt $stampTime) {
            Write-Warning ("header newer than last build: {0} ({1:HH:mm:ss} > {2:HH:mm:ss})" -f `
                $hdrNewest.Name, $hdrNewest.LastWriteTime, $stampTime)
            Write-Warning 'local ninja sometimes fails to rebuild the dependent .obj (pit-18) => use -Step build -Clean, or risk runtime heap corruption'
        }
    }
    Invoke-Native 'cmake' @('--build', $BuildDir, '--parallel')
    #   build succeeded => record the timestamp: the artifacts now include every header change above
    Set-Content -Path $stamp -Value (Get-Date -Format o) -Encoding ASCII
}

# == hlsl: qsb --hlsl 50 -> fxc ps_5_0 (mirrors HLSL compile check) ==================

function Invoke-HlslCheck {
    Write-Head 'HLSL compile check'
    $qsb = Join-Path $QtRoot 'bin\qsb.exe'
    if (-not (Test-Path $qsb)) { throw "qsb not found: $qsb" }

    $fxc = (Get-Command fxc -ErrorAction SilentlyContinue)
    if ($fxc) {
        $fxc = $fxc.Source
    } else {
        $cand = Join-Path $env:WindowsSdkVerBinPath 'x64\fxc.exe'
        if (Test-Path $cand) { $fxc = $cand }
    }
    if (-not $fxc) { throw 'fxc.exe not found (Windows SDK)' }

    $work = Join-Path $BuildDir 'hlsl-check'
    New-Item -ItemType Directory -Force -Path $work | Out-Null
    $failed = @()
    foreach ($s in Get-ChildItem (Join-Path $RepoRoot 'src\shaders') -Filter '*.frag') {
        $tmp = Join-Path $work ($s.BaseName + '.qsb')
        & $qsb --hlsl 50 -o $tmp $s.FullName
        if ($LASTEXITCODE -ne 0) { $failed += "$($s.Name)(qsb)"; continue }

        #   extract the HLSL source from --dump: from the Contents: after "Shader N: HLSL" up to the *** separator
        $lines = & $qsb --dump $tmp
        $hlsl = @(); $inHlsl = $false; $capture = $false
        foreach ($l in $lines) {
            if ($l -match '^Shader \d+: HLSL') { $inHlsl = $true; continue }
            if ($inHlsl -and $l -match '^Contents:$') { $capture = $true; continue }
            if ($capture -and $l -match '^\*{10,}$') { break }
            if ($capture) { $hlsl += $l }
        }
        if ($hlsl.Count -eq 0) { $failed += "$($s.Name)(dump)"; continue }

        $f = Join-Path $work ($s.BaseName + '.hlsl')
        #   !! no BOM allowed: fxc treats it as an illegal character (error X3000 at (1,1)).
        #      Windows PowerShell 5.1 with -Encoding UTF8 writes a BOM (PS7 does not).
        Set-Content -Path $f -Value $hlsl -Encoding ASCII
        & $fxc /nologo /T ps_5_0 /E main /Fo (Join-Path $work ($s.BaseName + '.cso')) $f
        if ($LASTEXITCODE -ne 0) { $failed += $s.Name }
    }
    if ($failed.Count -gt 0) { throw "HLSL compile failed: $($failed -join ', ')" }
    Write-Host '  HLSL all passed' -ForegroundColor Green
}

# == test: run each suite with its own timeout (mirrors the Test step) ==============

#   Spawn the child process through .NET Process and read the exit code.
#   !! do not use Start-Process -PassThru: on PS 5.1, after a timed WaitForExit(ms)
#      ExitCode is not available (observed: three suites were all misjudged as failures), and plain WaitForExit() is no better.
function Invoke-Captured([string]$Exe, [string[]]$ArgList, [int]$TimeoutMs,
                         [string]$OutFile, [string]$ErrFile, [string]$WorkingDir) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    if ($ArgList -and $ArgList.Count -gt 0) {
        $quoted = $ArgList | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
        $psi.Arguments = ($quoted -join ' ')
    }
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    if ($WorkingDir) { $psi.WorkingDirectory = $WorkingDir }

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $null = $proc.Start()
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $exited = $proc.WaitForExit($TimeoutMs)
    if (-not $exited) { try { $proc.Kill() } catch { } }

    $outText = ''
    $errText = ''
    try { $outText = $outTask.Result } catch { }
    try { $errText = $errTask.Result } catch { }
    if ($OutFile) { Set-Content -Path $OutFile -Value $outText -Encoding UTF8 }
    if ($ErrFile) { Set-Content -Path $ErrFile -Value $errText -Encoding UTF8 }

    $code = $null
    if ($exited) { $code = $proc.ExitCode }
    return [pscustomobject]@{ Exited = $exited; ExitCode = $code; StdOut = $outText; StdErr = $errText }
}

function Invoke-Tests {
    Write-Head 'Test'
    #   offscreen: this machine may have a desktop, but tests must not pop windows; stay consistent with CI
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    Add-EnvPath (Join-Path $VcpkgInstalled 'bin')
    Add-EnvPath (Join-Path $UnrarPrefix 'lib')
    Add-EnvPath (Join-Path $QtRoot 'bin')

    $failed = @()
    #   !! mirrors the suite list in tests/CMakeLists.txt (add to both when you add a suite).
    #   ib-test-ui-hidpi is NOT a binary: in ctest it is the same ib-test-ui plus
    #   QT_SCALE_FACTOR=1.35 run a second time (1.35 is this device's real condition and where the old code
    #   mis-computed the bubble position) => the exe + env fields express exactly that.
    $suites = @(
        @{ exe = 'ib-test-scan' },
        @{ exe = 'ib-test-progress' },
        @{ exe = 'ib-test-meta' },
        @{ exe = 'ib-test-viewer' },
        @{ exe = 'ib-test-decode' },
        @{ exe = 'ib-test-instance' },
        @{ exe = 'ib-test-progresslock' },
        @{ exe = 'ib-test-ui' },
        @{ exe = 'ib-test-ui'; label = 'ib-test-ui-hidpi'; env = @{ QT_SCALE_FACTOR = '1.35' } }
    )
    foreach ($s in $suites) {
        $t = $s.exe
        #   NOTE: this script runs under Set-StrictMode: reading a missing hashtable key THROWS => use ContainsKey
        $label = if ($s.ContainsKey('label')) { $s.label } else { $t }
        Write-Host "  == $label =="
        $exe = Join-Path $BuildDir ("$t.exe")
        if (-not (Test-Path $exe)) { $failed += "$label(missing)"; continue }
        #   suite-specific env vars: restore afterwards (the next suite must not inherit the previous scale)
        $restore = @{}
        if ($s.ContainsKey('env')) {
            foreach ($k in $s.env.Keys) {
                $restore[$k] = [Environment]::GetEnvironmentVariable($k)
                [Environment]::SetEnvironmentVariable($k, $s.env[$k])
            }
        }
        $out = Join-Path $BuildDir "$label.out"
        $err = Join-Path $BuildDir "$label.err"
        #   working directory matches ctest's WORKING_DIRECTORY (the source root)
        $r = Invoke-Captured $exe @() 120000 $out $err $RepoRoot
        foreach ($k in $restore.Keys) {
            [Environment]::SetEnvironmentVariable($k, $restore[$k])
        }
        if (-not $r.Exited) {
            Write-Host '  TIMEOUT after 120s' -ForegroundColor Yellow
            $failed += $label
        } elseif ($r.ExitCode -ne 0 -and $r.ExitCode -ne 77) {
            Write-Host "  exit code $($r.ExitCode)" -ForegroundColor Yellow
            $failed += $label
        } else {
            Write-Host "  exit code $($r.ExitCode) (0=pass, 77=skip)" -ForegroundColor Green
        }
        Get-Content $out -ErrorAction SilentlyContinue
        Get-Content $err -ErrorAction SilentlyContinue
    }

    Write-Host '  == smoke: --version =='
    $exe = Join-Path $BuildDir 'photoset-reader.exe'
    $r = Invoke-Captured $exe @('--version') 60000 (Join-Path $BuildDir 'ver.out') (Join-Path $BuildDir 'ver.err') $BuildDir
    if (-not $r.Exited) {
        Write-Warning 'version smoke timed out (GUI init is stuck)'
    } else {
        Get-Content (Join-Path $BuildDir 'ver.out') -ErrorAction SilentlyContinue
    }

    if ($failed.Count -gt 0) { throw "tests failed: $($failed -join ', ')" }
}

# == package: windeployqt + PE dependency closure (mirrors the Package step) ========

function Invoke-Package {
    Write-Head 'Package'
    $stage = Join-Path $BuildDir 'stage'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item (Join-Path $BuildDir 'photoset-reader.exe') $stage

    & (Join-Path $QtRoot 'bin\windeployqt.exe') --release --no-opengl-sw --no-translations --qmldir (Join-Path $RepoRoot 'src\ui') (Join-Path $stage 'photoset-reader.exe')
    if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }

    #   -- slimming: keep the Basic style only (user, 2026-11-06) ------------------------
    #   QQuickStyle::setStyle("Basic") is hard-coded in main.cpp, other styles are dead weight;
    #   windeployqt copies the whole Controls module dir (5 styles, about 15.6 MB of DLLs).
    #   vc_redist.x64.exe (about 18 MB) is deleted too: an installer windeployqt drags along; we ship the CRT DLLs directly.
    Remove-Item (Join-Path $stage 'vc_redist.x64.exe') -Force -ErrorAction SilentlyContinue
    foreach ($style in 'Fusion', 'Material', 'Universal', 'Imagine', 'FluentWinUI3', 'Windows') {
        Remove-Item (Join-Path $stage "qml\QtQuick\Controls\$style") -Recurse -Force -ErrorAction SilentlyContinue
        Get-ChildItem $stage -Filter "Qt6QuickControls2$style*.dll" -ErrorAction SilentlyContinue |
            Remove-Item -Force
    }

    #   -- dependency closure: start from the exe and the Qt DLLs, fill in recursively --
    $search = @(
        $stage
        (Join-Path $VcpkgInstalled 'bin')
        (Join-Path $UnrarPrefix 'lib')
        (Join-Path $QtRoot 'bin')
    )
    $system32 = "$env:SystemRoot\System32"
    $queue = New-Object System.Collections.Queue
    Get-ChildItem $stage -Recurse -Include *.exe, *.dll | ForEach-Object { $queue.Enqueue($_.FullName) }
    $seen = @{}
    $added = @()
    $missing = @()
    while ($queue.Count -gt 0) {
        $file = $queue.Dequeue()
        if ($seen.ContainsKey($file)) { continue }
        $seen[$file] = $true
        $deps = (& dumpbin /nologo /dependents $file 2>$null) |
                Select-String '^\s+(\S+\.dll)\s*$' |
                ForEach-Object { $_.Matches[0].Groups[1].Value }
        foreach ($d in $deps) {
            if ($d -match '^(api-ms-|ext-ms-)') { continue }
            $src = $null
            foreach ($dir in $search) {
                $cand = Join-Path $dir $d
                if (Test-Path $cand) { $src = $cand; break }
            }
            if (-not $src) {
                if (-not (Test-Path (Join-Path $system32 $d))) { $missing += "$d ($([IO.Path]::GetFileName($file)))" }
                continue
            }
            $dst = Join-Path $stage $d
            if (-not (Test-Path $dst)) { Copy-Item $src $dst; $added += $d }
            $queue.Enqueue($dst)
        }
    }
    Write-Host "  DLLs added: $($added -join ', ')"
    if ($missing.Count -gt 0) { throw "missing deps: $($missing -join '; ')" }

    #   -- red line: never ship an HEVC decoder/encoder (design 21 s8) --
    foreach ($bad in @('libde265.dll', 'libx265.dll', 'libheif-hevc.dll')) {
        if (Test-Path (Join-Path $stage $bad)) { throw "refusing to ship an HEVC component: $bad" }
    }
    if (Test-Path (Join-Path $UnrarPrefix 'lib\unrar.dll')) {
        Copy-Item (Join-Path $UnrarPrefix 'lib\unrar.dll') $stage -Force
    }

    #   -- ICU: Qt6Core.dll hard-imports icuuc.dll (since Qt 6.12) and windeployqt does not copy it --
    #   !! the Qt 6.12 Windows package does NOT ship ICU (verified: no icu*.dll anywhere in the Qt tree); it hard-imports
    #      the system ICU: System32\icuuc.dll (a 36 KB forwarder whose exports live in icu.dll next door).
    #   !! why "the system always has it": ICU in system32 has been part of Windows since 10 1703
    #      (Server 2019 on the server side; .NET also draws the loadable-ICU line at 1703 / Server 2019),
    #      while Qt 6.12 starts at Windows 10 1809 / 11 (Server 2019+), which is LATER than
    #      the ICU cut-in => every OS we support has it; no need to ship it (and shipping Microsoft system DLLs would be wrong).
    #      => the throw below can only fire in WinPE / stripped images that cannot run Qt 6.12 anyway.
    #   !! covering older systems (Win10 <=1703 / Server 2016) needs Qt 5.15, not a copied DLL;
    #      if such an image ever needs support, the only way is to ship ICU (vcpkg icu, +~30 MB; app-local
    #      wins over System32) - then change this to copy it into stage.
    $icu = Get-ChildItem $QtRoot -Recurse -Include icu*.dll, icu*.dat -ErrorAction SilentlyContinue
    if ($icu) {
        $icu | ForEach-Object { Copy-Item $_.FullName $stage -Force }
        Write-Host "  ICU (from Qt): $(($icu.Name | Sort-Object -Unique) -join ', ')"
    } elseif (Test-Path (Join-Path $env:SystemRoot 'System32\icuuc.dll')) {
        Write-Host "  ICU: using the system $env:SystemRoot\System32\icuuc.dll (Win10 1703+ / Server 2019+; Qt 6.12 needs 10 1809+)"
    } else {
        throw 'ICU missing: the system has no System32\icuuc.dll. Qt 6.12 requires Windows 10 1809+ / Server 2019+ (all of them ship ICU); check that the target is not WinPE or a stripped image, or use a Qt build that does not rely on the system ICU.'
    }

    #   -- VC++ runtime: windeployqt does not copy it; without the redistributable the app will not start --
    $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir 'x64') -Filter 'Microsoft.VC*.CRT' -Directory -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($crt) {
        Copy-Item (Join-Path $crt.FullName '*.dll') $stage
        Write-Host "  VC runtime from: $($crt.FullName)"
    } else {
        Write-Warning "VC runtime directory not found (VCToolsRedistDir=$env:VCToolsRedistDir)"
    }

    #   -- smoke: run once from the packaged dir (a missing DLL / platform plugin shows up immediately) --
    #   !! qoffscreen.dll must be put into stage first: windeployqt only deploys qwindows.dll
    #      and with offscreen requested but no plugin, Qt pops a modal error box and hangs.
    $off = Join-Path $QtRoot 'plugins\platforms\qoffscreen.dll'
    if (Test-Path $off) {
        Copy-Item $off (Join-Path $stage 'platforms') -Force
        $env:QT_QPA_PLATFORM = 'offscreen'
    }
    $r = Invoke-Captured (Join-Path $stage 'photoset-reader.exe') @('--version') 60000 `
        (Join-Path $BuildDir 'stage-ver.out') (Join-Path $BuildDir 'stage-ver.err') $stage
    if (-not $r.Exited) { throw 'post-package smoke timed out' }
    Get-Content (Join-Path $BuildDir 'stage-ver.out') -ErrorAction SilentlyContinue
    if ($r.ExitCode -ne 0) {
        Get-Content (Join-Path $BuildDir 'stage-ver.err') -ErrorAction SilentlyContinue
        throw "post-package smoke failed: exit $($r.ExitCode)"
    }

    Copy-Item (Join-Path $RepoRoot 'LICENSE') $stage
    Copy-Item (Join-Path $RepoRoot 'THIRD-PARTY.md') $stage
    Copy-Item (Join-Path $RepoRoot 'THIRD-PARTY.en.md') $stage
    Get-ChildItem (Join-Path $RepoRoot 'LICENSES') -Filter '*.txt' | ForEach-Object { Copy-Item $_.FullName $stage }

    $dlls = Get-ChildItem $stage -Filter '*.dll' | Measure-Object -Property Length -Sum
    Write-Host ("  DLLs: {0} files / {1:N1} MB" -f $dlls.Count, ($dlls.Sum / 1MB))

    $dist = Join-Path $RepoRoot 'dist'
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    $zip = Join-Path $dist 'photoset-reader-windows-x64.zip'
    if (Test-Path $zip) { Remove-Item $zip -Force }
    #   !! locally this is the RELEASE single-layer zip (unpack it and you get exe + DLLs).
    #     CI no longer zips this layer: upload-artifact packs by itself, zipping again gives a zip inside a zip
    #     (pointed out by the user on 2026-11-05); CI uploads the contents of stage directly.
    #   -- trim (2026-11-07, user approved) --------------------------------------
#   * dxcompiler.dll + dxil.dll: DirectX Shader Compiler, only needed by the D3D12
#     backend (D3D11 uses the OS-provided d3dcompiler_47.dll). They are NOT part of
#     Windows; we drop them (-15 MB) and main.cpp downgrades d3d12 -> d3d11.
#   * qmltooling\qmldbg_*: QML debugger plugins, never used in a release build.
#   * generic\qtuiotouchplugin.dll: TUIO (touch tables), unrelated to touchscreens.
$trim = @('dxcompiler.dll', 'dxil.dll', 'generic\qtuiotouchplugin.dll')
foreach ($f in $trim) {
    $p = Join-Path $stage $f
    if (Test-Path $p) { Remove-Item $p -Force; Write-Host "  removed $f" }
}
$qmlTooling = Join-Path $stage 'qmltooling'
if (Test-Path $qmlTooling) {
    Remove-Item $qmlTooling -Recurse -Force
    Write-Host "  removed qmltooling\ (12 qmldbg_* plugins)"
}
foreach ($f in $trim) {
    if (Test-Path (Join-Path $stage $f)) { throw "trim failed: $f is still in stage" }
}
if (Test-Path (Join-Path $stage 'qmltooling')) { throw 'trim failed: qmltooling still present' }

Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    Write-Host "  artifact: $zip" -ForegroundColor Green
}

# == main =========================================================================

Import-MsvcEnvironment

switch ($Step) {
    'deps'      { Invoke-Deps; break }
    'configure' { Invoke-Configure; break }
    #   !! -Clean must work for -Step build too (lesson from 2026-11-05, see docs pit-18):
    #     just deleting the build dir leaves cmake --build without a cache, so configure (which deletes) runs BEFORE build.
    'build'     { if ($Clean) { Invoke-Configure }; Invoke-Build; break }
    'hlsl'      { Invoke-HlslCheck; break }
    'test'      { Invoke-Tests; break }
    'package'   { Invoke-Package; break }
    'check'     { Invoke-Check; break }
    'run'       { Invoke-Run; break }
    'all' {
        Invoke-Deps
        Invoke-Configure
        Invoke-Build
        if (-not $SkipHlsl) { Invoke-HlslCheck }
        if (-not $SkipTests) { Invoke-Tests }
        if (-not $SkipPackage) { Invoke-Package }
        Invoke-Check
        Write-Head 'DONE'
        break
    }
}
