/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <string>

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QStyle>
#include <QMessageBox>
#include <QMenuBar>
#include <QFileDialog>
#include <QInputDialog>
#include <QPainter>
#include <QKeyEvent>
#include <QMimeData>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QVector>
#include <QCommandLineParser>
#include <QStandardPaths>
#ifndef _WIN32
#include <QGuiApplication>
#include <QSocketNotifier>
#include <unistd.h>
#include <sys/socket.h>
#include <csignal>
#endif

#include <SDL2/SDL.h>

#include "OpenGLSupport.h"
#include "duckstation/gl/context.h"

#include "main.h"
#include "version.h"

#include "Config.h"

#include "EmuInstance.h"
#include "ArchiveUtil.h"
#include "CameraManager.h"
#include "MPInterface.h"
#include "Net.h"

#include "CLI.h"

#include "Net_PCap.h"
#include "Net_Slirp.h"

using namespace melonDS;

QString* systemThemeName;


QString emuDirectory;

const int kMaxEmuInstances = 16;
EmuInstance* emuInstances[kMaxEmuInstances];

int g_DS64PendingPhonePort = 0;
int g_DS64TotalPlayers = 1;
QStringList g_DS64ROMPath;

QString ds64Path(const char* envVar, const char* builtIn)
{
    QString fromEnv = qEnvironmentVariable(envVar);
    return fromEnv.isEmpty() ? QString(builtIn) : fromEnv;
}

CameraManager* camManager[2];
bool camStarted[2];

std::optional<LibPCap> pcap;
Net net;


QElapsedTimer sysTimer;


void NetInit()
{
    Config::Table cfg = Config::GetGlobalTable();
    if (cfg.GetBool("LAN.DirectMode"))
    {
        if (!pcap)
            pcap = LibPCap::New();

        if (pcap)
        {
            std::string devicename = cfg.GetString("LAN.Device");
            std::unique_ptr<Net_PCap> netPcap = pcap->Open(devicename, [](const u8* data, int len) {
                net.RXEnqueue(data, len);
            });

            if (netPcap)
            {
                net.SetDriver(std::move(netPcap));
            }
        }
    }
    else
    {
        net.SetDriver(std::make_unique<Net_Slirp>([](const u8* data, int len) {
            net.RXEnqueue(data, len);
        }));
    }
}


bool createEmuInstance()
{
    int id = -1;
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (!emuInstances[i])
        {
            id = i;
            break;
        }
    }

    if (id == -1)
        return false;

    auto inst = new EmuInstance(id);
    emuInstances[id] = inst;

    return true;
}

void deleteEmuInstance(int id)
{
    auto inst = emuInstances[id];
    if (!inst) return;

    delete inst;
    emuInstances[id] = nullptr;
}

void deleteAllEmuInstances(int first)
{
    for (int i = first; i < kMaxEmuInstances; i++)
        deleteEmuInstance(i);
}

int numEmuInstances()
{
    int ret = 0;

    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (emuInstances[i])
            ret++;
    }

    return ret;
}

EmuInstance* getEmuInstanceByID(int id)
{
    if (id < 0 || id >= kMaxEmuInstances)
        return nullptr;

    return emuInstances[id];
}


void broadcastInstanceCommand(int cmd, QVariant& param, int sourceinst)
{
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        if (i == sourceinst) continue;
        if (!emuInstances[i]) continue;

        emuInstances[i]->handleCommand(cmd, param);
    }
}


void pathInit()
{
    // First, check for the portable directory next to the executable.
    QString appdirpath = QCoreApplication::applicationDirPath();
    QString portablepath = appdirpath + QDir::separator() + "portable";

#if defined(__APPLE__)
    // On Apple platforms we may need to navigate outside an app bundle.
    // The executable directory would be "melonDS.app/Contents/MacOS", so we need to go a total of three steps up.
    QDir bundledir(appdirpath);
    if (bundledir.cd("..") && bundledir.cd("..") && bundledir.dirName().endsWith(".app") && bundledir.cd(".."))
    {
        portablepath = bundledir.absolutePath() + QDir::separator() + "portable";
    }
#endif

    QDir portabledir(portablepath);
    if (portabledir.exists())
    {
        emuDirectory = portabledir.absolutePath();
    }
    else
    {
        // If no overrides are specified, use the default path.
#if defined(__WIN32__) && defined(WIN32_PORTABLE)
        emuDirectory = appdirpath;
#else
        QString confdir;
        QDir config(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation));
        // mkpath, not mkdir: on a fresh account ~/.config itself may not
        // exist yet, and then Config::Load() fails with a modal error.
        config.mkpath("melonDS");
        confdir = config.absolutePath() + QDir::separator() + "melonDS";
        emuDirectory = confdir;
#endif
    }
}

