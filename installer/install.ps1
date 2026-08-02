#Requires -RunAsAdministrator
<#
.SYNOPSIS
    Installe l'APO passthrough Kwiet sur UN endpoint de capture (jalon 1).

.DESCRIPTION
    - Sauvegarde .reg de la clé endpoint et de la clé Audio AVANT toute écriture.
    - Copie la DLL dans "C:\Program Files\Kwiet", enregistrement COM (regsvr32).
    - Écrit PKEY_FX_ModeEffectClsid (ou SFX/EFX selon -Slot) + les modes de
      traitement supportés dans MMDevices\...\FxProperties.
    - Pose DisableProtectedAudioDG=1 (nécessaire pour un APO non signé en dev),
      sauf -SkipUnsignedTweak.
    - Écrit un fichier d'état JSON consommé par uninstall.ps1.

    ⚠ À N'EXÉCUTER QU'EN VM OU SUR MACHINE DÉDIÉE (cf. docs/procedure-test-vm.md).
    Un APO buggé peut priver la machine de tout son.

.EXAMPLE
    .\install.ps1                          # sélection interactive de l'endpoint
    .\install.ps1 -EndpointId '{...}' -Force
#>
[CmdletBinding()]
param(
    # Chemin de la DLL compilée (défaut : build Release du repo).
    [string]$DllPath,
    # GUID de l'endpoint capture, ex. {a1b2c3...}. Interactif si absent.
    [string]$EndpointId,
    # Slot FX. MFX = décision d'architecture (docs/architecture.md).
    [ValidateSet('SFX', 'MFX', 'EFX')]
    [string]$Slot = 'MFX',
    # Ne pas poser DisableProtectedAudioDG=1 (si l'APO est signé).
    [switch]$SkipUnsignedTweak,
    # Saute les confirmations interactives (garde-fou VM inclus).
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib\common.ps1')

Write-Host '=== Kwiet — installation APO (jalon 1 : passthrough) ==='

# --- Garde-fou : VM uniquement ------------------------------------------------
if (-not (Test-KwietIsVirtualMachine)) {
    Write-Warning 'Cette machine ne ressemble PAS à une VM.'
    Write-Warning 'Un APO buggé = plus aucun son sur la machine. La règle projet est : VM ou machine dédiée uniquement.'
    if (-not $Force) {
        $answer = Read-Host 'Tape INSTALLER (en majuscules) pour continuer malgré tout'
        if ($answer -cne 'INSTALLER') { throw 'Installation annulée.' }
    }
}

# --- Résolution DLL -----------------------------------------------------------
if (-not $DllPath) {
    $DllPath = Join-Path $script:InstallerRoot '..\apo\build\Release\KwietApo.dll'
}
$DllPath = (Resolve-Path -Path $DllPath -ErrorAction SilentlyContinue).Path
if (-not $DllPath -or -not (Test-Path $DllPath)) {
    throw "DLL introuvable. Compile d'abord (cmake -S apo -B apo/build -A x64 ; cmake --build apo/build --config Release) ou passe -DllPath."
}

# --- Sélection de l'endpoint --------------------------------------------------
$endpoints = @(Get-KwietCaptureEndpoints)
if (-not $EndpointId) {
    if ($endpoints.Count -eq 0) { throw 'Aucun endpoint de capture trouvé.' }
    Write-Host ''
    Write-Host 'Endpoints de capture détectés :'
    for ($i = 0; $i -lt $endpoints.Count; $i++) {
        Write-Host ("  [{0}] {1}  {2}  ({3})" -f $i, $endpoints[$i].Id, $endpoints[$i].Name, $endpoints[$i].Status)
    }
    Write-Host ''
    Write-Host "Rappel : ne PAS équiper le micro « périphérique de communication par défaut » tant que le passthrough n'est pas validé."
    $choice = Read-Host "Numéro de l'endpoint à équiper"
    $index = 0
    if (-not [int]::TryParse($choice, [ref]$index) -or $index -lt 0 -or $index -ge $endpoints.Count) {
        throw "Choix invalide : $choice"
    }
    $EndpointId = $endpoints[$index].Id
}
$EndpointId = $EndpointId.ToLower()
$endpointObj  = $endpoints | Where-Object { $_.Id -eq $EndpointId } | Select-Object -First 1
$endpointName = if ($endpointObj) { $endpointObj.Name } else { '(nom inconnu)' }

$endpointKey = Join-Path $script:MMCaptureKey $EndpointId
if (-not (Test-Path $endpointKey)) {
    throw "Endpoint introuvable dans le registre : $endpointKey"
}

$slotPid        = $script:SlotPids[$Slot]
$clsidValueName = "$($script:FxClsidFmtid),$slotPid"
$modesValueName = "$($script:FxModesFmtid),$slotPid"
$fxKey          = Get-KwietFxKeyPath $EndpointId

# --- Récapitulatif + confirmation --------------------------------------------
Write-Host ''
Write-Host 'Récapitulatif :'
Write-Host "  Endpoint : $EndpointId"
Write-Host "             $endpointName"
Write-Host "  Slot     : $Slot (valeur `"$clsidValueName`")"
Write-Host "  CLSID    : $($script:KwietClsid)"
Write-Host "  DLL      : $DllPath"
Write-Host "        ->   $($script:KwietDllTarget)"
Write-Host '  Modes    : DEFAULT + COMMUNICATIONS'
Write-Host "  DisableProtectedAudioDG : $(if ($SkipUnsignedTweak) { 'inchangé' } else { '1 (APO non signé)' })"
Write-Host ''
if (-not $Force) {
    $ok = Read-Host 'Continuer ? (o/N)'
    if ($ok -notmatch '^[oO]') { throw 'Installation annulée.' }
}

# --- Sauvegardes AVANT toute écriture -----------------------------------------
New-Item -ItemType Directory -Force -Path $script:BackupDir, $script:StateDir | Out-Null
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

$backupEndpoint = Join-Path $script:BackupDir "endpoint-$stamp.reg"
& reg.exe export "$($script:MMCaptureReg)\$EndpointId" $backupEndpoint /y | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Échec de l'export .reg de l'endpoint (reg.exe code $LASTEXITCODE)." }

$backupAudio = Join-Path $script:BackupDir "audio-policy-$stamp.reg"
& reg.exe export $script:AudioPolicyReg $backupAudio /y | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Échec de l'export .reg de la clé Audio (reg.exe code $LASTEXITCODE)." }
Write-Host "-> Sauvegardes écrites : $backupEndpoint"

# --- Détection de collision (FX driver déjà présent sur ce slot) --------------
$previousFx = @{}
if (Test-Path $fxKey) {
    $existing = Get-ItemProperty -Path $fxKey
    foreach ($name in @($clsidValueName, $modesValueName)) {
        $prop = $existing.PSObject.Properties[$name]
        if ($null -ne $prop) { $previousFx[$name] = $prop.Value }
    }
    if ($previousFx.ContainsKey($clsidValueName) -and $previousFx[$clsidValueName] -ne $script:KwietClsid) {
        $msg = "Un APO $Slot est déjà enregistré sur cet endpoint : $($previousFx[$clsidValueName]). " +
               "La cohabitation (CompositeFX) n'est pas gérée au jalon 1 : utilise une VM/un endpoint vierge, " +
               "ou -Force pour écraser (l'ancienne valeur sera restaurée à la désinstallation)."
        if (-not $Force) { throw $msg }
        Write-Warning $msg
    }
}

# --- Lecture de l'état DisableProtectedAudioDG actuel --------------------------
$prevDpadg = $null
$audioPol = Get-ItemProperty -Path $script:AudioPolicyKey -ErrorAction SilentlyContinue
if ($audioPol -and $audioPol.PSObject.Properties['DisableProtectedAudioDG']) {
    $prevDpadg = $audioPol.DisableProtectedAudioDG
}

# --- Phase de mutation ---------------------------------------------------------
try {
    Stop-KwietAudioStack

    # 1) DLL dans Program Files (lisible par le compte de service d'audiodg).
    New-Item -ItemType Directory -Force -Path $script:KwietInstallDir | Out-Null
    Copy-Item -Path $DllPath -Destination $script:KwietDllTarget -Force
    Write-Host "-> DLL copiée : $($script:KwietDllTarget)"

    # 2) Enregistrement COM via DllRegisterServer (HKLM\Software\Classes\CLSID).
    $p = Start-Process -FilePath regsvr32.exe -ArgumentList '/s', "`"$($script:KwietDllTarget)`"" -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "regsvr32 a échoué (code $($p.ExitCode))." }
    Write-Host "-> Classe COM enregistrée : $($script:KwietClsid)"

    # 3) FxProperties de l'endpoint.
    if (-not (Test-Path $fxKey)) { New-Item -Path $fxKey -Force | Out-Null }
    New-ItemProperty -Path $fxKey -Name $clsidValueName -Value $script:KwietClsid -PropertyType String -Force | Out-Null
    New-ItemProperty -Path $fxKey -Name $modesValueName `
        -Value @($script:ModeDefault, $script:ModeCommunications) -PropertyType MultiString -Force | Out-Null
    Write-Host "-> FxProperties écrites ($Slot, modes DEFAULT + COMMUNICATIONS)"

    # 4) APO non signé : audiodg refuse les APO non signés sans ce réglage.
    if (-not $SkipUnsignedTweak) {
        New-ItemProperty -Path $script:AudioPolicyKey -Name 'DisableProtectedAudioDG' -Value 1 -PropertyType DWord -Force | Out-Null
        Write-Host '-> DisableProtectedAudioDG = 1'
    }

    # 5) Fichier d'état pour uninstall.ps1.
    $state = [ordered]@{
        timestamp                       = $stamp
        endpointId                      = $EndpointId
        endpointName                    = $endpointName
        slot                            = $Slot
        clsid                           = $script:KwietClsid
        clsidValueName                  = $clsidValueName
        modesValueName                  = $modesValueName
        previousFxValues                = $previousFx
        dllPath                         = $script:KwietDllTarget
        backupEndpointReg               = $backupEndpoint
        backupAudioPolicyReg            = $backupAudio
        previousDisableProtectedAudioDG = $prevDpadg
        unsignedTweakApplied            = (-not $SkipUnsignedTweak.IsPresent)
    }
    $statePath = Join-Path $script:StateDir "install-state-$stamp.json"
    $state | ConvertTo-Json -Depth 5 | Set-Content -Path $statePath -Encoding UTF8
    Write-Host "-> État écrit : $statePath"

    Start-KwietAudioStack
} catch {
    Write-Error "ÉCHEC de l'installation : $_" -ErrorAction Continue
    Write-Warning 'Restauration manuelle :'
    Write-Warning "  reg import `"$backupEndpoint`""
    Write-Warning "  reg import `"$backupAudio`""
    Write-Warning '  Start-Service AudioEndpointBuilder ; Start-Service Audiosrv'
    try { Start-KwietAudioStack } catch { Write-Warning "Redémarrage audio impossible : $_" }
    exit 1
}

Write-Host ''
Write-Host '=== Installation terminée ==='
Write-Host 'Vérifications :'
Write-Host '  1. .\status.ps1'
Write-Host "  2. Ouvre l'Enregistreur vocal (ou un test micro) puis : tasklist /m $($script:KwietDllName)"
Write-Host "  3. Windows > Son > le périphérique doit avoir « Améliorations audio » ACTIVÉES, sinon l'APO ne charge pas."
Write-Host "Désinstallation : .\uninstall.ps1 (backup : $backupEndpoint)"
