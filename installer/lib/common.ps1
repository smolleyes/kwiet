# Constantes et fonctions partagées par install.ps1 / uninstall.ps1 / status.ps1.
# Ce fichier est dot-sourcé par les scripts, ne pas l'exécuter directement.

Set-StrictMode -Version 3.0

# --- Identité Kwiet -----------------------------------------------------------
# ATTENTION : CLSID dupliqué depuis apo/src/KwietGuids.h (CLSID_KwietApo).
# Garder les deux synchronisés.
$script:KwietClsid      = '{65D564E6-9709-4F5C-85CF-449D92949CFE}'
$script:KwietDllName    = 'KwietApo.dll'
$script:KwietInstallDir = Join-Path $env:ProgramFiles 'Kwiet'
$script:KwietDllTarget  = Join-Path $script:KwietInstallDir $script:KwietDllName

# --- Clés registre ------------------------------------------------------------
$script:MMCaptureKey   = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'
$script:MMCaptureReg   = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'   # syntaxe reg.exe
$script:AudioPolicyKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'
$script:AudioPolicyReg = 'HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio'
$script:ClsidRegKey    = "HKLM:\SOFTWARE\Classes\CLSID\$($script:KwietClsid)"

# Valeurs FxProperties : nom = "{fmtid},pid" ; pid : 5 = SFX, 6 = MFX, 7 = EFX.
# PKEY_FX_(Stream|Mode|Endpoint)EffectClsid :
$script:FxClsidFmtid = '{D04E05A6-594B-4FB6-A80D-01AF5EED7D1D}'
# PKEY_(SFX|MFX|EFX)_ProcessingModes_Supported_For_Streaming (même pid) :
$script:FxModesFmtid = '{D3993A3F-99C2-4402-B5EC-A92A0367664B}'
$script:SlotPids     = @{ SFX = 5; MFX = 6; EFX = 7 }

# Modes de traitement déclarés (justification : docs/architecture.md).
$script:ModeDefault        = '{C18E2F7E-933D-4965-B7D1-1EEF228D2AF3}'   # AUDIO_SIGNALPROCESSINGMODE_DEFAULT
$script:ModeCommunications = '{98951333-B9CD-48B1-A0A3-FF40682D73F7}'   # AUDIO_SIGNALPROCESSINGMODE_COMMUNICATIONS

# --- Dossiers locaux (ignorés par git) ----------------------------------------
$script:InstallerRoot = Split-Path -Parent $PSScriptRoot    # lib/ -> installer/
$script:BackupDir     = Join-Path $script:InstallerRoot 'backups'
$script:StateDir      = Join-Path $script:InstallerRoot 'state'

# ------------------------------------------------------------------------------

function Test-KwietIsVirtualMachine {
    # Heuristique best-effort : Hyper-V, VMware, VirtualBox, QEMU/KVM, Parallels.
    $cs = Get-CimInstance -ClassName Win32_ComputerSystem
    return ($cs.Model -match 'Virtual Machine|VMware|VirtualBox|QEMU') -or
           ($cs.Manufacturer -match 'VMware|innotek|QEMU|Parallels|Xen')
}

function Get-KwietCaptureEndpoints {
    # Endpoints de capture vus par MMDevice.
    # InstanceId PnP : SWD\MMDEVAPI\{0.0.1.00000000}.{guid} (0.0.1 = capture).
    $result = @()
    $devices = @()
    try {
        $devices = @(Get-PnpDevice -Class AudioEndpoint -ErrorAction Stop)
    } catch {
        $devices = @()
    }
    foreach ($d in $devices) {
        if ($d.InstanceId -match '\{0\.0\.1\.00000000\}\.(\{[0-9a-fA-F-]{36}\})$') {
            $result += [pscustomobject]@{
                Id     = $Matches[1].ToLower()
                Name   = $d.FriendlyName
                Status = $d.Status
            }
        }
    }
    # Repli : énumération directe du registre si l'énumération PnP n'a rien donné.
    if ($result.Count -eq 0 -and (Test-Path $script:MMCaptureKey)) {
        foreach ($k in Get-ChildItem $script:MMCaptureKey) {
            $result += [pscustomobject]@{
                Id     = $k.PSChildName
                Name   = '(nom indisponible)'
                Status = '?'
            }
        }
    }
    return $result
}

function Get-KwietFxKeyPath([string]$EndpointId) {
    return Join-Path (Join-Path $script:MMCaptureKey $EndpointId) 'FxProperties'
}

function Get-KwietInstalledTargets {
    # Retourne chaque (endpoint, slot) dont la valeur FX Clsid référence Kwiet.
    $targets = @()
    if (-not (Test-Path $script:MMCaptureKey)) { return $targets }
    foreach ($ep in Get-ChildItem $script:MMCaptureKey) {
        $fxPath = Join-Path $ep.PSPath 'FxProperties'
        if (-not (Test-Path $fxPath)) { continue }
        $props = Get-ItemProperty -Path $fxPath
        foreach ($slot in @($script:SlotPids.Keys)) {
            $name = "$($script:FxClsidFmtid),$($script:SlotPids[$slot])"
            $p = $props.PSObject.Properties[$name]
            if ($null -ne $p -and $p.Value -eq $script:KwietClsid) {
                $targets += [pscustomobject]@{
                    EndpointId = $ep.PSChildName
                    Slot       = $slot
                    FxPath     = $fxPath
                }
            }
        }
    }
    return $targets
}

function Stop-KwietAudioStack {
    # Audiosrv dépend d'AudioEndpointBuilder : on arrête dans cet ordre.
    Write-Host '-> Arrêt de la pile audio (Audiosrv, AudioEndpointBuilder)...'
    foreach ($svc in 'Audiosrv', 'AudioEndpointBuilder') {
        $s = Get-Service -Name $svc -ErrorAction SilentlyContinue
        if ($null -ne $s -and $s.Status -ne 'Stopped') {
            Stop-Service -Name $svc -Force -ErrorAction Stop
        }
    }
}

function Start-KwietAudioStack {
    Write-Host '-> Démarrage de la pile audio...'
    Start-Service -Name AudioEndpointBuilder
    Start-Service -Name Audiosrv
}

function Get-KwietLatestStateFile {
    if (-not (Test-Path $script:StateDir)) { return $null }
    return Get-ChildItem $script:StateDir -Filter 'install-state-*.json' |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
}