// ---------------------------------------------------------- bundled runtime --
//
// Release builds ship the same pieces the Linux AppImage's AppRun used to set
// up by hand: a Python runtime with DS64's dependencies, the gesture/phone
// scripts, and web/controller.html. The code below finds that bundle next to
// the executable (or in the macOS .app's Resources) and gives the rest of the
// program the same environment AppRun provided, then starts the phone page
// server and pre-generates the per-player QR codes. Keeping this in the
// executable means the Windows .exe and macOS .app behave exactly like the
// AppImage when launched directly, with no helper script.
static QString g_ds64BundleDir;
static QString g_ds64PythonHome;
static QString g_ds64SitePackages;
static QString g_ds64WebRoot;
static QTcpServer* g_ds64WebServer = nullptr;

static QString ds64FindBundleDir()
{
    const QByteArray fromEnv = qgetenv("DS64_BUNDLE_DIR");
    if (!fromEnv.isEmpty())
    {
        const QString dir = QString::fromLocal8Bit(fromEnv);
        if (QFileInfo(dir + "/web").isDir())
            return QDir(dir).absolutePath();
    }

    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates;
#ifdef Q_OS_MAC
    // CMake puts our resources in Contents/Resources/ds64 (see CI packaging).
    candidates << appDir + "/../Resources/ds64";
#endif
    candidates << appDir + "/ds64";
    candidates << appDir;

    for (const QString& candidate : candidates)
    {
        if (QFileInfo(candidate + "/web").isDir())
            return QDir(candidate).absolutePath();
    }
    return {};
}

static QString ds64FirstExistingDir(const QStringList& candidates)
{
    for (const QString& candidate : candidates)
    {
        if (QFileInfo(candidate).isDir())
            return candidate;
    }
    return {};
}

static QString ds64PythonSitePackages(const QString& pythonHome)
{
    QStringList candidates;
#ifdef Q_OS_WIN
    candidates << pythonHome + "/Lib/site-packages";
#else
    QDir libDir(pythonHome + "/lib");
    for (const QString& entry : libDir.entryList(QStringList() << "python3.*", QDir::Dirs | QDir::NoDotAndDotDot))
        candidates << libDir.filePath(entry) + "/site-packages";
#endif
    return ds64FirstExistingDir(candidates);
}

