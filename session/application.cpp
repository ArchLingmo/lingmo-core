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
#include <QFile>
#include "sessionadaptor.h"

// Qt
#include <QCommandLineOption>
#include <QDBusConnection>
#include <QStandardPaths>
#include <QSettings>
#include <QProcess>
#include <QDebug>
#include <QDir>

#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDateTime>
#include <QLocale>

#include <KConfig>
#include <KConfigGroup>
#include <QSaveFile>
#include <QTimer>

// STL
#include <optional>

std::optional<QStringList> getSystemdEnvironment()
{
    QStringList list;
    auto msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
                                              QStringLiteral("/org/freedesktop/systemd1"),
                                              QStringLiteral("org.freedesktop.DBus.Properties"),
                                              QStringLiteral("Get"));
    msg << QStringLiteral("org.freedesktop.systemd1.Manager") << QStringLiteral("Environment");
    auto reply = QDBusConnection::sessionBus().call(msg);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        return std::nullopt;
    }

    // Make sure the returned type is correct.
    auto arguments = reply.arguments();
    if (arguments.isEmpty() || arguments[0].userType() != qMetaTypeId<QDBusVariant>()) {
        return std::nullopt;
    }
    auto variant = qdbus_cast<QVariant>(arguments[0]);
    if (variant.type() != QVariant::StringList) {
        return std::nullopt;
    }

    return variant.toStringList();
}

bool isShellVariable(const QByteArray &name)
{
    return name == "_" || name.startsWith("SHLVL");
}

bool isSessionVariable(const QByteArray &name)
{
    // Check is variable is specific to session.
    return name == "DISPLAY" || name == "XAUTHORITY" || //
        name == "WAYLAND_DISPLAY" || name == "WAYLAND_SOCKET" || //
        name.startsWith("XDG_") || //
        // Set by this session for its display server: the systemd user manager keeps
        // whatever the previous session (X11 or Wayland) pushed there
        name == "QT_QPA_PLATFORM" || name == "QT_SCREEN_SCALE_FACTORS" || //
        name == "GDK_SCALE" || name == "GDK_DPI_SCALE" || //
        name == "XCURSOR_THEME" || name == "XCURSOR_SIZE" || name == "KWIN_COMPOSE" || //
        name == "QSG_DISTANCEFIELD_ANTIALIASING";
}

void setEnvironmentVariable(const QByteArray &name, const QByteArray &value)
{
    if (qgetenv(name) != value) {
        qputenv(name, value);
    }
}

