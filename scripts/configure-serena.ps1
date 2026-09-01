[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$forgeRoot = Split-Path -Parent $PSScriptRoot
$forgeBuild = Join-Path $forgeRoot '.scratch\serena-build'
$forgeCompileDb = Join-Path $forgeBuild 'compile_commands.json'
$forgeRootCompileDb = Join-Path $forgeRoot 'compile_commands.json'

function Find-ForgeTool {
    param(
        [Parameter(Mandatory)]
        [string] $Name,

        [Parameter(Mandatory)]
        [string[]] $Candidates
    )

    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    throw "Could not find $Name. Install it or restore Forge's .tools directory."
}

$forgeCmake = Find-ForgeTool -Name 'cmake' -Candidates @(
    (Join-Path $forgeRoot '.tools\cmake\data\bin\cmake.exe'),
    (Join-Path $forgeRoot '.tools\bin\cmake.exe')
)
$forgeNinja = Find-ForgeTool -Name 'ninja' -Candidates @(
    (Join-Path $forgeRoot '.tools\bin\ninja.exe')
)

$forgeVswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $forgeVswhere)) {
    throw 'Visual Studio Installer (vswhere.exe) was not found.'
}

$forgeVsInstall = & $forgeVswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ([string]::IsNullOrWhiteSpace($forgeVsInstall)) {
    throw 'Visual Studio with the C++ toolchain was not found.'
}

$forgeVsDevCmd = Join-Path $forgeVsInstall 'Common7\Tools\VsDevCmd.bat'
$forgeConfigure = 'call "' + $forgeVsDevCmd + '" -no_logo -arch=x64 -host_arch=x64' +
    ' && "' + $forgeCmake + '"' +
    ' -S "' + $forgeRoot + '"' +
    ' -B "' + $forgeBuild + '"' +
    ' -G Ninja' +
    ' -DCMAKE_MAKE_PROGRAM="' + $forgeNinja + '"' +
    ' -DCMAKE_BUILD_TYPE=Debug' +
    ' -DCMAKE_EXPORT_COMPILE_COMMANDS=ON' +
    ' -DFORGE_WITH_LLAMA=OFF' +
    ' -DFORGE_BUILD_TESTS=ON'

& cmd.exe /d /c $forgeConfigure
if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed with exit code $LASTEXITCODE."
}

if (-not (Test-Path -LiteralPath $forgeCompileDb)) {
    throw "CMake did not generate $forgeCompileDb."
}

Copy-Item -LiteralPath $forgeCompileDb -Destination $forgeRootCompileDb -Force
Write-Host "Updated $forgeRootCompileDb"
