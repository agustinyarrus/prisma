# build.ps1 - compila las herramientas de prisma con clang-cl + lld-link (vienen con Visual Studio).
#
#   .\build.ps1                  release optimizada (LTO) -> bin\image2png.exe
#   .\build.ps1 -Mode dev        sin optimizar, con simbolos -> build\dev\image2png.exe
#   .\build.ps1 -Mode asan       AddressSanitizer (para fuzzing) -> build\asan\image2png.exe
#   .\build.ps1 -Check           solo compila (sin enlazar): chequeo rapido de sintaxis y tipos
#   .\build.ps1 -Tools unit      pruebas unitarias (tests\unit) -> build\<modo>\unit.exe
#   .\build.ps1 -Clean           borra build\ antes de compilar
#
# Por que clang-cl y no cl: auto-vectoriza mejor los nucleos de filtros y conversiones, acepta
# __attribute__((target("avx2"))) para compilar la misma funcion dos veces (base y AVX2) sin
# archivos aparte, y hace LTO completo con lld.
[CmdletBinding()]
param(
    [ValidateSet('release', 'dev', 'asan')] [string] $Mode = 'release',
    [switch] $Check,
    [switch] $Clean,
    [string[]] $Tools = @('image2png')
)
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$started = Get-Date

function Write-Step([string] $text, [string] $color = 'DarkGray') { Write-Host "  $text" -ForegroundColor $color }

# ---- Visual Studio y su entorno (cacheado: vcvars64 tarda ~3 s) --------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe no encontrado: instala Visual Studio 2022 con "Desarrollo de escritorio con C++" + "C++ Clang tools"' }
$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) { $vsPath = & $vswhere -all -prerelease -latest -products * -property installationPath }
if (-not $vsPath) { throw 'vswhere no encontro Visual Studio' }
$llvm = Join-Path $vsPath 'VC\Tools\Llvm\x64\bin'
$clang = Join-Path $llvm 'clang-cl.exe'
$lld = Join-Path $llvm 'lld-link.exe'
if (-not (Test-Path $clang)) { throw "clang-cl no esta instalado: agrega el componente 'C++ Clang tools for Windows' en el instalador de Visual Studio" }
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'

$buildDir = Join-Path $root "build\$Mode"
if ($Clean -and (Test-Path (Join-Path $root 'build'))) { Remove-Item -Recurse -Force (Join-Path $root 'build') }
New-Item -ItemType Directory -Force $buildDir | Out-Null

$envCache = Join-Path $root 'build\vcvars-env.json'
$stamp = (Get-Item $vcvars).LastWriteTimeUtc.Ticks.ToString()
$vcEnv = $null
if (Test-Path $envCache) {
    $cached = Get-Content $envCache -Raw | ConvertFrom-Json
    if ($cached.stamp -eq $stamp -and $cached.path -eq $vcvars) { $vcEnv = $cached.vars }
}
if (-not $vcEnv) {
    $vars = @{}
    cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { $vars[$matches[1]] = $matches[2] }
    }
    if (-not $vars['INCLUDE']) { throw 'vcvars64.bat no devolvio INCLUDE' }
    $vcEnv = [pscustomobject]$vars
    [pscustomobject]@{ stamp = $stamp; path = $vcvars; vars = $vcEnv } | ConvertTo-Json -Depth 3 | Set-Content $envCache -Encoding UTF8
}
foreach ($p in $vcEnv.PSObject.Properties) { Set-Item -Path "env:$($p.Name)" -Value $p.Value }

# ---- fuentes y banderas ----------------------------------------------------------------------
$ownSources = Get-ChildItem (Join-Path $root 'src') -Recurse -Include '*.c' |
    Where-Object { $_.FullName -notmatch '\\tools\\' }