Application::Application(int &argc, char **argv)
    : QApplication(argc, argv)
    , m_processManager(new ProcessManager(this))
    , m_networkProxyManager(new NetworkProxyManager)
    , m_wayland(false)
{
    new SessionAdaptor(this);

    // connect to D-Bus and register as an object:
    QDBusConnection::sessionBus().registerService(QStringLiteral("com.lingmo.Session"));
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Session"), this);

    QCommandLineParser parser;
    addOptions(parser);
    parser.process(*this);

    // --wayland (stage 1) never gets here: main() hands it to WaylandStart::exec()
    m_wayland = parser.isSet(QStringLiteral("wayland-child"));

    if (m_wayland) {
        qInfo() << "Lingmo Wayland session: stage 2" << "WAYLAND_DISPLAY=" << qgetenv("WAYLAND_DISPLAY")
                << "DISPLAY=" << qgetenv("DISPLAY");
        WaylandStart::markChildStarted();
    }

    createConfigDirectory();
    if (!m_wayland) {
        initKWinConfig();
    }
    initLanguage();
    if (!m_wayland) {
        initScreenScaleFactors();
        initXResource();
        WaylandStart::applySoftwareRenderingFallback(false);
    }

    initEnvironments();
    if (m_wayland)
        applyWaylandEnvironment();

    if (!syncDBusEnvironment()) {
        // Startup error
        qDebug() << "Could not sync environment to dbus.";
        qApp->exit(1);
    }

    // We import systemd environment after we sync the dbus environment here.
    // Otherwise it may leads to some unwanted order of applying environment
    // variables (e.g. LANG and LC_*)
    // ref plasma
    importSystemdEnvrionment();
    // The systemd user environment carries the host's Qt variables (PAM reads
    // /etc/environment): the shell must not inherit them back, and neither should
    // apps D-Bus/systemd start later
    applyQtEnvironment();
    // The systemd user manager outlives a session and keeps the LANG/LANGUAGE/LC_* of
    // the previous one: after a language change the import above would bring the old
    // language back. The language chosen in Settings wins, and syncDBusEnvironment()
    // below hands it to systemd and D-Bus for the apps they start.
    initLanguage();
    // unset-environment can't drop what the systemd user manager inherited from
    // PAM, but an empty value overrides it, and Qt ignores an empty style
    runSync(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("set-environment"),
                                          QStringLiteral("QT_STYLE_OVERRIDE="), QStringLiteral("QT_QUICK_CONTROLS_STYLE=")});
    // Whatever the other session type left in the systemd user manager (it outlives
    // sessions): Wayland's socket in an X11 session, X11's scale factors in a Wayland one
    if (m_wayland) {
        runSync(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("unset-environment"),
                                              QStringLiteral("QT_SCREEN_SCALE_FACTORS"), QStringLiteral("GDK_SCALE"),
                                              QStringLiteral("GDK_DPI_SCALE")});
    } else {
        runSync(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("unset-environment"),
                                              QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("KWIN_COMPOSE"),
                                              QStringLiteral("QSG_DISTANCEFIELD_ANTIALIASING")});
    }
    syncDBusEnvironment();
    restartStalePortal();

    if (!m_wayland) {
        qunsetenv("XCURSOR_THEME");
        qunsetenv("XCURSOR_SIZE");
    }
    qunsetenv("SESSION_MANAGER");

    m_networkProxyManager->update();

    QTimer::singleShot(50, this, &Application::updateUserDirs);

    if (m_wayland)
        watchShellStarted();
    else
        checkFailedWaylandStart();

    // Launch Lingmo and user defined processes !
    QTimer::singleShot(100, m_processManager, &ProcessManager::start);
}

void Application::addOptions(QCommandLineParser &parser)
{
    parser.setApplicationDescription(QStringLiteral("Lingmo Session"));
    parser.addHelpOption();
    parser.addOption(QCommandLineOption({QStringLiteral("w"), QStringLiteral("wayland")},
                                        QStringLiteral("Start the Wayland session (runs KWin, which starts --wayland-child)")));
    parser.addOption(QCommandLineOption(QStringLiteral("wayland-child"),
                                        QStringLiteral("Wayland session, second stage: started by KWin inside the compositor")));
}

void Application::applyWaylandEnvironment()
{
    qputenv("XDG_SESSION_TYPE", "wayland");
    // Not forced: Qt picks Wayland (XWayland if that fails). Empty, not unset, so
    // the xcb an X11 session left in the D-Bus/systemd activation environment is
    // overridden too (Qt ignores an empty value)
    qputenv("QT_QPA_PLATFORM", QByteArray());
    // KWin scales each output; these would scale Qt and GTK apps a second time
    qunsetenv("QT_SCREEN_SCALE_FACTORS");
    qunsetenv("QT_SCALE_FACTOR");
    qunsetenv("GDK_SCALE");
    qunsetenv("GDK_DPI_SCALE");
    qunsetenv("QT_AUTO_SCREEN_SCALE_FACTOR");
    // KWin reports the monitors' subpixel layout as unknown, so Qt Quick falls back to
    // grayscale text, softer than on X11 (where Xft.rgba gives it RGB). The Lingmo
    // fontconfig asks for RGB: Qt Quick text then matches the X11 session
    if (qEnvironmentVariableIsEmpty("QSG_DISTANCEFIELD_ANTIALIASING"))
        qputenv("QSG_DISTANCEFIELD_ANTIALIASING", "subpixel");
    // Native Wayland for SDL, Firefox and Electron where they don't pick it themselves
    if (qEnvironmentVariableIsEmpty("MOZ_ENABLE_WAYLAND"))
        qputenv("MOZ_ENABLE_WAYLAND", "1");
    if (qEnvironmentVariableIsEmpty("ELECTRON_OZONE_PLATFORM_HINT"))
        qputenv("ELECTRON_OZONE_PLATFORM_HINT", "auto");
}

