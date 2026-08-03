<#
.SYNOPSIS
    Lit et pilote le plan de contrôle partagé de l'APO Kwiet.

.DESCRIPTION
    Ouvre le bloc `Global\KwietControlV1` créé par l'APO et affiche son état, ou
    modifie l'activation et l'agressivité. Sert de banc d'essai au plan de
    contrôle avant l'UI Tauri, et d'outil de diagnostic ensuite.

    À lancer SANS élévation : c'est précisément ce qui valide que l'ACL posée
    par l'APO autorise bien un processus utilisateur ordinaire à écrire.

    Le bloc n'existe que pendant un flux de capture actif (l'APO le crée à
    LockForProcess). Ouvre l'Enregistreur vocal si rien ne s'affiche.

.EXAMPLE
    .\kwiet-control.ps1                      # état courant
    .\kwiet-control.ps1 -Watch               # rafraîchi, avec VU-mètres
    .\kwiet-control.ps1 -Aggressiveness 25   # 25 dB de suppression max
    .\kwiet-control.ps1 -Enabled $false      # bypass
#>
[CmdletBinding()]
param(
    [ValidateRange(0, 100)]
    [double]$Aggressiveness,
    [bool]$Enabled,
    [switch]$Watch
)

$ErrorActionPreference = 'Stop'

# Doit rester synchronisé avec apo/src/KwietControl.h.
$NAME    = 'Global\KwietControlV1'
$MAGIC   = 0x5449574B
$SIZE    = 120
$OFF = @{
    magic = 0; version = 4; enabled = 8; aggressiveness = 12; streaming = 16
    generation = 20; sampleRate = 24; channels = 28; latencyFrames = 32
    peakIn = 36; peakOut = 40; underruns = 44; dspErrors = 48; dspActive = 52
}

function Open-Block {
    try {
        return [System.IO.MemoryMappedFiles.MemoryMappedFile]::OpenExisting(
            $NAME, [System.IO.MemoryMappedFiles.MemoryMappedFileRights]::ReadWrite)
    } catch {
        return $null
    }
}

function Format-Db([int]$peak) {
    if ($peak -le 0) { return '   -inf' }
    return '{0,7:N1}' -f (20 * [math]::Log10($peak / 32767.0))
}

function Format-Bar([int]$peak, [int]$width = 28) {
    if ($peak -le 0) { return '.' * $width }
    $db = 20 * [math]::Log10($peak / 32767.0)
    $frac = [math]::Max(0.0, [math]::Min(1.0, ($db + 60.0) / 60.0))
    $n = [int]($frac * $width)
    return ('#' * $n) + ('.' * ($width - $n))
}

$mmf = Open-Block
if ($null -eq $mmf) {
    Write-Host "Bloc de contrôle introuvable ($NAME)."
    Write-Host "L'APO ne le crée que pendant un flux de capture : ouvre l'Enregistreur"
    Write-Host "vocal (ou le test micro des Paramètres) puis relance."
    exit 1
}
$view = $mmf.CreateViewAccessor(0, $SIZE)
try {
    $magic = $view.ReadInt32($OFF.magic)
    if ($magic -ne $MAGIC) {
        throw ("Signature inattendue : 0x{0:X8} (attendu 0x{1:X8})" -f $magic, $MAGIC)
    }

    if ($PSBoundParameters.ContainsKey('Aggressiveness')) {
        $tenths = [int][math]::Round($Aggressiveness * 10)
        $view.Write($OFF.aggressiveness, $tenths)
        Write-Host ("-> agressivité = {0} dB (écriture sans élévation : OK)" -f $Aggressiveness)
    }
    if ($PSBoundParameters.ContainsKey('Enabled')) {
        $view.Write($OFF.enabled, [int]$Enabled)
        Write-Host ("-> effet = {0}" -f $(if ($Enabled) { 'actif' } else { 'bypass' }))
    }

    do {
        $streaming = $view.ReadInt32($OFF.streaming)
        $line = @(
            "flux={0}"        -f $(if ($streaming) { 'actif' } else { 'arrêté' })
            "dsp={0}"         -f $(if ($view.ReadInt32($OFF.dspActive)) { 'on' } else { 'passthrough' })
            "effet={0}"       -f $(if ($view.ReadInt32($OFF.enabled)) { 'on' } else { 'bypass' })
            "agress={0} dB"   -f ($view.ReadInt32($OFF.aggressiveness) / 10.0)
            "{0} Hz/{1}ch"    -f $view.ReadInt32($OFF.sampleRate), $view.ReadInt32($OFF.channels)
            "lat={0} frames"  -f $view.ReadInt32($OFF.latencyFrames)
            "gen={0}"         -f $view.ReadInt32($OFF.generation)
            "under={0} err={1}" -f $view.ReadInt32($OFF.underruns), $view.ReadInt32($OFF.dspErrors)
        ) -join '  '

        if ($Watch) {
            $pIn = $view.ReadInt32($OFF.peakIn)
            $pOut = $view.ReadInt32($OFF.peakOut)
            Clear-Host
            Write-Host $line
            Write-Host ''
            Write-Host ("  entrée  {0} dB  [{1}]" -f (Format-Db $pIn), (Format-Bar $pIn))
            Write-Host ("  sortie  {0} dB  [{1}]" -f (Format-Db $pOut), (Format-Bar $pOut))
            Write-Host ''
            Write-Host '  Ctrl+C pour quitter'
            Start-Sleep -Milliseconds 100
        } else {
            Write-Host $line
            Write-Host ("  VU entrée {0} dB   sortie {1} dB" -f
                (Format-Db $view.ReadInt32($OFF.peakIn)),
                (Format-Db $view.ReadInt32($OFF.peakOut)))
        }
    } while ($Watch)
} finally {
    $view.Dispose()
    $mmf.Dispose()
}
