<#
.SYNOPSIS
Builds Video Compressor with CMake, Ninja and MSVC; optionally runs the tests and creates the zip package.

.EXAMPLE
.\build.ps1              # build (build\video_compressor.exe)
.\build.ps1 -Test        # build and run the tests
.\build.ps1 -Package     # build and create build\VideoCompressor-<version>-win64.zip
#>
param(
    [string]$QtDir = $(if ($env:QTDIR) { $env:QTDIR } else { "C:\Qt\6.11.3\msvc2022_64" }),
    [string]$BuildDir = "$PSScriptRoot\build",
    [switch]$Test,
    [switch]$Package
)
$ErrorActionPreference = "Stop"

# MSVC environment (cl, link) from the newest Visual Studio with the C++ tools
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $installer = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer"
    $env:Path = "$installer;$env:Path"   # the dev shell looks for vswhere in PATH
    $vs = & "$installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw "Visual Studio with the C++ tools not found" }
    Import-Module "$vs\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}
# CMake and Ninja installed with winget may not be in the PATH of an already open shell
$env:Path += ";" + [Environment]::GetEnvironmentVariable("Path", "User")

function Invoke-Step([scriptblock]$Command) {
    & $Command
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

Invoke-Step { cmake -S $PSScriptRoot -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtDir" }
Invoke-Step { cmake --build $BuildDir }
if ($Test) {
    Invoke-Step { ctest --test-dir $BuildDir --output-on-failure }
}
if ($Package) {
    Invoke-Step { cpack --config "$BuildDir\CPackConfig.cmake" -B $BuildDir }
}
