#!/usr/bin/env bash
# Independent personal GUI; Plot n Jot itself is not modified.
set -euo pipefail
DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
VENV="$DIR/.venv"
show_error() {
  if command -v zenity >/dev/null 2>&1; then zenity --error --title="E213 Personal Sync" --text="$1"
  else printf "E213 Personal Sync: %s\n" "$1" >&2; fi
}
if ! python3 -c 'import tkinter' >/dev/null 2>&1; then
  show_error "Tkinter is missing. On Ubuntu/Debian: sudo apt install python3-tk"
  exit 1
fi
if [[ ! -x "$VENV/bin/python" ]]; then
  if ! python3 -m venv --system-site-packages "$VENV"; then
    show_error "Python venv is missing. On Ubuntu/Debian: sudo apt install python3-venv"
    exit 1
  fi
fi
if ! "$VENV/bin/python" -c "import serial" >/dev/null 2>&1; then
  if ! "$VENV/bin/python" -m pip install "pyserial>=3.5,<4"; then
    show_error "Could not install PySerial in the private E213 environment."
    exit 1
  fi
fi
exec "$VENV/bin/python" "$DIR/personal_sync.py"
