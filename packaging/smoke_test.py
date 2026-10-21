#!/usr/bin/env python3
"""Smoke-tests a packaged DS64 build: the Linux AppImage, the Windows bundle
directory (containing melonDS.exe), or the macOS melonDS.app.

Two checks, both of which an end user would hit within seconds of launching:

1. The *bundled* Python (not the host's) can import everything the embedded
   gesture/phone/QR scripts import, and phone_bridge can open its websocket.
2. The packaged executable itself starts a 2-player phone session (via
   DS64_AUTO_MODE/DS64_AUTO_PLAYERS, so no setup dialog needs clicking),
   initializes its embedded Python (proven by the per-player QR PNGs it
   writes to the temp dir), serves the phone controller page on :8080,
   opens each player's websocket port, and is still running afterwards.

melonDS's stdout is block-buffered when piped, so the checks look at side
effects (files, HTTP) rather than log lines; the log is printed on failure.

Usage: python packaging/smoke_test.py <DS64.AppImage | bundle-dir | melonDS.app>
"""

import os
import platform
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

STARTUP_TIMEOUT = 90  # seconds; first launch also extracts the embedded ROM
WEB_PORT = 8080
BASE_WS_PORT = 8765
PLAYERS = 2

IMPORT_CHECK = r"""
import sys
sys.path[:0] = [p for p in sys.argv[1:] if p]
import cv2, numpy, qrcode, PIL, psutil, websockets
import mediapipe as mp
mp.solutions.hands  # gesture_engine.py uses the legacy solutions API
import lan_ip, gesture_engine, phone_bridge
session = phone_bridge.init(18765)
phone_bridge.shutdown(session)
print("bundled python OK:", sys.version.split()[0], sys.prefix)
"""


def fail(msg: str, log: Path | None = None) -> None:
    print(f"SMOKE TEST FAILED: {msg}", file=sys.stderr)
    if log and log.exists():
        print("---- melonDS output ----", file=sys.stderr)
        print(log.read_text(errors="replace")[-8000:], file=sys.stderr)
    sys.exit(1)


def layout(target: Path, scratch: Path):
    """Returns (launch argv, bundled python argv, env for python, script dirs)."""
    if target.suffix == ".AppImage":
        # Extract once so the bundled interpreter can be run directly; the
        # launch itself still goes through the real AppImage + AppRun.
        subprocess.run([str(target.resolve()), "--appimage-extract"], cwd=scratch,
                       check=True, stdout=subprocess.DEVNULL)
        root = scratch / "squashfs-root"
        share = root / "usr/share/ds64"
        launch = [str(target.resolve())]
        if not os.environ.get("DISPLAY") and shutil.which("xvfb-run"):
            launch = ["xvfb-run", "-a", "-s", "-screen 0 1280x1024x24"] + launch
        py = [str(root / "usr/bin/python3")]
        py_env = {"PYTHONHOME": str(root / "usr"), "PYTHONNOUSERSITE": "1"}
        return launch, py, py_env, [share, share / "tools/gesture", share / "tools/phone"], []

    if target.suffix == ".app":
        exe = target / "Contents/MacOS/melonDS"
        share = target / "Contents/Resources/ds64"
        home = target / "Contents/Frameworks/Python.framework/Versions/Current"
        # The bundle ships no standalone interpreter (melonDS embeds the
        # dylib), so check its stdlib, extension modules and wheels with the
        # runner's own Python 3.x pointed at it via PYTHONHOME. The bundled
        # dylib itself is exercised by the launch check (QR script and
        # websockets run inside melonDS). Run with the executable's
        # architecture, so an x86_64 bundle is checked under Rosetta.
        arch = subprocess.run(["lipo", "-archs", str(exe)], capture_output=True,
                              text=True).stdout.split()
        prefix = ["arch", "-x86_64"] if arch == ["x86_64"] else []
        py = prefix + [sys.executable]
        py_env = {"PYTHONHOME": str(home), "PYTHONNOUSERSITE": "1"}
        extra = [share / "site-packages"]
        return prefix + [str(exe)], py, py_env, [share, share / "tools/gesture", share / "tools/phone"], extra

    # Windows bundle directory.
    exe = target / "melonDS.exe"
    if not exe.is_file():
        fail(f"don't know how to launch {target}")
    py = [str(target / "python/python.exe")]
    py_env = {"PYTHONHOME": str(target / "python"), "PYTHONNOUSERSITE": "1"}
    return [str(exe)], py, py_env, [target, target / "tools/gesture", target / "tools/phone"], []


