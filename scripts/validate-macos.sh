#!/usr/bin/env bash
# Runs the two validators that decide whether a host will load this plugin at all.
#
# auval is Apple's, and Logic and GarageBand will refuse an AudioUnit that fails it.
# pluginval is Tracktion's, and at strictness 5 it hammers the lifecycle this plugin has
# to survive: repeated instantiation, editor open/close cycles, sample-rate changes
# mid-stream. A plugin that opens sockets and spawns threads is exactly the kind that
# fails these in ways unit tests cannot see.
#
# Usage: scripts/validate-macos.sh [build-directory] [config]

set -euo pipefail

BUILD_DIR="${1:-build}"
CONFIG="${2:-Debug}"
ARTEFACTS="$BUILD_DIR/PhonePostMix_artefacts/$CONFIG"

if [[ "$(uname)" != "Darwin" ]]; then
  echo "This script is macOS-only. On other platforms run pluginval against the VST3 directly." >&2
  exit 1
fi

if [[ ! -d "$ARTEFACTS" ]]; then
  echo "No artefacts at $ARTEFACTS — build first, e.g. cmake --build $BUILD_DIR" >&2
  exit 1
fi

echo "== auval =="
mkdir -p "$HOME/Library/Audio/Plug-Ins/Components"
cp -R "$ARTEFACTS/AU/PhonePostMix.component" "$HOME/Library/Audio/Plug-Ins/Components/"
killall -9 AudioComponentRegistrar 2>/dev/null || true
auval -v aufx Ppm1 Ppmx

PLUGINVAL="${PLUGINVAL:-}"
if [[ -z "$PLUGINVAL" ]]; then
  if [[ -x "/Applications/pluginval.app/Contents/MacOS/pluginval" ]]; then
    PLUGINVAL="/Applications/pluginval.app/Contents/MacOS/pluginval"
  else
    echo
    echo "pluginval not found. Download it from https://github.com/Tracktion/pluginval/releases"
    echo "and either put pluginval.app in /Applications or set PLUGINVAL=/path/to/pluginval."
    exit 0
  fi
fi

echo
echo "== pluginval (strictness 5) =="
"$PLUGINVAL" --validate-in-process --strictness-level 5 --timeout-ms 300000 \
  --validate "$ARTEFACTS/VST3/PhonePostMix.vst3"
"$PLUGINVAL" --validate-in-process --strictness-level 5 --timeout-ms 300000 \
  --validate "$HOME/Library/Audio/Plug-Ins/Components/PhonePostMix.component"
