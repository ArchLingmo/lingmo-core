/*
 * Copyright (C) 2023-2024 Lingmo OS Team.
 */

#include "processmanager.h"
#include "application.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QFileInfoList>
#include <QFileInfo>
#include <QFile>
#include <QSettings>
#include <QDebug>
#include <QTimer>
#include <QThread>
#include <QDir>
#include <QSet>
#include <QProcess>

#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>

#include <QtGui/private/qtx11extras_p.h>
#include <KWindowSystem>
#include <NETWM>

#include "daemon-helper.h"

ProcessManager::ProcessManager(Application *app, QObject *parent)
    : QObject(parent)
    , m_app(app)
    , m_wmStarted(false)
    , m_waitLoop(nullptr)
{
    qApp->installNativeEventFilter(this);
}

ProcessManager::~ProcessManager()
{
    qApp->removeNativeEventFilter(this);

    QMapIterator<QString, QProcess *> i(m_systemProcess);
    while (i.hasNext()) {
        i.next();
        QProcess *p = i.value();
        delete p;
        m_systemProcess[i.key()] = nullptr;
    }
}

void ProcessManager::start()
{
    applyScreenLayout();
    startGlobalShortcuts();
    startWindowManager();
    startDaemonProcess();
}

void ProcessManager::applyScreenLayout()
{
    // Monitor positions, rotation and primary set in Settings > Display: an xrandr
    // script, run before the window manager and the panels so they start on the
    // right monitors
    const QString script = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                           + QStringLiteral("/lingmoos/screenlayout.sh");
    if (!QFileInfo::exists(script))
        return;

    QProcess xrandr;
    xrandr.start(QStringLiteral("/bin/sh"), {script});
    if (!xrandr.waitForFinished(5000))
        qWarning() << "screen layout script took too long:" << script;
}

void ProcessManager::startGlobalShortcuts()
{
    // Global shortcuts server (alt-tab, KWin and lingmo-chotkeys shortcuts). Its
    // autostart entry is OnlyShowIn=KDE, so the Lingmo session must start it, and
    // before KWin: KWin hands its default shortcuts (Alt+Tab, Alt+F4, ...) over only
    // when it registers them, so with no server running they end up with no key.
    // kglobalacceld 6.8 no longer ships the daemon (it lives inside kwin_wayland):
    // kglobalacceld-x11 keeps the X11 one in its own directory
    QString kglobalacceld;
    for (const QString &path : {QStringLiteral("/usr/lib/kglobalacceld-x11/kglobalacceld"),
                                QStringLiteral("/usr/lib/kglobalacceld"),
                                QStringLiteral("/usr/libexec/kglobalacceld"),
                                QStringLiteral("/usr/lib/x86_64-linux-gnu/libexec/kglobalacceld")}) {
        if (QFileInfo(path).isExecutable()) {
            kglobalacceld = path;
            break;
        }
    }
    if (kglobalacceld.isEmpty())
        return;

    static const QString service = QStringLiteral("org.kde.kglobalaccel");
    QDBusConnection bus = QDBusConnection::sessionBus();
    m_shortcutsD = std::make_shared<LINGMO_SESSION::Daemon>(
        QList<QPair<QString, QStringList>>{qMakePair(kglobalacceld, QStringList())});

    // Wait (at most 3s) for it to own its bus name
    QEventLoop waitLoop;
    QDBusServiceWatcher watcher(service, bus, QDBusServiceWatcher::WatchForRegistration);
    connect(&watcher, &QDBusServiceWatcher::serviceRegistered, &waitLoop, &QEventLoop::quit);
    QTimer::singleShot(3000, &waitLoop, &QEventLoop::quit);
    if (!bus.interface()->isServiceRegistered(service))
        waitLoop.exec();
}

