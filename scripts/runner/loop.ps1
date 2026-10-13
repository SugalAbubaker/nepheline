# SPDX-License-Identifier: Apache-2.0
# Template for the registration loop of the self-hosted RTX 3060 Ti runner
# (docs/adr/0002-self-hosted-gpu-runner.md §4). scripts/runner/install.ps1
# fills in the __PLACEHOLDERS__ and installs the result as
# <Root>\admin\nph-runner-loop.ps1, which the scheduled task
# "Nepheline runner (register)" runs as SYSTEM at startup.
#
# Each pass: mint a registration token for the repository, register the
# runner ephemerally (one job per registration), start the task that runs
# run.cmd as the standard user, wait for it to finish, repeat. The token
# never leaves the admin folder, which the runner user cannot read.
$ErrorActionPreference = 'Stop'
$root = '__ROOT__'
$repo = '__REPO__'
$name = '__NAME__'
$labels = '__LABELS__'
$runTask = '__RUNTASK__'
$log = Join-Path $root 'admin\loop.log'
Add-Type -AssemblyName System.Security

function Write-Log($message) {
    Add-Content -Path $log -Value ('{0:yyyy-MM-dd HH:mm:ss} {1}' -f (Get-Date), $message)
}

function Get-Token {
    $bytes = [IO.File]::ReadAllBytes((Join-Path $root 'admin\token.bin'))
    $plain = [Security.Cryptography.ProtectedData]::Unprotect($bytes, $null,
        [Security.Cryptography.DataProtectionScope]::LocalMachine)
    [Text.Encoding]::UTF8.GetString($plain)
}

Set-Location $root
while ($true) {
    try {
        $headers = @{
            Authorization          = "Bearer $(Get-Token)"
            Accept                 = 'application/vnd.github+json'
            'X-GitHub-Api-Version' = '2022-11-28'
        }
        $reg = Invoke-RestMethod -Method Post -Headers $headers `
            -Uri "https://api.github.com/repos/$repo/actions/runners/registration-token"
        & (Join-Path $root 'config.cmd') --unattended --ephemeral --replace `
            --url "https://github.com/$repo" --token $reg.token --name $name --labels $labels `
            --work _work *> (Join-Path $root 'admin\config.log')
        if ($LASTEXITCODE -ne 0) { throw "config.cmd exited with $LASTEXITCODE (see admin\config.log)" }
        Write-Log 'registered (ephemeral); waiting for one job'
        Start-ScheduledTask -TaskName $runTask
        Start-Sleep -Seconds 10
        while ((Get-ScheduledTask -TaskName $runTask).State -eq 'Running') {
            Start-Sleep -Seconds 15
        }
        Write-Log 'run task finished'
    } catch {
        Write-Log "error: $_"
        Start-Sleep -Seconds 120
    }
}
