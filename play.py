#!/usr/bin/env python3
"""
DS64 one-command launcher — macOS / Linux / Windows.

Same entrypoint everywhere: this script. The thin per-OS wrappers
(play.sh for macOS/Linux, play.bat for Windows) only exist to find/
install a system Python 3 and then hand off to this file, which does
everything else:

  1. Creates/updates a project-local virtualenv (.venv) and installs
     requirements.txt into it — never touches the system Python.
  2. Checks for the C++ build toolchain melonDS itself needs (cmake,
     Qt6, SDL2, etc.) and, on macOS/Linux, installs it automatically
     via the OS package manager if missing. On Windows this is only
     reliably automatable from inside an MSYS2 shell (see BUILD.md),
     so we detect what's missing and print exact steps instead of
     silently running a multi-GB unattended install.
  3. Builds melonDS if the binary isn't already built.
  4. Pre-generates per-player QR codes, launches melonDS, serves the
     phone controller page, and tears everything down together on
     Ctrl+C (or when melonDS's own in-app "RESTART SETUP" button
     replaces the process — see the follow-restart loop below).

melonDS itself handles camera-vs-phone and player-count choice via its
own native "DS64 Setup" dialog at startup — nothing here needs to know
or care which mode gets picked.
"""

from __future__ import annotations

import os
import platform
import shutil
import signal
import subprocess
import sys
import sysconfig
import tempfile
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent
OS_NAME = platform.system()  # 'Darwin' | 'Linux' | 'Windows'

VENV_DIR = REPO_ROOT / ".venv"
MELONDS_SRC = REPO_ROOT / "vendor" / "melonDS-src"
MELONDS_BUILD = MELONDS_SRC / "build"
WEB_DIR = REPO_ROOT / "web"

WEB_PORT = 8080
BASE_WS_PORT = 8765
MAX_PLAYERS = 4


def log(msg: str) -> None:
    print(f"[INFO] {msg}", flush=True)


def warn(msg: str) -> None:
    print(f"[WARN] {msg}", flush=True)


def err(msg: str) -> None:
    print(f"[ERROR] {msg}", flush=True)


# ---------------------------------------------------------------- venv ----

def venv_python() -> Path:
    if OS_NAME == "Windows":
        return VENV_DIR / "Scripts" / "python.exe"
    return VENV_DIR / "bin" / "python3"


def ensure_venv() -> None:
    if not venv_python().exists():
        log("Creating Python virtualenv (.venv)...")
        subprocess.run([sys.executable, "-m", "venv", str(VENV_DIR)], check=True)

    # Skip pip entirely when requirements.txt is unchanged since the last
    # successful install — otherwise every launch needs network access and
    # stalls for minutes in pip's retry backoff when offline.
    requirements = REPO_ROOT / "requirements.txt"
    stamp = VENV_DIR / ".requirements-installed"
    if stamp.exists() and stamp.read_bytes() == requirements.read_bytes():
        log("Python dependencies up to date.")
        return

    log("Installing/updating Python dependencies into .venv...")
    subprocess.run([str(venv_python()), "-m", "pip", "install", "-q", "--upgrade", "pip"], check=True)
    subprocess.run(
        [str(venv_python()), "-m", "pip", "install", "-q", "-r", str(requirements)],
        check=True,
    )
    stamp.write_bytes(requirements.read_bytes())

    has_mediapipe = subprocess.run(
        [str(venv_python()), "-c", "import importlib.util, sys; sys.exit(importlib.util.find_spec('mediapipe') is None)"],
    ).returncode == 0
    if not has_mediapipe:
        warn("mediapipe isn't available for this Python version (needs <= 3.12) —")
        warn("Camera mode won't work; Phone mode is unaffected.")


def reexec_under_venv_if_needed() -> None:
    """The rest of this script (psutil, and on macOS Quartz) only exists
    in .venv, not whatever Python originally ran play.py (system python3,
    the play.sh/play.bat wrapper's python, etc.) — re-launch under the
    venv's interpreter so those imports actually resolve. A DS64_REEXECED
    env var guards against looping if something about path resolution
    ever makes the two compare unequal despite already being the venv."""
    if os.environ.get("DS64_REEXECED") == "1":
        return
    target = venv_python()
    try:
        if Path(sys.executable).resolve() == target.resolve():
            return
    except OSError:
        pass
    os.environ["DS64_REEXECED"] = "1"
    os.execv(str(target), [str(target), str(Path(__file__).resolve()), *sys.argv[1:]])


