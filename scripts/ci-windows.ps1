<#
.SYNOPSIS
    在 Windows 上本地复跑 `.github/workflows/windows.yml` 的构建流程。

.DESCRIPTION
    CI 那边依赖来自 aqt/手动 Qt 仓库 + runner 预装的 vcpkg + 自己编的 libunrar；
    本地版把这四件事都做在**工作区**里（默认 `.local/`），不动系统目录：

        .local/
          dl/           下载缓存（Qt 的 .7z、unrar 源码、vcpkg zip）
          tools/7zr.exe 解 Qt 的 .7z
          qt/6.12.0/msvc2022_64/
          vcpkg/        vcpkg + installed/x64-windows
          unrar/        lib/unrar.lib + include/unrar/dll.hpp

    与 CI 的**唯一实质差异**：
      · 没有 actions/cache —— 依赖装过一次就留在 `.local/`，脚本按「已存在则跳过」增量处理；
      · 没有 upload-artifact —— 产物是 `dist/photoset-reader-windows-x64.zip`，与 CI 同名同结构。
    其它步骤（MSVC 环境、configure 参数、HLSL 编译检查、测试逐个跑、windeployqt + PE 依赖闭包、
    ICU / VC 运行时的补齐、打包后冒烟）都照 workflow 逐条对应，方便本地验证工作流本身。

.PARAMETER Step
    all（默认）/ deps / configure / build / hlsl / test / package

.PARAMETER DepsRoot
    依赖安装根目录，默认 `<仓库>/.local`。

.PARAMETER BuildDir
    构建目录，默认 `<仓库>/build`。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1

.EXAMPLE
    # 只重编、跑测试，不重新装依赖、不打包
    powershell -ExecutionPolicy Bypass -File scripts\ci-windows.ps1 -Step all -SkipPackage
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'deps', 'configure', 'build', 'hlsl', 'test', 'package', 'check', 'run')]
    [string]$Step = 'all',

    [string]$DepsRoot = '',
    [string]$BuildDir = '',
    [string]$Config = 'Release',

    #   `-Step run` 传给程序的一整条参数（例如 '"H:\Dragon Ball" --log-level debug'；
    #   ⚠️ 用字符串而不是数组：`powershell -File` 传数组时会被逗号拼成一个参数）
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

# ── 输出：与 CI 一样分步骤、可读；失败即抛 ───────────────────────────────────
function Write-Head([string]$text) {
    Write-Host ''
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
    Write-Host "  $text" -ForegroundColor Cyan
    Write-Host ("=" * 78) -ForegroundColor DarkCyan
}
function Write-Note([string]$text) { Write-Host "  - $text" -ForegroundColor DarkGray }

# ── 版本信息：与 windows.yml 的 env 段一一对应 ──────────────────────────────
Write-Head "photoset-reader / local Windows CI"
Write-Note "repo      = $RepoRoot"
Write-Note "deps      = $DepsRoot"
Write-Note "build     = $BuildDir"
Write-Note "Qt        = $QtVersion ($QtArch)"
Write-Note "unrar     = $UnrarVersion"
Write-Note "config    = $Config"
Write-Note "step      = $Step"

# ══ 工具函数 ═══════════════════════════════════════════════════════════════

function Get-NodePath {
    $n = Get-Command node -ErrorAction SilentlyContinue
    if ($n) { return $n.Source }
    return $null
}

#   本机 PowerShell/.NET 的 Schannel 在受限环境里拿不到凭据（SEC_E_NO_CREDENTIALS），
#   而 Node 自带 OpenSSL，所以下载优先走 node，其次才退回 Invoke-WebRequest。
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
    #   ★ PS 5.1 把 native 的 stderr 当 ErrorRecord；`$ErrorActionPreference = 'Stop'` 下
    #     编译器/工具的一条 warning 就会打断整步（实测 vcpkg remove）。这里只看退出码。
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

