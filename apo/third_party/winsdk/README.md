# third_party/winsdk

`audioengineextensionapo.h` provient du repo officiel
[microsoft/win32metadata](https://github.com/microsoft/win32metadata)
(`generation/WinSDK/RecompiledIdlHeaders/um/`), licence MIT.

Vendored parce que le SDK Windows 10.0.19041 (VS2019 Build Tools) ne définit
pas `IAudioSystemEffects3` / `APOInitSystemEffects3`, alors que le moteur audio
de Windows 11 **exige** SE3 pour insérer un APO tiers dans le graphe (constaté
au log : QI `{C58B31CD-...}` juste après la création, éviction si E_NOINTERFACE).

À supprimer quand la toolchain passera à un SDK ≥ 10.0.22000.
