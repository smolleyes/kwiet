<#
.SYNOPSIS
    Assemble (et signe) le pack d'effets Kwiet prêt à être embarqué dans
    l'installeur.

.DESCRIPTION
    Produit `package/` : les deux DLL, les deux INF, le catalogue signé et
    `pack.ps1`. C'est ce dossier que Tauri embarque comme ressource et que
    l'installeur passe à pnputil.

    Signature :
      -CertPath / -CertPassword  → signe DLL et catalogue avec ce PFX.
      sans certificat            → package NON signé. Windows refusera de
                                   l'installer hors mode test-signing. C'est
                                   volontairement bruyant : un pack non signé
                                   qui s'installe en silence n'existe pas.

    La distribution publique demande une signature attestation (Partner
    Center) ; voir docs/architecture.md.

.EXAMPLE
    .\build-package.ps1 -Version 0.2.0
    .\build-package.ps1 -Version 0.2.0 -CertPath kwiet.pfx -CertPassword $env:PFX_PW
#>
[CmdletBinding()]
param(
    # Trois nombres : le quatrième champ est dérivé pour rester monotone.
    [string]$Version,
    [string]$CertPath,
    [string]$CertPassword,

    # Sans horodatage, la signature devient invalide le jour où le certificat
    # expire — y compris pour les paquets déjà installés chez les utilisateurs.
    # Passer une chaîne vide pour construire hors ligne.
    [string]$TimestampUrl = 'http://timestamp.digicert.com',
    [string]$ApoDll,
    [string]$DspDll,
    [string]$OutDir
)

$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $ApoDll) { $ApoDll = Join-Path $repo 'apo\build\Release\KwietApo.dll' }
if (-not $DspDll) { $DspDll = Join-Path $repo 'dsp\target\release\kwiet_dsp.dll' }
if (-not $OutDir) { $OutDir = Join-Path $PSScriptRoot 'package' }

foreach ($binary in $ApoDll, $DspDll) {
    if (-not (Test-Path $binary)) {
        throw "Binaire introuvable : $binary — compile d'abord (voir README)."
    }
}

# Une build KWIET_DEV_LOG écrit un journal depuis audiodg et lit une clé de
# registre qui écrase l'atténuation choisie par l'utilisateur. C'est précieux en
# développement et inacceptable chez quelqu'un d'autre — et ça ne se voit pas :
# le pack s'installe et fonctionne, simplement il n'obéit plus au panneau.
# Le chemin du journal est embarqué dans le binaire, ce qui le trahit.
$devMarker = [Text.Encoding]::ASCII.GetBytes('apo-log.txt')
$apoBytes = [IO.File]::ReadAllBytes($ApoDll)
$isDevBuild = $false
for ($i = 0; $i -le $apoBytes.Length - $devMarker.Length; $i++) {
    if ($apoBytes[$i] -ne $devMarker[0]) { continue }
    $match = $true
    for ($j = 1; $j -lt $devMarker.Length; $j++) {
        if ($apoBytes[$i + $j] -ne $devMarker[$j]) { $match = $false; break }
    }
    if ($match) { $isDevBuild = $true; break }
}
if ($isDevBuild) {
    $message = @'
KwietApo.dll est une build de developpement (KWIET_DEV_LOG).
Elle journalise depuis audiodg et obeit a HKLM\SOFTWARE\Kwiet\AttenuationDbTenths
plutot qu'au panneau. Reconfigure sans l'option :

    cmake -S apo -B apo/build -A x64 -DKWIET_DEV_LOG=OFF
    cmake --build apo/build --config Release --clean-first
'@
    if ($Version) { throw $message }
    Write-Warning $message   # sans -Version, c'est une build locale : on avertit
}

Write-Host '=== Kwiet — assemblage du pack d''effets ==='

