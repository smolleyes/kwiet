<#
.SYNOPSIS
    Installe ou retire le pack d'effets Kwiet du magasin de pilotes Windows.

.DESCRIPTION
    Appelé par l'installeur NSIS, en élévation. Utilisable à la main pour
    réparer une installation :

        .\pack.ps1 -Action install -PackageDir "C:\Program Files\Kwiet\effectpack"
        .\pack.ps1 -Action uninstall

    Le pack est un package pilote : Windows le copie dans le DriverStore et
    l'expose comme composant sélectionnable. Il reste ensuite à l'utilisateur de
    le CHOISIR dans Paramètres > Son > son micro > Améliorations audio ; ce
    script ne peut pas le faire à sa place, et le panneau Kwiet le lui dira.

.NOTES
    Journal : %ProgramData%\Kwiet\pack.log
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('install', 'uninstall')]
    [string]$Action,

    [string]$PackageDir = $PSScriptRoot,

    # L'installeur ne veut pas d'un échec bloquant : le pack manquant se répare,
    # une désinstallation à moitié faite non.
    [switch]$IgnoreFailure
)

$ErrorActionPreference = 'Stop'
$InformationPreference = 'Continue'

$INF_NAMES = @('kwiet_component.inf', 'kwiet_extension.inf')
$stateDir  = Join-Path $env:ProgramData 'Kwiet'
$logPath   = Join-Path $stateDir 'pack.log'

# pnputil : 0 = OK, 259 (ERROR_NO_MORE_ITEMS) = déjà à jour, 3010 = OK mais
# redémarrage conseillé. Aucun n'est un échec.
$PNPUTIL_OK = @(0, 259, 3010)

# Un processus 32 bits qui ouvre System32 est redirigé par WOW64 vers SysWOW64,
# où pnputil.exe n'existe pas — l'appel échoue alors sur un « commande
# introuvable » qui ne dit rien de la vraie cause. Sysnative est la porte de
# sortie, et elle n'existe QUE depuis un processus 32 bits, d'où le test.
# L'installeur NSIS est 32 bits : sans ceci, rien ne s'enregistre.
$PNPUTIL = if ([Environment]::Is64BitProcess) {
    Join-Path $env:WINDIR 'System32\pnputil.exe'
} else {
    Join-Path $env:WINDIR 'Sysnative\pnputil.exe'
}

function Write-Step($message) {
    $stamp = (Get-Date).ToString('HH:mm:ss')
    Write-Information "[$stamp] $message"
    Add-Content -Path $logPath -Value "[$((Get-Date).ToString('s'))] $message" -Encoding utf8
}

<#
    Retrouve les noms publiés (oemNN.inf) de nos packages.

    On lit la sortie de pnputil plutôt qu'un fichier d'état : un fichier peut
    manquer, mentir, ou dater d'une installation précédente, alors que le
    magasin de pilotes est la vérité. Le découpage se fait par blocs séparés
    d'une ligne vide et on ne cherche que des noms de fichiers — c'est
    indépendant de la langue de Windows, contrairement aux libellés.
#>
function Get-KwietPublishedDriver {
    $output = & $PNPUTIL /enum-drivers 2>&1 | Out-String
    foreach ($block in $output -split '(\r?\n){2,}') {
        $isOurs = $INF_NAMES | Where-Object { $block -match [regex]::Escape($_) }
        if (-not $isOurs) { continue }
        if ($block -match '(oem\d+\.inf)') { $Matches[1] }
    }
}

function Restart-AudioStack {
    # Le pack apparaît comme une arrivée de périphérique, mais la pile audio ne
    # relit ses effets qu'au redémarrage du service. Coupure de son de l'ordre
    # de deux secondes.
    Write-Step 'Redemarrage de la pile audio'
    foreach ($name in 'Audiosrv', 'AudioEndpointBuilder') {
        $service = Get-Service $name -ErrorAction SilentlyContinue
        if ($service -and $service.Status -ne 'Stopped') {
            Stop-Service $name -Force -ErrorAction SilentlyContinue
        }
    }
    Start-Service AudioEndpointBuilder -ErrorAction SilentlyContinue
    Start-Service Audiosrv -ErrorAction SilentlyContinue
}

function Install-Pack {
    foreach ($inf in $INF_NAMES) {
        $path = Join-Path $PackageDir $inf
        if (-not (Test-Path $path)) { throw "INF introuvable : $path" }
    }
    # Le composant d'abord : l'extension crée le périphérique qui le réclame.
    foreach ($inf in $INF_NAMES) {
        $path = Join-Path $PackageDir $inf
        Write-Step "pnputil /add-driver $inf /install"
        $output = & $PNPUTIL /add-driver $path /install 2>&1 | Out-String
        Add-Content -Path $logPath -Value $output -Encoding utf8
        if ($PNPUTIL_OK -notcontains $LASTEXITCODE) {
            throw "pnputil a echoue pour $inf (code $LASTEXITCODE)"
        }
    }
    Restart-AudioStack
    $published = @(Get-KwietPublishedDriver)
    Write-Step "Pack installe. Packages publies : $($published -join ', ')"
}

function Uninstall-Pack {
    $published = @(Get-KwietPublishedDriver)
    if ($published.Count -eq 0) {
        Write-Step 'Aucun package Kwiet dans le magasin de pilotes, rien a retirer'
        return
    }
    foreach ($name in $published) {
        Write-Step "pnputil /delete-driver $name /uninstall /force"
        $output = & $PNPUTIL /delete-driver $name /uninstall /force 2>&1 | Out-String
        Add-Content -Path $logPath -Value $output -Encoding utf8
        if ($PNPUTIL_OK -notcontains $LASTEXITCODE) {
            Write-Step "  echec (code $LASTEXITCODE), on continue"
        }
    }
    Restart-AudioStack
    $left = @(Get-KwietPublishedDriver)
    if ($left.Count -gt 0) {
        Write-Step "Restent apres retrait : $($left -join ', ')"
    } else {
        Write-Step 'Pack retire'
    }
}

New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
Write-Step "=== $Action (PackageDir=$PackageDir) ==="

try {
    if ($Action -eq 'install') { Install-Pack } else { Uninstall-Pack }
} catch {
    Write-Step "ECHEC : $_"
    if (-not $IgnoreFailure) { exit 1 }
}
exit 0
