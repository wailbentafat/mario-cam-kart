// Python.h must be the first include in this file: it pins certain POSIX
// feature-test macros (_POSIX_C_SOURCE etc.) that must be set before any
// system or Qt header transitively pulls in <unistd.h>/<stdio.h>. Including
// Qt headers first causes macro-redefinition errors against Python.h.
#include <Python.h>

#include "PhoneController.h"
#include "main.h"
#include "EmuInstance.h"

#include <QDebug>
#include <QMutexLocker>

#include <chrono>
#include <thread>

namespace
{

QByteArray PyStrToQByteArray(PyObject* dict, const char* key)
{
    PyObject* v = PyDict_GetItemString(dict, key);
    if (!v || !PyUnicode_Check(v))
        return {};
    return QByteArray(PyUnicode_AsUTF8(v));
}

bool PyBoolFromDict(PyObject* dict, const char* key)
{
    PyObject* v = PyDict_GetItemString(dict, key);
    return v && PyObject_IsTrue(v);
}

} // namespace

PhoneController::PhoneController() = default;

PhoneController::~PhoneController()
{
    Shutdown();
}

void PhoneController::Prepare(const QString& dir, int listenPort)
{
    scriptDir = dir;
    port = listenPort;
}

void PhoneController::Poll(EmuInstance* emuInstance)
{
    if (!started.exchange(true))
        listenThread = std::thread(&PhoneController::WorkerLoop, this);

    if (!active.load() || !emuInstance)
        return;

    PhoneSnapshot snap;
    {
        QMutexLocker lock(&snapshotMutex);
        snap = snapshot;
    }
    connected.store(snap.connected);

    // Keyboard/mouse drive by default. A connected phone claims control;
    // a real key press or mouse click (see onKeyPress / ScreenPanel) hands
    // it straight back by clearing this flag. Shared with GestureController
    // in spirit, but each input source has its own *KeyMask so two phones
    // (one per instance) never interfere with each other.
    if (snap.connected)
        emuInstance->setGestureControlActive(true);

    if (!emuInstance->isGestureControlActive())
    {
        if (wasActiveLastPoll)
            emuInstance->setPhoneKeys(false, false, false, false, false, false);
        wasActiveLastPoll = false;
        return;
    }
    wasActiveLastPoll = true;

    emuInstance->setPhoneKeys(snap.left, snap.right, snap.accel, snap.brake, snap.drift, snap.item);
}

void PhoneController::WorkerLoop()
{
    // This thread is the only one that ever touches the Python C API for
    // this controller — EmuThread only reads the mutex-protected
    // snapshot — so one GIL acquisition for the whole thread lifetime is
    // correct and avoids per-iteration Ensure()/Release() overhead.
    if (ds64PythonInitializationFailed())
    {
        qWarning() << "PhoneController: Python runtime is unavailable";
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

        // websockets isn't installed for the system interpreter we embed
        // — reuse the project's venv (same one GestureController uses).
#ifdef GESTURE_VENV_SITE_PACKAGES
        QByteArray venvSite = ds64Path("DS64_VENV_SITE_PACKAGES", GESTURE_VENV_SITE_PACKAGES).toUtf8();
        PyObject* venvDir = PyUnicode_FromString(venvSite.constData());
        // Insert first so the venv's versions win over any older copies in
        // the system dist-packages or ~/.local (e.g. Debian's websockets).
        PyList_Insert(sysPath, 0, venvDir);
        Py_DECREF(venvDir);
#endif
    }

    PyObject* name = PyUnicode_FromString("phone_bridge");
    PyObject* mod = PyImport_Import(name);
    Py_DECREF(name);

    if (!mod)
    {
        PyErr_Print();
        qWarning() << "PhoneController: failed to import phone_bridge";
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
        qWarning() << "PhoneController: phone_bridge missing init/poll/shutdown";
        PyGILState_Release(gstate);
        return;
    }
    initFunc = initFn;
    pollFunc = pollFn;
    shutdownFunc = shutdownFn;

    PyObject* portArg = PyLong_FromLong(port);
    PyObject* initResult = PyObject_CallFunctionObjArgs((PyObject*)initFunc, portArg, nullptr);
    Py_DECREF(portArg);
    bool ok = initResult && initResult != Py_None;
    if (!initResult)
        PyErr_Print();

    // init() returns a per-instance session object (not a bool!) — every
    // EmuInstance shares the same phone_bridge module object (CPython
    // caches imports by name), so this is what keeps each phone's
    // connection/state from clobbering the others'. Keep the reference
    // alive for the lifetime of this controller; released in the
    // shutdown path below.
    if (ok)
        sessionObj = initResult;
    else
        Py_XDECREF(initResult);

    active.store(ok);
    if (!ok)
    {
        qWarning() << "PhoneController: failed to bind port" << port;
        PyGILState_Release(gstate);
        return;
    }

    // Poll roughly in step with the emulated frame rate; unlike the
    // camera (naturally paced by cap.read()), a WS server with no
    // traffic would spin this loop as fast as possible otherwise.
    while (!stopRequested.load())
    {
        PyObject* result = PyObject_CallFunctionObjArgs((PyObject*)pollFunc, (PyObject*)sessionObj, nullptr);
        if (!result)
        {
            PyErr_Print();
            break;
        }

        PhoneSnapshot snap;
        snap.connected = PyBoolFromDict(result, "connected");
        QByteArray direction = PyStrToQByteArray(result, "direction");
        QByteArray throttle  = PyStrToQByteArray(result, "throttle");
        snap.left  = direction == "LEFT";
        snap.right = direction == "RIGHT";
        snap.accel = throttle == "ACCEL";
        snap.brake = throttle == "BRAKE";
        snap.drift = PyBoolFromDict(result, "drift");
        snap.item  = PyBoolFromDict(result, "item");

        Py_DECREF(result);

        {
            QMutexLocker lock(&snapshotMutex);
            snapshot = snap;
        }

        PyGILState_Release(gstate);
        std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60Hz
        gstate = PyGILState_Ensure();
    }

    if (shutdownFunc)
    {
        PyObject* result = PyObject_CallFunctionObjArgs((PyObject*)shutdownFunc, (PyObject*)sessionObj, nullptr);
        if (!result)
            PyErr_Print();
        Py_XDECREF(result);
    }

    Py_XDECREF((PyObject*)sessionObj);
    Py_XDECREF((PyObject*)initFunc);
    Py_XDECREF((PyObject*)pollFunc);
    Py_XDECREF((PyObject*)shutdownFunc);
    Py_XDECREF((PyObject*)module);
    sessionObj = initFunc = pollFunc = shutdownFunc = module = nullptr;

    active.store(false);
    PyGILState_Release(gstate);
}

void PhoneController::Shutdown()
{
    stopRequested.store(true);
    if (listenThread.joinable())
        listenThread.join();
}