# --- Contenu --------------------------------------------------------------
if (Test-Path $OutDir) { Remove-Item $OutDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

Copy-Item $ApoDll (Join-Path $OutDir 'KwietApo.dll') -Force
Copy-Item $DspDll (Join-Path $OutDir 'kwiet_dsp.dll') -Force
foreach ($file in 'kwiet_component.inf', 'kwiet_extension.inf', 'pack.ps1') {
    Copy-Item (Join-Path $PSScriptRoot $file) $OutDir -Force
}

# pnputil compare les packages sur DriverVer, jamais sur le contenu : sans bump,
# une DLL recompilée n'est pas redéployée et l'installeur signale un succès
# pour un paquet qu'il n'a pas posé. La version est donc réécrite dans la COPIE,
# jamais dans les sources du dépôt.
$now = Get-Date
if ($Version) {
    if ($Version -notmatch '^\d+\.\d+\.\d+$') {
        throw "Version attendue sous la forme x.y.z, reçu : $Version"
    }
    # Quatrième champ à zéro : la version publiée EST la version. Réinstaller
    # 0.2.0 par-dessus 0.2.0 ne fait rien, ce qui est correct puisque le contenu
    # est identique ; 0.2.1 passe devant, ce qui est correct aussi. Y glisser une
    # date casserait l'ordre au passage d'une année.
    $driverVersion = "$Version.0"
} else {
    $driverVersion = '0.0.{0}.{1}' -f [int]$now.ToString('MMdd'), [int]$now.ToString('HHmm')
    Write-Host "-> Pas de -Version : version de developpement $driverVersion"
}
$driverVerLine = 'DriverVer   = {0},{1}' -f $now.ToString('MM/dd/yyyy'), $driverVersion
foreach ($inf in 'kwiet_component.inf', 'kwiet_extension.inf') {
    $path = Join-Path $OutDir $inf
    (Get-Content $path) -replace '^\s*DriverVer\s*=.*$', $driverVerLine |
        Set-Content -Path $path -Encoding Ascii
}
Write-Host "-> Contenu prêt : $OutDir (DriverVer $driverVersion)"

# --- Signature ------------------------------------------------------------
if (-not $CertPath) {
    Write-Warning 'Aucun certificat : package NON signe.'
    Write-Warning 'Windows refusera de l''installer hors mode test-signing.'
    Write-Host '=== Pack assemble (non signe) ==='
    return
}
if (-not (Test-Path $CertPath)) { throw "Certificat introuvable : $CertPath" }

$kitBin = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $kitBin) { throw 'Windows SDK introuvable (signtool/makecat).' }
$signtool = Join-Path $kitBin.FullName 'x64\signtool.exe'
$makecat  = Join-Path $kitBin.FullName 'x64\makecat.exe'
foreach ($tool in $signtool, $makecat) {
    if (-not (Test-Path $tool)) { throw "Outil SDK introuvable : $tool" }
}

$signArgs = @('sign', '/fd', 'SHA256', '/f', $CertPath)
if ($CertPassword) { $signArgs += @('/p', $CertPassword) }
if ($TimestampUrl) { $signArgs += @('/tr', $TimestampUrl, '/td', 'SHA256') }

foreach ($binary in 'KwietApo.dll', 'kwiet_dsp.dll') {
    & $signtool @signArgs (Join-Path $OutDir $binary) | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "signtool a echoue sur $binary ($LASTEXITCODE)" }
}
Write-Host '-> DLL signees'

# Le catalogue couvre INF + DLL : c'est lui que Windows vérifie à l'installation.
$cdf = Join-Path $OutDir 'kwiet.cdf'
@"
[CatalogHeader]
Name=kwiet.cat
ResultDir=$OutDir
CatalogVersion=2
HashAlgorithms=SHA256
PageHashes=false
CATATTR1=0x10010001:OSAttr:2:10.0
[CatalogFiles]
<HASH>kwiet_component.inf=$OutDir\kwiet_component.inf
<HASH>kwiet_extension.inf=$OutDir\kwiet_extension.inf
<HASH>KwietApo.dll=$OutDir\KwietApo.dll
<HASH>kwiet_dsp.dll=$OutDir\kwiet_dsp.dll
"@ | Set-Content -Path $cdf -Encoding Ascii

& $makecat -v $cdf | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $OutDir 'kwiet.cat'))) {
    throw "makecat a echoue ($LASTEXITCODE)"
}
Remove-Item $cdf -Force
& $signtool @signArgs (Join-Path $OutDir 'kwiet.cat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "signtool a echoue sur le catalogue ($LASTEXITCODE)" }

Write-Host '-> Catalogue genere et signe'
Write-Host '=== Pack assemble et signe ==='
