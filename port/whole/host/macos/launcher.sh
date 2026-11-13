#!/bin/bash
# Contents/MacOS/RE4: finds the player's game data (the disc-data folder made from their own discs) and starts
# the front end. Looks in $RE4_DATA, the folder chosen before, next to the app, then Application Support;
# else asks once. The game's log of the last run: ~/Library/Application Support/RE4 Mac/last-run.log
HERE="$(cd "$(dirname "$0")" && pwd)"
APPDIR="$(cd "$HERE/../../.." && pwd)"
SUP="$HOME/Library/Application Support/RE4 Mac"
mkdir -p "$SUP"

dialog() {  # message [buttons]
  osascript -e "display dialog \"$1\" with title \"Resident Evil 4\" buttons {\"OK\"} default button 1 with icon caution" >/dev/null 2>&1
}
valid() { [ -n "$1" ] && [ -d "$1/disc0" ]; }

# the game itself runs as x86_64 code: Rosetta 2 must be installed
if ! arch -x86_64 /usr/bin/true 2>/dev/null; then
  dialog "Este juego necesita Rosetta 2. Abre la Terminal y ejecuta:\n\nsoftwareupdate --install-rosetta --agree-to-license\n\nLuego vuelve a abrir el juego."
  exit 1
fi

DATA=""
for d in "$RE4_DATA" "$(cat "$SUP/data-path" 2>/dev/null)" "$APPDIR/disc-data" "$SUP/disc-data"; do
  if valid "$d"; then DATA="$d"; break; fi
done
while [ -z "$DATA" ]; do
  d=$(osascript -e 'POSIX path of (choose folder with prompt "Elige la carpeta disc-data con los datos de tus discos de Resident Evil 4 (GameCube):")' 2>/dev/null) || exit 0
  d="${d%/}"
  if valid "$d"; then DATA="$d"; printf '%s\n' "$DATA" > "$SUP/data-path"
  else dialog "Esa carpeta no tiene los datos del juego (falta la subcarpeta disc0). Elige la carpeta disc-data."; fi
done

# texture packs next to the app (Dolphin's G4BE08 folder as the HD Project ships it, or a "textures" folder)
TEX=""
for d in "$APPDIR/G4BE08" "$APPDIR/textures" "$(dirname "$DATA")/G4BE08"; do
  [ -d "$d" ] && case ":$TEX:" in *":$d:"*) ;; *) TEX="${TEX:+$TEX:}$d" ;; esac
done
export RE4_TEXTURE_DIRS="$TEX"

cd "$SUP"
"$HERE/re4_mac" "$HERE/libre4_main.dylib" "$HERE/modules" "$DATA" > "$SUP/last-run.log" 2>&1
status=$?
if [ $status -ne 0 ] && [ $status -lt 128 ]; then
  why=$(grep -E "reserve:|start:|renderer:|core state|window:|GL context:" "$SUP/last-run.log" | tail -1 | tr -d '"\\')
  dialog "El juego se cerró con un error ($status).\n\n${why}\n\nRegistro: ~/Library/Application Support/RE4 Mac/last-run.log"
fi
exit $status
