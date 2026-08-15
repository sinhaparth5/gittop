#!/usr/bin/env bash
#
# Installs the NSIS artifact under Wine and checks the one thing the installer
# exists for: that gittop ends up on PATH.
#
# Runs *inside* the container from scripts/build-windows.sh --package; it is not
# meant to be run on the host, which has neither makensis nor a throwaway Wine
# prefix to spend.
#
# This exists because issue 26 shipped. CPack's NSIS template hardcodes
# `Push $INSTDIR\bin` for the add-to-PATH step and `AddToPath` opens with
# `IfFileExists "$0\*.*" "" AddToPath_done`, so any packaging change that moves
# the binary out of $INSTDIR\bin turns the feature off *silently* — no error, no
# message, installer exits 0, "Add to PATH for all users" ticked and PATH
# untouched. Nothing about the .exe on disk shows it. Only installing it does.
#
# The one liberty it takes: the shipped installer defaults to "do not add to
# PATH", and a silent install takes the InstallOptions defaults, so a plain
# `setup.exe /S` would exercise nothing. So it rebuilds the *CPack-generated*
# project.nsi verbatim with the radio button's default moved to all-users, which
# is what a user ticking that button produces. The script under test is the one
# that ships; only which radio starts selected differs.
set -euo pipefail

build_dir=${1:-build-win}
nsis_dir=$(echo "/src/$build_dir"/_CPack_Packages/*/NSIS)
work=/tmp/installer-check
prefix=/tmp/installer-check-wine
# Wine wants this and complains to stderr without it, which buries real output.
export XDG_RUNTIME_DIR=/tmp/installer-check-xdg
export WINEPREFIX=$prefix
export WINEDEBUG=-all

# An absolute path with no spaces, because /D= takes neither quotes nor anything
# after it. Not Program Files: this is about PATH, not about testing NSIS.
install_dir='C:\gittop-check'
install_unix=$prefix/drive_c/gittop-check
env_key='HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment'

rm -rf "$work" "$prefix" "$XDG_RUNTIME_DIR"
mkdir -p "$work" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
cp "$nsis_dir/project.nsi" "$nsis_dir/NSIS.InstallOptions.ini" "$work/"
cd "$work"

# Field 2 is "do not add", Field 3 "all users", Field 4 "current user". The
# ranges are bounded by the next header so a State= line in a later field is not
# caught by the same expression.
sed -i '/^\[Field 2\]/,/^\[Field 3\]/ s/^State=1/State=0/' NSIS.InstallOptions.ini
sed -i '/^\[Field 3\]/,/^\[Field 4\]/ s/^State=0/State=1/' NSIS.InstallOptions.ini
grep -q '^State=1' <(sed -n '/^\[Field 3\]/,/^\[Field 4\]/p' NSIS.InstallOptions.ini) || {
  echo "could not select the all-users radio button; the InstallOptions ini has changed shape" >&2
  exit 1
}
sed -i 's#^\(\s*OutFile\s*\).*#\1"'"$work"'/gittop-check-setup.exe"#I' project.nsi
makensis -V1 project.nsi >/dev/null

wineboot -i >/dev/null 2>&1 || true
wineserver -w

path_of() {
  wine reg query "$env_key" /v PATH 2>/dev/null | tr -d '\r' | sed -n 's/^ *PATH *REG_[A-Z_]* *//p'
}

before=$(path_of)
echo "==> PATH before: $before"
case "$before" in
  *gittop*) echo "the throwaway wine prefix already mentions gittop" >&2; exit 1 ;;
esac

echo "==> installing silently, all users, to $install_dir"
wine "$work/gittop-check-setup.exe" /S /D=$install_dir
wineserver -w

# The layout assertion first, because it is the cause and PATH is the symptom —
# a failure here says which of the two broke.
if [ ! -f "$install_unix/bin/gittop.exe" ]; then
  echo "gittop.exe is not at \$INSTDIR\\bin, which is the directory the installer puts on PATH:" >&2
  find "$install_unix" 2>/dev/null | sed 's/^/  /' >&2
  exit 1
fi
echo "==> installed: $("$(command -v wine)" "$install_unix/bin/gittop.exe" --version 2>/dev/null | tr -d '\r')"

after=$(path_of)
echo "==> PATH after:  $after"
case "$after" in
  *"$install_dir\\bin"*) echo "==> on PATH for all users" ;;
  *) echo "the installer did not add $install_dir\\bin to the system PATH" >&2; exit 1 ;;
esac

