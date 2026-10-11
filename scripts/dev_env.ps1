# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Imports the Visual Studio x64 developer environment into this PowerShell
    session and puts the VS-bundled CMake, Ninja, and LLVM tools on PATH.

.DESCRIPTION
    Dot-source it before using the win-* presets from a plain PowerShell:

        . .\scripts\dev_env.ps1
        cmake --preset win-msvc
        cmake --build --preset win-msvc-debug
        ctest --preset win-msvc-debug

    With -GitHubEnv the variables are appended to $GITHUB_ENV instead, which is
    how CI gives later workflow steps the MSVC environment.
#>
[CmdletBinding()]
param(
    [switch]$GitHubEnv,
    [ValidateSet('x64')]
    [string]$Arch = 'x64'
)

$ErrorActionPreference = 'Stop'

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found at '$vswhere'. Is Visual Studio 2022 installed?"
}

$vsPath = & $vswhere -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath
if (-not $vsPath) {
    throw 'No Visual Studio installation with the C++ x64 toolset was found.'
}

# vcvars64.bat for x64 (the file names drop the 'x').
$vcvarsName = @{ 'x64' = 'vcvars64.bat' }[$Arch]
$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\$vcvarsName"
if (-not (Test-Path $vcvars)) {
    throw "vcvars script not found: $vcvars"
}

# Run vcvars in cmd and capture the resulting environment.
$envLines = & cmd.exe /d /c "`"$vcvars`" >nul 2>&1 && set"
if ($LASTEXITCODE -ne 0) {
    throw "vcvars$Arch.bat failed with exit code $LASTEXITCODE"
}

$extraPaths = @(
    (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
    (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'),
    (Join-Path $vsPath 'VC\Tools\Llvm\x64\bin')
) | Where-Object { Test-Path $_ }

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
foreach ($line in $envLines) {
    $i = $line.IndexOf('=')
    if ($i -le 0) { continue }
    $name = $line.Substring(0, $i)
    $value = $line.Substring($i + 1)
    if ($name -ieq 'PATH') {
        foreach ($p in $extraPaths) {
            if (($value -split ';') -notcontains $p) { $value = "$p;$value" }
        }
    }
    if ($GitHubEnv) {
        if (-not $env:GITHUB_ENV) { throw '-GitHubEnv requires $GITHUB_ENV (run inside GitHub Actions).' }
        [System.IO.File]::AppendAllText($env:GITHUB_ENV, "$name=$value`n", $utf8NoBom)
    } else {
        Set-Item -Path "env:$name" -Value $value
    }
}

if (-not $GitHubEnv) {
    Write-Host "MSVC $Arch developer environment loaded from $vsPath"
}
