// Python.h must be the first include in this file: it pins certain POSIX
// feature-test macros (_POSIX_C_SOURCE etc.) that must be set before any
// system or Qt header transitively pulls in <unistd.h>/<stdio.h>. Including
// Qt headers first causes macro-redefinition errors against Python.h.
#include <Python.h>

#include "GestureController.h"
#include "main.h"
#include "EmuInstance.h"

#include <QDebug>
#include <QMutexLocker>

namespace
{

QByteArray PyStrToQByteArray(PyObject* dict, const char* key)
{
    PyObject* v = PyDict_GetItemString(dict, key);
    if (!v || !PyUnicode_Check(v))
        return {};
    return QByteArray(PyUnicode_AsUTF8(v));
}

long PyLongFromDict(PyObject* dict, const char* key)
{
    PyObject* v = PyDict_GetItemString(dict, key);
    return v ? PyLong_AsLong(v) : 0;
}

bool PyBoolFromDict(PyObject* dict, const char* key)
{
    PyObject* v = PyDict_GetItemString(dict, key);
    return v && PyObject_IsTrue(v);
}

} // namespace

GestureController::GestureController() = default;

GestureController::~GestureController()
{
    Shutdown();
}

void GestureController::Prepare(const QString& dir)
{
    scriptDir = dir;
}

void GestureController::Poll(EmuInstance* emuInstance)
{
    if (!started.exchange(true))
        captureThread = std::thread(&GestureController::WorkerLoop, this);

    if (!active.load() || !emuInstance)
        return;

    GestureSnapshot snap;
    {
        QMutexLocker lock(&snapshotMutex);
        snap = snapshot;
    }

    // Keyboard/mouse drive by default. Showing both hands claims control;
    // a real key press or mouse click (see onKeyPress / ScreenPanel) hands
    // it straight back by clearing this flag.
    if (snap.bothHandsVisible)
        emuInstance->setGestureControlActive(true);

    if (!emuInstance->isGestureControlActive())
    {
        if (wasActiveLastPoll)
            emuInstance->setGestureKeys(false, false, false, false, false); // release
                // our own held keys on the way out, but never touch the
                // touch screen here — a real mouse drag may be in progress.
        wasActiveLastPoll = false;
        return;
    }
    wasActiveLastPoll = true;

    emuInstance->setGestureKeys(snap.left, snap.right, snap.accel, snap.brake, snap.item);
    if (snap.touching)
        emuInstance->touchScreen(snap.touchX, snap.touchY);
    else
        emuInstance->releaseScreen();
}

QImage GestureController::WebcamFrame()
{
    QMutexLocker lock(&snapshotMutex);
    return snapshot.frame;
}

