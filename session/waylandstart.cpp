/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "waylandstart.h"
#include "application.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <vector>

namespace WaylandStart
{

QString markerDir()
{
    // GenericStateLocation needs Qt 6.7; spelled out so the root helper
    // (lingmo-wayland-check) can find it without the user's environment
    QString state = qEnvironmentVariable("XDG_STATE_HOME");
    if (state.isEmpty() || !QDir::isAbsolutePath(state))
        state = QDir::homePath() + QStringLiteral("/.local/state");
    return state + QStringLiteral("/lingmo-session");
}

QString markerPath()
{
    return markerDir() + QStringLiteral("/wayland-start");
}

QString failedPath()
{
    return markerDir() + QStringLiteral("/wayland-failed");
}

QMap<QString, QString> readMarker(const QString &path)
{
    QMap<QString, QString> values;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return values;
    // Small key=value file; anything odd in it is ignored
    const QList<QByteArray> lines = file.read(4096).split('\n');
    for (const QByteArray &line : lines) {
        const int eq = line.indexOf('=');
        if (eq > 0)
            values.insert(QString::fromUtf8(line.left(eq)).trimmed(), QString::fromUtf8(line.mid(eq + 1)).trimmed());
    }
    return values;
}

void writeMarker(const QMap<QString, QString> &values)
{
    QDir().mkpath(markerDir());
    QMap<QString, QString> all = values;
    if (!all.contains(QStringLiteral("started")))
        all.insert(QStringLiteral("started"), QDateTime::currentDateTime().toString(Qt::ISODate));

    QSaveFile file(markerPath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Could not write the Wayland start marker" << markerPath();
        return;
    }
    for (auto it = all.constBegin(); it != all.constEnd(); ++it)
        file.write(it.key().toUtf8() + '=' + it.value().toUtf8() + '\n');
    file.commit();
}

void markChildStarted()
{
    QMap<QString, QString> values = readMarker(markerPath());
    if (values.isEmpty())
        return;
    values.insert(QStringLiteral("reason"), QStringLiteral("shell"));
    values.insert(QStringLiteral("child"), QStringLiteral("1"));
    writeMarker(values);
}

void clearMarker()
{
    if (QFile::exists(markerPath())) {
        QFile::remove(markerPath());
        qInfo() << "Wayland session is up: start marker cleared";
    }
}

QString unsupportedReason()
{
    const QString sys = qEnvironmentVariable("LINGMO_SYSFS", QStringLiteral("/sys"));

    // Proprietary NVIDIA driver loaded: Wayland needs DRM KMS (modeset) and GBM (495+)
    if (!QFileInfo::exists(sys + QStringLiteral("/module/nvidia")))
        return QString();

    QFile version(sys + QStringLiteral("/module/nvidia/version"));
    if (version.open(QIODevice::ReadOnly)) {
        const int major = QString::fromLatin1(version.readAll()).trimmed().section(QLatin1Char('.'), 0, 0).toInt();
        if (major > 0 && major < 495)
            return QStringLiteral("nvidia-legacy");
    }

    QFile modeset(sys + QStringLiteral("/module/nvidia_drm/parameters/modeset"));
    if (!modeset.open(QIODevice::ReadOnly) || modeset.readAll().trimmed() != "Y")
        return QStringLiteral("nvidia-modeset");

    return QString();
}

void applySoftwareRenderingFallback(bool wayland)
{
    const QStringList nodes = QDir(QStringLiteral("/dev/dri")).entryList({QStringLiteral("renderD*"), QStringLiteral("card*")},
                                                                         QDir::System | QDir::Files);
    if (!nodes.isEmpty())
        return;

    qWarning() << "No GPU device in /dev/dri: software rendering";
    // QtQuick shell components would render blank without OpenGL
    if (qEnvironmentVariableIsEmpty("QT_QUICK_BACKEND"))
        qputenv("QT_QUICK_BACKEND", "software");
    if (wayland) {
        // KWin's QPainter compositor
        if (qEnvironmentVariableIsEmpty("KWIN_COMPOSE"))
            qputenv("KWIN_COMPOSE", "Q");
    } else if (qEnvironmentVariableIsEmpty("LIBGL_ALWAYS_SOFTWARE")) {
        qputenv("LIBGL_ALWAYS_SOFTWARE", "1");
    }
}

static QString shellQuote(const QString &arg)
{
    QString quoted = arg;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

int exec(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    qInfo() << "Lingmo Wayland session: stage 1";

    // Drivers that can't run it get the X11 session at the next login (the
    // greeter helper sees the marker) and a notification there explaining why
    const QString reason = qEnvironmentVariableIntValue("LINGMO_WAYLAND_FORCE") ? QString() : unsupportedReason();
    if (!reason.isEmpty()) {
        qWarning() << "Wayland session not started:" << reason;
        writeMarker({{QStringLiteral("reason"), reason}});
        return 1;
    }

    Application::createConfigDirectory();
    Application::initKWinConfig();
    Application::initLanguage();
    Application::initEnvironments();
    Application::applyWaylandEnvironment();

    // Cursor: KWin reads kcminputrc; Xwayland and GTK apps the XCURSOR_* variables
    QSettings theme(QSettings::UserScope, QStringLiteral("lingmoos"), QStringLiteral("theme"));
    const QString cursorTheme = theme.value(QStringLiteral("CursorTheme"), QStringLiteral("default")).toString();
    const int cursorSize = theme.value(QStringLiteral("CursorSize"), 24).toInt();
    Application::runSync(QStringLiteral("cupdatecursor"), {cursorTheme, QString::number(cursorSize)});
    qputenv("XCURSOR_THEME", cursorTheme.toLocal8Bit());
    qputenv("XCURSOR_SIZE", QByteArray::number(cursorSize));

    applySoftwareRenderingFallback(true);

    // Services D-Bus starts while KWin comes up (xdg-desktop-portal among them) must
    // already see which desktop this is: with no XDG_CURRENT_DESKTOP the portal skips
    // lingmo-portals.conf and takes the GTK backend, which can't share the screen on
    // KWin (browsers then offer only their tabs). KWin adds WAYLAND_DISPLAY itself.
    Application::runSync(QStringLiteral("dbus-update-activation-environment"),
                         {QStringLiteral("--systemd"), QStringLiteral("XDG_CURRENT_DESKTOP"),
                          QStringLiteral("XDG_SESSION_DESKTOP"), QStringLiteral("DESKTOP_SESSION"),
                          QStringLiteral("XDG_SESSION_TYPE")});

    writeMarker({{QStringLiteral("reason"), QStringLiteral("compositor")}});

    QString kwin = QStandardPaths::findExecutable(QStringLiteral("kwin_wayland_wrapper"));
    if (kwin.isEmpty())
        kwin = QStandardPaths::findExecutable(QStringLiteral("kwin_wayland"));
    if (kwin.isEmpty()) {
        qCritical() << "kwin_wayland is not installed";
        return 1;
    }

    // KWin splits this with KShell::splitArgs
    const QString child = shellQuote(QCoreApplication::applicationFilePath()) + QStringLiteral(" --wayland-child");
    const QStringList args = {kwin, QStringLiteral("--xwayland"), QStringLiteral("--exit-with-session"), child};
    qInfo() << "exec" << args;

    std::vector<QByteArray> storage;
    for (const QString &arg : args)
        storage.push_back(arg.toLocal8Bit());
    std::vector<char *> argvExec;
    for (QByteArray &arg : storage)
        argvExec.push_back(arg.data());
    argvExec.push_back(nullptr);

    ::execv(argvExec[0], argvExec.data());
    qCritical() << "Could not start" << kwin << std::strerror(errno);
    return 1;
}

} // namespace WaylandStart
