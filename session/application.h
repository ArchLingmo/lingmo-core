/*
 * Copyright (C) 2023-2024 LingmoOS Team.
 *
 * Author:     revenmartin <revenmartin@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef APPLICATION_H
#define APPLICATION_H

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <chrono>
#include <thread>

#include "processmanager.h"
#include "networkproxymanager.h"
#include "powermanager/power.h"

class Application : public QApplication
{
    Q_OBJECT

public:
    explicit Application(int &argc, char **argv);

    // true in the Wayland session's stage 2 (--wayland-child, started by KWin)
    bool wayland() const;

    static void addOptions(QCommandLineParser &parser);

    // Shared with the Wayland session's stage 1 (waylandstart.cpp)
    static void createConfigDirectory();
    static void initKWinConfig();
    static void initLanguage();
    static void initEnvironments();
    static void applyQtEnvironment();
    // Variables only the X11 session sets (or that a previous X11 session left in
    // the systemd/D-Bus activation environment) out of the Wayland session
    static void applyWaylandEnvironment();
    static int runSync(const QString &program, const QStringList &args, const QStringList &env = {});

public slots:
    void logout() {
        m_processManager->logout();
    }

    void reboot() {
        m_power.reboot();
        QCoreApplication::exit(0);
    }

    void powerOff() {
        m_power.shutdown();
        QCoreApplication::exit(0);
    }

    void suspend() {
        m_power.suspend();
    }

    [[maybe_unused]] void startDesktopProcess() {
        // Start Lingmo Desktop Environment
        m_processManager->startDesktopProcess();
    }

    [[maybe_unused]] void updateNetworkProxy() {
        m_networkProxyManager->update();
    }

    void launch(const QString &exec, const QStringList &args);
    void launch(const QString &exec, const QString &workingDir, const QStringList &args);

    // For callers that can't pass a string list (KWin scripts: the lock screen hot corner)
    void lockScreen() {
        // Wayland: KWin's locker (lingmo-screenlocker is X11's)
        if (m_wayland) {
            QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(
                QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("/ScreenSaver"),
                QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("Lock")));
            return;
        }
        launch(QStringLiteral("lingmo-screenlocker"), QStringList());
    }

private:
    void initScreenScaleFactors();
    void initXResource();
    static void repairKWinShortcuts();
    static void applyWindowShortcuts();
    bool syncDBusEnvironment();
    void restartStalePortal();
    void importSystemdEnvrionment();
    void updateUserDirs();
    // X11 session: a Wayland start that never came up (marker left behind)
    void checkFailedWaylandStart();
    void notifyFailedWaylandStart(const QString &reason);
    // Wayland session: the start marker goes once the status bar is on the bus
    void watchShellStarted();

private:
    ProcessManager *m_processManager;
    NetworkProxyManager *m_networkProxyManager;
    Power m_power;

    bool m_wayland;
};

#endif // APPLICATION_H
