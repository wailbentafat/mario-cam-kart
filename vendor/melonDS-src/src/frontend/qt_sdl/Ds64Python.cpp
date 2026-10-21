/*
    DS64 runtime helper: runs small bundled Python scripts (currently the
    per-player QR code generator) using the same embedded interpreter the
    gesture/phone controllers use. Kept in its own translation unit so
    <Python.h> is included before any Qt/system header, as required on POSIX.
*/

#include <Python.h>

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>

#include <cstdio>
#include <string>

static bool g_ds64PythonInitFailed = false;

bool ds64PythonInitializationFailed()
{
    return g_ds64PythonInitFailed;
}

static bool ds64InitPython(const QString& pythonHome)
{
    PyConfig config;
    PyConfig_InitPythonConfig(&config);
    config.parse_argv = 0;
    config.user_site_directory = 0;
    // Match Py_InitializeEx(0): never take over Qt's/console's SIGINT.
    config.install_signal_handlers = 0;

    // On Windows the bundled python312.dll can have its own copy of the
    // static CRT's environment block, so setting PYTHONHOME with qputenv()
    // is not enough for it to find python\Lib. Pass the home explicitly to
    // Python's config instead. On macOS this also avoids relying on the
    // framework being found relative to the executable.
    if (!pythonHome.isEmpty())
    {
        const std::wstring home = pythonHome.toStdWString();
        PyStatus status = PyConfig_SetString(&config, &config.home, home.c_str());
        if (PyStatus_Exception(status))
        {
            fprintf(stderr, "DS64: could not set Python home to %s%s%s\n",
                    pythonHome.toUtf8().constData(),
                    status.err_msg ? ": " : "",
                    status.err_msg ? status.err_msg : "");
            PyConfig_Clear(&config);
            g_ds64PythonInitFailed = true;
            return false;
        }
    }

    PyStatus status = Py_InitializeFromConfig(&config);
    PyConfig_Clear(&config);
    if (PyStatus_Exception(status))
    {
        fprintf(stderr, "DS64: Python initialization failed%s%s\n",
                status.err_msg ? ": " : "",
                status.err_msg ? status.err_msg : "");
        g_ds64PythonInitFailed = true;
        return false;
    }
    g_ds64PythonInitFailed = false;
    return true;
}

bool ds64RunPythonScript(const QString& scriptPath, const QStringList& args,
                         const QString& pythonHome, const QString& sitePackages)
{
    const bool startedHere = !Py_IsInitialized();
    if (startedHere && !ds64InitPython(pythonHome))
        return false;

    bool ok = false;
    PyGILState_STATE gstate = PyGILState_Ensure();

    do
    {
        PyObject* sysPath = PySys_GetObject("path");
        if (!sysPath)
            break;

        const QByteArray scriptDir = QFileInfo(scriptPath).absolutePath().toUtf8();
        PyObject* dirObj = PyUnicode_FromString(scriptDir.constData());
        if (!dirObj)
            break;
        PyList_Insert(sysPath, 0, dirObj);
        Py_DECREF(dirObj);

        // Match GestureController/PhoneController: make bundled site-packages
        // win over any older copies on the system path.
        if (!sitePackages.isEmpty())
        {
            PyObject* siteObj = PyUnicode_FromString(sitePackages.toUtf8().constData());
            if (siteObj)
            {
                PyList_Insert(sysPath, 0, siteObj);
                Py_DECREF(siteObj);
            }
        }

        PyObject* argv = PyList_New(0);
        if (!argv)
            break;
        auto appendArg = [&argv](const QByteArray& value) {
            PyObject* item = PyUnicode_FromString(value.constData());
            if (item)
            {
                PyList_Append(argv, item);
                Py_DECREF(item);
            }
        };
        appendArg(scriptPath.toUtf8());
        for (const QString& arg : args)
            appendArg(arg.toUtf8());
        PySys_SetObject("argv", argv);
        Py_DECREF(argv);

        QFile scriptFile(scriptPath);
        if (!scriptFile.open(QIODevice::ReadOnly))
        {
            fprintf(stderr, "DS64: could not open Python script %s\n",
                    scriptPath.toUtf8().constData());
            break;
        }
        const QByteArray source = scriptFile.readAll();
        scriptFile.close();

        PyObject* mainModule = PyImport_AddModule("__main__");
        PyObject* mainDict = mainModule ? PyModule_GetDict(mainModule) : nullptr;
        if (!mainDict)
            break;

        // PyRun_String does not populate __file__ like the normal interpreter
        // does; bundled scripts use it to locate their sibling files.
        PyObject* fileObj = PyUnicode_FromString(scriptPath.toUtf8().constData());
        if (fileObj)
        {
            PyDict_SetItemString(mainDict, "__file__", fileObj);
            Py_DECREF(fileObj);
        }

        PyObject* result = PyRun_String(source.constData(), Py_file_input, mainDict, mainDict);
        if (!result)
        {
            PyErr_Print();
            break;
        }
        Py_DECREF(result);
        ok = true;
    } while (false);

    PyGILState_Release(gstate);
    if (startedHere && Py_IsInitialized())
        PyEval_SaveThread(); // leave the interpreter usable by the worker threads

    return ok;
}