# ------------------------------------------------------- build toolchain --

def _brew_prefix(formula: str) -> str | None:
    try:
        out = subprocess.run(["brew", "--prefix", formula], capture_output=True, text=True, check=True)
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def ensure_build_tools() -> list[str]:
    """Best-effort, OS-appropriate install of melonDS's C++ build deps.
    Returns extra -D args to pass to the cmake configure step (e.g. a
    CMAKE_PREFIX_PATH on macOS, where Qt6/libarchive live in Homebrew's
    non-default prefix)."""
    have_cmake = shutil.which("cmake") is not None

    if OS_NAME == "Darwin":
        qt6_prefix = _brew_prefix("qt@6")
        if not have_cmake or not qt6_prefix:
            if not shutil.which("brew"):
                err("Homebrew not found. Install it from https://brew.sh, then re-run.")
                sys.exit(1)
            log("Installing build dependencies via Homebrew (this can take a while)...")
            subprocess.run(
                ["brew", "install", "git", "pkg-config", "cmake", "sdl2", "qt@6",
                 "libarchive", "enet", "zstd", "faad2"],
                check=True,
            )
            qt6_prefix = _brew_prefix("qt@6")
        libarchive_prefix = _brew_prefix("libarchive")
        prefixes = [p for p in (qt6_prefix, libarchive_prefix) if p]
        return [f"-DCMAKE_PREFIX_PATH={';'.join(prefixes)}"] if prefixes else []

    if OS_NAME == "Linux":
        if not have_cmake:
            apt = shutil.which("apt-get") or shutil.which("apt")
            if not apt:
                err("No apt/apt-get found. Install cmake + Qt6 dev packages for your distro")
                err("manually — see vendor/melonDS-src/BUILD.md (Linux section: Fedora/Arch")
                err("commands are listed there too), then re-run.")
                sys.exit(1)
            log("Installing build dependencies via apt (you may be prompted for your password)...")
            subprocess.run(["sudo", apt, "update"], check=True)
            # Matches vendor/melonDS-src/BUILD.md's Ubuntu 24.04 package list.
            # Older Ubuntu releases use different Qt6 package names — see
            # BUILD.md if this install step fails on an older release.
            subprocess.run(
                ["sudo", apt, "install", "-y",
                 "cmake", "extra-cmake-modules", "libcurl4-gnutls-dev", "libpcap0.8-dev",
                 "libsdl2-dev", "libarchive-dev", "libenet-dev", "libzstd-dev", "libfaad-dev",
                 "qt6-base-dev", "qt6-base-private-dev", "qt6-multimedia-dev", "libqt6svg6-dev",
                 "python3-dev", "python3-venv"],
                check=True,
            )
        return []

    if OS_NAME == "Windows":
        if not have_cmake:
            err("Build tools not found. melonDS on Windows builds via MSYS2, which isn't")
            err("something this script can safely drive unattended. One-time setup:")
            err("  1. winget install MSYS2.MSYS2   (or https://www.msys2.org)")
            err("  2. Open 'MSYS2 UCRT64' from the Start menu, then run:")
            err("       pacman -Syu")
            err("       pacman -S git mingw-w64-ucrt-x86_64-toolchain mingw-w64-ucrt-x86_64-cmake \\")
            err("                 mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-libarchive \\")
            err("                 mingw-w64-ucrt-x86_64-enet mingw-w64-ucrt-x86_64-zstd \\")
            err("                 mingw-w64-ucrt-x86_64-faad2 mingw-w64-ucrt-x86_64-qt6-base \\")
            err("                 mingw-w64-ucrt-x86_64-qt6-svg mingw-w64-ucrt-x86_64-qt6-multimedia \\")
            err("                 mingw-w64-ucrt-x86_64-qt6-tools")
            err("  3. Re-run `python play.py` from inside that same MSYS2 UCRT64 terminal.")
            err("Full details: vendor/melonDS-src/BUILD.md (Windows section).")
            sys.exit(1)
        return []

    err(f"Unrecognized platform: {OS_NAME}")
    sys.exit(1)


def melonds_binary() -> Path:
    if OS_NAME == "Darwin":
        return MELONDS_BUILD / "melonDS.app" / "Contents" / "MacOS" / "melonDS"
    if OS_NAME == "Windows":
        return MELONDS_BUILD / "melonDS.exe"
    return MELONDS_BUILD / "melonDS"


