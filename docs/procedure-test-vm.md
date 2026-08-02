# Procédure de test en VM — jalon 1

**Règle absolue : l'APO ne s'installe JAMAIS sur le poste de travail.**
Un APO buggé peut crasher `audiodg.exe` en boucle = plus aucun son sur la
machine, y compris pendant la session de debug. Tout se fait en VM Windows 11
(ou machine dédiée sacrifiable).

## 1. Préparation de la VM

1. VM Windows 11 x64 (Hyper-V « Création rapide », VMware ou VirtualBox), avec
   audio virtuel activé :
   - Hyper-V : session étendue (`vmconnect` avec redirection audio) ;
   - VMware : `sound.present = TRUE` ; VirtualBox : contrôleur audio + micro hôte.
   Il faut **au moins un endpoint de capture** visible dans Paramètres > Son.
2. Installer les outils de build (VS Build Tools C++ + CMake) **ou** compiler
   sur l'hôte et copier `KwietApo.dll` + le dossier `installer/` dans la VM.
3. Créer un **checkpoint/snapshot « clean »** de la VM.
4. Dans la VM, créer un point de restauration Windows, puis exporter
   manuellement les clés sensibles (ceinture + bretelles, l'installeur refait
   ses propres backups) :

   ```powershell
   reg export "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture" C:\backup-capture.reg /y
   reg export "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Audio" C:\backup-audio.reg /y
   ```

## 2. Ordre des opérations — première fois

> **Le désinstalleur se teste AVANT la première installation.**

```powershell
cd installer

# (a) Sur VM vierge : doit afficher « Rien à désinstaller » et sortir en code 0.
.\uninstall.ps1

# (b) Installation (choisir un endpoint qui N'EST PAS le périphérique de
#     communication par défaut).
.\install.ps1

# (c) Vérification.
.\status.ps1

# (d) Test fonctionnel (voir §3).

# (e) Désinstallation immédiate + vérification du retour à l'état initial.
.\uninstall.ps1
reg export "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Capture" C:\after-uninstall.reg /y
fc C:\backup-capture.reg C:\after-uninstall.reg
```

Seulement quand ce cycle install → uninstall → diff propre passe, commencer
les vraies sessions de test.

## 3. Vérifier que l'APO charge

1. `Paramètres > Son > [périphérique] > Propriétés` : **« Améliorations
   audio » doit être activé** — sinon audiodg ne charge aucun APO tiers.
2. Ouvrir l'Enregistreur vocal (ou le test micro des Paramètres) pour créer un
   flux de capture, puis :

   ```powershell
   tasklist /m KwietApo.dll     # doit lister audiodg.exe
   .\status.ps1                 # section audiodg : [OK]
   ```

3. Le son enregistré doit être identique à l'entrée (passthrough bit-exact,
   retard nul).

En cas d'échec de chargement, vérifier dans l'ordre : améliorations audio
activées → `DisableProtectedAudioDG = 1` → CLSID présent sous
`HKLM\Software\Classes\CLSID` → valeurs FxProperties (`status.ps1`) → Observateur
d'événements (journal Application, sources *Audio* / *AudioEndpointBuilder* et
crashs `audiodg.exe`).

## 4. Batterie jalon 1 (stabilité 48 h)

L'APO doit survivre à tout ce qui suit, **sans crash d'audiodg ni perte
audio** (un passage en passthrough est OK, un silence définitif non) :

| Test | Procédure |
|---|---|
| Changement de sample rate | Propriétés du périphérique > Avancé : alterner 44,1 kHz / 48 kHz pendant un enregistrement |
| Débranchement à chaud | Retirer/rebrancher le micro USB passthrough, ou désactiver/réactiver le périphérique dans le Gestionnaire de périphériques pendant un flux actif |
| Veille/reprise | Cycle veille → reprise avec flux actif (selon hyperviseur : save/restore de la VM) |
| Multi-applis | Chrome (meet.google.com, test micro) + Discord simultanés ≥ 1 h |
| Modes | Un flux « communications » (Discord) + un flux « default » (Enregistreur vocal) en parallèle |
| Soak | 48 h avec flux de capture actif ; surveiller crashs audiodg (Observateur d'événements) et fuite mémoire (`Get-Process audiodg` périodique) |

Journal de bord des runs dans `bench/` (jalon 3 formalisera l'outillage).

## 5. Récupération d'urgence

Si l'audio de la VM est mort après une install :

1. `installer\uninstall.ps1` (ou `-ImportBackup`) puis redémarrer la pile :
   `Restart-Service AudioEndpointBuilder -Force ; Start-Service Audiosrv`.
2. Si audiodg crashe en boucle et empêche tout : renommer
   `C:\Program Files\Kwiet\KwietApo.dll` (au besoin en mode sans échec), rebooter,
   puis `reg import` des backups de `installer\backups\`.
3. Dernier recours : point de restauration Windows, ou restauration du
   checkpoint de la VM.

Nota : après plusieurs crashs d'audiodg, Windows peut désactiver de lui-même
les améliorations audio de l'endpoint — les réactiver après correction.

## 6. Rappels

- Ne jamais équiper l'endpoint « périphérique de communication par défaut »
  tant que le passthrough n'est pas validé 48 h.
- Shared mode uniquement : les flux exclusifs/RAW bypassent l'APO (accepté).
- Recréer un checkpoint « clean » après chaque évolution validée de la
  procédure d'install.