void ProcessManager::logout()
{
    QMapIterator<QString, QProcess *> i(m_systemProcess);

    while (i.hasNext()) {
        i.next();
        QProcess *p = i.value();
        p->terminate();
    }
    i.toFront();

    while (i.hasNext()) {
        i.next();
        QProcess *p = i.value();
        if (p->state() != QProcess::NotRunning && !p->waitForFinished(2000)) {
            p->kill();
        }
    }

    QCoreApplication::exit(0);
}

void ProcessManager::startWindowManager()
{
    auto *wmProcess = new QProcess;
    wmProcess->setProcessChannelMode(QProcess::ForwardedChannels);

    wmProcess->start(m_app->wayland() ? "kwin_wayland" : "kwin_x11", QStringList());

    if (!m_app->wayland()) {
        QEventLoop waitLoop;
        m_waitLoop = &waitLoop;
        // add a timeout to avoid infinite blocking if a WM fail to execute.
        QTimer::singleShot(30 * 1000, &waitLoop, SLOT(quit()));
        waitLoop.exec();
        m_waitLoop = nullptr;
    }
}

void ProcessManager::startDesktopProcess()
{
    // lingmo-settings-daemon asks for this once its theme module is up, which happens
    // again whenever the daemon restarts: the desktop is already running (and
    // supervised) by then, and a second set would kill the first one
    if (m_desktopAutoStartD)
        return;

    // When the lingmo-settings-daemon theme module is loaded, start the desktop.
    // In the way, there will be no problem that desktop and launcher can't get wallpaper.

    QList<QPair<QString, QStringList>> list;
    // Desktop components
    list << qMakePair(QString("lingmo-notificationd"), QStringList());
    list << qMakePair(QString("lingmo-statusbar"), QStringList());
    list << qMakePair(QString("lingmo-dock"), QStringList());
    list << qMakePair(QString("lingmo-filemanager"), QStringList("--desktop"));
    list << qMakePair(QString("lingmo-launcher"), QStringList());
    list << qMakePair(QString("lingmo-powerman"), QStringList());
    list << qMakePair(QString("lingmo-clipboard"), QStringList());
    list << qMakePair(QString("lingmo-wallpaper-color-pick"), QStringList());

    m_desktopAutoStartD = std::make_shared<LINGMO_SESSION::Daemon>(list);

    // Auto start once the statusbar hosts the tray (org.kde.StatusNotifierWatcher):
    // apps that check for a tray at startup (Qt's QSystemTrayIcon, Electron, ...)
    // otherwise give up on their icon. Don't hold the session back for more than 5s.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (bus.interface()->isServiceRegistered(QStringLiteral("org.kde.StatusNotifierWatcher"))) {
        QTimer::singleShot(100, this, &ProcessManager::loadAutoStartProcess);
        return;
    }

    auto *trayWatcher = new QDBusServiceWatcher(QStringLiteral("org.kde.StatusNotifierWatcher"), bus,
                                                QDBusServiceWatcher::WatchForRegistration, this);
    auto *timeout = new QTimer(this);
    timeout->setSingleShot(true);
    auto start = [this, trayWatcher, timeout] {
        trayWatcher->deleteLater();
        timeout->deleteLater();
        trayWatcher->disconnect(this);
        timeout->disconnect(this);
        loadAutoStartProcess();
    };
    connect(trayWatcher, &QDBusServiceWatcher::serviceRegistered, this, start);
    connect(timeout, &QTimer::timeout, this, start);
    timeout->start(5000);
}

void ProcessManager::startDaemonProcess()
{
    QList<QPair<QString, QStringList>> list;

    list << qMakePair(QString("lingmo-settings-daemon"), QStringList());
    list << qMakePair(QString("lingmo-xembedsniproxy"), QStringList());
    list << qMakePair(QString("lingmo-gmenuproxy"), QStringList());
//    list << qMakePair(QString("lingmo-clipboard"), QStringList());
    list << qMakePair(QString("lingmo-chotkeys"), QStringList());
    // Password prompts for pkexec and apps asking for admin rights: restarted if it
    // ever dies, or the session is left unable to authorize anything until logout
    list << qMakePair(QString("lingmo-polkit-agent"), QStringList());

    m_daemonAutoStartD = std::make_shared<LINGMO_SESSION::Daemon>(list);
}