# ── MSVC 环境：等价于 CI 里的 `ilammy/msvc-dev-cmd@v1` ─────────────────────
function Import-MsvcEnvironment {
    if ($env:VSCMD_VER) { Write-Note "MSVC env already present (VSCMD_VER=$env:VSCMD_VER)"; return }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere not found: $vswhere" }

    #   ★ 不能只用 `-latest`：本机同时装了 BuildTools（新，但没有 VS 的 CMake 组件）
    #     与 Community（带 CMake）⇒ 取「有 vcvars 且带 CMake」的那一套。
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

    #   把 vcvars 设出来的环境变量搬进当前进程（cmd /c "call ... && set"）
    $lines = & cmd.exe /c "`"$vcvars`" >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'vcvars64.bat failed' }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$') {
            Set-Item -Path ("env:" + $matches[1]) -Value $matches[2]
        }
    }
    Write-Note "MSVC = $env:VCToolsVersion"

    #   VS 自带的 cmake / ninja 不在 PATH 上（CI 的 runner 有独立安装，本地用 VS 的）
    $vsCmakeDir = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
    if (Test-Path (Join-Path $vsCmakeDir 'cmake.exe')) {
        $env:PATH = $vsCmakeDir + ';' + (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja') + ';' + $env:PATH
    }
    if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
        throw 'cmake not found（该 VS 实例没装 CMake 组件；装它或把 cmake 放进 PATH）'
    }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
        throw 'ninja not found（随 VS 的 CMake 组件一起安装）'
    }
}

function Add-EnvPath([string]$Path) {
    if ($Path -and (Test-Path $Path)) { $env:PATH = $Path + ';' + $env:PATH }
}

# ══ 运行期依赖检查（避免 Windows 弹「找不到 xxx.dll」的模态框）════════════════
#   Windows 在**加载期**找不到 DLL 会弹系统错误框、进程挂住。开发态的
#   `photoset-reader.exe` 不会把 Qt / vcpkg / unrar 的 DLL 拷到自己旁边，
#   所以直接双击它就会弹框。这里在脚本里先查一遍：缺什么就打在终端里并报错，
#   而不是留给用户去点那个框。
function Get-PeDependents([string]$File) {
    if (-not (Get-Command dumpbin -ErrorAction SilentlyContinue)) { throw 'dumpbin 不在 PATH 上（先导入 MSVC 环境）' }
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
                $missing.Add(("{0}（被 {1} 引用）" -f $d, [IO.Path]::GetFileName($file)))
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
    if (-not (Test-Path $exe)) { throw "找不到 $exe（先 -Step build）" }
    $missing = @(Get-MissingRuntimeDeps $exe (Get-RuntimeSearchDirs $BuildDir))
    if ($missing.Count -gt 0) {
        foreach ($m in $missing) { Write-Host "  缺: $m" -ForegroundColor Yellow }
        throw ("运行期依赖不全（{0} 个）。开发态 exe 需要 Qt/vcpkg/unrar 的 DLL —— " -f $missing.Count) +
              '请用 -Step run 启动，或 -Step package 生成自包含目录（stage）后双击它。'
    }
    Write-Host '  依赖齐全：Qt / vcpkg / libunrar 全部能解析' -ForegroundColor Green
}

function Invoke-Run {
    Write-Head 'Run'
    $exe = Join-Path $BuildDir 'photoset-reader.exe'
    if (-not (Test-Path $exe)) { throw "找不到 $exe（先 -Step build）" }
    $dirs = Get-RuntimeSearchDirs $BuildDir
    $missing = @(Get-MissingRuntimeDeps $exe $dirs)
    if ($missing.Count -gt 0) {
        foreach ($m in $missing) { Write-Host "  缺: $m" -ForegroundColor Yellow }
        throw '运行期依赖不全 —— 拒绝启动（否则 Windows 会弹「找不到 xxx.dll」的框）'
    }
    foreach ($d in $dirs) { Add-EnvPath $d }
    $argLine = $RunArgs.Trim()
    #   没给 --log-file 就补一个（GUI 子系统没有控制台，出问题只能看文件）
    if ($argLine -notmatch '--log-file') {
        $argLine = ($argLine + ' --log-file "' + (Join-Path $BuildDir 'run.log') + '"').Trim()
    }
    Write-Note ("启动: {0} {1}" -f $exe, $argLine)
    #   ★ `--version` / `--help`：GUI 子系统没有控制台，Qt 会把它们弹成**消息框**
    #     （用户 2026-11-05 看到「应用名 + 版本」的框）。这里改成重定向输出直接打印。
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

# ══ deps：Qt / vcpkg / libunrar（对应 workflow 的 Restore Qt → vcpkg → libunrar）══

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

    #   与 workflow 相同的三个包；qtdoc / qttranslations 不要（体积大、用不上）
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
            #   文件名 = <包内 Version> + <DownloadableArchives>（与 workflow 一致）
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

    #   与 workflow 相同的包与 feature：
    #     · libheif 关 hevc（AVIF 走 dav1d，不随包分发 HEVC 解码器 —— 设计 21 §8）
    #     · libarchive 关 crypto / xar（用不上，只会加 DLL）
    #   ★ 钉住 VS 实例：本机有两套 VS，vcpkg 默认取最新的一套，可能与已导入的
    #     vcvars 环境（$script:VsInstallPath）不一致。
    if ($script:VsInstallPath) { $env:VCPKG_VISUAL_STUDIO_PATH = $script:VsInstallPath }
    Write-Note 'vcpkg install dependencies'
    & $vcpkgExe install 'libarchive[core,bzip2,lz4,lzma,zstd]' 'libheif[core,dav1d]' libexif liblzma zlib --triplet x64-windows --recurse
    if ($LASTEXITCODE -ne 0) { throw 'vcpkg install failed' }

    #   万一树里还留着 HEVC（旧缓存 / 别的包带进来），装完直接卸掉。
    #   ★ PowerShell 5.1：native 命令往 stderr 写东西时，`2>$null` 会变成 ErrorRecord，
    #     配上 `$ErrorActionPreference = 'Stop'` 直接终止脚本（实测：这行把整步 deps 打断）。
    #     ⇒ 这里临时放宽，只看退出码。
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

    #   ★★ 关掉 unrar 的 SMP（线程池）：7.1.6 在 DLL 模式 + 本工程"逐条读、随时关包"的用法下，
    #      `RARCloseArchive` 会死等线程池里的工作线程（2026-11-05 真机：打开加密 solid 包、
    #      解到第 ~43 条时整个窗口卡死；线程栈停在 RARCloseArchive → WaitForSingleObject）。
    #      单线程解压只是慢一点，但不会卡死。详见 docs/windows/pitfalls.md 坑-15。
    $osHpp = Join-Path $srcDir.FullName 'os.hpp'
    $osText = [IO.File]::ReadAllText($osHpp)
    $patched = $osText -replace '#if !defined\(ZIPSFX\)\s*\r?\n#define RAR_SMP\s*\r?\n#endif',
        "// [photoset] RAR_SMP disabled (dll-mode deadlock); see docs/windows/pitfalls.md`r`n#if 0`r`n#define RAR_SMP`r`n#endif"
    if ($patched -eq $osText) {
        Write-Warning 'unrar: RAR_SMP 补丁没匹配上（源码版本变了？）—— 可能仍会卡死'
    } else {
        [IO.File]::WriteAllText($osHpp, $patched, (New-Object System.Text.UTF8Encoding($false)))
        Write-Note 'patched: RAR_SMP disabled'
    }

    #   ★ 与 workflow 相同：不用 msbuild（vcxproj 钉死旧工具集 / Windows SDK 8.1），
    #     源文件清单从 vcxproj 里取，宏照它的 5 个设置。
    [xml]$proj = Get-Content (Join-Path $srcDir.FullName 'UnRARDll.vcxproj')
    #   ★ 用 GetAttribute：有些 `<ClCompile>` 子节点没有 Include（StrictMode 下访问属性会直接抛）
    $srcs = @($proj.SelectNodes("//*[local-name()='ClCompile']") |
              ForEach-Object { $_.GetAttribute('Include') } |
              Where-Object { $_ })
    Write-Note "libunrar: $($srcs.Count) source files"

    Push-Location $srcDir.FullName
    try {
        #   同 Invoke-Native：native 的 stderr 在 5.1 下会变成 ErrorRecord，别让它打断构建
        $eap = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        #   ★ 必须带 `/utf-8`：unrar 源码里有非 ASCII 注释，中文 locale（CP936）下
        #     按 GBK 解码会把注释后面的代码吞掉（实测 cmddata.cpp 报 `Flags` 未声明，
        #     并伴 C4819）。CI 的英文 runner 不会暴露这个坑。
        & cl /nologo /utf-8 /MP /O2 /EHsc /MT /DRARDLL /DUNRAR /DSILENT $srcs /link /DLL /DEF:dll.def /OUT:unrar.dll /IMPLIB:unrar.lib kernel32.lib user32.lib advapi32.lib shell32.lib ole32.lib oleaut32.lib uuid.lib
        $code = $LASTEXITCODE
        $ErrorActionPreference = $eap
        if ($code -ne 0) { throw 'libunrar build failed' }

        New-Item -ItemType Directory -Force -Path (Join-Path $UnrarPrefix 'lib'), (Join-Path $UnrarPrefix 'include\unrar') | Out-Null
        Copy-Item 'unrar.lib' (Join-Path $UnrarPrefix 'lib\unrar.lib') -Force
        Copy-Item 'unrar.dll' (Join-Path $UnrarPrefix 'lib\unrar.dll') -Force
        Copy-Item 'dll.hpp'  (Join-Path $UnrarPrefix 'include\unrar\dll.hpp') -Force
        #   标记：这份 unrar 是关掉 SMP 编出来的（见上面 RAR_SMP 补丁），下次可跳过重建
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

# ══ configure / build ══════════════════════════════════════════════════════

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
    Invoke-Native 'cmake' @('--build', $BuildDir, '--parallel')
}

# ══ hlsl：qsb --hlsl 50 → fxc ps_5_0（对应 workflow 的 HLSL compile check）══

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

        #   从 --dump 里抠出 HLSL 源：取 "Shader N: HLSL" 之后那个 Contents: 到 *** 分隔符
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
        #   ★ 不能写 BOM：`fxc` 会把 BOM 当成非法字符（`error X3000 at (1,1)`）。
        #     Windows PowerShell 5.1 的 `-Encoding UTF8` 恰恰会写 BOM（PS7 不会）。
        Set-Content -Path $f -Value $hlsl -Encoding ASCII
        & $fxc /nologo /T ps_5_0 /E main /Fo (Join-Path $work ($s.BaseName + '.cso')) $f
        if ($LASTEXITCODE -ne 0) { $failed += $s.Name }
    }
    if ($failed.Count -gt 0) { throw "HLSL 编译失败：$($failed -join ', ')" }
    Write-Host '  HLSL 全部通过' -ForegroundColor Green
}

# ══ test：逐个跑、逐个超时（对应 workflow 的 Test 步骤）════════════════════

#   用 .NET Process 起子进程并拿退出码。
#   ★ 不用 `Start-Process -PassThru`：PS 5.1 上带超时的 `WaitForExit(ms)` 之后
#     拿不到 `ExitCode`（实测三个测试全被误判成失败），无参 `WaitForExit()` 也不行。
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
    #   offscreen：本机可能有桌面，但测试不该弹窗；与 CI 保持一致
    $env:QT_QPA_PLATFORM = 'offscreen'
    $env:QT_FORCE_STDERR_LOGGING = '1'
    Add-EnvPath (Join-Path $VcpkgInstalled 'bin')
    Add-EnvPath (Join-Path $UnrarPrefix 'lib')
    Add-EnvPath (Join-Path $QtRoot 'bin')

    $failed = @()
    #   ★ 与 `tests/CMakeLists.txt` 里的套件列表一一对应（新增套件时两边都要加）。
    foreach ($t in @('ib-test-scan', 'ib-test-progress', 'ib-test-meta', 'ib-test-viewer')) {
        Write-Host "  == $t =="
        $exe = Join-Path $BuildDir ("$t.exe")
        if (-not (Test-Path $exe)) { $failed += "$t(missing)"; continue }
        $out = Join-Path $BuildDir "$t.out"
        $err = Join-Path $BuildDir "$t.err"
        #   工作目录与 ctest 的 WORKING_DIRECTORY 一致（源码根）
        $r = Invoke-Captured $exe @() 120000 $out $err $RepoRoot
        if (-not $r.Exited) {
            Write-Host '  TIMEOUT after 120s' -ForegroundColor Yellow
            $failed += $t
        } elseif ($r.ExitCode -ne 0 -and $r.ExitCode -ne 77) {
            Write-Host "  exit code $($r.ExitCode)" -ForegroundColor Yellow
            $failed += $t
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
        Write-Warning 'version smoke 超时（GUI 初始化卡住）'
    } else {
        Get-Content (Join-Path $BuildDir 'ver.out') -ErrorAction SilentlyContinue
    }

    if ($failed.Count -gt 0) { throw "测试未通过：$($failed -join ', ')" }
}

# ══ package：windeployqt + PE 依赖闭包（对应 workflow 的 Package 步骤）══════

function Invoke-Package {
    Write-Head 'Package'
    $stage = Join-Path $BuildDir 'stage'
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    Copy-Item (Join-Path $BuildDir 'photoset-reader.exe') $stage

    & (Join-Path $QtRoot 'bin\windeployqt.exe') --release --no-opengl-sw --no-translations --qmldir (Join-Path $RepoRoot 'src\ui') (Join-Path $stage 'photoset-reader.exe')
    if ($LASTEXITCODE -ne 0) { throw 'windeployqt failed' }

    #   ── 依赖闭包：从 exe 与 Qt 的 DLL 出发，递归补齐 ──
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
    Write-Host "  补齐的 DLL：$($added -join ', ')"
    if ($missing.Count -gt 0) { throw "缺依赖：$($missing -join '; ')" }

    #   ── 红线：禁止分发 HEVC 解码/编码器（设计 21 §8）──
    foreach ($bad in @('libde265.dll', 'libx265.dll', 'libheif-hevc.dll')) {
        if (Test-Path (Join-Path $stage $bad)) { throw "禁止分发 HEVC 组件：$bad" }
    }
    if (Test-Path (Join-Path $UnrarPrefix 'lib\unrar.dll')) {
        Copy-Item (Join-Path $UnrarPrefix 'lib\unrar.dll') $stage -Force
    }

    #   ── ICU：Qt6Core.dll 硬导入 icuuc.dll（Qt 6.12 起），windeployqt 不拷 ──
    #   ★ Qt 6.12 的 Windows 包不自带 ICU：它用系统 ICU（System32\icuuc.dll，Win10 1703+，
    #     导出转发到 icu.dll）。Qt 目录里有就拷，否则确认系统有 —— 都没有说明目标系统太旧。
    $icu = Get-ChildItem $QtRoot -Recurse -Include icu*.dll, icu*.dat -ErrorAction SilentlyContinue
    if ($icu) {
        $icu | ForEach-Object { Copy-Item $_.FullName $stage -Force }
        Write-Host "  ICU（来自 Qt）：$(($icu.Name | Sort-Object -Unique) -join ', ')"
    } elseif (Test-Path (Join-Path $env:SystemRoot 'System32\icuuc.dll')) {
        Write-Host "  ICU：使用系统自带的 $env:SystemRoot\System32\icuuc.dll（Win10 1703+）"
    } else {
        throw 'ICU 缺失：Qt 未自带，系统也没有 icuuc.dll（目标系统过旧？）'
    }

    #   ── VC++ 运行时：windeployqt 不拷，目标机没装可再发行包就起不来 ──
    $crt = Get-ChildItem -Path (Join-Path $env:VCToolsRedistDir 'x64') -Filter 'Microsoft.VC*.CRT' -Directory -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($crt) {
        Copy-Item (Join-Path $crt.FullName '*.dll') $stage
        Write-Host "  VC 运行时来自：$($crt.FullName)"
    } else {
        Write-Warning "找不到 VC 运行时目录（VCToolsRedistDir=$env:VCToolsRedistDir）"
    }

    #   ── 冒烟：用打包好的目录跑一次（缺 DLL / 缺平台插件会立刻露出来）──
    #   ★ 必须先把 qoffscreen.dll 放进 stage：windeployqt 只部署 qwindows.dll，
    #     指定 offscreen 却没有插件时，Qt 会弹模态错误框把进程挂住。
    $off = Join-Path $QtRoot 'plugins\platforms\qoffscreen.dll'
    if (Test-Path $off) {
        Copy-Item $off (Join-Path $stage 'platforms') -Force
        $env:QT_QPA_PLATFORM = 'offscreen'
    }
    $r = Invoke-Captured (Join-Path $stage 'photoset-reader.exe') @('--version') 60000 `
        (Join-Path $BuildDir 'stage-ver.out') (Join-Path $BuildDir 'stage-ver.err') $stage
    if (-not $r.Exited) { throw '打包后冒烟超时' }
    Get-Content (Join-Path $BuildDir 'stage-ver.out') -ErrorAction SilentlyContinue
    if ($r.ExitCode -ne 0) {
        Get-Content (Join-Path $BuildDir 'stage-ver.err') -ErrorAction SilentlyContinue
        throw "打包后冒烟失败：exit $($r.ExitCode)"
    }

    Copy-Item (Join-Path $RepoRoot 'LICENSE') $stage
    Copy-Item (Join-Path $RepoRoot 'THIRD-PARTY.md') $stage
    Copy-Item (Join-Path $RepoRoot 'THIRD-PARTY.en.md') $stage
    Get-ChildItem (Join-Path $RepoRoot 'LICENSES') -Filter '*.txt' | ForEach-Object { Copy-Item $_.FullName $stage }

    $dlls = Get-ChildItem $stage -Filter '*.dll' | Measure-Object -Property Length -Sum
    Write-Host ("  DLL 共 {0} 个 / {1:N1} MB" -f $dlls.Count, ($dlls.Sum / 1MB))

    $dist = Join-Path $RepoRoot 'dist'
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    $zip = Join-Path $dist 'photoset-reader-windows-x64.zip'
    if (Test-Path $zip) { Remove-Item $zip -Force }
    #   ★ 本地这里压的是**发布用**的单层 zip（解开就是 exe + DLL）。
    #     CI 那边不再压这一层：`upload-artifact` 自己会打包，再压就成「zip 里套 zip」
    #     （用户 2026-11-05 指出）——CI 直接上传 stage 的内容。
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -Force
    Write-Host "  产物：$zip" -ForegroundColor Green
}

# ══ 主流程 ════════════════════════════════════════════════════════════════

Import-MsvcEnvironment

switch ($Step) {
    'deps'      { Invoke-Deps; break }
    'configure' { Invoke-Configure; break }
    #   ★ `-Clean` 在 `-Step build` 上也要生效（2026-11-05 教训，见 docs 坑-18）：
    #     只删 build 目录会让 `cmake --build` 没有缓存，所以这里**先 configure（它负责删）**再 build。
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
