#!/usr/bin/env bash
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
install=0
for arg in "$@"; do
  case "$arg" in
    --install) install=1 ;;
    *)
      echo "usage: $0 [--install]" >&2
      exit 2
      ;;
  esac
done
OBS_SRC="${OBS_SRC:-$HOME/Developer/obs-studio}"
OBS_APP="${OBS_APP:-/Applications/OBS.app}"
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qtbase)}"
if [ ! -f "$OBS_SRC/deps/simde/simde/x86/sse2.h" ]; then
  git clone --depth 1 --branch v0.8.2 \
    https://github.com/simd-everywhere/simde.git "$OBS_SRC/deps/simde"
fi
cmake -S "$here" -B "$here/build-mac" -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
  -DOBS_SRC="$OBS_SRC" -DOBS_APP="$OBS_APP"
cmake --build "$here/build-mac"
if [ "$install" -eq 1 ]; then
  dest="$HOME/Library/Application Support/obs-studio/plugins"
  mkdir -p "$dest"
  rm -rf "$dest/iso-recorder.plugin"
  cp -R "$here/build-mac/iso-recorder.plugin" "$dest/"
  echo "Installed to $dest. Restart OBS."
else
  echo "Built. Install with:"
  echo "  $0 --install"
fi
