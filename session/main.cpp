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

#include "application.h"
#include "waylandstart.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QQuickWindow>

#include <cstring>

static bool hasArg(int argc, char *argv[], const char *longName, const char *shortName = nullptr)
{
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], longName) || (shortName && !std::strcmp(argv[i], shortName)))
            return true;
    }
    return false;
}

int main(int argc, char *argv[])
{
    // putenv((char *)"SESSION_MANAGER=");

    // --help needs no display (and the session's QApplication would want one)
    if (hasArg(argc, argv, "--help", "-h")) {
        QCoreApplication app(argc, argv);
        QCommandLineParser parser;
        Application::addOptions(parser);
        parser.process(app);
        return 0;
    }

    // Wayland, stage 1: what SDDM runs from wayland-sessions/lingmo-wayland.desktop.
    // No display exists yet: prepare the environment, then exec KWin, which starts
    // stage 2 (--wayland-child) once its Wayland and Xwayland sockets are up.
    if (hasArg(argc, argv, "--wayland", "-w"))
        return WaylandStart::exec(argc, argv);

    const bool waylandChild = hasArg(argc, argv, "--wayland-child");

    if (!waylandChild) {
        // force xcb QPA plugin as session manager server is very X11 specific.
        qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("xcb"));
    } else {
        // Not forced: Qt picks Wayland itself (XWayland when Wayland fails). Empty
        // rather than unset, so it also overrides the xcb an earlier X11 session left
        // in the D-Bus/systemd activation environment (Qt ignores an empty value).
        qputenv("QT_QPA_PLATFORM", QByteArray());
    }

    QQuickWindow::setDefaultAlphaBuffer(true);

    Application a(argc, argv);
    a.setQuitOnLastWindowClosed(false);

    return a.exec();
}