// Values of a ';'-separated list key (OnlyShowIn=, NotShowIn=) in a .desktop file's main group
static QStringList desktopList(const QString &path, const QString &key)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    bool inMainGroup = false;
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (line.startsWith(QLatin1Char('[')))
            inMainGroup = (line == QLatin1String("[Desktop Entry]"));
        else if (inMainGroup && line.startsWith(key + QLatin1Char('=')))
            return line.mid(key.size() + 1).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    }
    return {};
}

void ProcessManager::loadAutoStartProcess()
{
    QList<QPair<QString, QStringList>> list;

    const QStringList dirs = QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation,
                                                       QStringLiteral("autostart"),
                                                       QStandardPaths::LocateDirectory);
    // XDG autostart: an entry in ~/.config/autostart replaces the system one with
    // the same file name (locateAll lists the user directory first)
    QSet<QString> seen;
    for (const QString &dir : dirs) {
        const QDir d(dir);
        const QStringList fileNames = d.entryList(QStringList() << QStringLiteral("*.desktop"));
        for (const QString &file : fileNames) {
            if (seen.contains(file))
                continue;
            seen.insert(file);

            QSettings desktop(d.absoluteFilePath(file), QSettings::IniFormat);

            desktop.beginGroup("Desktop Entry");

            // Turned off (Settings > Startup writes Hidden=true into the user copy)
            if (desktop.value("Hidden").toString() == QLatin1String("true")
                || desktop.value("X-GNOME-Autostart-enabled").toString() == QLatin1String("false"))
                continue;

            const QString tryExec = desktop.value("TryExec").toString();
            if (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty()
                && !QFileInfo(tryExec).isExecutable())
                continue;

            // Ignore files the require a specific desktop environment
            // QSettings reads ';' as a comment, which would cut "GNOME;Lingmo;" down to
            // "GNOME": read these desktop-entry lists straight from the file instead
            const QStringList notShowIn = desktopList(d.absoluteFilePath(file), QStringLiteral("NotShowIn"));
            if (notShowIn.contains("Lingmo"))
                continue;
            const QStringList onlyShowIn = desktopList(d.absoluteFilePath(file), QStringLiteral("OnlyShowIn"));
            if (!onlyShowIn.isEmpty() && !onlyShowIn.contains("Lingmo"))
                continue;

            const QString execValue = desktop.value("Exec").toString();

            // 避免冲突
            if (execValue.contains("gmenudbusmenuproxy"))
                continue;

            // 使用 QProcess::splitCommand 来解析命令和参数
            QStringList args = QProcess::splitCommand(execValue);
            // Desktop entry field codes (%U, %f, ...) have no files to expand to here
            args.removeIf([](const QString &arg) {
                return arg.size() == 2 && arg.startsWith(QLatin1Char('%'));
            });

            // 检查是否至少有一个元素（即程序路径）
            if (!args.isEmpty()) {
                auto program = args.first();
                args.removeFirst(); // 移除程序路径，剩下的都是参数

                list << qMakePair(program,  args);
            } else {
                qWarning() << "Invalid 'Exec' found in file!";
            }
        }
    }

    m_userAutoStartD = std::make_shared<LINGMO_SESSION::Daemon>(list, false);
}

bool ProcessManager::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    if (eventType != "xcb_generic_event_t") // We only want to handle XCB events
        return false;

    // ref: lxqt session
    if (!m_wmStarted && m_waitLoop) {
        // all window managers must set their name according to the spec
        if (!QString::fromUtf8(NETRootInfo(QX11Info::connection(), NET::SupportingWMCheck).wmName()).isEmpty()) {
            qDebug() << "Window manager started";
            m_wmStarted = true;
            if (m_waitLoop && m_waitLoop->isRunning())
                m_waitLoop->exit();

            qApp->removeNativeEventFilter(this);
        }
    }

    return false;
}