# Cada herramienta vive en src\tools\<nombre>; las pruebas unitarias ("unit") en tests\unit y se
# enlazan contra los mismos objetos comunes, pero nunca van a bin\.
function Get-ToolDir([string] $name) {
    if ($name -eq 'unit') { return Join-Path $root 'tests\unit' }
    return Join-Path $root "src\tools\$name"
}
$toolSources = @{}
$toolFiles = @{}
foreach ($t in $Tools) {
    $toolSources[$t] = @(Get-ChildItem (Get-ToolDir $t) -Recurse -Include '*.c')
    foreach ($f in $toolSources[$t]) { $toolFiles[$f.FullName] = $true }
}
$deflateDir = Join-Path $root 'third_party\libdeflate\lib'
$thirdParty = @(Get-ChildItem $deflateDir -Filter '*.c') + @(Get-Item (Join-Path $deflateDir 'x86\cpu_features.c'))

$common = @('/nologo', '/c', '/utf-8', '/std:c17', '/GS-', '/Gy', '/Gw', '-march=x86-64-v2',
            '/D_CRT_SECURE_NO_WARNINGS', '/DUNICODE', '/D_UNICODE')
$warn = @('/W4', '-Wno-unused-function', '-Wno-missing-field-initializers', '-Wno-sign-compare',
          '-Wno-gnu-zero-variadic-macro-arguments', '-Wno-language-extension-token')
switch ($Mode) {
    'release' { $opt = @('/O2', '/Ob3', '-flto', '/DNDEBUG', '/MT') }
    'dev'     { $opt = @('/Od', '/Zi', '/DDEBUG', '/MTd') }
    'asan'    { $opt = @('/O1', '/Zi', '-fsanitize=address', '/DDEBUG', '/MD') }
}

$jobs = @()
foreach ($f in $ownSources) { $jobs += [pscustomobject]@{ src = $f.FullName; third = $false } }
foreach ($t in $Tools) { foreach ($f in $toolSources[$t]) { $jobs += [pscustomobject]@{ src = $f.FullName; third = $false } } }
foreach ($f in $thirdParty) { $jobs += [pscustomobject]@{ src = $f.FullName; third = $true } }

$objDir = Join-Path $buildDir 'obj'
New-Item -ItemType Directory -Force $objDir | Out-Null
foreach ($j in $jobs) {
    $rel = $j.src.Substring($root.Length + 1) -replace '[\\/]', '_' -replace '\.c$', '.obj'
    $j | Add-Member -NotePropertyName obj -NotePropertyValue (Join-Path $objDir $rel)
}

Write-Host ''
Write-Host "  prisma · build $Mode · $($jobs.Count) fuentes · clang-cl $((& $clang --version | Select-Object -First 1) -replace '^.*version ','')" -ForegroundColor Cyan

# ---- compilacion en paralelo (PS 7) o secuencial (PS 5.1) ---------------------------------------
$compileOne = {
    param($job, $clang, $common, $warn, $opt)
    $flags = @() + $common + $opt
    if ($job.third) { $flags += @('/W0', '-Wno-everything') } else { $flags += $warn }
    $out = & $clang @flags "/Fo$($job.obj)" $job.src 2>&1 | Out-String
    [pscustomobject]@{ src = $job.src; ok = ($LASTEXITCODE -eq 0); log = $out.Trim() }
}
if ($PSVersionTable.PSVersion.Major -ge 7) {
    $fn = $compileOne.ToString()
    $results = $jobs | ForEach-Object -ThrottleLimit ([Environment]::ProcessorCount) -Parallel {
        $sb = [scriptblock]::Create($using:fn)
        & $sb $_ $using:clang $using:common $using:warn $using:opt
    }
} else {
    $results = foreach ($j in $jobs) { & $compileOne $j $clang $common $warn $opt }
}

$failed = @($results | Where-Object { -not $_.ok })
foreach ($r in $results) {
    if ($r.log -and ($r.log -match 'warning|error')) {
        $name = $r.src.Substring($root.Length + 1)
        Write-Host "  ── $name" -ForegroundColor $(if ($r.ok) { 'Yellow' } else { 'Red' })
        Write-Host $r.log
    }
}
if ($failed.Count) { throw "fallaron $($failed.Count) de $($jobs.Count) fuentes" }
Write-Step "compilado: $($jobs.Count) objetos"
if ($Check) { Write-Host "  ✓ chequeo OK en $([int]((Get-Date) - $started).TotalSeconds) s" -ForegroundColor Green; return }