static void ds64ConfigureBundledRuntime()
{
    g_ds64BundleDir = ds64FindBundleDir();
    if (g_ds64BundleDir.isEmpty())
        return;

    if (!qEnvironmentVariableIsSet("DS64_BUNDLE_DIR"))
        qputenv("DS64_BUNDLE_DIR", g_ds64BundleDir.toUtf8());

    g_ds64SitePackages = QString::fromLocal8Bit(qgetenv("DS64_VENV_SITE_PACKAGES"));

    const QString toolsDir = g_ds64BundleDir + "/tools";
    if (!qEnvironmentVariableIsSet("DS64_TOOLS_DIR") && QFileInfo(toolsDir).isDir())
        qputenv("DS64_TOOLS_DIR", toolsDir.toUtf8());

    g_ds64WebRoot = QDir(g_ds64BundleDir + "/web").canonicalPath();

    // Layouts used by packaging:
    //   Windows: <bundle>/python (Lib/, DLLs/, Python DLL next to the exe)
    //   macOS:   melonDS.app/Contents/Frameworks/Python.framework/Versions/X
    //   AppImage: DS64_BUNDLE_DIR is set by AppRun and PYTHONHOME is already
    //             configured there.
    QString pythonHome;
    QStringList pythonHomeCandidates;
    pythonHomeCandidates << g_ds64BundleDir + "/python";
#ifdef Q_OS_MAC
    QDir frameworksDir(QCoreApplication::applicationDirPath() + "/../Frameworks/Python.framework/Versions");
    // Current is the version the bundle was built for (make-bundle.sh);
    // the rest are only a fallback.
    pythonHomeCandidates << frameworksDir.filePath("Current");
    for (const QString& version : frameworksDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        pythonHomeCandidates << frameworksDir.filePath(version);
#endif

    for (const QString& candidate : pythonHomeCandidates)
    {
        QStringList libCandidates;
#ifdef Q_OS_WIN
        libCandidates << candidate + "/Lib";
#else
        libCandidates << candidate + "/lib";
#endif
        if (ds64FirstExistingDir(libCandidates).isEmpty())
            continue;
        pythonHome = candidate;
        break;
    }
    g_ds64PythonHome = pythonHome;

    if (!pythonHome.isEmpty())
    {
        // Set this before any controller calls Py_InitializeEx(). The installed
        // Python finds Lib/, DLLs/ and Lib/site-packages (Windows) or the
        // framework's stdlib/site-packages (macOS) from here.
        if (!qEnvironmentVariableIsSet("PYTHONHOME"))
            qputenv("PYTHONHOME", pythonHome.toUtf8());
        if (!qEnvironmentVariableIsSet("PYTHONNOUSERSITE"))
            qputenv("PYTHONNOUSERSITE", "1");
        qunsetenv("PYTHONPATH");

        // macOS packages are staged separately under Resources/ds64 so the
        // target-arch wheels don't have to live inside the Python framework.
        QString sitePackages = g_ds64BundleDir + "/site-packages";
        if (!QFileInfo(sitePackages).isDir())
            sitePackages = ds64PythonSitePackages(pythonHome);
        if (!sitePackages.isEmpty())
        {
            g_ds64SitePackages = sitePackages;
            if (!qEnvironmentVariableIsSet("DS64_VENV_SITE_PACKAGES"))
                qputenv("DS64_VENV_SITE_PACKAGES", sitePackages.toUtf8());
        }
    }

    printf("DS64: bundled runtime at %s\n", g_ds64BundleDir.toUtf8().constData());
}

static void ds64GenerateQRs()
{
    if (g_ds64BundleDir.isEmpty())
        return;

    const QString script = g_ds64BundleDir + "/make_controller_qr.py";
    if (!QFileInfo(script).isFile())
        return;

    if (ds64RunPythonScript(script, QStringList() << "8080" << "8765",
                            g_ds64PythonHome, g_ds64SitePackages))
        printf("DS64: phone controller QR codes generated\n");
    else
        qWarning() << "DS64: QR generation failed (check the bundled Python output above)";
}

static void ds64HttpReadyRead(QTcpSocket* socket)
{
    if (socket->property("ds64Handled").toBool())
        return;

    QByteArray request = socket->property("ds64Buffer").toByteArray();
    request += socket->readAll();
    socket->setProperty("ds64Buffer", request);

    if (request.size() > 16384)
    {
        socket->abort();
        return;
    }

    const int headerEnd = request.indexOf("\r\n\r\n");
    if (headerEnd < 0)
        return;
    socket->setProperty("ds64Handled", true);

    const QList<QByteArray> requestLines = request.left(headerEnd).split('\n');
    const QList<QByteArray> requestLine = requestLines.value(0).trimmed().split(' ');
    const QByteArray method = requestLine.value(0);
    QByteArray target = requestLine.value(1);
    const int query = target.indexOf('?');
    if (query >= 0)
        target.truncate(query);

    QString path = QUrl::fromPercentEncoding(target);
    if (path.isEmpty() || path == "/")
        path = "/controller.html";

    QByteArray status = "200 OK";
    QByteArray body;
    QByteArray contentType = "application/octet-stream";

    const QString root = QDir(g_ds64WebRoot).canonicalPath();
    const QString filePath = QDir(root).filePath(path.mid(1));
    const QString canonicalFile = QFileInfo(filePath).canonicalFilePath();
    // QFileInfo::canonicalFilePath() uses '/' on every platform (including
    // Windows), not QDir::separator(), which is '\' there.
    if (!canonicalFile.startsWith(root + QLatin1Char('/')) || !QFileInfo(canonicalFile).isFile())
    {
        status = "404 Not Found";
        body = "Not found";
        contentType = "text/plain; charset=utf-8";
    }
    else
    {
        QFile file(canonicalFile);
        if (!file.open(QIODevice::ReadOnly))
        {
            status = "500 Internal Server Error";
            body = "Could not read file";
            contentType = "text/plain; charset=utf-8";
        }
        else
        {
            body = file.readAll();
            contentType = QMimeDatabase().mimeTypeForFile(canonicalFile).name().toUtf8();
        }
    }

    QByteArray response = "HTTP/1.1 " + status + "\r\n";
    response += "Content-Type: " + contentType + "\r\n";
    response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += "Connection: close\r\n\r\n";
    socket->write(response);
    if (method != "HEAD")
        socket->write(body);
    socket->disconnectFromHost();
}

static void ds64StartWebServer()
{
    if (g_ds64WebServer || g_ds64WebRoot.isEmpty())
        return;

    auto* server = new QTcpServer(qApp);
    if (!server->listen(QHostAddress::Any, 8080))
    {
        qWarning() << "DS64: could not start the controller web server on port 8080";
        delete server;
        return;
    }

    QObject::connect(server, &QTcpServer::newConnection, server, [server]() {
        while (QTcpSocket* socket = server->nextPendingConnection())
        {
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket]() {
                ds64HttpReadyRead(socket);
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        }
    });

    g_ds64WebServer = server;
    printf("DS64: controller page served on port 8080\n");
}

void ds64StartBundledServices()
{
    // Called only after the setup dialog chooses phone mode. Camera/keyboard
    // sessions never start the QR-generation Python interpreter or open the
    // controller HTTP port.
    if (g_ds64BundleDir.isEmpty())
        return;

    ds64GenerateQRs();
    ds64StartWebServer();
}

void ds64StopBundledServices()
{
    if (!g_ds64WebServer)
        return;
    g_ds64WebServer->close();
    delete g_ds64WebServer;
    g_ds64WebServer = nullptr;
}

#ifdef DS64_EMBEDDED_ROM_PATH
#ifdef Q_OS_LINUX
// Older personal AppImages kept the ROM and its .sav in
// $XDG_DATA_HOME/ds64/<rom name>.sav. The embedded-ROM releases use
// <AppDataLocation>/rom/game.sav, so bring an existing save across once.
static void ds64MigrateLegacySave(const QString& romDir)
{
    const QString newSave = romDir + QStringLiteral("/game.sav");
    if (QFileInfo::exists(newSave))
        return;

    QStringList legacyDirs;
    const QByteArray xdgData = qgetenv("XDG_DATA_HOME");
    if (!xdgData.isEmpty())
        legacyDirs << QString::fromLocal8Bit(xdgData) + QStringLiteral("/ds64");
    legacyDirs << QDir::homePath() + QStringLiteral("/.local/share/ds64");
    legacyDirs.removeDuplicates();

    for (const QString& legacyDir : legacyDirs)
    {
        QDir dir(legacyDir);
        const QStringList saves = dir.entryList(QStringList() << QStringLiteral("*.sav"),
                                                QDir::Files, QDir::Name);
        if (saves.isEmpty())
            continue;

        const QString legacySave = dir.filePath(saves.first());
        if (QFile::copy(legacySave, newSave))
        {
            printf("DS64: migrated save %s -> %s\n",
                   legacySave.toUtf8().constData(), newSave.toUtf8().constData());
            return;
        }
    }
}
#endif

// Releases are built with the ROM compiled into the executable as a Qt
// resource (see DS64_EMBED_ROM in src/frontend/qt_sdl/CMakeLists.txt). The
// emulator's ROM/save handling is file-based, so materialize that resource
// once into the per-user data directory and hand the resulting path to the
// normal loader. Keeping the save next to the extracted ROM (game.sav) also
// means it survives moving/replacing the executable. The embedded hash in
// the stamp file makes a rebuilt binary with a different ROM refresh the copy
// without touching an existing save.
static QString ds64ExtractEmbeddedROM()
{
    QFile resource(QString::fromUtf8(DS64_EMBEDDED_ROM_PATH));
    if (!resource.exists())
    {
        printf("DS64: embedded ROM resource is missing from this build\n");
        return {};
    }

    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (dataDir.isEmpty())
    {
        printf("DS64: no writable application data directory for the embedded ROM\n");
        return {};
    }

    const QString romDir = dataDir + QStringLiteral("/rom");
    if (!QDir().mkpath(romDir))
    {
        printf("DS64: could not create %s\n", romDir.toUtf8().constData());
        return {};
    }

#ifdef Q_OS_LINUX
    ds64MigrateLegacySave(romDir);
#endif

    const QString romPath = romDir + QStringLiteral("/game.nds");
    const QString stampPath = romDir + QStringLiteral("/.game.nds.sha256");
    const QByteArray wantedHash = DS64_EMBEDDED_ROM_HASH;

    QFile stamp(stampPath);
    if (QFileInfo(romPath).isFile()
        && QFileInfo(romPath).size() == resource.size()
        && stamp.open(QIODevice::ReadOnly)
        && stamp.readAll().trimmed() == wantedHash)
    {
        return romPath;
    }

    if (!resource.open(QIODevice::ReadOnly))
    {
        printf("DS64: could not read the embedded ROM resource\n");
        return {};
    }

    QSaveFile out(romPath);
    if (!out.open(QIODevice::WriteOnly))
    {
        printf("DS64: could not write %s\n", romPath.toUtf8().constData());
        return {};
    }

    while (true)
    {
        const QByteArray chunk = resource.read(4 * 1024 * 1024);
        if (chunk.isEmpty())
            break;
        if (out.write(chunk) != chunk.size())
        {
            printf("DS64: could not write %s\n", romPath.toUtf8().constData());
            out.cancelWriting();
            return {};
        }
    }

    if (!out.commit())
    {
        printf("DS64: could not finish writing %s\n", romPath.toUtf8().constData());
        return {};
    }

    QSaveFile stampOut(stampPath);
    if (stampOut.open(QIODevice::WriteOnly))
    {
        stampOut.write(wantedHash);
        stampOut.commit();
    }

    printf("DS64: extracted embedded ROM to %s\n", romPath.toUtf8().constData());
    return romPath;
}
#endif


void setMPInterface(MPInterfaceType type)
{
    // switch to the requested MP interface
    MPInterface::Set(type);

    // set receive timeout
    // TODO: different settings per interface?
    MPInterface::Get().SetRecvTimeout(Config::GetGlobalTable().GetInt("MP.RecvTimeout"));

    // update UI appropriately
    // TODO: decide how to deal with multi-window when it becomes a thing
    for (int i = 0; i < kMaxEmuInstances; i++)
    {
        EmuInstance* inst = emuInstances[i];
        if (!inst) continue;

        MainWindow* win = inst->getMainWindow();
        if (win) win->updateMPInterface(type);
    }
}



MelonApplication::MelonApplication(int& argc, char** argv)
    : QApplication(argc, argv)
{
#if !defined(Q_OS_APPLE)
    setWindowIcon(QIcon(":/melon-icon"));
    #if defined(Q_OS_UNIX)
        setDesktopFileName(QString("net.kuribo64.melonDS"));
    #endif
#endif
}

// TODO: ROM loading should be moved to EmuInstance
// especially so the preloading below and in main() can be done in a nicer fashion

bool MelonApplication::event(QEvent *event)
{
    if (event->type() == QEvent::FileOpen)
    {
        EmuInstance* inst = emuInstances[0];
        MainWindow* win = inst->getMainWindow();
        QFileOpenEvent *openEvent = static_cast<QFileOpenEvent*>(event);

        const QStringList file = win->splitArchivePath(openEvent->file(), true);
        win->preloadROMs(file, {}, true);
    }

    return QApplication::event(event);
}

#ifndef _WIN32
static void signalHandler(int signal)
{
    std::signal(signal, SIG_DFL);
    qApp->quit();
}
#endif

int main(int argc, char** argv)
{
    sysTimer.start();
    srand(time(nullptr));

    for (int i = 0; i < kMaxEmuInstances; i++)
        emuInstances[i] = nullptr;

    qputenv("QT_SCALE_FACTOR", "1");

#if defined(_WIN32)
#if QT_VERSION_MAJOR == 6
    // Allow using the system dark theme palette on Windows
    qputenv("QT_QPA_PLATFORM", "windows:darkmode=2");
#endif

    // argc and argv are passed as UTF8 by SDL's WinMain function
    // QT checks for the original value in local encoding though
    // to see whether it is unmodified to activate its hack that
    // retrieves the unicode value via CommandLineToArgvW.
    argc = __argc;
    argv = __argv;

    // Check whether we are already attached to an output stream.
    HANDLE outputHandle = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!outputHandle || (outputHandle == INVALID_HANDLE_VALUE))
    {
        // If started from terminal, attach and output logs to it.
        if (AttachConsole(ATTACH_PARENT_PROCESS))
        {
            freopen("CONOUT$", "a", stdout);
            freopen("CONOUT$", "a", stderr);
        }
        else
        {
            // Otherwise, discard log output.
            freopen("NUL:", "w", stdout);
            freopen("NUL:", "w", stderr);
        }
    }
#else
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
#endif

    printf("melonDS " MELONDS_VERSION "\n");
    printf(MELONDS_URL "\n");

    // easter egg - not worth checking other cases for something so dumb
    if (argc != 0 && (!strcasecmp(argv[0], "derpDS") || !strcasecmp(argv[0], "./derpDS")))
        printf("did you just call me a derp???\n");

    MelonApplication melon(argc, argv);
    pathInit();
    ds64ConfigureBundledRuntime();
    QObject::connect(&melon, &QCoreApplication::aboutToQuit, []() {
        ds64StopBundledServices();
    });

    CLI::CommandLineOptions* options = CLI::ManageArgs(melon);

#ifdef DS64_EMBEDDED_ROM_PATH
    // A standalone release has no external .nds alongside it. If the user
    // didn't pass a ROM, boot the one compiled into this executable.
    if (!options->dsRomPath.has_value())
    {
        const QString embeddedRom = ds64ExtractEmbeddedROM();
        if (!embeddedRom.isEmpty())
        {
            options->dsRomPath = embeddedRom;
            options->boot = true;
        }
    }
#endif

    // http://stackoverflow.com/questions/14543333/joystick-wont-work-using-sdl
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    SDL_SetHint(SDL_HINT_APP_NAME, "melonDS");

    if (SDL_Init(SDL_INIT_HAPTIC) < 0)
    {
        printf("SDL couldn't init rumble\n");
    }
    if (SDL_Init(SDL_INIT_JOYSTICK) < 0)
    {
        printf("SDL couldn't init joystick\n");
    }
    if (SDL_Init(SDL_INIT_SENSOR) < 0)
    {
        printf("SDL couldn't init motion sensors\n");
    }
    if (SDL_Init(SDL_INIT_AUDIO) < 0)
    {
        const char* err = SDL_GetError();
        QString errorStr = "Failed to initialize SDL. This could indicate an issue with your audio driver.\n\nThe error was: ";
        errorStr += err;

        QMessageBox::critical(nullptr, "melonDS", errorStr);
        return 1;
    }

    SDL_JoystickEventState(SDL_ENABLE);

    SDL_InitSubSystem(SDL_INIT_VIDEO);
    SDL_EnableScreenSaver(); SDL_DisableScreenSaver();

    if (!Config::Load())
        QMessageBox::critical(nullptr,
                              "melonDS",
                              "Unable to write to config.\nPlease check the write permissions of the folder you placed melonDS in.");

    camStarted[0] = false;
    camStarted[1] = false;
    camManager[0] = new CameraManager(0, 640, 480, true);
    camManager[1] = new CameraManager(1, 640, 480, true);

    systemThemeName = new QString(QApplication::style()->objectName());

    {
        Config::Table cfg = Config::GetGlobalTable();
        QString uitheme = cfg.GetQString("UITheme");
        if (!uitheme.isEmpty())
        {
            QApplication::setStyle(uitheme);
        }
    }

    // fix for Wayland OpenGL glitches
    QGuiApplication::setAttribute(Qt::AA_NativeWindows, false);
    QGuiApplication::setAttribute(Qt::AA_DontCreateNativeWidgetSiblings, true);

    // default MP interface type is local MP
    // this will be changed if a LAN or netplay session is initiated
    setMPInterface(MPInterface_Local);

    NetInit();

    createEmuInstance();

    {
        MainWindow* win = emuInstances[0]->getMainWindow();
        bool memberSyntaxUsed = false;
        const auto prepareRomPath = [&](const std::optional<QString> &romPath,
                                        const std::optional<QString> &romArchivePath) -> QStringList
        {
            if (!romPath.has_value())
                return {};

            if (romArchivePath.has_value())
                return {*romPath, *romArchivePath};

            const QStringList path = win->splitArchivePath(*romPath, true);
            if (path.size() > 1) memberSyntaxUsed = true;
            return path;
        };

        const QStringList dsfile = prepareRomPath(options->dsRomPath, options->dsRomArchivePath);
        const QStringList gbafile = prepareRomPath(options->gbaRomPath, options->gbaRomArchivePath);

        if (memberSyntaxUsed) printf("Warning: use the a.zip|b.nds format at your own risk!\n");

        g_DS64ROMPath = dsfile;
        win->preloadROMs(dsfile, gbafile, options->boot);

        if (options->fullscreen)
            win->toggleFullscreen();
    }

    int ret = melon.exec();

    delete options;

    // if we get here, all the existing emu instances should have been deleted already
    // but with this we make extra sure they are all deleted
    deleteAllEmuInstances();

    delete camManager[0];
    delete camManager[1];

    Config::Save();

    SDL_Quit();
    return ret;
}
