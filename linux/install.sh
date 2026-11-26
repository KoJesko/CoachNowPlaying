#!/usr/bin/env bash
# CoachNowPlaying for Linux (ETS2 / ATS under Proton) - installer, per user, no root needed.
#   ./linux/install.sh                      build (if possible), install the DLL into every ETS2/ATS found + the bridge service
#   ./linux/install.sh --game-dir "<dir>"   install into this game folder instead (repeat for ETS2 and ATS)
#   ./linux/install.sh --uninstall          stop/remove the bridge service (the DLL stays; delete it by hand)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
NAME=coach-mpris-bridge
GAME_DIRS=()
uninstall=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game-dir) GAME_DIRS+=("$2"); shift 2 ;;
    --uninstall) uninstall=1; shift ;;
    -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
have() { command -v "$1" >/dev/null 2>&1; }

if [[ $uninstall == 1 ]]; then
  systemctl --user disable --now "$NAME.service" 2>/dev/null || true
  rm -f "$UNIT_DIR/$NAME.service"; systemctl --user daemon-reload || true
  echo "Bridge removed. Delete CoachNowPlaying.dll from <ETS2 or ATS>/bin/win_x64/plugins to remove the plugin."
  exit 0
fi

python3 -c "from gi.repository import Gio" 2>/dev/null || {
  echo "PyGObject is missing (Arch: python-gobject, Debian/Ubuntu: python3-gi, Fedora: python3-gobject)" >&2; exit 1; }

# 1. The DLL: build with llvm-mingw if it's there, otherwise use a downloaded release DLL next to this script.
DLL=""
if have cmake && compgen -G "$HOME/.local/opt/llvm-mingw-*" >/dev/null; then
  echo "==> building CoachNowPlaying.dll (llvm-mingw)"
  cmake -S "$REPO" -B "$REPO/build-linux" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/toolchain-llvm-mingw.cmake" >/dev/null
  cmake --build "$REPO/build-linux" -j >/dev/null
  DLL="$REPO/build-linux/CoachNowPlaying.dll"
elif [[ -f "$HERE/CoachNowPlaying.dll" ]]; then
  DLL="$HERE/CoachNowPlaying.dll"
else
  echo "No build toolchain and no prebuilt DLL: download CoachNowPlaying.dll from the Linux release and put it in $HERE/" >&2
  exit 1
fi

