#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Désinstalle le pack d'effets Kwiet (drivers pnputil + device SWC).

.DESCRIPTION
    Retrouve les packages publiés (state/published-drivers.txt, sinon scan
    pnputil sur le nom d'origine kwiet_*.inf), les désinstalle, supprime le
    devnode SWC restant et redémarre la pile audio. -RemoveCert retire aussi
    le certificat de test des magasins machine.
#>
[CmdletBinding()]
param(
    [switch]$RemoveCert
)

$ErrorActionPreference = 'Stop'
$stateDir = Join-Path $PSScriptRoot 'state'

Write-Host '=== Kwiet — désinstallation du pack d''effets ==='

# --- Drivers publiés ------------------------------------------------------
$published = @()
$stateFile = Join-Path $stateDir 'published-drivers.txt'
if (Test-Path $stateFile) {
    $published = @(Get-Content $stateFile | Where-Object { $_ })
}
if ($published.Count -eq 0) {
    $enum = & pnputil.exe /enum-drivers | Out-String
    $blocks = $enum -split '(?m)^\s*$' | Where-Object { $_ -match 'kwiet_(component|extension)\.inf' }
    foreach ($b in $blocks) {
        if ($b -match '(oem\d+\.inf)') { $published += $Matches[1] }
    }
}
if ($published.Count -eq 0) {
    Write-Host 'Aucun package Kwiet publié trouvé (no-op).'
} else {
    foreach ($drv in ($published | Select-Object -Unique)) {
        Write-Host "-> pnputil /delete-driver $drv /uninstall /force"
        $out = & pnputil.exe /delete-driver $drv /uninstall /force 2>&1 | Out-String
        Write-Host ($out -replace '(?m)^', '   ')
    }
    Remove-Item $stateFile -Force -ErrorAction SilentlyContinue
}

# --- Devnode SWC restant --------------------------------------------------
$dev = Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match 'AUDIO_EFFECTPACK_KWIET' }
foreach ($d in $dev) {
    Write-Host "-> Suppression du device $($d.InstanceId)"
    & pnputil.exe /remove-device $d.InstanceId 2>&1 | Out-Null
}

# --- Certificat de test ---------------------------------------------------
if ($RemoveCert) {
    foreach ($store in 'Root', 'TrustedPublisher') {
        $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
        $s.Open('ReadWrite')
        @($s.Certificates | Where-Object { $_.Subject -eq 'CN=Kwiet Dev Test' }) |
            ForEach-Object { $s.Remove($_); Write-Host "-> Certificat retiré de LocalMachine\$store" }
        $s.Close()
    }
}

Write-Host '-> Redémarrage de la pile audio...'
foreach ($svc in 'Audiosrv', 'AudioEndpointBuilder') {
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -ne 'Stopped') { Stop-Service $svc -Force }
}
Start-Service AudioEndpointBuilder
Start-Service Audiosrv

Write-Host '=== Désinstallation du pack terminée ==='
