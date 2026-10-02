#!/bin/sh
# Re-import the Dolphin sources used by iso2rvz from a Dolphin checkout and re-apply the
# iso2rvz patches. Usage: scripts/update-dolphin.sh /path/to/dolphin
set -eu

if [ $# -ne 1 ] || [ ! -d "$1/Source/Core/DiscIO" ]; then
  echo "usage: $0 /path/to/dolphin" >&2
  exit 1
fi

DOLPHIN=$(cd "$1" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEST="$ROOT/dolphin/Source/Core"

# Copy every file that is currently vendored, so the list stays in one place (the tree itself).
cd "$DEST"
find . -type f | while read -r f; do
  if [ ! -f "$DOLPHIN/Source/Core/$f" ]; then
    echo "warning: $f no longer exists in Dolphin" >&2
    continue
  fi
  cp "$DOLPHIN/Source/Core/$f" "$f"
done

git -C "$DOLPHIN" rev-parse HEAD > "$ROOT/dolphin/DOLPHIN_REVISION" 2>/dev/null || true

cd "$ROOT"
if ! git apply patches/dolphin-iso2rvz.patch; then
  echo "error: patches/dolphin-iso2rvz.patch did not apply cleanly; resolve and regenerate it" >&2
  exit 1
fi

echo "Imported Dolphin $(cat dolphin/DOLPHIN_REVISION). Also compare compat/Core/IOS/ES/Formats.*"
echo "with Dolphin's Core/IOS/ES/Formats.* and rebuild; new DiscIO dependencies show up as"
echo "compile or link errors."