# 2. Copy it into the games (keeps a backup of an existing build). Both games load the same DLL.
if [[ ${#GAME_DIRS[@]} == 0 ]]; then
  libs=()
  for root in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
    [[ -d "$root/steamapps" ]] || continue
    libs+=("$root")
    # Extra Steam library folders (other drives) are listed in libraryfolders.vdf.
    if [[ -f "$root/steamapps/libraryfolders.vdf" ]]; then
      while IFS= read -r lib; do libs+=("$lib"); done < <(sed -n 's/^[[:space:]]*"path"[[:space:]]*"\(.*\)"[[:space:]]*$/\1/p' "$root/steamapps/libraryfolders.vdf")
    fi
  done
  declare -A seen=()
  for lib in "${libs[@]}"; do
    for game in "Euro Truck Simulator 2" "American Truck Simulator"; do
      dir="$lib/steamapps/common/$game"
      [[ -d "$dir/bin" ]] || continue
      key="$(realpath "$dir")"
      [[ -n "${seen[$key]:-}" ]] && continue
      seen[$key]=1
      GAME_DIRS+=("$dir")
    done
  done
fi
[[ ${#GAME_DIRS[@]} -gt 0 ]] || { echo "Neither ETS2 nor ATS found - pass --game-dir \"<path to the game folder>\"" >&2; exit 1; }
for dir in "${GAME_DIRS[@]}"; do
  [[ -d "$dir/bin" ]] || { echo "not a game folder (no bin/): $dir" >&2; exit 1; }
  P="$dir/bin/win_x64/plugins"
  mkdir -p "$P"
  if [[ -f "$P/CoachNowPlaying.dll" ]] && ! cmp -s "$DLL" "$P/CoachNowPlaying.dll"; then
    cp -p "$P/CoachNowPlaying.dll" "$P/CoachNowPlaying.dll.bak-$(date +%Y%m%d-%H%M%S)"
  fi
  # Atomic swap: a running game keeps its loaded copy (overwriting a loaded DLL in place can crash it).
  cp "$DLL" "$P/.CoachNowPlaying.dll.new" && mv -f "$P/.CoachNowPlaying.dll.new" "$P/CoachNowPlaying.dll"
  echo "==> plugin: $P/CoachNowPlaying.dll"
done
# Voices: the piper folder comes from the release ZIP. If one game has it, give it to the others too.
PIPER=""
for dir in "${GAME_DIRS[@]}"; do
  [[ -d "$dir/bin/win_x64/plugins/CoachNowPlaying_piper" ]] && PIPER="$dir/bin/win_x64/plugins/CoachNowPlaying_piper" && break
done
for dir in "${GAME_DIRS[@]}"; do
  P="$dir/bin/win_x64/plugins"
  if [[ -d "$P/CoachNowPlaying_piper" ]]; then continue
  elif [[ -n "$PIPER" ]]; then cp -a "$PIPER" "$P/" && echo "==> voices: copied CoachNowPlaying_piper into $P"
  else echo "    note: copy CoachNowPlaying_piper/ from the release ZIP into $P too if you want the voice announcements"
  fi
done
# 3. Talk detection (optional): YAMNet speech/music model in a private Python environment.
DATA="${XDG_DATA_HOME:-$HOME/.local/share}/$NAME"
PY=python3
if [[ "${NO_YAMNET:-0}" != 1 ]]; then
  echo "==> talk detection: Python venv with onnxruntime + YAMNet (set NO_YAMNET=1 to skip)"
  if [[ ! -x "$DATA/venv/bin/python" ]]; then
    python3 -m venv --system-site-packages "$DATA/venv"
  fi
  if "$DATA/venv/bin/pip" install -q onnxruntime numpy >/dev/null 2>&1; then
    PY="$DATA/venv/bin/python"
    if [[ ! -f "$DATA/model/yamnet-onnx-float/yamnet.onnx" ]]; then
      mkdir -p "$DATA/model"
      # Pinned: the model is loaded by onnxruntime, so only unpack the exact release archive.
      YAMNET_SHA256=cf08212c3b9c03160d3c343a5cf99dd53edcb3176b49b6dd330d7d0480abd309
      curl -fsSL -o "$DATA/model/y.zip" \
        https://qaihub-public-assets.s3.us-west-2.amazonaws.com/qai-hub-models/models/yamnet/releases/v0.66.0/yamnet-onnx-float.zip \
        && echo "$YAMNET_SHA256  $DATA/model/y.zip" | sha256sum -c --status \
        && (cd "$DATA/model" && unzip -o -q y.zip && rm -f y.zip) \
        || { rm -f "$DATA/model/y.zip"; echo "    (model download failed or checksum mismatch - talk detection by name only)"; }
    fi
  else
    echo "    (pip install failed - talk detection by name and Shazam misses only)"
  fi
fi

# 4. The bridge as a user service (starts at login).
mkdir -p "$UNIT_DIR"
chmod +x "$HERE/$NAME.py"
sed -e "s|@BRIDGE@|$HERE/$NAME.py|" -e "s|/usr/bin/env python3|$PY|" "$HERE/$NAME.service" > "$UNIT_DIR/$NAME.service"
systemctl --user daemon-reload
systemctl --user enable "$NAME.service" >/dev/null 2>&1; systemctl --user restart "$NAME.service"
echo "==> bridge service running: systemctl --user status $NAME"

have songrec && have pw-record || echo "    tip: install songrec (and PipeWire's pw-record) to identify songs on stations that don't send them"
echo "Done. Start ETS2 or ATS; the coach screen's music page shows what your system plays."
