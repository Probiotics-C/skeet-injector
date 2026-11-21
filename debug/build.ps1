# 构建顺序：
#   1) source code(dll)\skeet.vcxproj  -> source code(dll)\Release\skeet.dll
#      (PostBuild 会自动复制到 ..\debug\skeet.dll 并删掉注入器的 resource.res)
#   2) skeet_tr\skeet_tr.vcxproj       -> ..\skeet_tr.dll
#   3) 再删一次 ..\obj\<Config>\resource.res（rc.exe 不跟踪 RCDATA 依赖）
#   4) debug.vcxproj                   -> ..\..\debug.exe（内嵌 skeet.dll + skeet_tr.dll + skeet_tr.txt）
param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path          # source code\debug
$sourceDir = Split-Path -Parent $root                        # source code
$dllProject = Join-Path $sourceDir 'source code(dll)\skeet.vcxproj'

function Find-MSBuild {
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $found = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' 2>$null
        if ($found) { return ($found | Select-Object -First 1) }
    }
    foreach ($candidate in @(
            'E:\123456\MSBuild\Current\Bin\MSBuild.exe',
            'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe',
            'C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe',
            'C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\MSBuild\Current\Bin\MSBuild.exe')) {
        if (Test-Path $candidate) { return $candidate }
    }
    $cmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw 'MSBuild.exe not found'
}

$msbuild = Find-MSBuild
Write-Host "MSBuild : $msbuild"
Write-Host "Config  : $Configuration|Win32"

$common = @('/nologo', '/v:minimal', '/m', "/p:Configuration=$Configuration", '/p:Platform=Win32')
$dllCommon = @('/nologo', '/v:minimal', '/m', '/p:Configuration=Release', '/p:Platform=Win32')

if (-not (Test-Path $dllProject)) { throw "missing project: $dllProject" }

Write-Host ""
Write-Host "[1/4] building skeet.dll (Release|Win32) ..."
& $msbuild $dllProject @dllCommon
if ($LASTEXITCODE -ne 0) { throw "skeet build failed ($LASTEXITCODE)" }

Write-Host "[2/4] building skeet_tr.dll ..."
& $msbuild "$root\skeet_tr\skeet_tr.vcxproj" @common
if ($LASTEXITCODE -ne 0) { throw "skeet_tr build failed ($LASTEXITCODE)" }

$res = Join-Path $root "obj\$Configuration\resource.res"
Write-Host "[3/4] invalidating $res"
Remove-Item $res -Force -ErrorAction SilentlyContinue

Write-Host "[4/4] building debug.exe ..."
& $msbuild "$root\debug.vcxproj" @common
if ($LASTEXITCODE -ne 0) { throw "debug build failed ($LASTEXITCODE)" }

$dll = Join-Path $root 'skeet_tr.dll'
$embedded = Join-Path $root 'skeet.dll'
$exe = Join-Path $sourceDir 'debug.exe'
Write-Host ""
foreach ($f in @($embedded, $dll, $exe)) {
    if (Test-Path $f) {
        $item = Get-Item $f
        '{0,-24} {1,12:N0} B  {2:yyyy-MM-dd HH:mm:ss}' -f $item.Name, $item.Length, $item.LastWriteTime | Write-Host
    }
    else {
        Write-Warning "missing output: $f"
    }
}
