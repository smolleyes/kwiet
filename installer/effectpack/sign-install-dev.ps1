#Requires -RunAsAdministrator
<#
.SYNOPSIS
    DEV UNIQUEMENT — signe et installe le pack d'effets Kwiet (INF composant +
    extension) avec un certificat de test auto-signé.

.DESCRIPTION
    - Prépare package/ : KwietApo.dll (build Release) + les deux INF.
    - Certificat code-signing auto-signé "CN=Kwiet Dev Test" ajouté aux
      magasins MACHINE Root et TrustedPublisher (⚠ dev seulement — à retirer
      via uninstall-effectpack.ps1 -RemoveCert).
    - Signe la DLL (PETrust) puis génère le catalogue avec makecat (SDK) et le
      signe.
    - pnputil /add-driver /install pour les deux INF, puis redémarre la pile
      audio.

    La distribution réelle passera par une signature attestation (Partner
    Center) — ce script est le pendant local pour le développement.
#>
[CmdletBinding()]
param(
    [string]$DllPath,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$root       = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)   # repo
$packageDir = Join-Path $PSScriptRoot 'package'
$stateDir   = Join-Path $PSScriptRoot 'state'
$certSubject = 'CN=Kwiet Dev Test'
$pfxPassword = 'kwiet-dev'

Write-Host '=== Kwiet — installation du pack d''effets (DEV, certificat de test) ==='

if (-not $DllPath) { $DllPath = Join-Path $root 'apo\build\Release\KwietApo.dll' }
if (-not (Test-Path $DllPath)) {
    throw "DLL introuvable : $DllPath — compile d'abord (cmake --build apo/build --config Release)."
}

# --- Outils SDK -----------------------------------------------------------
$kitBin = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory |
    Where-Object { $_.Name -match '^\d+(\.\d+)+$' } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
$signtool = Join-Path $kitBin.FullName 'x64\signtool.exe'
$makecat  = Join-Path $kitBin.FullName 'x64\makecat.exe'
foreach ($t in $signtool, $makecat) {
    if (-not (Test-Path $t)) { throw "Outil SDK introuvable : $t" }
}

# --- Package --------------------------------------------------------------
New-Item -ItemType Directory -Force -Path $packageDir, $stateDir | Out-Null
Copy-Item $DllPath (Join-Path $packageDir 'KwietApo.dll') -Force
Copy-Item (Join-Path $PSScriptRoot 'kwiet_component.inf') $packageDir -Force
Copy-Item (Join-Path $PSScriptRoot 'kwiet_extension.inf') $packageDir -Force
Remove-Item (Join-Path $packageDir 'kwiet.cat') -Force -ErrorAction SilentlyContinue
Write-Host "-> Package prêt : $packageDir"

# --- Certificat de test ---------------------------------------------------
$cert = Get-ChildItem 'Cert:\CurrentUser\My' |
    Where-Object { $_.Subject -eq $certSubject } |
    Sort-Object NotAfter -Descending | Select-Object -First 1
if (-not $cert -or $cert.NotAfter -lt (Get-Date).AddYears(5)) {
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $certSubject `
        -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddYears(10)
    Write-Host "-> Certificat de test créé : $($cert.Thumbprint)"
} else {
    Write-Host "-> Certificat de test réutilisé : $($cert.Thumbprint)"
}
$pfxPath = Join-Path $stateDir 'kwiet-dev-test.pfx'
$secure  = ConvertTo-SecureString -String $pfxPassword -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $secure | Out-Null

foreach ($store in 'Root', 'TrustedPublisher') {
    $s = New-Object System.Security.Cryptography.X509Certificates.X509Store($store, 'LocalMachine')
    $s.Open('ReadWrite'); $s.Add($cert); $s.Close()
}
Write-Host '-> Certificat approuvé (machine : Root + TrustedPublisher) — DEV UNIQUEMENT'

# --- Signature DLL + catalogue -------------------------------------------
& $signtool sign /fd SHA256 /f $pfxPath /p $pfxPassword (Join-Path $packageDir 'KwietApo.dll') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "signtool (DLL) a échoué ($LASTEXITCODE)." }
Write-Host '-> DLL signée (PETrust)'

$cdfPath = Join-Path $stateDir 'kwiet.cdf'
@"
[CatalogHeader]
Name=kwiet.cat
ResultDir=$packageDir
CatalogVersion=2
HashAlgorithms=SHA256
PageHashes=false
CATATTR1=0x10010001:OSAttr:2:10.0
[CatalogFiles]
<HASH>kwiet_component.inf=$packageDir\kwiet_component.inf
<HASH>kwiet_extension.inf=$packageDir\kwiet_extension.inf
<HASH>KwietApo.dll=$packageDir\KwietApo.dll
"@ | Set-Content -Path $cdfPath -Encoding Ascii

& $makecat -v $cdfPath | Out-Null
if ($LASTEXITCODE -ne 0 -or -not (Test-Path (Join-Path $packageDir 'kwiet.cat'))) {
    throw "makecat a échoué ($LASTEXITCODE)."
}
& $signtool sign /fd SHA256 /f $pfxPath /p $pfxPassword (Join-Path $packageDir 'kwiet.cat') | Out-Null
if ($LASTEXITCODE -ne 0) { throw "signtool (catalogue) a échoué ($LASTEXITCODE)." }
Write-Host '-> Catalogue généré et signé'

# --- Installation ---------------------------------------------------------
$published = @()
foreach ($inf in 'kwiet_component.inf', 'kwiet_extension.inf') {
    Write-Host "-> pnputil /add-driver $inf /install"
    $out = & pnputil.exe /add-driver (Join-Path $packageDir $inf) /install 2>&1 | Out-String
    Write-Host ($out -replace '(?m)^', '   ')
    if ($LASTEXITCODE -ne 0) { throw "pnputil a échoué pour $inf (code $LASTEXITCODE)." }
    if ($out -match '(oem\d+\.inf)') { $published += $Matches[1] }
}
Set-Content -Path (Join-Path $stateDir 'published-drivers.txt') -Value $published -Encoding Ascii
Write-Host "-> Drivers publiés : $($published -join ', ')"

Write-Host '-> Redémarrage de la pile audio...'
foreach ($svc in 'Audiosrv', 'AudioEndpointBuilder') {
    $s = Get-Service $svc -ErrorAction SilentlyContinue
    if ($s -and $s.Status -ne 'Stopped') { Stop-Service $svc -Force }
}
Start-Service AudioEndpointBuilder
Start-Service Audiosrv

Write-Host ''
Write-Host '=== Pack installé ==='
Write-Host 'Vérifie : Get-PnpDevice -Class AudioProcessingObject  (device "Kwiet" attendu)'
Write-Host 'Désinstallation : .\uninstall-effectpack.ps1'