# REG_EXPAND_SZ, not REG_SZ. Writing the value back as a plain string is the
# classic way to break every %SystemRoot% entry already in there, so what the
# existing entries expand to is worth one grep.
wine reg query "$env_key" /v PATH 2>/dev/null | tr -d '\r' | grep -q REG_EXPAND_SZ || {
  echo "PATH came back as something other than REG_EXPAND_SZ; the existing %VAR% entries are now literal" >&2
  exit 1
}
case "$after" in
  "$before"*) ;;
  *) echo "the existing PATH was not preserved:\n  was: $before\n  now: $after" >&2; exit 1 ;;
esac
echo "==> existing PATH preserved, still REG_EXPAND_SZ"

# The uninstaller reads $INSTDIR\bin the same way the installer does, so it goes
# wrong for the same reason and is worth the extra thirty seconds. A PATH entry
# left pointing at a deleted directory is the failure nobody notices for months.
echo "==> uninstalling"
wine "$install_unix/Uninstall.exe" /S
wineserver -w
sleep 2
final=$(path_of)
echo "==> PATH final:  $final"
case "$final" in
  *gittop-check*) echo "uninstalling left $install_dir\\bin on the system PATH" >&2; exit 1 ;;
esac
[ "$final" = "$before" ] || echo "note: PATH differs from the original, though gittop is gone"

# ---------------------------------------------------------------- long PATH
# The second half, and the one with teeth. NSIS strings stop at 1024 characters,
# ReadRegStr returns *empty* rather than truncating past that, and CPack's
# AddToPath reads empty as "no PATH yet" and writes only its own directory into
# the value. On a machine with a long PATH the installer therefore deletes it.
#
# That is not hypothetical and it is not old: it only became reachable when the
# top-level-directory fix made $INSTDIR\bin exist, because until then AddToPath
# returned at its IfFileExists before it could do any harm. So the fix and this
# check arrived together. packaging/gittop-path.nsh is what refuses it.
echo
echo "==> long-PATH case: the installer must refuse rather than overwrite"
long=$(python3 -c 'print(";".join("C:\\\\pad\\\\dir%03d" % i for i in range(90)))')
echo "==> planting a ${#long}-character system PATH"
wine reg add "$env_key" /v PATH /t REG_EXPAND_SZ /d "$long" /f >/dev/null 2>&1
wineserver -w
planted=$(path_of)
[ "${#planted}" -gt 1024 ] || { echo "the planted PATH came back at ${#planted} chars, under the limit this is testing" >&2; exit 1; }

wine "$work/gittop-check-setup.exe" /S /D=$install_dir
wineserver -w
survived=$(path_of)
if [ "$survived" != "$planted" ]; then
  echo "the installer rewrote a PATH it cannot safely rewrite:" >&2
  echo "  ${#planted} chars before, ${#survived} chars after" >&2
  exit 1
fi
echo "==> PATH survived intact at ${#survived} characters"
case "$survived" in
  *gittop-check*) echo "and it managed to add gittop as well, which it should not have been able to do" >&2; exit 1 ;;
esac
echo "==> gittop was not added, which is the correct refusal"

# ------------------------------------------------------------- current user
# A separate branch with its own hive, its own key and — in the guard — its own
# labels, so passing the all-users case says nothing about it. It is also the
# one where "the value read back empty" is *ordinary*: plenty of accounts have
# never had a user PATH, and a guard that cannot tell an absent value from an
# unreadable one would refuse every one of them.
echo
echo "==> current-user case, with no HKCU PATH to begin with"
wine reg add "$env_key" /v PATH /t REG_EXPAND_SZ /d "$before" /f >/dev/null 2>&1
wine reg delete 'HKCU\Environment' /v PATH /f >/dev/null 2>&1 || true
wineserver -w

sed -i '/^\[Field 3\]/,/^\[Field 4\]/ s/^State=1/State=0/' NSIS.InstallOptions.ini
sed -i '/^\[Field 4\]/,/^\[Field 5\]/ s/^State=0/State=1/' NSIS.InstallOptions.ini
makensis -V1 project.nsi >/dev/null
wine "$work/gittop-check-setup.exe" /S /D=$install_dir
wineserver -w

user_path=$(wine reg query 'HKCU\Environment' /v PATH 2>/dev/null | tr -d '\r' | sed -n 's/^ *PATH *REG_[A-Z_]* *//p')
echo "==> HKCU PATH: ${user_path:-(still unset)}"
case "$user_path" in
  *"$install_dir\\bin"*) echo "==> on PATH for the current user" ;;
  *) echo "the installer did not add $install_dir\\bin to the current user's PATH" >&2; exit 1 ;;
esac
# And the machine PATH must be untouched by a per-user install.
[ "$(path_of)" = "$before" ] || { echo "a current-user install modified the system PATH" >&2; exit 1; }
echo "==> system PATH untouched"

echo "==> installer check passed"
