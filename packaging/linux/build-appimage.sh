#!/bin/bash
# Builds a fully self-contained DS64 AppImage: melonDS + Qt + a relocatable
# Python 3.12 (python-build-standalone) with every requirements.txt package
# bundled, so it runs on any Linux with glibc >= 2.35 and needs nothing
# else installed. Builds for the container's own architecture (x86_64, or
# aarch64 on an arm64 host/runner).
#
# The Mario Kart DS ROM is compiled into the melonDS executable itself (see
# DS64_EMBED_ROM in vendor/melonDS-src/src/frontend/qt_sdl/CMakeLists.txt),
# so the AppImage needs no separate .nds file next to it.
#
# Meant to run inside an ubuntu:22.04 container (the old-ish glibc is what
# makes the result portable). From the repo root:
#
#   docker run --rm -e HOST_UID=$(id -u) -e HOST_GID=$(id -g) \
#       -v "$PWD":/src:ro -v "$PWD/dist":/out \
#       -v "$HOME/.cache/ds64-appimage":/cache ubuntu:22.04 \
#       bash /src/packaging/linux/build-appimage.sh
#
# The AppImage lands in ./dist/. The ROM at the repo root is embedded
# automatically; set DS64_EMBED_ROM_FILE=/path/to/game.nds to override it,
# or mount one at /rom.nds for a "personal" build (output:
# DS64-<arch>-personal.AppImage). The /cache mount is optional: it keeps
# downloads and the compiled melonDS between runs, so a retry after a
# network failure resumes instead of starting over.
set -euo pipefail

SRC=/src
OUT=/out
WORK=/work
CACHE=/cache
PY_VERSION=3.12
ARCH="$(uname -m)"   # x86_64 / aarch64, as the AppImage tooling names them
export DEBIAN_FRONTEND=noninteractive
export APPIMAGE_EXTRACT_AND_RUN=1   # no FUSE inside containers

mkdir -p "$CACHE"/{apt,bin,tools,pip}
# The container runs as root; hand the cache back to the host user even
# when a step fails partway.
trap 'chown -R "${HOST_UID:-0}:${HOST_GID:-0}" "$CACHE" 2>/dev/null || true' EXIT
chown -R 0:0 "$CACHE"   # pip ignores a cache dir not owned by the current user
export PATH="$CACHE/bin:$PATH"

# Downloads: short timeouts with many retries, so a flaky connection
# retries quickly instead of hanging for many minutes on one attempt.
fetch() { # url dest
    [ -s "$2" ] && return
    wget -q -c --timeout=30 --tries=30 --waitretry=5 --retry-connrefused -O "$2.part" "$1"
    mv "$2.part" "$2"
}

echo "==> Installing build dependencies"
rm -f /etc/apt/apt.conf.d/docker-clean   # keep downloaded .debs in the cache
APT_OPTS=(-o Dir::Cache::Archives="$CACHE/apt" -o Acquire::Retries=10 -o Acquire::http::Timeout=30)
apt-get "${APT_OPTS[@]}" update -qq
apt-get "${APT_OPTS[@]}" install -y -qq --no-install-recommends \
    build-essential cmake ninja-build pkg-config extra-cmake-modules git ca-certificates curl wget file \
    libpcap0.8-dev libsdl2-dev libenet-dev libarchive-dev libzstd-dev libfaad-dev \
    qt6-base-dev qt6-base-private-dev qt6-multimedia-dev qt6-wayland libqt6svg6-dev libqt6opengl6-dev \
    libgl-dev libegl-dev libwayland-dev libxkbcommon-dev >/dev/null

echo "==> Fetching a relocatable Python $PY_VERSION"
command -v uv >/dev/null \
    || curl -LsSf --retry 10 --retry-all-errors https://astral.sh/uv/install.sh \
       | env UV_INSTALL_DIR="$CACHE/bin" UV_NO_MODIFY_PATH=1 sh >/dev/null
export UV_PYTHON_INSTALL_DIR="$CACHE/python" UV_HTTP_TIMEOUT=60
uv python install "$PY_VERSION" >/dev/null
PYBIN="$(uv python find "$PY_VERSION")"
PYROOT="$(dirname "$(dirname "$(readlink -f "$PYBIN")")")"
echo "    $PYROOT"

echo "==> Building melonDS"
rm -rf "$WORK" && mkdir -p "$WORK"
cp -a "$SRC/vendor/melonDS-src" "$WORK/melonDS-src"
rm -rf "$WORK/melonDS-src/build"
# Build dir lives in the cache (sources are copied with their mtimes), so
# a rerun only recompiles what changed. Passed to CMake as a real path —
# a symlinked build dir leaves ninja's manifest permanently "dirty".
BUILD="$CACHE/melonDS-build"
# A cache created for a different build-dir path can't be reused.
grep -qx "CMAKE_CACHEFILE_DIR:INTERNAL=$BUILD" "$BUILD/CMakeCache.txt" 2>/dev/null || rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$WORK/melonDS-src"
# CMake 3.22's FindPython can't derive python-build-standalone's include/
# lib paths on its own, so hand them over explicitly.
EXTRA_CMAKE_ARGS=()
[ -n "${MELONDS_GIT_HASH:-}" ] && EXTRA_CMAKE_ARGS+=(-DMELONDS_EMBED_BUILD_INFO=ON)

# Put the ROM inside the executable, not in AppDir, so the resulting
# AppImage is a single self-contained file. Precedence: an explicit
# DS64_EMBED_ROM_FILE, a /rom.nds mount (personal build), then the ROM
# committed at the repo root.
EMBED_ROM_PATH="${DS64_EMBED_ROM_FILE:-}"
if [ -z "$EMBED_ROM_PATH" ] && [ -f /rom.nds ]; then
    EMBED_ROM_PATH=/rom.nds
