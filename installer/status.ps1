<#
.SYNOPSIS
    Affiche l'état de l'installation Kwiet : COM, DLL, FxProperties, audiodg.

.DESCRIPTION
    Script purement informatif, aucune écriture. Fonctionne mieux en
    administrateur (lecture des modules d'audiodg), mais dégrade proprement.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\common.ps1')

function Write-Check([bool]$Ok, [string]$Label, [string]$Detail = '') {
    $mark = if ($Ok) { '[OK]' } else { '[--]' }
    $line = "  $mark $Label"
    if ($Detail) { $line += " : $Detail" }
    Write-Host $line
}

Write-Host '=== Kwiet — état de l''installation ==='

# --- Classe COM ----------------------------------------------------------------
Write-Host ''
Write-Host "Classe COM ($($script:KwietClsid)) :"
$comOk = Test-Path $script:ClsidRegKey
Write-Check $comOk 'CLSID enregistré (HKLM\Software\Classes)'
$serverPath = $null
if ($comOk) {
    $inproc = Get-ItemProperty -Path (Join-Path $script:ClsidRegKey 'InprocServer32') -ErrorAction SilentlyContinue
    if ($inproc -and $inproc.PSObject.Properties['(default)']) {
        $serverPath = $inproc.'(default)'
    }
    if ($serverPath) {
        Write-Check (Test-Path $serverPath) 'InprocServer32 pointe sur une DLL existante' $serverPath
    }
}
Write-Check (Test-Path $script:KwietDllTarget) 'DLL en place' $script:KwietDllTarget
Write-Check (Test-Path $script:ApoCatalogKey) 'Catalogue APO (AudioEngine\AudioProcessingObjects)'

# --- Politique audiodg ---------------------------------------------------------
Write-Host ''
Write-Host 'Politique audiodg :'
$dpadg = $null
$audioPol = Get-ItemProperty -Path $script:AudioPolicyKey -ErrorAction SilentlyContinue
if ($audioPol -and $audioPol.PSObject.Properties['DisableProtectedAudioDG']) {
    $dpadg = $audioPol.DisableProtectedAudioDG
}
Write-Check ($dpadg -eq 1) 'DisableProtectedAudioDG = 1 (requis pour APO non signé)' "valeur actuelle : $(if ($null -eq $dpadg) { 'absente' } else { $dpadg })"

# --- Endpoints -----------------------------------------------------------------
Write-Host ''
Write-Host 'Endpoints de capture référençant Kwiet :'
$targets = @(Get-KwietInstalledTargets)
if ($targets.Count -eq 0) {
    Write-Host '  (aucun)'
} else {
    $endpoints = @(Get-KwietCaptureEndpoints)
    foreach ($t in $targets) {
        $ep = $endpoints | Where-Object { $_.Id -eq $t.EndpointId } | Select-Object -First 1
        $name = if ($ep) { $ep.Name } else { '(nom inconnu)' }
        Write-Host "  [$($t.Slot)] $($t.EndpointId)  $name"
        $props = Get-ItemProperty -Path $t.FxPath -ErrorAction SilentlyContinue
        $modesName = "$($script:FxModesFmtid),$($script:SlotPids[$t.Slot])"
        if ($props -and $props.PSObject.Properties[$modesName]) {
            Write-Host "        modes : $($props.$modesName -join ', ')"
        } else {
            Write-Host '        modes : (valeur absente !)'
        }
    }
}

# --- audiodg -------------------------------------------------------------------
Write-Host ''
Write-Host 'audiodg.exe :'
$audiodg = Get-Process -Name audiodg -ErrorAction SilentlyContinue
if ($null -eq $audiodg) {
    Write-Host '  Pas de processus audiodg (normal sans flux audio actif).'
} else {
    $loaded = $false
    try {
        $loaded = @($audiodg.Modules | Where-Object { $_.ModuleName -eq $script:KwietDllName }).Count -gt 0
        Write-Check $loaded "$($script:KwietDllName) chargée dans audiodg" 'nécessite un flux de capture actif pour charger'
    } catch {
        Write-Host '  (lecture des modules impossible — lance en administrateur)'
    }
}

# --- État local ----------------------------------------------------------------
Write-Host ''
Write-Host 'Fichiers d''état locaux :'
$stateFile = Get-KwietLatestStateFile
if ($null -eq $stateFile) {
    Write-Host '  (aucun install-state-*.json)'
} else {
    Write-Host "  Dernier : $($stateFile.Name)"
}
Write-Host ''
Write-Host "Rappel : « Améliorations audio » doit être ACTIVÉ sur le périphérique (Paramètres > Son), sinon audiodg ne charge aucun APO."