void Application::checkFailedWaylandStart()
{
    const QString marker = WaylandStart::markerPath();
    if (!QFile::exists(marker))
        return;

    // A Wayland start that never got as far as the status bar
    QMap<QString, QString> values = WaylandStart::readMarker(marker);
    values.insert(QStringLiteral("detected"), QDateTime::currentDateTime().toString(Qt::ISODate));
    QSaveFile failed(WaylandStart::failedPath());
    if (failed.open(QIODevice::WriteOnly | QIODevice::Text)) {
        for (auto it = values.constBegin(); it != values.constEnd(); ++it)
            failed.write(it.key().toUtf8() + '=' + it.value().toUtf8() + '\n');
        failed.commit();
    }
    QFile::remove(marker);

    const QString reason = values.value(QStringLiteral("reason"), QStringLiteral("compositor"));
    qWarning() << "The last Wayland session did not start:" << reason << "- back on X11";

    // Shown once the notification server is up
    QDBusConnection bus = QDBusConnection::sessionBus();
    static const QString service = QStringLiteral("org.freedesktop.Notifications");
    if (bus.interface()->isServiceRegistered(service)) {
        QTimer::singleShot(3000, this, [this, reason] { notifyFailedWaylandStart(reason); });
        return;
    }
    auto *watcher = new QDBusServiceWatcher(service, bus, QDBusServiceWatcher::WatchForRegistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this, watcher, reason] {
        watcher->deleteLater();
        // Let the shell settle so the notification isn't lost behind it
        QTimer::singleShot(3000, this, [this, reason] { notifyFailedWaylandStart(reason); });
    });
}

void Application::notifyFailedWaylandStart(const QString &reason)
{
    const bool pt = QLocale().name().startsWith(QLatin1String("pt"));
    QString summary = pt ? QStringLiteral("A sessão Wayland não abriu") : QStringLiteral("The Wayland session did not start");
    QString body;
    if (reason == QLatin1String("nvidia-modeset")) {
        body = pt ? QStringLiteral("O driver NVIDIA está sem o modeset do kernel (nvidia_drm.modeset=1), que o Wayland precisa. "
                                   "Você está na sessão X11. Ligue o modeset e reinicie para usar o Wayland.")
                  : QStringLiteral("The NVIDIA driver runs without kernel modesetting (nvidia_drm.modeset=1), which Wayland needs. "
                                   "You are in the X11 session. Turn modesetting on and restart to use Wayland.");
    } else if (reason == QLatin1String("nvidia-legacy")) {
        body = pt ? QStringLiteral("Este driver NVIDIA é antigo demais para o Wayland (precisa da versão 495 ou mais nova). "
                                   "Você está na sessão X11.")
                  : QStringLiteral("This NVIDIA driver is too old for Wayland (it needs version 495 or newer). "
                                   "You are in the X11 session.");
    } else if (reason == QLatin1String("shell")) {
        body = pt ? QStringLiteral("A barra e a área de trabalho não abriram na sessão Wayland, então a tela de login voltou "
                                   "para a sessão X11. Você pode tentar o Wayland de novo na tela de login.")
                  : QStringLiteral("The bar and the desktop did not come up in the Wayland session, so the login screen went "
                                   "back to the X11 session. You can try Wayland again from the login screen.");
    } else {
        body = pt ? QStringLiteral("O compositor (KWin) não iniciou na sessão Wayland, então a tela de login voltou para a "
                                   "sessão X11. Você pode tentar o Wayland de novo na tela de login.")
                  : QStringLiteral("The compositor (KWin) did not start in the Wayland session, so the login screen went back "
                                   "to the X11 session. You can try Wayland again from the login screen.");
    }

    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                          QStringLiteral("/org/freedesktop/Notifications"),
                                                          QStringLiteral("org.freedesktop.Notifications"),
                                                          QStringLiteral("Notify"));
    message << QStringLiteral("Lingmo") << 0u << QStringLiteral("dialog-warning") << summary << body
            << QStringList() << QVariantMap{{QStringLiteral("urgency"), QVariant::fromValue<uchar>(1)}} << -1;
    QDBusConnection::sessionBus().asyncCall(message);
}

