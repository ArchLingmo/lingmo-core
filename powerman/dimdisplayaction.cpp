/***************************************************************************
 *   Copyright (C) 2023-2024 by Reion Wong <reion@lingmoos.com>               *
 *   Copyright (C) 2010 by Dario Freddi <drf@kde.org>                      *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA .        *
 ***************************************************************************/

#include "dimdisplayaction.h"
#include <QSettings>
#include <QTimer>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QtGui/private/qtx11extras_p.h>
#include <QProcess>
#include <QDebug>

#include <X11/Xlib.h>
#include <xcb/dpms.h>

DimDisplayAction::DimDisplayAction(QObject *parent)
    : Action(parent)
    , m_iface("com.lingmo.Settings",
              "/Brightness",
              "com.lingmo.Brightness", QDBusConnection::sessionBus())
{
    if (QX11Info::isPlatformX11()) {
        // Disable a default timeout, if any
        // Using DPMS on a server without it (Xephyr, some VMs) kills the xcb connection
        const xcb_query_extension_reply_t *dpms = xcb_get_extension_data(QX11Info::connection(), &xcb_dpms_id);
        if (dpms && dpms->present)
            xcb_dpms_set_timeouts(QX11Info::connection(), 0, 0, 0);

        XSetScreenSaver(QX11Info::display(), 0, 0, 0, 0);
    }
}

void DimDisplayAction::onWakeupFromIdle()
{   
    if (!m_dimmed) {
        return;
    }

    if (m_oldScreenBrightness < 0)
        m_oldScreenBrightness = 1;

    // An active inhibition may not let us restore the brightness.
    // We should wait a bit screen to wake-up from sleep
    QTimer::singleShot(0, this, [this]() {
        m_iface.asyncCall("setValue", QVariant::fromValue(m_oldScreenBrightness));
    });

    m_dimmed = false;
}

void DimDisplayAction::onIdleTimeout(int msec)
{
    int sec = msec / 1000;

    if (m_iface.property("brightness").toInt() == 0)
        return;

    if (sec == m_dimOnIdleTime) {
        m_iface.asyncCall("setValue", QVariant::fromValue(0));

        // Sleep
        if (m_sleep) {
            QDBusInterface iface("com.lingmo.Session",
                                 "/Session",
                                 "com.lingmo.Session", QDBusConnection::sessionBus());

            if (iface.isValid()) {
                iface.call("suspend");
            }
        }

        if (m_lock) {
            lockScreen();
        }

    } else if (sec == (m_dimOnIdleTime * 3 / 4)) {
        const int newBrightness = qRound(m_oldScreenBrightness / 8.0);
        m_iface.asyncCall("setValue", QVariant::fromValue(newBrightness));
    } else if (sec == (m_dimOnIdleTime * 1 / 2)) {
        m_oldScreenBrightness = m_iface.property("brightness").toInt();

        const int newBrightness = qRound(m_oldScreenBrightness / 2.0);

        m_iface.asyncCall("setValue", QVariant::fromValue(newBrightness));
    }

    m_dimmed = true;
}

void DimDisplayAction::lockScreen()
{
    // Wayland: KWin's screen locker (kscreenlocker with the Lingmo theme). Its own
    // idle lock is off in the Lingmo session (kscreenlockerrc Autolock=false), so
    // this stays the only idle lock, as on X11.
    if (qEnvironmentVariable("XDG_SESSION_TYPE") == QLatin1String("wayland")
            || !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
        QDBusMessage message = QDBusMessage::createMethodCall("org.freedesktop.ScreenSaver", "/ScreenSaver",
                                                              "org.freedesktop.ScreenSaver", "Lock");
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [](QDBusPendingCallWatcher *call) {
            if (call->isError())
                QProcess::startDetached("loginctl", {QStringLiteral("lock-session")});
            call->deleteLater();
        });
        return;
    }

    QProcess::startDetached("lingmo-screenlocker", QStringList());
}

void DimDisplayAction::setTimeout(int timeout)
{
    unregisterIdleTimeout();

    if (timeout < 0) {
        m_dimOnIdleTime = timeout;
        return;
    }

    m_dimOnIdleTime = timeout;
    registerIdleTimeout(m_dimOnIdleTime * 3 / 4);
    registerIdleTimeout(m_dimOnIdleTime / 2);
    registerIdleTimeout(m_dimOnIdleTime);
}

void DimDisplayAction::setSleep(bool sleep)
{
    m_sleep = sleep;
}

void DimDisplayAction::setLock(bool lock)
{
    m_lock = lock;
}
