# SPDX-License-Identifier: Apache-2.0
<#
.SYNOPSIS
    Installs the self-hosted GitHub Actions runner on the RTX 3060 Ti
    reference machine as docs/adr/0002-self-hosted-gpu-runner.md requires.

.DESCRIPTION
    Run once, from an elevated PowerShell, on the machine with the reference
    GPU:

        .\scripts\runner\install.ps1

    It asks for a fine-grained personal access token restricted to this one
    repository with the single permission "Administration: Read and write"
    (what minting runner registration tokens needs; -Pat passes it
    non-interactively), then:

      1. creates nph-runner, a dedicated standard (non-administrator) local
         user with a random password that is handed to the Task Scheduler
         and then forgotten, and grants it the "Log on as a batch job" right
         the task needs;
      2. downloads the latest actions/runner release into -Root
         (C:\actions-runner), the only folder nph-runner can write. The
         token is stored DPAPI-protected (machine scope) under -Root\admin,
         readable by SYSTEM and Administrators only, never by nph-runner;
      3. registers two scheduled tasks. "Nepheline runner (register)" runs
         as SYSTEM at startup and loops forever (scripts/runner/loop.ps1):
         mint a registration token, `config.cmd --ephemeral`, start
         "Nepheline runner (run)", wait for it to finish, repeat.
         "Nepheline runner (run)" runs run.cmd as nph-runner for exactly one
         job. Ephemeral registration means no job can leave state behind for
         the next one, and the job's user never holds a credential.

    Labels: self-hosted, windows, x64 (added by the runner) plus gpu and
    rtx3060ti, matching .github/workflows/gpu-perf.yml. Fork pull requests
    never reach the machine (the workflow's `if:`); keep "Require approval
    for all outside collaborators" enabled in the repository settings.

    Prerequisites on the machine: Visual Studio 2022 (MSVC, CMake, Ninja),
    the Vulkan SDK (glslc, validation layers), Git with git-lfs, and a
    current GPU driver. Builds land under -Root\_work.

    The tasks run in session 0 (no interactive desktop). If the swapchain
    tests cannot present there, switch "Nepheline runner (run)" to an
    interactive logon (Task Scheduler: "Run only when user is logged on",
    trigger "At log on" of nph-runner) and configure auto-logon for
    nph-runner with Sysinternals Autologon; see docs/build.md.

    Re-running rotates the password and the token and replaces the tasks.
    -Uninstall removes the tasks, the folder, and the user.
#>
[CmdletBinding()]
param(
    [string]$Repo = 'SugalAbubaker/nepheline',
    [string]$Root = 'C:\actions-runner',
    [string]$User = 'nph-runner',
    [string]$RunnerName = 'rtx3060ti',
    [string]$Labels = 'gpu,rtx3060ti',
    [string]$Pat = '',
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'
$registerTask = 'Nepheline runner (register)'
$runTask = 'Nepheline runner (run)'

$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from an elevated PowerShell (it creates a user, ACLs, and scheduled tasks).'
}

# ---------------------------------------------------------------------------
# Uninstall
# ---------------------------------------------------------------------------
if ($Uninstall) {
    foreach ($task in @($registerTask, $runTask)) {
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
    if (Get-LocalUser -Name $User -ErrorAction SilentlyContinue) {
        Remove-LocalUser -Name $User
        Write-Host "Removed user $User."
    }
    Write-Host 'Any ephemeral registration still listed under Settings > Actions > Runners expires on its own.'
    return
}

# ---------------------------------------------------------------------------
# 0. The token (never written to disk in clear text)
# ---------------------------------------------------------------------------
if ($Pat -eq '') {
    $secure = Read-Host -AsSecureString -Prompt 'Fine-grained token (this repository, Administration: read and write)'
    $Pat = (New-Object System.Net.NetworkCredential('', $secure)).Password
}
if ($Pat -eq '') { throw 'A token is required.' }

# Check it before changing anything on the machine.
$headers = @{
    Authorization          = "Bearer $Pat"
    Accept                 = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}
try {
    $probe = Invoke-RestMethod -Method Post -Headers $headers `
        -Uri "https://api.github.com/repos/$Repo/actions/runners/registration-token"
} catch {
    throw "The token cannot mint registration tokens for $Repo (needs Administration: read and write): $_"
}
if (-not $probe.token) { throw 'Unexpected response from the registration-token endpoint.' }
Write-Host "Token verified against $Repo."

# ---------------------------------------------------------------------------
# 1. The dedicated standard user
# ---------------------------------------------------------------------------
Add-Type -AssemblyName System.Web
$password = [System.Web.Security.Membership]::GeneratePassword(32, 8)
$securePassword = ConvertTo-SecureString $password -AsPlainText -Force
if (Get-LocalUser -Name $User -ErrorAction SilentlyContinue) {
    Set-LocalUser -Name $User -Password $securePassword -PasswordNeverExpires $true
    Write-Host "Local user $User exists; password rotated."
} else {
    New-LocalUser -Name $User -Password $securePassword -PasswordNeverExpires -UserMayNotChangePassword `
        -Description 'Nepheline GitHub Actions runner (standard user, ADR-0002)' | Out-Null
    Write-Host "Local user $User created."
}
$sid = (Get-LocalUser -Name $User).SID.Value

# "Log on as a batch job" (SeBatchLogonRight) so a scheduled task can run as
# this user without an interactive session. Standard users do not have it.
$inf = Join-Path $env:TEMP 'nph-runner-rights.inf'
$sdb = Join-Path $env:TEMP 'nph-runner-rights.sdb'
secedit /export /cfg $inf /areas USER_RIGHTS | Out-Null
$lines = Get-Content $inf
$current = $lines | Where-Object { $_ -like 'SeBatchLogonRight*' }
if (-not ($current -and $current -like "*$sid*")) {
    if ($current) {
        $lines = $lines | ForEach-Object { if ($_ -like 'SeBatchLogonRight*') { "$_,*$sid" } else { $_ } }
    } else {
        $lines += "SeBatchLogonRight = *$sid"   # the export ends with [Privilege Rights]
    }
    Set-Content -Path $inf -Value $lines -Encoding Unicode
    secedit /configure /db $sdb /cfg $inf /areas USER_RIGHTS | Out-Null
    Write-Host "Granted 'Log on as a batch job' to $User."
}
Remove-Item $inf, $sdb -Force -ErrorAction SilentlyContinue

# ---------------------------------------------------------------------------
# 2. The runner package and the folder layout
# ---------------------------------------------------------------------------
$release = Invoke-RestMethod -Uri 'https://api.github.com/repos/actions/runner/releases/latest' `
    -Headers @{ Accept = 'application/vnd.github+json' }
$version = $release.tag_name.TrimStart('v')
New-Item -ItemType Directory -Force -Path $Root | Out-Null
if (-not (Test-Path (Join-Path $Root 'config.cmd'))) {
    $zip = Join-Path $env:TEMP "actions-runner-win-x64-$version.zip"
    $url = "https://github.com/actions/runner/releases/download/v$version/actions-runner-win-x64-$version.zip"
    Write-Host "Downloading $url"
    Invoke-WebRequest -Uri $url -OutFile $zip -UseBasicParsing
    Expand-Archive -Path $zip -DestinationPath $Root -Force
    Remove-Item $zip -Force
    Write-Host "Runner $version unpacked into $Root."
} else {
    Write-Host "Runner already present in $Root (latest release is $version)."
}

# nph-runner may modify the runner folder (registration files, _work) and
# nothing else on the machine. The admin subfolder is closed to it.
icacls $Root /grant "${User}:(OI)(CI)M" /T /Q | Out-Null
$admin = Join-Path $Root 'admin'
New-Item -ItemType Directory -Force -Path $admin | Out-Null
icacls $admin /inheritance:r /grant 'SYSTEM:(OI)(CI)F' 'Administrators:(OI)(CI)F' /Q | Out-Null

# The token, DPAPI-protected in machine scope: only processes on this machine
# with read access to the file (SYSTEM, Administrators) can unprotect it.
Add-Type -AssemblyName System.Security
$protected = [Security.Cryptography.ProtectedData]::Protect(
    [Text.Encoding]::UTF8.GetBytes($Pat), $null,
    [Security.Cryptography.DataProtectionScope]::LocalMachine)
[IO.File]::WriteAllBytes((Join-Path $admin 'token.bin'), $protected)
$Pat = ''

# The registration loop (scripts/runner/loop.ps1 with the placeholders filled in).
$loop = Get-Content -Raw (Join-Path $PSScriptRoot 'loop.ps1')
$loop = $loop.Replace('__ROOT__', $Root).Replace('__REPO__', $Repo).Replace('__NAME__', $RunnerName)
$loop = $loop.Replace('__LABELS__', $Labels).Replace('__RUNTASK__', $runTask)
$loopPath = Join-Path $admin 'nph-runner-loop.ps1'
Set-Content -Path $loopPath -Value $loop -Encoding UTF8

# ---------------------------------------------------------------------------
# 3. The scheduled tasks
# ---------------------------------------------------------------------------
$noLimit = New-TimeSpan -Seconds 0   # PT0S: no execution time limit

$runAction = New-ScheduledTaskAction -Execute (Join-Path $Root 'run.cmd') -WorkingDirectory $Root
$runSettings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit $noLimit -MultipleInstances IgnoreNew `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
Register-ScheduledTask -TaskName $runTask -Action $runAction -Settings $runSettings `
    -User ".\$User" -Password $password -RunLevel Limited -Force | Out-Null
$password = ''

$registerAction = New-ScheduledTaskAction -Execute 'powershell.exe' `
    -Argument "-NoProfile -ExecutionPolicy RemoteSigned -File `"$loopPath`"" -WorkingDirectory $Root
$registerTrigger = New-ScheduledTaskTrigger -AtStartup
$registerSettings = New-ScheduledTaskSettingsSet -ExecutionTimeLimit $noLimit -MultipleInstances IgnoreNew `
    -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -RestartCount 100 -RestartInterval (New-TimeSpan -Minutes 1)
Register-ScheduledTask -TaskName $registerTask -Action $registerAction -Trigger $registerTrigger `
    -Settings $registerSettings -User 'SYSTEM' -RunLevel Highest -Force | Out-Null

Stop-ScheduledTask -TaskName $registerTask -ErrorAction SilentlyContinue
Start-ScheduledTask -TaskName $registerTask

Write-Host ''
Write-Host "Installed. '$registerTask' (SYSTEM) is running and registers '$RunnerName' for $Repo;"
Write-Host "'$runTask' executes each job as $User. Log: $admin\loop.log"
Write-Host 'Check Settings > Actions > Runners, then run the "GPU performance" workflow once by hand.'
