#!/usr/bin/env bash
# CoachNowPlaying for Linux (ETS2 under Proton) - installer, per user, no root needed.
#   ./linux/install.sh                      build (if possible), install the DLL into ETS2 and the bridge service
#   ./linux/install.sh --game-dir "<ETS2>"  game folder if it isn't in ~/.local/share/Steam
#   ./linux/install.sh --uninstall          stop/remove the bridge service (the DLL stays; delete it by hand)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
NAME=coach-mpris-bridge
GAME_DIR=""
uninstall=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --game-dir) GAME_DIR="$2"; shift 2 ;;
    --uninstall) uninstall=1; shift ;;
    -h|--help) sed -n '2,5p' "$0"; exit 0 ;;
    *) echo "unknown option: $1" >&2; exit 2 ;;
  esac
done
have() { command -v "$1" >/dev/null 2>&1; }

if [[ $uninstall == 1 ]]; then
  systemctl --user disable --now "$NAME.service" 2>/dev/null || true
  rm -f "$UNIT_DIR/$NAME.service"; systemctl --user daemon-reload || true
  echo "Bridge removed. Delete CoachNowPlaying.dll from <ETS2>/bin/win_x64/plugins to remove the plugin."
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

# 2. Copy it into the game (keeps a backup of an existing Windows build).
if [[ -z "$GAME_DIR" ]]; then
  for lib in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
    [[ -d "$lib/steamapps/common/Euro Truck Simulator 2/bin" ]] && GAME_DIR="$lib/steamapps/common/Euro Truck Simulator 2" && break
  done
fi
[[ -d "$GAME_DIR/bin" ]] || { echo "ETS2 not found - pass --game-dir \"<path to Euro Truck Simulator 2>\"" >&2; exit 1; }
P="$GAME_DIR/bin/win_x64/plugins"
mkdir -p "$P"
if [[ -f "$P/CoachNowPlaying.dll" ]] && ! cmp -s "$DLL" "$P/CoachNowPlaying.dll"; then
  cp -p "$P/CoachNowPlaying.dll" "$P/CoachNowPlaying.dll.bak-$(date +%Y%m%d-%H%M%S)"
fi
# Atomic swap: a running game keeps its loaded copy (overwriting a loaded DLL in place can crash it).
cp "$DLL" "$P/.CoachNowPlaying.dll.new" && mv -f "$P/.CoachNowPlaying.dll.new" "$P/CoachNowPlaying.dll"
echo "==> plugin: $P/CoachNowPlaying.dll"
[[ -d "$P/CoachNowPlaying_piper" ]] || echo "    note: copy CoachNowPlaying_piper/ from the release ZIP too if you want the voice announcements"

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
echo "Done. Start ETS2; the coach screen's music page shows what your system plays."
