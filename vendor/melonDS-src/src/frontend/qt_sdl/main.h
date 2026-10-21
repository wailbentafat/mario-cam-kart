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

#ifndef MAIN_H
#define MAIN_H

#include "glad/glad.h"

#include <QApplication>
#include <QEvent>
#include <QElapsedTimer>
#include <QStringList>

#include "EmuInstance.h"
#include "Window.h"
#include "EmuThread.h"
#include "ScreenLayout.h"
#include "MPInterface.h"

enum
{
    InstCmd_Pause,
    InstCmd_Unpause,

    InstCmd_UpdateRecentFiles,
    //InstCmd_UpdateVideoSettings,
};

class MelonApplication : public QApplication
{
    Q_OBJECT

public:
    MelonApplication(int &argc, char** argv);
    bool event(QEvent* event) override;
};

extern QString* systemThemeName;
extern QString emuDirectory;

extern QElapsedTimer sysTimer;

bool createEmuInstance();
void deleteEmuInstance(int id);
void deleteAllEmuInstances(int first = 0);
int numEmuInstances();
EmuInstance* getEmuInstanceByID(int id);

// DS64 multi-phone local multiplayer: when g_DS64PendingPhonePort is set
// (>0) before calling createEmuInstance(), the new instance skips its own
// setup dialog and configures itself as phone-controlled on that port
// instead (consumed/reset by that instance's constructor). See
// EmuInstance.cpp and PhoneController.
extern int g_DS64PendingPhonePort;
// Total players in the current DS64 session (1 = no tiling), read by
// Window.cpp to lay out windows as split-screen tiles.
extern int g_DS64TotalPlayers;
// The NDS ROM path instance 0 booted, stashed so each staggered-created
// secondary instance (which has no MainWindow to call preloadROMs()
// through) can boot the same game into itself directly via its own
// EmuThread. See EmuInstance.cpp.
extern QStringList g_DS64ROMPath;
// A DS64 venv path (scripts use DS64_TOOLS_DIR, see EmuInstance.cpp): the DS64_* environment variable envVar when set
// (play.py always sets it), else the path compiled in by CMake. Release
// binaries are built on CI, so their compiled-in source-tree paths don't
// exist on the player's machine — the runtime value must win.
QString ds64Path(const char* envVar, const char* builtIn);

// Starts the bundled-runtime services (QR generation and the phone controller
// HTTP server). Called once phone mode has been selected, so camera/keyboard
// sessions never initialize Python or open a listening socket.
void ds64StartBundledServices();

// Stops the bundled-runtime services (the phone controller HTTP server)
// before a hard _Exit when "RESTART SETUP" replaces this process. Also called
// on normal Qt shutdown; safe to call more than once.
void ds64StopBundledServices();

// Runs a bundled Python script with the embedded interpreter. pythonHome may
// be empty to let Python's own environment (AppImage's PYTHONHOME) decide.
bool ds64RunPythonScript(const QString& scriptPath, const QStringList& args,
                         const QString& pythonHome, const QString& sitePackages);
bool ds64PythonInitializationFailed();

void broadcastInstanceCommand(int cmd, QVariant& param, int sourceinst);

void setMPInterface(melonDS::MPInterfaceType type);

#endif // MAIN_H