def ensure_melonds_built() -> Path:
    binary = melonds_binary()
    if binary.exists():
        return binary

    log(f"melonDS binary not found at {binary} — building from source.")
    extra_configure_args = ensure_build_tools()

    log("Configuring build (cmake -B build)...")
    subprocess.run(
        ["cmake", "-B", str(MELONDS_BUILD), *extra_configure_args],
        cwd=str(MELONDS_SRC),
        check=True,
    )

    log("Compiling melonDS (this can take several minutes the first time)...")
    nproc = str(os.cpu_count() or 4)
    subprocess.run(
        ["cmake", "--build", str(MELONDS_BUILD), "-j", nproc, "--target", "melonDS"],
        cwd=str(MELONDS_SRC),
        check=True,
    )

    if not binary.exists():
        err(f"Build finished but the binary still isn't at {binary} — check the build log above.")
        sys.exit(1)
    return binary


# --------------------------------------------------------- process mgmt --

def _iter_processes():
    import psutil
    return psutil.process_iter(["pid", "name"])


def kill_melonds() -> None:
    import psutil
    for p in _iter_processes():
        name = (p.info.get("name") or "").lower()
        if name == "melonds" or name == "melonds.exe":
            try:
                p.kill()
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass


def find_melonds_pids() -> list[int]:
    import psutil
    pids = []
    for p in _iter_processes():
        name = (p.info.get("name") or "").lower()
        if name == "melonds" or name == "melonds.exe":
            pids.append(p.info["pid"])
    return pids


def pid_alive(pid: int) -> bool:
    import psutil
    try:
        return psutil.pid_exists(pid)
    except Exception:
        return False


def kill_port(port: int) -> None:
    import psutil
    try:
        conns = psutil.net_connections(kind="inet")
    except (psutil.AccessDenied, PermissionError):
        return
    for c in conns:
        if c.laddr and getattr(c.laddr, "port", None) == port and c.pid:
            try:
                psutil.Process(c.pid).kill()
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                pass


def local_ip() -> str:
    # Same picker the QR generator uses (needs psutil, so venv-only —
    # imported lazily like kill_port's psutil import above).
    sys.path.insert(0, str(REPO_ROOT / "tools"))
    from lan_ip import lan_ip
    return lan_ip()


# -------------------------------------------------------------- windowing -

def macos_fix_window_size() -> None:
    """macOS-only: melonDS's renderer has a known glitch at unusual
    window sizes (not a capture/display bug — confirmed by direct
    comparison). Resizing to a known-good size right after launch
    works around it. No equivalent issue has been observed on
    Linux/Windows, so this step is skipped entirely there."""
    if OS_NAME != "Darwin":
        return

    try:
        import Quartz
    except ImportError:
        warn("pyobjc-framework-Quartz not installed — skipping window auto-resize.")
        return

    target_w, target_h = 700, 1050
    target_x, target_y = 100, 50

    def get_bounds():
        window_list = Quartz.CGWindowListCopyWindowInfo(
            Quartz.kCGWindowListOptionAll, Quartz.kCGNullWindowID
        )
        for w in window_list:
            if w.get("kCGWindowOwnerName") == "melonDS" and w.get("kCGWindowName"):
                return w["kCGWindowBounds"]
        return None

    def run_osascript(script: str) -> None:
        subprocess.run(["osascript", "-e", script], capture_output=True)

    for _ in range(600):  # the DS64 Setup dialog(s) block the window until answered
        if get_bounds() is not None:
            break
        time.sleep(0.5)
    else:
        warn("melonDS window never appeared — skipping auto-resize.")
        return

    for attempt in range(6):
        run_osascript(
            f'tell application "System Events" to tell process "melonDS" '
            f'to set size of window 1 to {{{target_w}, {target_h}}}'
        )
        time.sleep(0.3)
        run_osascript(
            f'tell application "System Events" to tell process "melonDS" '
            f'to set position of window 1 to {{{target_x}, {target_y}}}'
        )
        time.sleep(0.3)
        b = get_bounds()
        if b and b["Width"] == target_w and b["Height"] == target_h and b["X"] == target_x and b["Y"] == target_y:
            log(f"melonDS window set to {b}")
            return
        warn(f"resize attempt {attempt + 1}: bounds={b}, retrying...")
    warn(f"could not perfectly set window bounds, using current: {get_bounds()}")


