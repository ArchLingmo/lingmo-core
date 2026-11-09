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

#include "actions.h"
#include <QCommandLineParser>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QApplication>
#include <QProcess>

const static QString s_dbusName = "com.lingmo.Session";
const static QString s_pathName = "/Session";
const static QString s_interfaceName = "com.lingmo.Session";

Actions::Actions(QObject *parent)
    : QObject(parent)
{

}

void Actions::shutdown()
{
    QDBusInterface iface(s_dbusName, s_pathName, s_interfaceName, QDBusConnection::sessionBus());
    if (iface.isValid()) {
        iface.call("powerOff");
    }
}

void Actions::logout()
{
    QDBusInterface iface(s_dbusName, s_pathName, s_interfaceName, QDBusConnection::sessionBus());
    if (iface.isValid()) {
        iface.call("logout");
    }
}

void Actions::reboot()
{
    QDBusInterface iface(s_dbusName, s_pathName, s_interfaceName, QDBusConnection::sessionBus());
    if (iface.isValid()) {
        iface.call("reboot");
    }
}

void Actions::lockScreen()
{
    // Wayland: the lock screen is KWin's (kscreenlocker with the Lingmo theme);
    // lingmo-screenlocker is the X11 locker. We exit right after, so wait for KWin.
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland")
            || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.ScreenSaver", "/ScreenSaver",
                                                              "org.freedesktop.ScreenSaver", "Lock");
        if (QDBusConnection::sessionBus().call(message, QDBus::Block, 10000).type() != QDBusMessage::ReplyMessage)
            QProcess::startDetached("loginctl", {QStringLiteral("lock-session")});
        qApp->exit(0);
        return;
    }

    QProcess::startDetached("lingmo-screenlocker", QStringList());
    qApp->exit(0);
}

void Actions::suspend()
{
    QDBusInterface iface(s_dbusName, s_pathName, s_interfaceName, QDBusConnection::sessionBus());
    if (iface.isValid()) {
        iface.call("suspend");
    }
    QCoreApplication::exit(0);
}