def check_bundled_python(py, py_env, script_dirs, extra_paths) -> None:
    env = {k: v for k, v in os.environ.items() if not k.startswith("PYTHON")}
    env.update(py_env)
    paths = [str(p) for p in extra_paths + script_dirs]
    result = subprocess.run(py + ["-c", IMPORT_CHECK] + paths, env=env,
                            capture_output=True, text=True, timeout=300)
    print(result.stdout, end="")
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        fail("the bundled Python cannot import DS64's dependencies")


def fetch(url: str):
    try:
        with urllib.request.urlopen(url, timeout=3) as resp:
            return resp.status, resp.read()
    except Exception as exc:  # noqa: BLE001 - any failure just means "not yet"
        return None, str(exc).encode()


def port_open(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=2):
            return True
    except OSError:
        return False


def check_launch(launch, scratch: Path) -> None:
    tmp = scratch / "tmp"
    home = scratch / "home"
    tmp.mkdir()
    home.mkdir()
    env = dict(os.environ)
    # Fresh temp dir (where the QR PNGs go) and a fresh user profile, so the
    # run behaves like a first launch on a clean machine.
    for var in ("TMPDIR", "TEMP", "TMP"):
        env[var] = str(tmp)
    if platform.system() == "Linux":
        env.update(HOME=str(home), XDG_CONFIG_HOME=str(home / ".config"),
                   XDG_DATA_HOME=str(home / ".local/share"))
        env.setdefault("QT_QPA_PLATFORM", "xcb")
    env["SDL_AUDIODRIVER"] = "dummy"
    # Skip the interactive "camera or phone?" / player-count dialogs.
    env["DS64_AUTO_MODE"] = "phone"
    env["DS64_AUTO_PLAYERS"] = str(PLAYERS)

    log = scratch / "melonDS.log"
    with open(log, "wb") as out:
        kwargs = {}
        if os.name == "nt":
            kwargs["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
        else:
            kwargs["start_new_session"] = True
        proc = subprocess.Popen(launch, env=env, stdout=out, stderr=subprocess.STDOUT, **kwargs)

    try:
        deadline = time.time() + STARTUP_TIMEOUT
        ws_ports = [BASE_WS_PORT + i for i in range(PLAYERS)]
        page = None
        while time.time() < deadline:
            if proc.poll() is not None:
                fail(f"melonDS exited early with code {proc.returncode}", log)
            qrs = sorted(p.name for p in tmp.glob("ds64_qr_p*.png"))
            if page is None:
                status, body = fetch(f"http://127.0.0.1:{WEB_PORT}/controller.html")
                if status == 200 and b"<html" in body.lower():
                    page = body
            ws_up = [port for port in ws_ports if port_open(port)]
            if page and len(qrs) >= PLAYERS and ws_up == ws_ports:
                break
            time.sleep(1)

        if not page:
            fail(f"controller page was not served on port {WEB_PORT}", log)
        if len(qrs) < PLAYERS:
            fail(f"expected {PLAYERS} QR codes, found {qrs} — embedded Python did not run", log)
        if ws_up != ws_ports:
            fail(f"phone websocket ports {sorted(set(ws_ports) - set(ws_up))} not listening", log)
        status, _ = fetch(f"http://127.0.0.1:{WEB_PORT}/../../etc/passwd")
        if status == 200:
            fail("web server served a file outside web/", log)

        time.sleep(5)  # let it boot the ROM / settle, then make sure it's alive
        if proc.poll() is not None:
            fail(f"melonDS crashed after startup (exit code {proc.returncode})", log)
        print(f"launch OK: served controller.html ({len(page)} bytes), wrote {', '.join(qrs)}, "
              f"websockets on {ws_ports}")
    finally:
        if proc.poll() is None:
            if os.name == "nt":
                proc.kill()
            else:
                os.killpg(proc.pid, signal.SIGTERM)
                try:
                    proc.wait(10)
                except subprocess.TimeoutExpired:
                    os.killpg(proc.pid, signal.SIGKILL)
        proc.wait()


def main() -> None:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    target = Path(sys.argv[1]).resolve()
    if not target.exists():
        fail(f"{target} does not exist")
    scratch = Path(tempfile.mkdtemp(prefix="ds64-smoke-"))
    try:
        launch, py, py_env, script_dirs, extra = layout(target, scratch)
        check_bundled_python(py, py_env, script_dirs, extra)
        check_launch(launch, scratch)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("SMOKE TEST PASSED")


if __name__ == "__main__":
    main()
