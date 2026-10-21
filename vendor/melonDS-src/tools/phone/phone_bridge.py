"""
Embedded inside melonDS via the CPython C API (see PhoneController.cpp).

Each EmuInstance that picks "Phone" at the DS64 Setup dialog gets its own
copy of this module (one per instance, one port per instance) — this is
what makes multi-phone local multiplayer work: every melonDS instance
has its own focus-independent, process-direct phone listener, instead of
one external process trying to guess which OS-focused window to send
synthetic keystrokes to.

All EmuInstances in a session share ONE embedded Python interpreter, and
CPython caches imported modules by name (sys.modules) — so every
PhoneController's `import phone_bridge` returns the exact same module
object. That means init()/poll()/shutdown() must never keep state in a
module-level global: two+ players would silently share one `_state`
dict, each phone's input overwriting the other's (this was a real bug —
players "fighting" each other, one's brake landing on the other's kart).
init() instead returns a per-call _Session object that the caller must
pass back into poll()/shutdown(), so each instance's state is isolated
regardless of the shared module.

melonDS calls init(port) once, then poll(session) repeatedly in a loop
on its own dedicated thread (see PhoneController::WorkerLoop), same
contract as tools/gesture/gesture_engine.py except session-scoped.
"""

import json
import threading

from websockets.exceptions import ConnectionClosed
from websockets.sync.server import serve

STEER_DEADZONE = 0.25

# Close code sent to a phone that connects to a player slot another phone
# already owns (4000-4999 is the application-defined range). controller.html
# shows a dedicated message for it instead of a generic "disconnected".
SLOT_TAKEN_CODE = 4001

# A phone that vanishes without a clean close (Wi-Fi drop, screen lock)
# still holds its slot until keepalive notices — keep that window short so
# the player can rejoin, instead of websockets' 20s+20s default.
PING_INTERVAL = 5
PING_TIMEOUT = 5
CLOSE_TIMEOUT = 2

_IDLE_STATE = {"steer": 0.0, "accel": False, "brake": False, "drift": False, "item": False}


class _Session:
    def __init__(self):
        self.lock = threading.Lock()
        # The one phone driving this player, or None. Exactly one phone per
        # port: a second one (e.g. both players scanned the same QR) used to
        # be accepted too, interleaving its input with the first's and
        # flipping `connected` to False when either one left.
        self.owner = None
        self.latest = dict(_IDLE_STATE)
        self.server = None
        self.thread = None


def _make_handler(session: _Session):
    def _handler(websocket):
        with session.lock:
            if session.owner is not None:
                rejected = True
            else:
                rejected = False
                session.owner = websocket
        if rejected:
            websocket.close(SLOT_TAKEN_CODE, "player slot already taken")
            return

        try:
            for message in websocket:
                try:
                    data = json.loads(message)
                except (ValueError, TypeError):
                    continue
                with session.lock:
                    session.latest = {
                        "steer": float(data.get("steer", 0.0)),
                        "accel": bool(data.get("accel", False)),
                        "brake": bool(data.get("brake", False)),
                        "drift": bool(data.get("drift", False)),
                        "item": bool(data.get("item", False)),
                    }
        except ConnectionClosed:
            pass  # phone dropped without a clean close (keepalive timeout)
        finally:
            with session.lock:
                session.owner = None
                session.latest = dict(_IDLE_STATE)

    return _handler


def init(port: int):
    """Returns an opaque per-instance session object, or None on failure.
    Pass it back into poll()/shutdown() — never share it between ports."""
    session = _Session()

    try:
        server = serve(_make_handler(session), "0.0.0.0", port,
                       ping_interval=PING_INTERVAL, ping_timeout=PING_TIMEOUT,
                       close_timeout=CLOSE_TIMEOUT)
    except OSError:
        return None

    session.server = server
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    session.thread = thread
    return session


def shutdown(session) -> None:
    if session is None:
        return
    if session.server is not None:
        session.server.shutdown()
    session.server = None
    session.thread = None


def poll(session) -> dict:
    """
    Returns a dict:
      connected: bool — whether a phone is currently attached
      direction: "LEFT" | "RIGHT" | "STRAIGHT"
      throttle:  "ACCEL" | "BRAKE" | "NEUTRAL"
      drift, item: bool
    Same vocabulary as gesture_engine.poll() so the C++ side (and the
    arbitration logic that hands control back on real keyboard/mouse
    input) can treat both input sources identically.
    """
    if session is None:
        return {"connected": False, "direction": "STRAIGHT", "throttle": "NEUTRAL", "drift": False, "item": False}

    with session.lock:
        connected = session.owner is not None
        latest = dict(session.latest)

    steer = latest["steer"]
    direction = "STRAIGHT"
    if steer <= -STEER_DEADZONE:
        direction = "LEFT"
    elif steer >= STEER_DEADZONE:
        direction = "RIGHT"

    if latest["brake"]:
        throttle = "BRAKE"
    elif latest["accel"]:
        throttle = "ACCEL"
    else:
        throttle = "NEUTRAL"

    return {
        "connected": connected,
        "direction": direction,
        "throttle": throttle,
        "drift": latest["drift"],
        "item": latest["item"],
    }
