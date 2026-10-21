#!/bin/bash
# Assembles the standalone macOS DS64 app bundle:
#
#   melonDS.app/
#     Contents/MacOS/melonDS        (embedded ROM, static Qt)
#     Contents/Frameworks/Python.framework   (runtime used by embedded Python)
#     Contents/Resources/ds64/
#       site-packages/              (target-arch wheels from requirements.txt)
#       tools/{gesture,phone}
#       make_controller_qr.py, lan_ip.py
#       web/
#
# melonDS finds this layout at runtime (see ds64ConfigureBundledRuntime() in
# src/frontend/qt_sdl/main.cpp), so launching the .app directly generates the
# QR codes, serves the controller page, and boots the embedded ROM exactly
# like the Linux AppImage does.
#
# Usage: make-bundle.sh <melonDS.app> <repo-root> <python3> <site-packages-dir>

set -euo pipefail

APP="$1"
REPO_ROOT="$2"
PYTHON="$3"
SITE_PACKAGES="$4"

EXE="$APP/Contents/MacOS/melonDS"
DEST_FRAMEWORK="$APP/Contents/Frameworks/Python.framework"

if [ ! -x "$EXE" ]; then
    echo "error: $EXE not found" >&2
    exit 1
fi

PY_VERSION="$("$PYTHON" -c 'import sys; print("%d.%d" % sys.version_info[:2])')"
PY_PREFIX="$("$PYTHON" -c 'import sys; print(sys.base_prefix)')"
SOURCE_FRAMEWORK="$(cd "$PY_PREFIX/../.." && pwd)"
if [ "$(basename "$SOURCE_FRAMEWORK")" != "Python.framework" ]; then
    echo "error: $PYTHON is not a framework Python build (base_prefix=$PY_PREFIX)" >&2
    exit 1
fi

echo "==> Bundling Python.framework ($PY_VERSION)"
rm -rf "$DEST_FRAMEWORK"
mkdir -p "$DEST_FRAMEWORK/Versions"
# Only the version melonDS was linked against. The runner's framework can
# hold several (3.11, 3.12, 3.13...), and melonDS picks the first
# Versions/* entry with a lib/ as its Python home — an extra version could
# pair the 3.12 dylib with another version's stdlib.
cp -a "$SOURCE_FRAMEWORK/Versions/$PY_VERSION" "$DEST_FRAMEWORK/Versions/"
ln -s "$PY_VERSION" "$DEST_FRAMEWORK/Versions/Current"
for LINK in Python Resources Headers; do
    if [ -e "$DEST_FRAMEWORK/Versions/Current/$LINK" ]; then
        ln -s "Versions/Current/$LINK" "$DEST_FRAMEWORK/$LINK"
    fi
done
# Not needed at runtime; mirrors the slimming the AppImage/Windows builds do.
STDLIB="$DEST_FRAMEWORK/Versions/$PY_VERSION/lib/python$PY_VERSION"
rm -rf "$STDLIB/test" "$STDLIB/idlelib" "$STDLIB/turtledemo" "$STDLIB/ensurepip"

FW_BIN="$DEST_FRAMEWORK/Versions/$PY_VERSION/Python"
NEW_ID="@rpath/Python.framework/Versions/$PY_VERSION/Python"
if [ ! -f "$FW_BIN" ]; then
    echo "error: framework binary not found at $FW_BIN" >&2
    exit 1
fi
install_name_tool -id "$NEW_ID" "$FW_BIN"

