#!/usr/bin/env bash
#
# package-release.sh -- build a publicly redistributable ZIP of this fork.
#
#   INCLUDES:  every git-tracked file (the GPL source) + ALL built binaries
#              found under bin/ (linux, windows, sprint, ...).  The scan is
#              automatic: any platform present under bin/<platform>/ is
#              included, with no need to list it.
#   EXCLUDES:  build-support/ (untracked), the update.zip files (copyrighted
#              BIOS + ROMs), .sav files, the .git history, and .gitignore.
#
# Usage:  ./package-release.sh [output.zip]
# Env:    NAME=<inner-folder-name>   (default: this repo's directory name)
#
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

for t in git zip tar; do
    command -v "$t" >/dev/null || { echo "[X] '$t' is required"; exit 1; }
done
git rev-parse --git-dir >/dev/null 2>&1 || { echo "[X] not a git repository"; exit 1; }

# Default archive name = the repo's own directory name (e.g. jzintv-20260811-src),
# so the .zip and its top folder match it.  Override with NAME=... if desired.
NAME="${NAME:-$(basename "$SCRIPT_DIR")}"
OUT="${1:-$SCRIPT_DIR/${NAME}.zip}"

STAGE="$(mktemp -d)"; DEST="$STAGE/$NAME"; mkdir -p "$DEST"
trap 'rm -rf "$STAGE"' EXIT

echo "==> Source: git-tracked files (build-support/ and .git excluded automatically)"
git ls-files -z | tar --null -T - -cf - | tar -xf - -C "$DEST"

# .gitignore is a repo-development file, not part of the shipped release.
rm -f "$DEST/.gitignore"

echo "==> Binaries: auto-scan of bin/ (update.zip and .sav excluded)"
if [ -d bin ]; then
    find bin -type f ! -name 'update.zip' ! -name '*.sav' -print0 |
    while IFS= read -r -d '' f; do
        mkdir -p "$DEST/$(dirname "$f")"
        cp -p "$f" "$DEST/$f"
        echo "    + $f"
    done
fi

echo "==> Safety net: strip any non-redistributable material"
find "$DEST" \( -name '*.sav' -o -name 'update.zip' -o -iname 'wbexec.bin' \
             -o -iname 'wbgrom.bin' -o -path '*/build-support/*' \) \
             -print -delete 2>/dev/null || true

echo "==> Creating the zip"
rm -f "$OUT"
( cd "$STAGE" && zip -qr "$OUT" "$NAME" )

echo
echo "OK -> $OUT   ($(du -h "$OUT" | cut -f1), $(unzip -l "$OUT" | tail -1 | awk '{print $2}') files)"
echo "Final check (no non-redistributable material in the zip):"
if unzip -l "$OUT" | grep -iqE 'update\.zip|wbexec|wbgrom|/build-support/|\.sav$|/\.gitignore$'; then
    echo "  [X] WARNING: excluded material found in the zip!"; exit 1
else
    echo "  [OK] clean: no update.zip / Sprint BIOS / build-support / .sav / .gitignore"
fi
