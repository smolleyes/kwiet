<#
.SYNOPSIS
    Crée la clé de signature auto-signée de Kwiet.

.DESCRIPTION
    À exécuter **une seule fois**. Produit :

      state/kwiet-signing.pfx   clé privée — NE JAMAIS DIFFUSER, jamais commiter
      state/kwiet-signing.cer   certificat public — embarqué dans l'installeur

    Le certificat porte l'EKU *Code Signing* et rien d'autre. Placé dans le
    magasin racine d'une machine, il ne peut donc valider que des chaînes de
    signature de code : il ne peut pas cautionner un certificat TLS, ni servir à
    intercepter du trafic. La portée du risque est bornée à ça.

    Ce qu'il reste comme risque, et il est réel : quiconque obtient la clé
    privée peut signer du code que toutes les machines ayant installé Kwiet
    considéreront comme provenant d'un éditeur de confiance. Le PFX se traite
    comme un secret : `state/` est dans .gitignore, et pour le CI il va dans un
    secret GitHub, jamais dans le dépôt.

    L'alternative sans ce compromis est un certificat de signature de code
    commercial, qui chaîne vers une racine publique déjà présente partout et ne
    demande donc d'ajouter rien du tout chez l'utilisateur.

.EXAMPLE
    .\new-signing-cert.ps1 -Password (Read-Host -AsSecureString)
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [System.Security.SecureString]$Password,

    # Ce que verra l'utilisateur dans son magasin de certificats : autant que ce
    # soit explicite sur la nature de la clé.
    [string]$Subject = 'CN=Kwiet Project, O=Kwiet, OU=Self-signed release key',

    [int]$Years = 10,

    [switch]$Force
)

$ErrorActionPreference = 'Stop'

$stateDir = Join-Path $PSScriptRoot 'state'
New-Item -ItemType Directory -Force -Path $stateDir | Out-Null
$pfxPath = Join-Path $stateDir 'kwiet-signing.pfx'
$cerPath = Join-Path $stateDir 'kwiet-signing.cer'

if ((Test-Path $pfxPath) -and -not $Force) {
    throw "Une clé existe déjà : $pfxPath. Utiliser -Force pour la remplacer — " +
          "ce qui invalidera les paquets déjà signés avec l'ancienne."
}

Write-Host '=== Kwiet — création de la clé de signature ==='

# -Type CodeSigningCert pose l'EKU 1.3.6.1.5.5.7.3.3 (Code Signing) et lui seul.
$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject $Subject `
    -KeyUsage DigitalSignature `
    -KeyLength 3072 `
    -KeyAlgorithm RSA `
    -HashAlgorithm SHA256 `
    -CertStoreLocation 'Cert:\CurrentUser\My' `
    -NotAfter (Get-Date).AddYears($Years)

Export-PfxCertificate -Cert $cert -FilePath $pfxPath -Password $Password | Out-Null
Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null

# La copie du magasin personnel n'a plus lieu d'être : le PFX est la référence,
# et une clé qui traîne dans un magasin est une clé de plus à surveiller.
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -Force -ErrorAction SilentlyContinue

Write-Host "-> Empreinte : $($cert.Thumbprint)"
Write-Host "-> Clé privée : $pfxPath  (secret — ne pas diffuser)"
Write-Host "-> Certificat public : $cerPath"
Write-Host ''
Write-Host 'Pour le CI, poser les secrets du dépôt :'
Write-Host '  gh secret set SIGNING_PFX --body (base64 du .pfx)'
Write-Host '  gh secret set SIGNING_PFX_PASSWORD'
