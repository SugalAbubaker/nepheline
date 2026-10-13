# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Installs the LunarG Vulkan SDK silently on Windows (for CI runners and
    fresh development machines).

.DESCRIPTION
    Downloads the pinned SDK installer from LunarG and runs it unattended
    into C:\VulkanSDK\<version> (or -Root). The engine itself compiles
    against the vendored Vulkan headers; the SDK supplies glslc for the
    shader build, the validation layers for Debug and Development builds,
    and the loader.

        ./scripts/install_vulkan_sdk.ps1 -Version 1.4.321.1
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version,
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'
if (-not $Root) { $Root = "C:\VulkanSDK\$Version" }

$installer = Join-Path $env:TEMP "vulkansdk-windows-X64-$Version.exe"
$url = "https://sdk.lunarg.com/sdk/download/$Version/windows/vulkansdk-windows-X64-$Version.exe"
Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $installer -UseBasicParsing

Write-Host "Installing into $Root"
$args = @('--root', $Root, '--accept-licenses', '--default-answer', '--confirm-command', 'install')
$proc = Start-Process -FilePath $installer -ArgumentList $args -Wait -PassThru -NoNewWindow
if ($proc.ExitCode -ne 0) {
    throw "The Vulkan SDK installer failed with exit code $($proc.ExitCode)"
}
if (-not (Test-Path (Join-Path $Root 'Bin\glslc.exe'))) {
    throw "glslc.exe is missing from $Root after the installation"
}
Remove-Item $installer -Force
Write-Host "Vulkan SDK $Version installed in $Root"
