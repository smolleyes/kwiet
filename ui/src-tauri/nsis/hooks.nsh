; Kwiet installer hooks.
;
; Tauri's NSIS template handles the application itself. The effect pack is a
; driver package: it needs pnputil and elevation, so it is installed here -- and
; removed here too, because a driver package left in the store outlives the app
; that put it there and there is no other moment to catch it.
;
; The work itself lives in pack.ps1, which ships next to the pack and can be run
; by hand to repair an installation. NSIS only orchestrates.

!include LogicLib.nsh
!include x64.nsh

; Tauri inserts the language macros for us, so LangString is not reliably
; available this early. Two lines and a comparison are worth more than a clever
; string table that might load at the wrong moment.
!define KWIET_LANG_FRENCH 1036

!macro KwietDetail fr en
  ${If} $LANGUAGE == ${KWIET_LANG_FRENCH}
    DetailPrint "${fr}"
  ${Else}
    DetailPrint "${en}"
  ${EndIf}
!macroend

; This installer is 32-bit. Without lifting WOW64 redirection, "powershell.exe"
; resolves to the 32-bit copy in SysWOW64, and from there pnputil.exe -- which
; exists only in the native System32 -- is simply not found. The failure reads
; as "command not recognised" and says nothing about redirection, so it is worth
; spelling out here rather than rediscovering it.
!macro KwietRunPack args
  ${DisableX64FSRedirection}
  nsExec::ExecToLog 'powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "$INSTDIR\effectpack\pack.ps1" ${args}'
  Pop $0
  ${EnableX64FSRedirection}
!macroend

!macro NSIS_HOOK_POSTINSTALL
  !insertmacro KwietDetail \
    "Installation du pack d'effets (le son sera brievement coupe)..." \
    "Installing the effect pack (sound will cut out briefly)..."

  !insertmacro KwietRunPack '-Action install -PackageDir "$INSTDIR\effectpack"'

  ${If} $0 != 0
    ${If} $LANGUAGE == ${KWIET_LANG_FRENCH}
      MessageBox MB_ICONEXCLAMATION|MB_OK "Kwiet est installe, mais le pack d'effets n'a pas pu etre enregistre dans Windows.$\r$\n$\r$\nDetails : %ProgramData%\Kwiet\pack.log$\r$\n$\r$\nOuvre Kwiet : le panneau dira ce qui manque."
    ${Else}
      MessageBox MB_ICONEXCLAMATION|MB_OK "Kwiet is installed, but the effect pack could not be registered with Windows.$\r$\n$\r$\nDetails: %ProgramData%\Kwiet\pack.log$\r$\n$\r$\nOpen Kwiet: the panel will say what is missing."
    ${EndIf}
  ${Else}
    !insertmacro KwietDetail \
      "Pack installe. Il reste a choisir Kwiet dans Parametres > Son > ton micro > Ameliorations audio." \
      "Pack installed. Now choose Kwiet in Settings > Sound > your microphone > Audio enhancements."
  ${EndIf}
!macroend

!macro NSIS_HOOK_PREUNINSTALL
  !insertmacro KwietDetail \
    "Retrait du pack d'effets..." \
    "Removing the effect pack..."

  ; The app is going away regardless; a pack that refuses to leave must not
  ; block that, it is reported in the log instead.
  !insertmacro KwietRunPack '-Action uninstall -IgnoreFailure'
!macroend
