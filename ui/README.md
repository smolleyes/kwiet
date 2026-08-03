# ui/ — panneau Kwiet (Tauri v2)

Application de barre d'état : état du micro, intensité du nettoyage, VU-mètres.
Elle ne parle **jamais** au thread temps-réel — uniquement au bloc de mémoire
partagée décrit dans [`../apo/src/KwietControl.h`](../apo/src/KwietControl.h),
qui est le contrat.

```powershell
npm install
npm run dev      # développement, rechargement du frontend
npm run build    # installeur NSIS dans src-tauri/target/release/bundle
```

## Ce que fait le backend

| Fichier | Rôle |
|---|---|
| `src-tauri/src/control.rs` | ouvre `Global\KwietControlV1`, convertit les pics en dB, borne l'intensité |
| `src-tauri/src/settings.rs` | persiste les préférences dans le dossier de config de l'app |
| `src-tauri/src/main.rs` | icône de barre d'état, fenêtre, commandes, et le fil qui surveille `generation` |

Deux contraintes dictent la conception :

- **Le bloc n'existe que pendant un flux de capture.** L'APO le crée à
  `LockForProcess`. L'UI doit donc afficher un état d'attente quand il est
  absent, et retenter l'ouverture à chaque sondage.
- **Un nouveau flux repart des valeurs par défaut de l'APO.** Un fil de fond
  surveille le compteur `generation` et repousse les préférences dès qu'un flux
  apparaît, même panneau fermé. Tant que l'UI tourne, elle garde aussi la
  section ouverte, ce qui suffit à préserver les réglages entre deux flux.

L'écriture ne demande **aucune élévation** : c'est l'ACL posée par l'APO qui
l'autorise (cf. `docs/architecture.md` §6).

## Le parti pris visuel

Le sujet du produit, c'est l'écart entre deux signaux. Le panneau en fait son
élément central : un historique défilant où l'enveloppe brute (ambre) et
l'enveloppe nettoyée (céladon) se superposent. La zone ambre visible au-dessus
du céladon **est** le bruit retiré. Quand on arrête de parler, le céladon
s'effondre et l'ambre reste : on voit le produit travailler.

Typographie d'instrument de mesure : **Bahnschrift** (le DIN variable livré
avec Windows) pour les libellés, **Consolas** pour tout ce qui est numérique,
afin que les chiffres restent alignés. Aucune police n'est téléchargée.

Fond ardoise bleu-vert, deux couleurs de signal seulement. Pas de troisième
teinte : ce qui est gardé, ce qui est retiré, rien d'autre.

## Limites de cette version

Périmètre volontairement restreint au **contrôle**. L'installation et la
désinstallation du pack restent dans [`../installer/effectpack/`](../installer/effectpack/),
et le panneau ne les propose pas encore. Il ne détecte pas non plus le cas
« pack installé mais non sélectionné dans les Paramètres Son », qui est
pourtant le piège le plus courant après une mise à jour.
