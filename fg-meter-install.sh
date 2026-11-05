#!/bin/bash
# Put the flip-metering igdext64.dll into ClawDeck's XMX package and the game's prefix (backups kept as .bak).
# Undo: bash fg-meter-install.sh undo
set -e
URL=https://raw.githubusercontent.com/IHulkSmashI/xess-xmx-linux/fg-meter-test-build/igdext64.dll
SHA=dfe3e5ddcdd53a4351807507400af363ca18c1026817efeb95fe993556973b90
APPID=${APPID:-3669870}   # CONTROL Resonant; another game: APPID=<steam app id> bash fg-meter-install.sh
PKG=$(dirname "$(find ~/.local/share/clawdeck -path '*xess-xmx-linux-*' -name igdext64.dll | sort | tail -1)")
PFX=$(ls -d ~/.local/share/Steam/steamapps/compatdata/$APPID /run/media/*/*/steamapps/compatdata/$APPID 2>/dev/null | head -1)
D="$PFX/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1"
[ "$PKG" != . ] && [ -d "$PKG" ] && [ -d "$D" ] || { echo "not found: package '$PKG' or prefix of app $APPID '$D'"; exit 1; }
if [ "$1" = undo ]; then
  for f in "$PKG/igdext64.dll" "$D/igdext64.dll"; do [ -f "$f.bak" ] && mv -f "$f.bak" "$f" && echo "restored $f"; done
  exit 0
fi
curl -fsSL "$URL" -o /tmp/igdext64-meter.dll
echo "$SHA  /tmp/igdext64-meter.dll" | sha256sum -c -
for f in "$PKG/igdext64.dll" "$D/igdext64.dll"; do
  [ -f "$f.bak" ] || cp -a "$f" "$f.bak"
  cp -f /tmp/igdext64-meter.dll "$f"
done
rm -f "$PFX/pfx/drive_c/igdext_xell.log"
echo "installed into $PKG and $D"
