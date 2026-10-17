# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Installs the self-hosted GitHub Actions runner on the RTX 3060 Ti
    reference machine as docs/adr/0002-self-hosted-gpu-runner.md and
    docs/adr/0028-gpu-runner-service.md require.

.DESCRIPTION
    Run from an elevated PowerShell on the machine with the reference GPU:

        .\scripts\runner\install.ps1

    It needs one runner registration token for the repository, used once and
    never stored. -Token passes it; otherwise the script asks `gh` for one
    (the GitHub CLI, logged in as a repository administrator) and, failing
    that, prompts for the token shown under Settings > Actions > Runners >
    New self-hosted runner. Registration tokens expire after an hour.

      1. Removes any earlier installation: the runner service, the scheduled
         tasks and folder of the old registration loop (ADR-0028 replaces
         it), and the runner folder. Nothing inside an old runner folder is
         executed, since the job's user could have modified it.
      2. Creates nph-runner, a dedicated standard (non-administrator) local
         user with a random password that goes to the service manager and
         is then forgotten (an existing user gets a new password).
      3. Downloads the latest actions/runner release, checks its SHA-256
         against the one GitHub publishes, and unpacks it into -Root
         (C:\actions-runner), a folder that inherits nothing from C:\ and
         that only Administrators, SYSTEM and nph-runner can write.
      4. Registers the runner once and installs it as a Windows service
         running as nph-runner, started automatically at boot. The listener
         keeps its own registration across restarts and job after job.

    Labels: self-hosted, windows, x64 (added by the runner) plus gpu and
    rtx3060ti, matching .github/workflows/gpu-perf.yml. Fork pull requests
    never reach the machine (the workflow's `if:`); keep "Require approval
    for all outside collaborators" enabled in the repository settings.

    Prerequisites on the machine: Visual Studio 2022 (MSVC, CMake, Ninja),
    the Vulkan SDK (glslc, validation layers) installed where the runner
    user can read it (C:\VulkanSDK via scripts\install_vulkan_sdk.ps1; an
    SDK inside a user profile is not, and the script warns), Git with
    git-lfs, and a current GPU driver. Builds land under -Root\_work.

    Re-running reinstalls from scratch. -Uninstall removes the service, the
    folder, the user and, through `gh` when it is available, the runner's
    registration on GitHub.
#>
[CmdletBinding()]
param(
    [string]$Repo = 'SugalAbubaker/nepheline',
    [string]$Root = 'C:\actions-runner',
    [string]$User = 'nph-runner',
    [string]$RunnerName = 'rtx3060ti',
    [string]$Labels = 'gpu,rtx3060ti',
    [string]$Token = '',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell (it creates a user, a service, and ACLs).'
}

$gh = Get-Command gh -ErrorAction SilentlyContinue

# Removes the user right $Right from the account with SID $Sid, if it has it.
function Remove-UserRight([string]$Right, [string]$Sid) {
    $inf = Join-Path $env:TEMP 'nph-runner-rights.inf'
    $sdb = Join-Path $env:TEMP 'nph-runner-rights.sdb'
    secedit /export /cfg $inf /areas USER_RIGHTS | Out-Null
    $lines = Get-Content $inf
    $changed = $false
    $lines = $lines | ForEach-Object {
        if ($_.StartsWith("$Right ") -and $_.Contains("*$Sid")) {
            $changed = $true
            $holders = ($_ -split '=', 2)[1].Split(',') | ForEach-Object { $_.Trim() } |
                Where-Object { $_ -ne "*$Sid" }
            "$Right = " + ($holders -join ',')
        } else { $_ }
    }
    if ($changed) {
        Set-Content -Path $inf -Value $lines -Encoding Unicode
        secedit /configure /db $sdb /cfg $inf /areas USER_RIGHTS | Out-Null
        Write-Host "Removed '$Right' from $User."
    }
    Remove-Item $inf, $sdb -Force -ErrorAction SilentlyContinue
}

# Removes everything an earlier installation left on the machine except the
# user. Only Windows tools run here, never a file from the runner folder.
function Remove-Installation {
    foreach ($service in @(Get-Service -Name "actions.runner.*" -ErrorAction SilentlyContinue)) {
        Stop-Service -Name $service.Name -Force -ErrorAction SilentlyContinue
        sc.exe delete $service.Name | Out-Null
        Write-Host "Removed service $($service.Name)."
    }
    # The registration loop of the first design (ADR-0002 §4, replaced by ADR-0028).
    foreach ($task in @('Nepheline runner (register)', 'Nepheline runner (run)')) {
        if (Get-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue) {
            Stop-ScheduledTask -TaskName $task -ErrorAction SilentlyContinue
            Unregister-ScheduledTask -TaskName $task -Confirm:$false
            Write-Host "Removed task '$task'."
        }
    }
    Get-Process -Name 'Runner.Listener', 'Runner.Worker' -ErrorAction SilentlyContinue | Stop-Process -Force
    if (Test-Path $Root) {
        Remove-Item -Recurse -Force $Root
        Write-Host "Removed $Root."
    }
    $existing = Get-LocalUser -Name $User -ErrorAction SilentlyContinue
    if ($existing) {
        # The old run task logged on as a batch job; the service does not.
        Remove-UserRight 'SeBatchLogonRight' $existing.SID.Value
    }
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------
if ($Uninstall) {
    Remove-Installation
    if (Get-LocalUser -Name $User -ErrorAction SilentlyContinue) {
        Remove-LocalUser -Name $User
        Write-Host "Removed user $User."
    }
    $id = $null
    if ($gh) {
        $id = & gh api "repos/$Repo/actions/runners" --jq ".runners[] | select(.name == `"$RunnerName`") | .id"
    }
    if ($id) {
        & gh api -X DELETE "repos/$Repo/actions/runners/$id" | Out-Null
        Write-Host "Removed the registration of $RunnerName from $Repo."
    } else {
        Write-Host "Remove '$RunnerName' under Settings > Actions > Runners if it is still listed."
    }
    return
}

# ---------------------------------------------------------------------------
# 0. The registration token (used once, never written to disk)
# ---------------------------------------------------------------------------
if ($Token -eq '' -and $gh) {
    $Token = & gh api -X POST "repos/$Repo/actions/runners/registration-token" --jq .token
    if ($LASTEXITCODE -ne 0) { $Token = '' }
    if ($Token -ne '') { Write-Host "Registration token obtained through gh." }
}
if ($Token -eq '') {
    $secure = Read-Host -AsSecureString -Prompt 'Registration token (Settings > Actions > Runners > New self-hosted runner)'
    $Token = (New-Object System.Net.NetworkCredential('', $secure)).Password
}
if ($Token -eq '') { throw 'A registration token is required.' }

# ---------------------------------------------------------------------------
# 1. Remove the earlier installation
# ---------------------------------------------------------------------------
Remove-Installation

# ---------------------------------------------------------------------------
# 2. The dedicated standard user
# ---------------------------------------------------------------------------
# Letters and digits only: the password travels on a command line. 32 random
# characters from a CSPRNG, redrawn until all three classes appear (Windows'
# complexity rule wants three of four).
$alphabet = 'ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789'
$rng = [Security.Cryptography.RandomNumberGenerator]::Create()
do {
    $bytes = New-Object byte[] 32
    $rng.GetBytes($bytes)
    $password = -join ($bytes | ForEach-Object { $alphabet[$_ % $alphabet.Length] })
} until ($password -cmatch '[A-Z]' -and $password -cmatch '[a-z]' -and $password -match '[0-9]')
$rng.Dispose()
$securePassword = ConvertTo-SecureString $password -AsPlainText -Force
if (Get-LocalUser -Name $User -ErrorAction SilentlyContinue) {
    Set-LocalUser -Name $User -Password $securePassword -PasswordNeverExpires $true
    Write-Host "Local user $User exists; password rotated."
} else {
    New-LocalUser -Name $User -Password $securePassword -PasswordNeverExpires -UserMayNotChangePassword `
        -Description 'Nepheline GitHub Actions runner (ADR-0002)' | Out-Null
    Write-Host "Local user $User created."
}

# The job needs the Vulkan SDK (glslc for the shader build, the validation
# layer for the gpu tests) readable by the runner user. The SDK's location
# comes from the machine-wide VULKAN_SDK; an SDK installed inside a user
# profile is unreadable by every other account unless the runner user was
# granted read access to it (an explicit allow rule naming that user).
$sdk = [Environment]::GetEnvironmentVariable('VULKAN_SDK', 'Machine')
$sdkReadable = $false
if ($sdk -and (Test-Path $sdk)) {
    $readExecute = [Security.AccessControl.FileSystemRights]::ReadAndExecute
    $sdkReadable = [bool]((Get-Acl $sdk).Access | Where-Object {
            $_.AccessControlType -eq 'Allow' -and
            $_.IdentityReference.Value -like "*\$User" -and
            ($_.FileSystemRights -band $readExecute) -eq $readExecute
        })
}
if (-not $sdk) {
    Write-Warning ("VULKAN_SDK is not set machine-wide, so the job will not find glslc. Install the SDK " +
        "with scripts\install_vulkan_sdk.ps1 (into C:\VulkanSDK) or set VULKAN_SDK for the machine.")
} elseif ($sdk -like (Join-Path $env:SystemDrive 'Users\*') -and -not $sdkReadable) {
    Write-Warning ("VULKAN_SDK ($sdk) is inside a user profile, which $User cannot read. Either grant " +
        "read access: icacls `"$sdk`" /grant `"${User}:(OI)(CI)RX`" /T, or install the SDK " +
        "machine-wide with scripts\install_vulkan_sdk.ps1.")
}

# ---------------------------------------------------------------------------
# 3. The runner package, checked and unpacked into a closed folder
# ---------------------------------------------------------------------------
$release = Invoke-RestMethod -Uri 'https://api.github.com/repos/actions/runner/releases/latest' `
    -Headers @{ Accept = 'application/vnd.github+json' }
$version = $release.tag_name.TrimStart('v')
# The release notes carry each package's hash between these markers.
if ($release.body -notmatch '<!-- BEGIN SHA win-x64 -->([0-9a-fA-F]{64})<!-- END SHA win-x64 -->') {
    throw "The release notes of actions/runner $version do not list the win-x64 SHA-256."
}
$expected = $Matches[1].ToLowerInvariant()

# C:\ grants Authenticated Users modify rights on new folders: break the
# inheritance before anything is unpacked, so no other account could swap a
# file between the download and the elevated configuration below.
New-Item -ItemType Directory -Force -Path $Root | Out-Null
icacls $Root /inheritance:r /grant:r '*S-1-5-32-544:(OI)(CI)F' '*S-1-5-18:(OI)(CI)F' /Q | Out-Null
if ($LASTEXITCODE -ne 0) { throw "icacls could not close $Root." }

$zip = Join-Path $Root "actions-runner-win-x64-$version.zip"
$url = "https://github.com/actions/runner/releases/download/v$version/actions-runner-win-x64-$version.zip"
Write-Host "Downloading $url"
Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
$actual = (Get-FileHash -Algorithm SHA256 -Path $zip).Hash.ToLowerInvariant()
if ($actual -ne $expected) { throw "SHA-256 mismatch for $url (expected $expected, got $actual)." }
Expand-Archive -Path $zip -DestinationPath $Root -Force
Remove-Item $zip -Force
Write-Host "Runner $version unpacked into $Root (SHA-256 verified)."

# ---------------------------------------------------------------------------
# 4. Register once and install the service
# ---------------------------------------------------------------------------
# The listener directly, not config.cmd: no cmd.exe parsing of the arguments.
# The files were unpacked a moment ago into a folder only Administrators and
# SYSTEM can write, so this is the one time elevated code runs from it.
& (Join-Path $Root 'bin\Runner.Listener.exe') configure --unattended --replace `
    --url "https://github.com/$Repo" --token $Token --name $RunnerName --labels $Labels --work _work `
    --runasservice --windowslogonaccount "$env:COMPUTERNAME\$User" --windowslogonpassword $password
$code = $LASTEXITCODE
$Token = ''
$password = ''
if ($code -ne 0) { throw "Runner configuration failed with exit code $code." }

# The service account updates the runner in place, writes its diagnostics,
# and builds under _work.
icacls $Root /grant "${User}:(OI)(CI)M" /Q | Out-Null
if ($LASTEXITCODE -ne 0) { throw "icacls could not grant $User access to $Root." }

$service = Get-Service -Name "actions.runner.*" | Select-Object -First 1
if (-not $service) { throw 'The runner service was not created.' }
Set-Service -Name $service.Name -StartupType Automatic
if ($service.Status -ne 'Running') { Start-Service -Name $service.Name }

Write-Host ''
Write-Host "Installed. Service '$($service.Name)' runs as $User and starts at boot."
Write-Host 'Check Settings > Actions > Runners, then run the "GPU performance" workflow once by hand.'