void Application::watchShellStarted()
{
    // The shell is up once the status bar owns its name: then the Wayland start
    // counts as good and the marker goes
    QDBusConnection bus = QDBusConnection::sessionBus();
    static const QString service = QStringLiteral("com.lingmo.Statusbar");
    if (bus.interface()->isServiceRegistered(service)) {
        WaylandStart::clearMarker();
        return;
    }
    auto *watcher = new QDBusServiceWatcher(service, bus, QDBusServiceWatcher::WatchForRegistration, this);
    connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [watcher] {
        watcher->deleteLater();
        WaylandStart::clearMarker();
    });
}

bool Application::wayland() const
{
    return m_wayland;
}

void Application::launch(const QString &exec, const QStringList &args)
{
    QProcess process;
    process.setProgram(exec);
    process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process.setArguments(args);
    process.startDetached();
}

void Application::launch(const QString &exec, const QString &workingDir, const QStringList &args)
{
    QProcess process;
    process.setProgram(exec);
    process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process.setWorkingDirectory(workingDir);
    process.setArguments(args);
    process.startDetached();
}

// The Lingmo platform theme and QtQuick style. A style forced by the host
// environment (e.g. QT_STYLE_OVERRIDE=kvantum from /etc/environment) overrides
// the Lingmo style; since Qt 6.12 QtQuick honours it too and the shell's QML
// fails to load ("module kvantum is not installed"): nothing but black.
void Application::applyQtEnvironment()
{
    qputenv("QT_QPA_PLATFORMTHEME", "lingmo");
    qputenv("QT_PLATFORM_PLUGIN", "lingmo");
    qunsetenv("QT_STYLE_OVERRIDE");
    qunsetenv("QT_QUICK_CONTROLS_STYLE");

    // Lingmo's system-wide KDE config (kwinrulesrc…) lives in its own XDG config dir,
    // ahead of /etc/xdg, so it cascades over files KDE packages own there
    const QString lingmoDir = QStringLiteral("/usr/share/lingmo/xdg");
    QStringList dirs = qEnvironmentVariable("XDG_CONFIG_DIRS", QStringLiteral("/etc/xdg")).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    if (!dirs.contains(lingmoDir)) {
        dirs.prepend(lingmoDir);
        qputenv("XDG_CONFIG_DIRS", dirs.join(QLatin1Char(':')).toLocal8Bit());
    }
}