# ---- recursos y enlace --------------------------------------------------------------------------
$libs = @('kernel32.lib', 'user32.lib', 'ole32.lib', 'oleaut32.lib', 'windowscodecs.lib', 'uuid.lib', 'advapi32.lib')
$commonObjs = @($jobs | Where-Object { -not $toolFiles.ContainsKey($_.src) } | ForEach-Object { $_.obj })
foreach ($t in $Tools) {
    $toolObjs = @($toolSources[$t] | ForEach-Object { $s = $_.FullName; ($jobs | Where-Object src -eq $s).obj })
    $rc = Join-Path (Get-ToolDir $t) "$t.rc"
    $res = @()
    if (Test-Path $rc) {
        $resFile = Join-Path $buildDir "$t.res"
        $toolDir = Split-Path $rc
        $rcFlags = @('/nologo', '/i', $toolDir)
        if (Test-Path (Join-Path $toolDir "$t.ico")) { $rcFlags += @('/d', 'HAVE_ICON') }
        Push-Location $toolDir
        try { $rcOut = & rc.exe @rcFlags /fo $resFile $rc 2>&1 | Out-String } finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { Write-Host $rcOut; throw "rc.exe fallo con $rc" }
        $res = @($resFile)
    }
    $outDir = if ($Mode -eq 'release' -and $t -ne 'unit') { Join-Path $root 'bin' } else { $buildDir }
    New-Item -ItemType Directory -Force $outDir | Out-Null
    $exe = Join-Path $outDir "$t.exe"
    $linkFlags = @('/nologo', "/OUT:$exe", '/SUBSYSTEM:CONSOLE', '/INCREMENTAL:NO', '/MACHINE:X64')
    if ($Mode -eq 'release') { $linkFlags += @('/OPT:REF', '/OPT:ICF', '/opt:lldlto=3', '/DEBUG:NONE') }
    else { $linkFlags += @('/DEBUG') }
    $asanDll = $null
    if ($Mode -eq 'asan') {
        # Runtime de ASan: el del toolset MSVC si esta. Es mas nuevo que el de LLVM que trae Visual
        # Studio y sabe interceptar las funciones de Windows 11 24H2 (el de LLVM 19 se cae al
        # arrancar con "interception_win: unhandled instruction"). La instrumentacion es la misma.
        $msvcDll = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'
        if (Test-Path $msvcDll) {
            $asanLib = Join-Path $env:VCToolsInstallDir 'lib\x64'
            $asanDll = $msvcDll
        } else {
            $asanDir = Get-ChildItem (Join-Path $vsPath 'VC\Tools\Llvm\x64\lib\clang') -Directory | Select-Object -Last 1
            $asanLib = Join-Path $asanDir.FullName 'lib\windows'
            $asanDll = Join-Path $asanLib 'clang_rt.asan_dynamic-x86_64.dll'
        }
        $linkFlags += @("/LIBPATH:$asanLib", 'clang_rt.asan_dynamic-x86_64.lib', 'clang_rt.asan_dynamic_runtime_thunk-x86_64.lib')
    }
    $out = & $lld @linkFlags @commonObjs @toolObjs @res @libs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { Write-Host $out; throw "fallo el enlace de $t" }
    # El runtime de ASan es una DLL: al lado del exe, para que arranque sin tocar el PATH.
    if ($asanDll) { Copy-Item $asanDll $outDir -Force }
    if ($out.Trim()) { Write-Host $out.Trim() -ForegroundColor Yellow }
    $kb = [math]::Round((Get-Item $exe).Length / 1KB, 1)
    Write-Host ("  ✓ {0}  ({1} KB)  en {2:N1} s" -f $exe.Substring($root.Length + 1), $kb, ((Get-Date) - $started).TotalSeconds) -ForegroundColor Green
}
