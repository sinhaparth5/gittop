; !include'd into the end of CPack's core install section through
; CPACK_NSIS_EXTRA_INSTALL_COMMANDS; see the block in CMakeLists.txt.
;
; Everything here is therefore straight-line instructions and preprocessor
; macros, not a Function: the include lands inside a Section, and a Function
; cannot be declared there. The one global-scope hook the template offers,
; CPACK_NSIS_DEFINES, is undocumented and the generator overwrites whatever is
; put in it, which costs an afternoon to discover — the setting simply arrives
; empty and the script fails to resolve a function nobody defined.
;
; This exists to stop CPack's own AddToPath from eating the machine's PATH.
;
; NSIS strings are capped at ${NSIS_MAX_STRLEN}, which is 1024 in every stock
; build — Debian's and the one Chocolatey puts on the CI runner alike. Over that
; limit ReadRegStr does not truncate, it returns the empty string, and CPack's
; AddToPath reads that as "there is no PATH yet" and writes *only its own
; directory* into the value. Measured, not theorised: against a 1439-character
; system PATH the installer left a system PATH of 18 characters.
;
; CPack does try to guard this, and the guard looks right until you line the two
; up. It measures `ReadEnvStr PATH` — the *process* environment, HKLM and HKCU
; already merged — and then writes based on `ReadRegStr` of one hive. Those are
; different strings, so the check can pass on a truncated-but-non-empty 1019
; characters while the read that matters comes back empty. That is exactly the
; case above.
;
; Nothing here writes PATH. It decides whether CPack's section is safe to run
; and switches it off when it is not, which keeps one implementation of the
; append rather than adding a second that could disagree with the uninstaller's.
; A refusal says which directory to add by hand, because an installer that
; quietly does nothing is the bug this whole file is downstream of.

!macro _GittopGuardPath HIVE SUBKEY
  ClearErrors
  ReadRegStr $R1 ${HIVE} "${SUBKEY}" "PATH"

  StrCmp $R1 "" 0 _gittop_measure_${HIVE}
    ; Empty. Either the hive has no PATH value at all — which is ordinary for
    ; HKCU, where plenty of accounts have never set one — or it has one that is
    ; too long to read. Only an enumeration separates them, and the difference is
    ; the whole point: the first is safe to write fresh, the second is the one
    ; that destroys the value.
    StrCpy $R2 0
    _gittop_enum_${HIVE}:
      EnumRegValue $R3 ${HIVE} "${SUBKEY}" $R2
      StrCmp $R3 "" _gittop_ok_${HIVE}
      IntOp $R2 $R2 + 1
      StrCmp $R3 "PATH" _gittop_refuse_${HIVE}
      StrCmp $R3 "Path" _gittop_refuse_${HIVE}
      Goto _gittop_enum_${HIVE}

  _gittop_measure_${HIVE}:
    ; The read fitting is not enough — the *result* has to fit too. AddToPath
    ; builds "$1;$0" with StrCpy, which truncates at the same limit and says
    ; nothing, so a PATH one character under the cap loses its tail instead of
    ; its whole self. Same bug, one step later, and worth catching in the same
    ; place. The +1 is the separator.
    StrLen $R2 $R1
    StrLen $R3 $R0
    IntOp $R2 $R2 + $R3
    IntOp $R2 $R2 + 1
    IntCmp $R2 ${NSIS_MAX_STRLEN} _gittop_ok_${HIVE} _gittop_ok_${HIVE} _gittop_refuse_${HIVE}

  _gittop_refuse_${HIVE}:
    ; Turning this on is what makes CPack's "-Add to path" section skip. The
    ; user's actual choice has already been written to the uninstall registry a
    ; few lines earlier, so the uninstaller still knows what was asked for.
    StrCpy $DO_NOT_ADD_TO_PATH "1"
    ; /SD IDOK, or a silent install waits forever for a button nobody can see.
    MessageBox MB_OK|MB_ICONEXCLAMATION "gittop is installed, but it was not added to PATH.$\n$\nThe PATH already on this machine is longer than this installer can rewrite without truncating it, so it has been left exactly as it was.$\n$\nTo finish, add this directory to PATH yourself:$\n$\n$R0" /SD IDOK
  _gittop_ok_${HIVE}:
!macroend

; The body. This runs at the end of the core install section — after the install
; options have been read into these variables and after they have been recorded
; for the uninstaller, and before the "-Add to path" section. That ordering is
; the only reason any of this works, and it is a property of where CPack puts
; CPACK_NSIS_EXTRA_INSTALL_COMMANDS in its template.
Push $R0
Push $R1
Push $R2
Push $R3

; Nothing to guard if the user did not ask for PATH in the first place.
StrCmp $DO_NOT_ADD_TO_PATH "1" _gittop_guard_done

; The same directory AddToPath is about to be handed, spelled the same way. If
; these two ever disagree the check is measuring the wrong string.
StrCpy $R0 "$INSTDIR\bin"

StrCmp $ADD_TO_PATH_ALL_USERS "1" 0 _gittop_guard_user
  !insertmacro _GittopGuardPath HKLM "SYSTEM\CurrentControlSet\Control\Session Manager\Environment"
  Goto _gittop_guard_done
_gittop_guard_user:
  !insertmacro _GittopGuardPath HKCU "Environment"

_gittop_guard_done:
Pop $R3
Pop $R2
Pop $R1
Pop $R0