# ------------------------------------------------------------------ main --

def find_rom() -> Path:
    preferred = REPO_ROOT / "Mario Kart DS (USA) (En,Fr,De,Es,It).nds"
    if preferred.exists():
        return preferred
    candidates = sorted(REPO_ROOT.glob("*.nds"))
    if candidates:
        return candidates[0]
    err("No .nds ROM found at the repo root. Place your Mario Kart DS ROM there and re-run.")
    sys.exit(1)


def main() -> None:
    log(f"Platform: {OS_NAME}")
    rom_path = find_rom()
    log(f"ROM: {rom_path.name}")

    ensure_venv()
    reexec_under_venv_if_needed()
    binary = ensure_melonds_built()

    log("Stopping any leftover melonDS / server processes...")
    kill_melonds()
    kill_port(WEB_PORT)
    for i in range(MAX_PLAYERS):
        kill_port(BASE_WS_PORT + i)
    time.sleep(1)

    web_proc: subprocess.Popen | None = None

    def cleanup(*_args) -> None:
        log("Shutting down everything...")
        if web_proc is not None and web_proc.poll() is None:
            web_proc.terminate()
        kill_port(WEB_PORT)
        for i in range(MAX_PLAYERS):
            kill_port(BASE_WS_PORT + i)
        kill_melonds()
        sys.exit(0)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    log(f"Pre-generating QR codes for up to {MAX_PLAYERS} players...")
    subprocess.run(
        [str(venv_python()), str(REPO_ROOT / "tools" / "make_controller_qr.py"),
         str(WEB_PORT), str(BASE_WS_PORT)],
        check=True,
    )

    log("Launching melonDS...")
    melon_log_path = Path(tempfile.gettempdir()) / "melonds_play.log"
    melon_log = open(melon_log_path, "w", encoding="utf-8", errors="replace")
    # Tell the embedded Python where this checkout's scripts and venv
    # actually are — a prebuilt release binary only knows the CI runner's
    # paths (see DS64ScriptDir()/ds64Path() in melonDS). We're running under
    # the venv's interpreter by now, so purelib is its site-packages.
    melon_env = dict(
        os.environ,
        DS64_TOOLS_DIR=str(MELONDS_SRC / "tools"),
        DS64_VENV_SITE_PACKAGES=sysconfig.get_paths()["purelib"],
    )
    melon_proc = subprocess.Popen([str(binary), "-b", "auto", str(rom_path)],
                                   stdout=melon_log, stderr=subprocess.STDOUT, env=melon_env)

    log("Waiting for melonDS to start...")
    for _ in range(600):
        if melon_proc.poll() is not None or find_melonds_pids():
            break
        time.sleep(0.5)
    if not find_melonds_pids():
        err(f"melonDS did not start. Check {melon_log_path}")
        sys.exit(1)
    tracked_pid = find_melonds_pids()[0]
    log(f"melonDS running, pid={tracked_pid}")

    log("Applying platform-specific window fixups (if any)...")
    macos_fix_window_size()

    log(f"Starting local web server (port {WEB_PORT})...")
    web_proc = subprocess.Popen(
        [sys.executable, "-m", "http.server", str(WEB_PORT)],
        cwd=str(WEB_DIR), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
    )
    time.sleep(1)
    if web_proc.poll() is not None:
        err("Web server failed to start.")
        cleanup()
        return

    ip = local_ip()
    print("")
    print("=======================================================")
    print(" Everything is running:")
    print("   - melonDS (camera or phone chosen at its own startup dialog)")
    print("   - If phone mode: each player's QR shown right in their window")
    print(f"   - Controller page served at http://{ip}:{WEB_PORT}/controller.html")
    print("=======================================================")
    print(" Press Ctrl+C here to stop everything together.")
    print("=======================================================")

    # melonDS's own "RESTART SETUP" button / "DS64 Setup..." menu item
    # force-exits the current process and spawns a brand-new one that is
    # NOT a child of this script — so once the tracked pid disappears,
    # check whether a fresh melonDS is already running before tearing
    # everything down, and keep following it if so.
    while True:
        while pid_alive(tracked_pid):
            time.sleep(0.5)
        time.sleep(0.5)
        remaining = find_melonds_pids()
        if remaining:
            tracked_pid = remaining[0]
            log(f"melonDS restarted via DS64 Setup, now tracking pid={tracked_pid}")
            continue
        break

    cleanup()


if __name__ == "__main__":
    main()