# python.org's framework links some of its own extension modules (e.g.
# _ssl -> lib/libssl.3.dylib, which websockets needs) by absolute path under
# /Library/Frameworks/Python.framework. That path exists on the CI runner,
# so everything works there — and breaks on a player's Mac without Python.
# Rewrite those references to @loader_path-relative ones.
DEST_VERSION="$DEST_FRAMEWORK/Versions/$PY_VERSION"
# melonDS embeds the Python dylib directly, so the framework's command-line
# interpreters are dead weight: bin/python3.x is only a stub that re-execs
# Resources/Python.app, and neither is ever launched.
rm -rf "$DEST_VERSION/Resources/Python.app" "$DEST_VERSION/bin"
ABS_DEP_RE="^/.*/Python\.framework/Versions/$PY_VERSION/"
relpath() { "$PYTHON" -c 'import os, sys; print(os.path.relpath(sys.argv[1], sys.argv[2]))' "$1" "$2"; }
# Every Mach-O file in the bundled framework: extension modules and dylibs.
list_macho() {
    find "$DEST_VERSION" -type f \( -name '*.so' -o -name '*.dylib' \) -print0 |
    while IFS= read -r -d '' F; do
        if file -b "$F" | grep -q 'Mach-O'; then echo "$F"; fi
    done
}
while IFS= read -r MACHO; do
    CHANGED=0
    for DEP in $(otool -L "$MACHO" | awk 'NR > 1 {print $1}' | { grep -E "$ABS_DEP_RE" || true; }); do
        TARGET="$DEST_VERSION/${DEP#*/Python.framework/Versions/$PY_VERSION/}"
        NEW_DEP="@loader_path/$(relpath "$TARGET" "$(dirname "$MACHO")")"
        install_name_tool -change "$DEP" "$NEW_DEP" "$MACHO" 2>/dev/null
        CHANGED=1
    done
    # The bundled dylibs' own ids, so nothing re-resolves to the old path.
    if [[ "$MACHO" == *.dylib ]] && otool -D "$MACHO" | tail -n +2 | grep -qE "$ABS_DEP_RE"; then
        install_name_tool -id "@loader_path/$(basename "$MACHO")" "$MACHO" 2>/dev/null
        CHANGED=1
    fi
    # Editing load commands breaks python.org's signature, and Apple
    # Silicon refuses to load a Mach-O with an invalid one. The final
    # `codesign --deep` below doesn't reach loose files under lib/.
    if [ "$CHANGED" = 1 ]; then
        codesign --force --sign - "$MACHO" >/dev/null 2>&1
    fi
done < <(list_macho)
codesign --force --sign - "$FW_BIN" >/dev/null 2>&1

LEFTOVER=""
while IFS= read -r MACHO; do
    BAD="$(otool -L "$MACHO" | awk 'NR > 1 {print $1}' | { grep -E '^/.*/Python\.framework/' || true; })"
    if [ -n "$BAD" ]; then LEFTOVER+="$MACHO -> $BAD"$'\n'; fi
done < <(list_macho; echo "$FW_BIN")
if [ -n "$LEFTOVER" ]; then
    echo "error: bundled Python still references an absolute framework path:" >&2
    echo "$LEFTOVER" >&2
    exit 1
fi

# Point the executable at the copy inside the app, and let @rpath resolve to
# Contents/Frameworks.
OLD_DEP="$(otool -L "$EXE" | awk '/Python\.framework/ {print $1; exit}')"
if [ -n "$OLD_DEP" ]; then
    install_name_tool -change "$OLD_DEP" "$NEW_ID" "$EXE"
else
    echo "error: $EXE does not link Python.framework" >&2
    exit 1
fi
if ! otool -l "$EXE" | grep -A2 'LC_RPATH' | grep -q '@executable_path/../Frameworks'; then
    install_name_tool -add_rpath "@executable_path/../Frameworks" "$EXE"
fi

echo "==> Adding DS64 scripts, Python packages, and controller page"
BUNDLE="$APP/Contents/Resources/ds64"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE/tools"
cp -a "$REPO_ROOT/vendor/melonDS-src/tools/gesture" "$BUNDLE/tools/"
cp -a "$REPO_ROOT/vendor/melonDS-src/tools/phone" "$BUNDLE/tools/"
cp -a "$REPO_ROOT/tools/make_controller_qr.py" "$BUNDLE/"
cp -a "$REPO_ROOT/tools/lan_ip.py" "$BUNDLE/"
cp -a "$REPO_ROOT/web" "$BUNDLE/web"
if [ -d "$SITE_PACKAGES" ]; then
    cp -a "$SITE_PACKAGES" "$BUNDLE/site-packages"
fi

find "$BUNDLE" -name __pycache__ -prune -exec rm -rf {} +

# install_name_tool invalidated the old ad-hoc signature; re-sign the complete
# bundle after adding Resources.
codesign --force --deep --sign - "$APP" >/dev/null

echo "==> Bundle ready: $APP"