void GestureController::WorkerLoop()
{
    // This thread is the only one that ever touches the Python C API —
    // EmuThread (and the main thread) only read the mutex-protected
    // snapshot, never Python objects directly — so one GIL acquisition
    // for the whole thread lifetime is correct and avoids per-iteration
    // Ensure()/Release() overhead.
    if (ds64PythonInitializationFailed())
    {
        qWarning() << "GestureController: Python runtime is unavailable";
        return;
    }
    if (!Py_IsInitialized())
        Py_InitializeEx(0);
    PyGILState_STATE gstate = PyGILState_Ensure();

    PyObject* sysPath = PySys_GetObject("path");
    if (sysPath)
    {
        PyObject* dir = PyUnicode_FromString(scriptDir.toUtf8().constData());
        PyList_Append(sysPath, dir);
        Py_DECREF(dir);

        // cv2/mediapipe aren't installed for the system interpreter we
        // embed — reuse the project's venv (built against the same Python
        // version) instead of requiring a separate global install.
#ifdef GESTURE_VENV_SITE_PACKAGES
        QByteArray venvSite = ds64Path("DS64_VENV_SITE_PACKAGES", GESTURE_VENV_SITE_PACKAGES).toUtf8();
        PyObject* venvDir = PyUnicode_FromString(venvSite.constData());
        // Insert first so the venv's versions win over any older copies in
        // the system dist-packages or ~/.local (e.g. Debian's websockets).
        PyList_Insert(sysPath, 0, venvDir);
        Py_DECREF(venvDir);
#endif
    }

    PyObject* name = PyUnicode_FromString("gesture_engine");
    PyObject* mod = PyImport_Import(name);
    Py_DECREF(name);

    if (!mod)
    {
        PyErr_Print();
        qWarning() << "GestureController: failed to import gesture_engine";
        PyGILState_Release(gstate);
        return;
    }
    module = mod;

    PyObject* initFn = PyObject_GetAttrString(mod, "init");
    PyObject* pollFn = PyObject_GetAttrString(mod, "poll");
    PyObject* shutdownFn = PyObject_GetAttrString(mod, "shutdown");

    if (!initFn || !pollFn || !shutdownFn)
    {
        PyErr_Print();
        qWarning() << "GestureController: gesture_engine missing init/poll/shutdown";
        PyGILState_Release(gstate);
        return;
    }
    initFunc = initFn;
    pollFunc = pollFn;
    shutdownFunc = shutdownFn;

    PyObject* initResult = PyObject_CallObject((PyObject*)initFunc, nullptr);
    bool ok = initResult && PyObject_IsTrue(initResult);
    if (!initResult)
        PyErr_Print();
    Py_XDECREF(initResult);

    active.store(ok);
    if (!ok)
    {
        qWarning() << "GestureController: camera/hand-tracking init failed "
                       "(check camera permission and requirements.txt deps)";
        PyGILState_Release(gstate);
        return;
    }

    while (!stopRequested.load())
    {
        PyObject* result = PyObject_CallObject((PyObject*)pollFunc, nullptr);
        if (!result)
        {
            PyErr_Print();
            break;
        }

        GestureSnapshot snap;
        QByteArray direction = PyStrToQByteArray(result, "direction");
        QByteArray throttle  = PyStrToQByteArray(result, "throttle");
        snap.left     = direction == "LEFT";
        snap.right    = direction == "RIGHT";
        snap.accel    = throttle == "ACCEL";
        snap.brake    = throttle == "BRAKE";
        snap.item               = PyBoolFromDict(result, "item");
        snap.touching          = PyBoolFromDict(result, "touching");
        snap.touchX            = (int)PyLongFromDict(result, "touch_x");
        snap.touchY            = (int)PyLongFromDict(result, "touch_y");
        snap.bothHandsVisible  = PyBoolFromDict(result, "both_visible");

        PyObject* frameObj = PyDict_GetItemString(result, "frame");
        int frameW = (int)PyLongFromDict(result, "frame_w");
        int frameH = (int)PyLongFromDict(result, "frame_h");

        if (frameObj && PyBytes_Check(frameObj) && frameW > 0 && frameH > 0)
        {
            Py_ssize_t len = PyBytes_Size(frameObj);
            if (len >= (Py_ssize_t)frameW * frameH * 3)
            {
                const uchar* data = (const uchar*)PyBytes_AsString(frameObj);
                QImage img(data, frameW, frameH, frameW * 3, QImage::Format_BGR888);
                snap.frame = img.copy(); // detach from Python-owned buffer
            }
        }

        Py_DECREF(result);

        {
            QMutexLocker lock(&snapshotMutex);
            snapshot = snap;
        }
        // No extra sleep: gesture_engine.poll() blocks on cap.read() at
        // the camera's own frame rate, which naturally paces this loop
        // independently of NDS emulation speed.
    }

    if (shutdownFunc)
    {
        PyObject* result = PyObject_CallObject((PyObject*)shutdownFunc, nullptr);
        if (!result)
            PyErr_Print();
        Py_XDECREF(result);
    }

    Py_XDECREF((PyObject*)initFunc);
    Py_XDECREF((PyObject*)pollFunc);
    Py_XDECREF((PyObject*)shutdownFunc);
    Py_XDECREF((PyObject*)module);
    initFunc = pollFunc = shutdownFunc = module = nullptr;

    active.store(false);
    PyGILState_Release(gstate);
}

void GestureController::Shutdown()
{
    stopRequested.store(true);
    if (captureThread.joinable())
        captureThread.join();
}
