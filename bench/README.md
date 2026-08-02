# bench/ — mesures et tests A/B (jalon 3)

Contenu prévu :

- scripts de mesure latence et CPU de la chaîne APO + worker ;
- enregistrements A/B (brut vs traité) ;
- **test AGC Chrome/WebRTC** : enregistrer le flux tel que reçu côté Meet avec
  l'APO actif, mesurer ce que l'AGC WebRTC fait du bruit résiduel entre les
  phrases, calibrer l'atténuation résiduelle pour ne pas déclencher l'AGC.

Dès le jalon 1, ce dossier peut accueillir les journaux des runs de stabilité
48 h (cf. [`../docs/procedure-test-vm.md`](../docs/procedure-test-vm.md)).
