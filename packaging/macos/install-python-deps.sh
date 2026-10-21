#!/bin/bash
# Installs DS64's Python packages for the target macOS architecture into a
# standalone site-packages directory (then packaged by make-bundle.sh).
#
# On the arm64 GitHub runner the x86_64 build links a universal2 Python, but
# pip would otherwise install arm64 wheels. --platform makes it fetch the
# Intel binary wheels that the x86_64 app will actually load.
#
# Usage: install-python-deps.sh <x86_64|arm64> <python3> <repo-root> <target-dir>

set -euo pipefail

ARCH="$1"
PYTHON="$2"
REPO_ROOT="$3"
TARGET="$4"

rm -rf "$TARGET"
mkdir -p "$TARGET"

PIP_COMMON=(
    --disable-pip-version-check
    --no-input
    -q
    --target "$TARGET"
)
# Expanded below as ${PLATFORM_ARGS[@]+...}: macOS's /bin/bash is 3.2, where
# "${arr[@]}" on an empty array is an unbound-variable error under set -u.
PLATFORM_ARGS=()

if [ "$ARCH" = "x86_64" ]; then
    # The runner's Python is 3.12, so pip still knows the right cp312/abi3
    # tags; only the platform needs overriding. Deliberately request wheels
    # no newer than the app's 10.15 deployment target. (That makes pip pick
    # opencv 4.9 for Intel; newer opencv x86_64 wheels require macOS 13+.)
    PLATFORM_ARGS+=(
        --only-binary=:all:
        --platform macosx_10_15_x86_64
        --platform macosx_11_0_x86_64
        # That old OpenCV is compiled against NumPy 1.x; with NumPy 2 its
        # import fails ("_ARRAY_API not found").
        "numpy<2"
    )
fi

# Same split as the AppImage: mediapipe itself without its jax/scipy/
# sounddevice dependency tree, then only the packages the embedded gesture
# and phone scripts actually import. This also avoids installing both
# opencv-python and mediapipe's opencv-contrib-python over the same cv2 files.
"$PYTHON" -m pip install "${PIP_COMMON[@]}" ${PLATFORM_ARGS[@]+"${PLATFORM_ARGS[@]}"} \
    --no-deps "mediapipe==0.10.14"
"$PYTHON" -m pip install "${PIP_COMMON[@]}" ${PLATFORM_ARGS[@]+"${PLATFORM_ARGS[@]}"} \
    -r "$REPO_ROOT/packaging/linux/requirements-appimage.txt"

echo "==> Installed Python packages for $ARCH into $TARGET"
