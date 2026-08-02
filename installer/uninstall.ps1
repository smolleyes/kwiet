#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Désinstalle proprement l'APO Kwiet (jalon 1).

.DESCRIPTION
    Par défaut, lit le dernier fichier d'état écrit par install.ps1
    (installer/state/install-state-*.json) et défait exactement ce qui a été
    fait : valeurs FxProperties, restauration des valeurs préexistantes,
    désenregistrement COM, suppression de la DLL, restauration de
    DisableProtectedAudioDG.

    Sans fichier d'état, scanne tous les endpoints de capture à la recherche du
    CLSID Kwiet et nettoie ce qu'il trouve. Sur une machine vierge, le script
    est un no-op et le dit : c'est le test à exécuter AVANT la première
    installation (cf. docs/procedure-test-vm.md).

.EXAMPLE
    .\uninstall.ps1                 # utilise le dernier état, sinon scan
    .\uninstall.ps1 -ImportBackup   # réimporte aussi le .reg d'origine
#>
[CmdletBinding()]
param(
    # Fichier d'état précis à utiliser (défaut : le plus récent).
    [string]$StatePath,
    # Réimporte le backup .reg de l'endpoint après nettoyage (restauration
    # maximale des valeurs écrasées).
    [switch]$ImportBackup,
    # Saute la confirmation.
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\common.ps1')

Write-Host '=== Kwiet — désinstallation APO ==='

# --- Chargement de l'état ------------------------------------------------------
$state = $null
$stateFile = $null
if ($StatePath) {
    $stateFile = Get-Item -Path $StatePath
} else {
    $stateFile = Get-KwietLatestStateFile
}
if ($null -ne $stateFile) {
    $state = Get-Content -Path $stateFile.FullName -Raw | ConvertFrom-Json
    Write-Host "-> État chargé : $($stateFile.Name) (endpoint $($state.endpointId), slot $($state.slot))"
} else {
    Write-Host "-> Aucun fichier d'état : scan des endpoints de capture."
}

# --- Détermination des cibles --------------------------------------------------
$targets = @(Get-KwietInstalledTargets)
if ($null -ne $state) {
    # L'état prime, mais on garde aussi ce que le scan a trouvé (install partielle).
    $fromState = [pscustomobject]@{
        EndpointId = $state.endpointId
        Slot       = $state.slot
        FxPath     = Get-KwietFxKeyPath $state.endpointId
    }
    if (-not ($targets | Where-Object { $_.EndpointId -eq $fromState.EndpointId -and $_.Slot -eq $fromState.Slot })) {
        if (Test-Path $fromState.FxPath) { $targets += $fromState }
    }
}

$comRegistered = Test-Path $script:ClsidRegKey
$dllPresent    = Test-Path $script:KwietDllTarget

if ($targets.Count -eq 0 -and -not $comRegistered -and -not $dllPresent) {
    Write-Host ''
    Write-Host 'Rien à désinstaller : aucun endpoint ne référence le CLSID Kwiet,'
    Write-Host 'pas de classe COM enregistrée, pas de DLL en place. (No-op propre.)'
    exit 0
}

Write-Host ''
Write-Host 'Actions prévues :'
foreach ($t in $targets) {
    Write-Host "  - Retirer les valeurs FX $($t.Slot) de l'endpoint $($t.EndpointId)"
}
if ($comRegistered) { Write-Host "  - Désenregistrer la classe COM $($script:KwietClsid)" }
if ($dllPresent)    { Write-Host "  - Supprimer $($script:KwietDllTarget)" }
if ($null -ne $state -and $state.unsignedTweakApplied) {
    Write-Host '  - Restaurer DisableProtectedAudioDG à son état antérieur'
}
if ($ImportBackup -and $null -ne $state) {
    Write-Host "  - Réimporter $($state.backupEndpointReg)"
}
Write-Host ''
if (-not $Force) {
    $ok = Read-Host 'Continuer ? (o/N)'
    if ($ok -notmatch '^[oO]') { throw 'Désinstallation annulée.' }
}

# --- Phase de mutation ---------------------------------------------------------
$hadError = $false
try {
    Stop-KwietAudioStack

    # 1) Valeurs FxProperties.
    foreach ($t in $targets) {
        $slotPidLocal = $script:SlotPids[$t.Slot]
        $clsidName = "$($script:FxClsidFmtid),$slotPidLocal"
        $modesName = "$($script:FxModesFmtid),$slotPidLocal"
        foreach ($name in @($clsidName, $modesName)) {
            Remove-ItemProperty -Path $t.FxPath -Name $name -ErrorAction SilentlyContinue
        }
        Write-Host "-> FX $($t.Slot) retirées de $($t.EndpointId)"
    }

    # 2) Restauration des valeurs préexistantes écrasées par -Force à l'install.
    if ($null -ne $state -and $null -ne $state.previousFxValues) {
        $fxPath = Get-KwietFxKeyPath $state.endpointId
        foreach ($prop in $state.previousFxValues.PSObject.Properties) {
            if ($null -eq $prop.Value) { continue }
            if ($prop.Value -is [array]) {
                New-ItemProperty -Path $fxPath -Name $prop.Name -Value ([string[]]$prop.Value) -PropertyType MultiString -Force | Out-Null
            } else {
                New-ItemProperty -Path $fxPath -Name $prop.Name -Value ([string]$prop.Value) -PropertyType String -Force | Out-Null
            }
            Write-Host "-> Valeur préexistante restaurée : $($prop.Name)"
        }
    }

    # 3) Réimport complet du backup si demandé.
    if ($ImportBackup -and $null -ne $state -and (Test-Path $state.backupEndpointReg)) {
        & reg.exe import $state.backupEndpointReg 2>&1 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "reg import a échoué (code $LASTEXITCODE)." }
        Write-Host "-> Backup réimporté : $($state.backupEndpointReg)"
    }

    # 4) Désenregistrement COM.
    if (Test-Path $script:KwietDllTarget) {
        $p = Start-Process -FilePath regsvr32.exe -ArgumentList '/u', '/s', "`"$($script:KwietDllTarget)`"" -Wait -PassThru
        if ($p.ExitCode -ne 0) { Write-Warning "regsvr32 /u a renvoyé $($p.ExitCode)." }
    }
    if (Test-Path $script:ClsidRegKey) {
        Remove-Item -Path $script:ClsidRegKey -Recurse -Force
    }
    Write-Host '-> Classe COM désenregistrée'

    # 5) Suppression de la DLL (audiodg peut la garder quelques instants).
    if (Test-Path $script:KwietDllTarget) {
        $deleted = $false
        for ($i = 0; $i -lt 5 -and -not $deleted; $i++) {
            try {
                Remove-Item -Path $script:KwietDllTarget -Force -ErrorAction Stop
                $deleted = $true
            } catch {
                Start-Sleep -Milliseconds 500
            }
        }
        if (-not $deleted) {
            Write-Warning "DLL encore verrouillée : $($script:KwietDllTarget). Supprime-la après reboot."
            $hadError = $true
        }
    }
    if ((Test-Path $script:KwietInstallDir) -and -not (Get-ChildItem $script:KwietInstallDir)) {
        Remove-Item -Path $script:KwietInstallDir -Force
    }

    # 6) DisableProtectedAudioDG : uniquement si c'est NOUS qui l'avons posé.
    if ($null -ne $state -and $state.unsignedTweakApplied) {
        if ($null -eq $state.previousDisableProtectedAudioDG) {
            Remove-ItemProperty -Path $script:AudioPolicyKey -Name 'DisableProtectedAudioDG' -ErrorAction SilentlyContinue
            Write-Host '-> DisableProtectedAudioDG supprimé (absent avant install)'
        } else {
            New-ItemProperty -Path $script:AudioPolicyKey -Name 'DisableProtectedAudioDG' `
                -Value ([int]$state.previousDisableProtectedAudioDG) -PropertyType DWord -Force | Out-Null
            Write-Host "-> DisableProtectedAudioDG restauré à $($state.previousDisableProtectedAudioDG)"
        }
    } elseif ($null -eq $state) {
        Write-Warning "Pas de fichier d'état : DisableProtectedAudioDG laissé tel quel (d'autres outils type Equalizer APO l'utilisent aussi)."
    }

    Start-KwietAudioStack
} catch {
    Write-Error "ÉCHEC de la désinstallation : $_" -ErrorAction Continue
    if ($null -ne $state) {
        Write-Warning "Restauration manuelle : reg import `"$($state.backupEndpointReg)`""
    }
    try { Start-KwietAudioStack } catch { Write-Warning "Redémarrage audio impossible : $_" }
    exit 1
}

# --- Marquer l'état comme consommé --------------------------------------------
if ($null -ne $stateFile -and (Test-Path $stateFile.FullName)) {
    $doneName = "$($stateFile.Name).uninstalled-$(Get-Date -Format 'yyyyMMdd-HHmmss')"
    Rename-Item -Path $stateFile.FullName -NewName $doneName
}

Write-Host ''
if ($hadError) {
    Write-Host '=== Désinstallation terminée AVEC AVERTISSEMENTS (voir plus haut) ==='
    exit 2
}
Write-Host '=== Désinstallation terminée ==='
Write-Host 'Vérifications conseillées :'
Write-Host '  1. .\status.ps1  (tout doit être absent)'
Write-Host "  2. reg export de l'endpoint et diff avec le backup d'install (cf. docs/procedure-test-vm.md)"
Write-Host '  3. Teste le micro : le son doit fonctionner normalement.'
