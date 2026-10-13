#!/usr/bin/env bash
# One-time menu shortcut for E213 Personal Sync; Plot n Jot stays untouched.
set -euo pipefail
DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
APPDIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
if ! python3 -c 'import tkinter' >/dev/null 2>&1; then
  echo "Install Tkinter first (Ubuntu/Debian: sudo apt install python3-tk)"
  exit 1
fi
mkdir -p "$APPDIR"
chmod +x "$DIR/launch-linux.sh"
printf "%s\n" "[Desktop Entry]" "Type=Application" "Version=1.0" "Name=E213 Personal Sync" "Comment=Local USB writing sync with Plot n Jot" "Exec=\"$DIR/launch-linux.sh\"" "Icon=accessories-text-editor" "Terminal=false" "Categories=Office;Utility;" "StartupNotify=true" > "$APPDIR/e213-personal-sync.desktop"
echo "Installed E213 Personal Sync in your Linux applications menu."
