<#
.SYNOPSIS
    Vérifie si le pack d'effets Kwiet est appliqué aux endpoints de capture.

.DESCRIPTION
    Lecture seule. Fonctionne sans élévation, sauf la vérification du
    chargement dans audiodg (dégrade proprement en indiquant « admin requis »).
    À lancer notamment APRÈS UN REDÉMARRAGE, la construction des endpoints
    étant le moment où AudioEndpointBuilder évalue les packs d'effets.
#>
[CmdletBinding()]
param(
    # Endpoint de capture à solliciter (défaut : périphérique de communication par défaut).
    [string]$EndpointId
)

$ErrorActionPreference = 'Continue'

$KwietClsid   = '{65D564E6-9709-4F5C-85CF-449D92949CFE}'
$VocaClsid    = '{9E6136E0-57AB-4949-B57A-3627BE142855}'
$ApoClassKey  = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{5989fce8-9cd0-467d-8a6a-5419e31529d4}'
$MMCapture    = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture'

function Write-Check([bool]$Ok, [string]$Label, [string]$Detail = '') {
    $mark = if ($Ok) { '[OK]' } else { '[--]' }
    $line = "  $mark $Label"
    if ($Detail) { $line += " : $Detail" }
    Write-Host $line
}

Write-Host '=== Kwiet — état du pack d''effets ==='

# --- 1. Devnode ------------------------------------------------------------
Write-Host ''
Write-Host 'Device (classe AudioProcessingObject) :'
$dev = @(Get-PnpDevice -Class AudioProcessingObject -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -match 'KWIETEFFECTPACK' })
Write-Check ($dev.Count -gt 0) 'devnode Kwiet présent' $(if ($dev) { "$($dev[0].Status)  $($dev[0].InstanceId)" } else { 'absent' })

# --- 2. Registre de classe -------------------------------------------------
Write-Host ''
Write-Host 'Enregistrement de classe :'
$comp = Get-ChildItem $ApoClassKey -ErrorAction SilentlyContinue |
    Where-Object { $_.PSChildName -match '^\d+$' } |
    Where-Object { (Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue).DriverDesc -eq 'Kwiet' } |
    Select-Object -First 1
Write-Check ($null -ne $comp) 'composant Kwiet' $(if ($comp) { $comp.PSChildName } else { 'introuvable' })
if ($comp) {
    foreach ($sub in 'Classes', 'AudioEngine', 'EffectPackRegistration') {
        Write-Check (Test-Path (Join-Path $comp.PSPath $sub)) "sous-clé $sub"
    }
}

# --- 3. Application aux endpoints -----------------------------------------
Write-Host ''
Write-Host 'Endpoints de capture ciblés :'
$applied = @()
foreach ($ep in Get-ChildItem $MMCapture -ErrorAction SilentlyContinue) {
    $fx = Join-Path $ep.PSPath 'FxProperties'
    if (-not (Test-Path $fx)) { continue }
    $props = Get-ItemProperty $fx
    $name = '(nom inconnu)'
    $devProps = Get-ItemProperty (Join-Path $ep.PSPath 'Properties') -ErrorAction SilentlyContinue
    if ($devProps -and $devProps.PSObject.Properties['{b3f8fa53-0004-438e-9003-51a46e139bfc},6']) {
        $name = $devProps.'{b3f8fa53-0004-438e-9003-51a46e139bfc},6'
    }
    foreach ($p in $props.PSObject.Properties) {
        if ($p.Name -match [regex]::Escape($KwietClsid)) {
            $applied += $ep.PSChildName
            Write-Host "  [Kwiet] $($ep.PSChildName)  $name"
            Write-Host "          $($p.Name) = $($p.Value)"
        } elseif ($p.Name -match [regex]::Escape($VocaClsid)) {
            Write-Host "  [Voice Clarity] $($ep.PSChildName)  $name"
        }
    }
}
if ($applied.Count -eq 0) {
    Write-Host '  (aucun endpoint ne référence Kwiet)'
}

# --- 4. Chargement effectif ------------------------------------------------
Write-Host ''
Write-Host 'Chargement dans audiodg :'
Write-Host '  (ouvre un flux micro — Enregistreur vocal ou test micro — puis relance ce script)'
$audiodg = Get-Process -Name audiodg -ErrorAction SilentlyContinue
if ($null -eq $audiodg) {
    Write-Host '  Pas de processus audiodg (aucun flux audio actif).'
} else {
    try {
        $mods = @($audiodg.Modules | ForEach-Object { $_.ModuleName })
        Write-Check ($mods -contains 'KwietApo.dll') 'KwietApo.dll chargée'
        Write-Check ($mods -contains 'voiceclaritycpuapo.dll') 'voiceclaritycpuapo.dll chargée (témoin Voice Clarity)'
    } catch {
        Write-Host '  (lecture des modules impossible — relance en administrateur)'
    }
}

# --- 5. Log dev de l'APO ---------------------------------------------------
Write-Host ''
Write-Host 'Log dev de l''APO (build KWIET_DEV_LOG) :'
if (Test-Path 'C:\ProgramData\Kwiet\apo-log.txt') {
    $l = @(Get-Content 'C:\ProgramData\Kwiet\apo-log.txt')
    Write-Host "  $($l.Count) lignes ; dernières :"
    $l | Select-Object -Last 5 | ForEach-Object { Write-Host "    $_" }
    $lock = @($l | Where-Object { $_ -match 'LockForProcess' }).Count
    Write-Check ($lock -gt 0) 'LockForProcess atteint (APO réellement inséré dans le graphe)'
} else {
    Write-Host '  (aucun log : la DLL n''a jamais été chargée)'
}