fi
if [ -z "$EMBED_ROM_PATH" ]; then
    EMBED_ROM_PATH="$(find "$SRC" -maxdepth 1 -type f -iname '*.nds' -print -quit)"
fi
if [ -n "$EMBED_ROM_PATH" ]; then
    echo "==> Embedding ROM $EMBED_ROM_PATH"
    EXTRA_CMAKE_ARGS+=(-DDS64_EMBED_ROM=ON -DDS64_EMBED_ROM_FILE="$EMBED_ROM_PATH")
else
    # Don't inherit a stale ON/path from a previous cached configure.
    EXTRA_CMAKE_ARGS+=(-DDS64_EMBED_ROM=OFF -DDS64_EMBED_ROM_FILE=)
fi

cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
    -DPython3_EXECUTABLE="$PYROOT/bin/python$PY_VERSION" \
    -DPython3_INCLUDE_DIR="$PYROOT/include/python$PY_VERSION" \
    -DPython3_LIBRARY="$PYROOT/lib/libpython$PY_VERSION.so" \
    "${EXTRA_CMAKE_ARGS[@]}" >/dev/null
cmake --build "$BUILD" --target melonDS
DESTDIR="$WORK/AppDir" cmake --install "$BUILD" >/dev/null

echo "==> Deploying Qt and system libraries"
cd "$WORK"
fetch https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$ARCH.AppImage "$CACHE/tools/linuxdeploy-$ARCH.AppImage"
fetch https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$ARCH.AppImage "$CACHE/tools/linuxdeploy-plugin-qt-$ARCH.AppImage"
fetch https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-$ARCH.AppImage "$CACHE/tools/appimagetool-$ARCH.AppImage"
# appimagetool would otherwise download this itself at pack time.
fetch https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-$ARCH "$CACHE/tools/runtime-$ARCH"
cp "$CACHE"/tools/*-"$ARCH".AppImage .   # run from a copy; the cache stays pristine
chmod +x ./*.AppImage
LD_LIBRARY_PATH="$PYROOT/lib" \
QMAKE=/usr/lib/qt6/bin/qmake \
EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so" \
EXTRA_QT_PLUGINS="waylandcompositor" \
    ./linuxdeploy-$ARCH.AppImage --appdir AppDir --plugin qt \
    --custom-apprun "$SRC/packaging/linux/AppRun" >/dev/null

# Python goes in after linuxdeploy so it never rewrites the RPATHs of the
# interpreter or of the wheels' own bundled libraries.
echo "==> Bundling Python and DS64's Python dependencies"
cp -a "$PYROOT/bin" "$PYROOT/lib" AppDir/usr/
rm -rf AppDir/usr/lib/python$PY_VERSION/test AppDir/usr/lib/python$PY_VERSION/idlelib \
       AppDir/usr/lib/python$PY_VERSION/tkinter AppDir/usr/lib/python$PY_VERSION/turtledemo
rm -f AppDir/usr/lib/python$PY_VERSION/EXTERNALLY-MANAGED
SITE="AppDir/usr/lib/python$PY_VERSION/site-packages"
if [ -d /prebuilt-site-packages ]; then
    # Offline path: reuse an existing Python $PY_VERSION venv's packages
    # (must be manylinux wheels for glibc <= 2.35, which PyPI's are).
    echo "    using packages from /prebuilt-site-packages"
    cp -a /prebuilt-site-packages/. "$SITE/"
else
    PYTHONHOME="$WORK/AppDir/usr" AppDir/usr/bin/python3 -m pip --version >/dev/null 2>&1 \
        || PYTHONHOME="$WORK/AppDir/usr" AppDir/usr/bin/python3 -m ensurepip -q
    PIP=(env PYTHONHOME="$WORK/AppDir/usr" AppDir/usr/bin/python3 -m pip install -q --cache-dir "$CACHE/pip"
         --retries 20 --timeout 30 --disable-pip-version-check)
    # mediapipe without its declared deps, then only the ones it really
    # needs — see requirements-appimage.txt. Same mediapipe pin as
    # requirements.txt.
    "${PIP[@]}" --no-deps "$(grep -E '^mediapipe' "$SRC/requirements.txt")"
    "${PIP[@]}" -r "$SRC/packaging/linux/requirements-appimage.txt"
fi
find AppDir/usr/lib/python$PY_VERSION -name __pycache__ -prune -exec rm -rf {} +

echo "==> Adding DS64 scripts and controller page"
mkdir -p AppDir/usr/share/ds64/tools
cp -a "$SRC/vendor/melonDS-src/tools/gesture" "$SRC/vendor/melonDS-src/tools/phone" AppDir/usr/share/ds64/tools/
cp -a "$SRC/tools/make_controller_qr.py" "$SRC/tools/lan_ip.py" AppDir/usr/share/ds64/
cp -a "$SRC/web" AppDir/usr/share/ds64/web
find AppDir/usr/share/ds64 -name __pycache__ -prune -exec rm -rf {} +

OUT_NAME=DS64-$ARCH.AppImage
if [ -f /rom.nds ]; then
    OUT_NAME=DS64-$ARCH-personal.AppImage
fi

echo "==> Packing the AppImage"
mkdir -p "$OUT"
ARCH="$ARCH" ./appimagetool-$ARCH.AppImage --no-appstream \
    --runtime-file "$CACHE/tools/runtime-$ARCH" AppDir "$OUT/$OUT_NAME" >/dev/null
chmod +x "$OUT/$OUT_NAME"
chown "${HOST_UID:-0}:${HOST_GID:-0}" "$OUT/$OUT_NAME"
ls -lh "$OUT/$OUT_NAME"
