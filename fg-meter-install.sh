#!/bin/bash
# Put the flip-metering igdext64.dll into ClawDeck's XMX package and CONTROL's prefix (backups kept as .bak).
# Undo: bash fg-meter-install.sh undo
set -e
URL=https://raw.githubusercontent.com/IHulkSmashI/xess-xmx-linux/fg-meter-test-build/igdext64.dll
SHA=08d25a637559635fd2bcf50eafa1c24545742304e64482ba38bbd61ba5a8e537
PKG=$(ls -d ~/.local/share/clawdeck/xess-xmx-linux-* | tail -1)
PFX=$(ls -d ~/.local/share/Steam/steamapps/compatdata/870780 /run/media/*/*/steamapps/compatdata/870780 2>/dev/null | head -1)
D="$PFX/pfx/drive_c/windows/system32/driverstore/filerepository/igd_faux.inf_1"
[ -d "$PKG" ] && [ -d "$D" ] || { echo "not found: package '$PKG' or CONTROL prefix '$D'"; exit 1; }
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