void Application::initEnvironments()
{
    // Set defaults
    if (qEnvironmentVariableIsEmpty("XDG_DATA_HOME"))
        qputenv("XDG_DATA_HOME", QDir::home().absoluteFilePath(QStringLiteral(".local/share")).toLocal8Bit());
    if (qEnvironmentVariableIsEmpty("XDG_DESKTOP_DIR"))
        qputenv("XDG_DESKTOP_DIR", QDir::home().absoluteFilePath(QStringLiteral("/Desktop")).toLocal8Bit());
    if (qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME"))
        qputenv("XDG_CONFIG_HOME", QDir::home().absoluteFilePath(QStringLiteral(".config")).toLocal8Bit());
    if (qEnvironmentVariableIsEmpty("XDG_CACHE_HOME"))
        qputenv("XDG_CACHE_HOME", QDir::home().absoluteFilePath(QStringLiteral(".cache")).toLocal8Bit());
    if (qEnvironmentVariableIsEmpty("XDG_DATA_DIRS"))
        qputenv("XDG_DATA_DIRS", "/usr/local/share/:/usr/share/");
    if (qEnvironmentVariableIsEmpty("XDG_CONFIG_DIRS"))
        qputenv("XDG_CONFIG_DIRS", "/etc/xdg");

    // Environment
    qputenv("DESKTOP_SESSION", "Lingmo");
    qputenv("XDG_CURRENT_DESKTOP", "Lingmo");
    qputenv("XDG_SESSION_DESKTOP", "Lingmo");

    // Qt
    applyQtEnvironment();
    
    // ref: https://stackoverflow.com/questions/34399993/qml-performance-issue-when-updating-an-item-in-presence-of-many-non-overlapping
    qputenv("QT_QPA_UPDATE_IDLE_TIME", "10");

    qputenv("QT_AUTO_SCREEN_SCALE_FACTOR", "0");

    // IM Config
    // qputenv("GTK_IM_MODULE", "fcitx5");
    // qputenv("QT4_IM_MODULE", "fcitx5");
    // qputenv("QT_IM_MODULE", "fcitx5");
    // qputenv("CLUTTER_IM_MODULE", "fcitx5");
    // qputenv("XMODIFIERS", "@im=fcitx");
}

void Application::initLanguage()
{
    QSettings settings(QSettings::UserScope, "lingmoos", "language");
    QString value = settings.value("language", "").toString();

    // Init Language
    if (value.isEmpty()) {
        QFile file("/etc/locale.gen");
        if (file.open(QIODevice::ReadOnly)) {
            QStringList lines = QString(file.readAll()).split('\n');

            for (const QString &line : lines) {
                if (line.startsWith('#'))
                    continue;

                if (line.trimmed().isEmpty())
                    continue;

                value = line.split(' ').first().split('.').first();
            }
        }
    }

    if (value.isEmpty())
        value = "en_US";

    settings.setValue("language", value);

    QString str = QString("%1.UTF-8").arg(value);

    const auto lcValues = {
        "LANG", "LC_NUMERIC", "LC_TIME", "LC_MONETARY", "LC_MEASUREMENT", "LC_COLLATE", "LC_CTYPE"
    };

    for (auto lc : lcValues) {
        const QString value = str;
        if (!value.isEmpty()) {
            qputenv(lc, value.toUtf8());
        }
    }

    if (!value.isEmpty()) {
        qputenv("LANGUAGE", value.toUtf8());
    }
    // LC_ALL and LC_MESSAGES would override the interface language
    qunsetenv("LC_ALL");
    qputenv("LC_MESSAGES", str.toUtf8());
}

void Application::initScreenScaleFactors()
{
    QSettings settings(QSettings::UserScope, "lingmoos", "theme");
    qreal scaleFactor = settings.value("PixelRatio", 1.0).toReal();

    qputenv("QT_SCREEN_SCALE_FACTORS", QByteArray::number(scaleFactor));

    // for Gtk
    if (qFloor(scaleFactor) > 1) {
        qputenv("GDK_SCALE", QByteArray::number(scaleFactor, 'g', 0));
        qputenv("GDK_DPI_SCALE", QByteArray::number(1.0 / scaleFactor, 'g', 3));
    } else {
        qputenv("GDK_SCALE", QByteArray::number(qFloor(scaleFactor), 'g', 0));
        qputenv("GDK_DPI_SCALE", QByteArray::number(qFloor(scaleFactor), 'g', 0));
    }
}

void Application::initXResource()
{
    QSettings settings(QSettings::UserScope, "lingmoos", "theme");
    qreal scaleFactor = settings.value("PixelRatio", 1.0).toReal();
    int fontDpi = 96 * scaleFactor;
    QString cursorTheme = settings.value("CursorTheme", "default").toString();
    int cursorSize = settings.value("CursorSize", 24).toInt() * scaleFactor;
    int xftAntialias = settings.value("XftAntialias", 1).toBool();
    QString xftHintStyle = settings.value("XftHintStyle", "hintslight").toString();

    const QString datas = QString("Xft.dpi: %1\n"
                                  "Xcursor.theme: %2\n"
                                  "Xcursor.size: %3\n"
                                  "Xft.antialias: %4\n"
                                  "Xft.hintstyle: %5\n"
                                  "Xft.rgba: rgb")
                          .arg(fontDpi)
                          .arg(cursorTheme)
                          .arg(cursorSize)
                          .arg(xftAntialias)
                          .arg(xftHintStyle);

    QProcess p;
    p.start(QStringLiteral("xrdb"), {QStringLiteral("-quiet"), QStringLiteral("-merge"), QStringLiteral("-nocpp")});
    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.write(datas.toLatin1());
    p.closeWriteChannel();
    p.waitForFinished(-1);

    // For lingmo-wine
    qputenv("LINGMO_FONT_DPI", QByteArray::number(fontDpi));

    // Init cursor
    runSync("cupdatecursor", {cursorTheme, QString::number(cursorSize)});
    // qputenv("XCURSOR_THEME", cursorTheme.toLatin1());
    // qputenv("XCURSOR_SIZE", QByteArray::number(cursorSize * scaleFactor));
}

void Application::initKWinConfig()
{
    // KConfig, not QSettings: QSettings rewrites the whole file and mangles KWin's
    // own groups ([Tiling][<uuid>] loses its id, [$Version] becomes [%24Version],
    // JSON values lose their quotes)
    KConfig config(QStringLiteral("kwinrc"));

    KConfigGroup blur = config.group(QStringLiteral("Effect-Blur"));
    blur.writeEntry("BlurStrength", 10);
    blur.writeEntry("NoiseStrength", 0);

    KConfigGroup windows = config.group(QStringLiteral("Windows"));
    windows.writeEntry("FocusStealingPreventionLevel", 0);
    windows.writeEntry("HideUtilityWindowsForInactive", false);
    windows.writeEntry("BorderlessMaximizedWindows", false);
    windows.writeEntry("Placement", "Centered");

    KConfigGroup lingmo = config.group(QStringLiteral("Lingmo"));

    // Decoration defaults are written once per version, not on every login, so a
    // layout the user picks later is kept. v2: macOS-style buttons on the left.
    constexpr int kDecorationDefaultsVersion = 2;
    if (lingmo.readEntry("DecorationDefaultsVersion", 0) < kDecorationDefaultsVersion) {
        // KWin 6.3+ reads the plugin from kdecoration3 and the button layout from kdecoration2
        for (const char *name : {"org.kde.kdecoration2", "org.kde.kdecoration3"}) {
            KConfigGroup group = config.group(QString::fromLatin1(name));
            group.writeEntry("BorderSize", "Normal");
            group.writeEntry("ButtonsOnLeft", "XIA");   // close, minimize, maximize
            group.writeEntry("ButtonsOnRight", "");
            group.writeEntry("library", "org.lingmo.decoration");
            group.writeEntry("theme", "");
        }
        lingmo.writeEntry("DecorationDefaultsVersion", kDecorationDefaultsVersion);
    }

    // Lingmo used to ship a partial [kwin] group in /etc/xdg/kglobalshortcutsrc, which
    // made kglobalacceld register every other KWin shortcut (Alt+Tab, Alt+F4, ...) with
    // no key and no default, and it saved them that way. Drop those empty entries once
    // so KWin registers its defaults again; anything the user set is left alone.
    constexpr int kShortcutsRepairVersion = 1;
    if (lingmo.readEntry("ShortcutsRepairVersion", 0) < kShortcutsRepairVersion) {
        repairKWinShortcuts();
        lingmo.writeEntry("ShortcutsRepairVersion", kShortcutsRepairVersion);
    }

    // Lingmo's window shortcuts on the Meta key (maximize, minimize, close, ...)
    constexpr int kWindowShortcutsVersion = 1;
    if (lingmo.readEntry("WindowShortcutsVersion", 0) < kWindowShortcutsVersion) {
        applyWindowShortcuts();
        lingmo.writeEntry("WindowShortcutsVersion", kWindowShortcutsVersion);
    }

    config.sync();
}

void Application::applyWindowShortcuts()
{
    // KWin action, Lingmo keys, KWin's default keys, description. Written only when
    // the action still has KWin's default (or no) keys, so the user's own choices stay.
    struct Shortcut { const char *action; const char *keys; const char *kwinDefault; const char *text; };
    static const Shortcut shortcuts[] = {
        {"Window Maximize", "Meta+Up\\tMeta+PgUp", "Meta+PgUp", "Maximize Window"},
        {"Window Minimize", "Meta+Down\\tMeta+PgDown", "Meta+PgDown", "Minimize Window"},
        {"Window Quick Tile Top", "none", "Meta+Up", "Quick Tile Window to the Top"},
        {"Window Quick Tile Bottom", "none", "Meta+Down", "Quick Tile Window to the Bottom"},
        {"Window Close", "Alt+F4\\tMeta+Q", "Alt+F4", "Close Window"},
        {"Window Fullscreen", "Meta+F", "none", "Make Window Fullscreen"},
    };

    // Plain text on purpose: QSettings would rewrite the tab/comma separated values
    QFile file(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/kglobalshortcutsrc");
    QStringList lines;
    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        lines = QString::fromUtf8(file.readAll()).split('\n');
        file.close();
    }

    int groupStart = lines.indexOf(QStringLiteral("[kwin]"));
    if (groupStart < 0) {
        if (!lines.isEmpty() && !lines.last().isEmpty())
            lines << QString();
        lines << QStringLiteral("[kwin]");
        groupStart = lines.size() - 1;
    }
    int groupEnd = groupStart + 1;
    while (groupEnd < lines.size() && !lines.at(groupEnd).startsWith('['))
        ++groupEnd;

    for (const Shortcut &shortcut : shortcuts) {
        const QString key = QString::fromLatin1(shortcut.action) + '=';
        const QString entry = key + QString::fromLatin1("%1,%2,%3").arg(shortcut.keys, shortcut.kwinDefault, shortcut.text);

        int at = -1;
        for (int i = groupStart + 1; i < groupEnd; ++i) {
            if (lines.at(i).startsWith(key)) {
                at = i;
                break;
            }
        }
        if (at < 0) {
            lines.insert(groupEnd++, entry);
            continue;
        }
        const QString active = lines.at(at).mid(key.size()).section(',', 0, 0);
        if (active == QLatin1String("none") || active == QLatin1String(shortcut.kwinDefault))
            lines[at] = entry;
    }

    if (file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        file.write(lines.join('\n').toUtf8());
}

void Application::repairKWinShortcuts()
{
    // Plain text on purpose: QSettings would rewrite the tab/comma separated values
    QFile file(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/kglobalshortcutsrc");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    const QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    file.close();

    QStringList kept;
    bool inKWin = false;
    int dropped = 0;
    for (const QString &line : lines) {
        if (line.startsWith('['))
            inKWin = line == QLatin1String("[kwin]");
        const int eq = line.indexOf('=');
        if (inKWin && eq > 0 && line.mid(eq + 1).startsWith(QLatin1String("none,none,"))) {
            ++dropped;
            continue;
        }
        kept << line;
    }

    if (dropped && file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        file.write(kept.join('\n').toUtf8());
        qDebug() << "Reset" << dropped << "empty KWin shortcuts to their defaults";
    }
}

bool Application::syncDBusEnvironment()
{
    int exitCode = 0;

    // At this point all environment variables are set, let's send it to the DBus session server to update the activation environment
    if (!QStandardPaths::findExecutable(QStringLiteral("dbus-update-activation-environment")).isEmpty()) {
        exitCode = runSync(QStringLiteral("dbus-update-activation-environment"), { QStringLiteral("--systemd"), QStringLiteral("--all") });
    }

    return exitCode == 0;
}

// xdg-desktop-portal reads XDG_CURRENT_DESKTOP once, when it starts, to pick its
// backends (lingmo-portals.conf: KDE's, then GTK's). One D-Bus started before the
// environment above reached it runs with GTK's alone: no screen sharing on KWin,
// browsers offer only their tabs. Such a portal is restarted with this session's.
void Application::restartStalePortal()
{
    QProcess show;
    show.start(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("show"), QStringLiteral("--property=MainPID"),
                                            QStringLiteral("--value"), QStringLiteral("xdg-desktop-portal.service")});
    if (!show.waitForFinished(3000))
        return;
    const int pid = show.readAllStandardOutput().trimmed().toInt();
    if (pid <= 0)
        return;     // not running yet: it starts with the right environment

    QFile environ(QStringLiteral("/proc/%1/environ").arg(pid));
    if (!environ.open(QIODevice::ReadOnly))
        return;
    const QByteArray wanted = "XDG_CURRENT_DESKTOP=" + qgetenv("XDG_CURRENT_DESKTOP");
    if (environ.readAll().split('\0').contains(wanted))
        return;

    qInfo() << "xdg-desktop-portal started without" << wanted << "- restarting it";
    QProcess::startDetached(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("restart"),
                                                          QStringLiteral("xdg-desktop-portal.service")});
}

// Import systemd user environment.
// Systemd read ~/.config/environment.d which applies to all systemd user unit.
// But it won't work if lingmoDE is not started by systemd.
void Application::importSystemdEnvrionment()
{
    auto environment = getSystemdEnvironment();
    if (!environment) {
        return;
    }

    for (auto &envString : environment.value()) {
        const auto env = envString.toLocal8Bit();
        const int idx = env.indexOf('=');
        if (Q_UNLIKELY(idx <= 0)) {
            continue;
        }

        const auto name = env.left(idx);
        if (isShellVariable(name) || isSessionVariable(name)) {
            continue;
        }
        setEnvironmentVariable(name, env.mid(idx + 1));
    }
}

void Application::createConfigDirectory()
{
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);

    if (!QDir().mkpath(configDir))
        qDebug() << "Could not create config directory XDG_CONFIG_HOME: " << configDir;
}

void Application::updateUserDirs()
{
    // Folder names are not forced here (the old `LC_ALL=C xdg-user-dirs-update
    // --force` only created new English folders next to the localized ones).
    // xdg-user-dirs-update (autostart/systemd) creates missing folders, and
    // lingmo-userdirs --login-check (/etc/xdg/autostart/lingmo-userdirs.desktop)
    // offers to rename them when the language differs from user-dirs.locale.
    // The session skips xdg-user-dirs-gtk's prompt (see ProcessManager).
}

int Application::runSync(const QString &program, const QStringList &args, const QStringList &env)
{
    QProcess p;

    if (!env.isEmpty())
        p.setEnvironment(QProcess::systemEnvironment() << env);

    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.start(program, args);
    p.waitForFinished(-1);

    if (p.exitCode()) {
        qWarning() << program << args << "exited with code" << p.exitCode();
    }

    return p.exitCode();
}
