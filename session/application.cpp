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
        name.startsWith("XDG_");
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
    parser.setApplicationDescription(QStringLiteral("Lingmo Session"));
    parser.addHelpOption();

    QCommandLineOption waylandOption(QStringList() << "w" << "wayland" << "Wayland Mode");
    parser.addOption(waylandOption);
    parser.process(*this);

    m_wayland = parser.isSet(waylandOption);

    createConfigDirectory();
    initKWinConfig();
    initLanguage();
    initScreenScaleFactors();
    initXResource();

    initEnvironments();

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
    // unset-environment can't drop what the systemd user manager inherited from
    // PAM, but an empty value overrides it, and Qt ignores an empty style
    runSync(QStringLiteral("systemctl"), {QStringLiteral("--user"), QStringLiteral("set-environment"),
                                          QStringLiteral("QT_STYLE_OVERRIDE="), QStringLiteral("QT_QUICK_CONTROLS_STYLE=")});
    syncDBusEnvironment();

    qunsetenv("XCURSOR_THEME");
    qunsetenv("XCURSOR_SIZE");
    qunsetenv("SESSION_MANAGER");

    m_networkProxyManager->update();

    QTimer::singleShot(50, this, &Application::updateUserDirs);

    // Launch Lingmo and user defined processes !
    QTimer::singleShot(100, m_processManager, &ProcessManager::start);
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
    QSettings settings(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/kwinrc",
                       QSettings::IniFormat);

    settings.beginGroup("Effect-Blur");
    settings.setValue("BlurStrength", 10);
    settings.setValue("NoiseStrength", 0);
    settings.endGroup();

    settings.beginGroup("Windows");
    settings.setValue("FocusStealingPreventionLevel", 0);
    settings.setValue("HideUtilityWindowsForInactive", false);
    settings.setValue("BorderlessMaximizedWindows", false);
    settings.setValue("Placement", "Centered");
    settings.endGroup();

    // Decoration defaults are written once per version, not on every login, so a
    // layout the user picks later is kept. v2: macOS-style buttons on the left.
    constexpr int kDecorationDefaultsVersion = 2;
    if (settings.value("Lingmo/DecorationDefaultsVersion", 0).toInt() < kDecorationDefaultsVersion) {
        // KWin 6.3+ reads the plugin from kdecoration3 and the button layout from kdecoration2
        for (const char *group : {"org.kde.kdecoration2", "org.kde.kdecoration3"}) {
            settings.beginGroup(group);
            settings.setValue("BorderSize", "Normal");
            settings.setValue("ButtonsOnLeft", "XIA");   // close, minimize, maximize
            settings.setValue("ButtonsOnRight", "");
            settings.setValue("library", "org.lingmo.decoration");
            settings.setValue("theme", "");
            settings.endGroup();
        }
        settings.setValue("Lingmo/DecorationDefaultsVersion", kDecorationDefaultsVersion);
    }

    // Lingmo used to ship a partial [kwin] group in /etc/xdg/kglobalshortcutsrc, which
    // made kglobalacceld register every other KWin shortcut (Alt+Tab, Alt+F4, ...) with
    // no key and no default, and it saved them that way. Drop those empty entries once
    // so KWin registers its defaults again; anything the user set is left alone.
    constexpr int kShortcutsRepairVersion = 1;
    if (settings.value("Lingmo/ShortcutsRepairVersion", 0).toInt() < kShortcutsRepairVersion) {
        repairKWinShortcuts();
        settings.setValue("Lingmo/ShortcutsRepairVersion", kShortcutsRepairVersion);
    }

    // Lingmo's window shortcuts on the Meta key (maximize, minimize, close, ...)
    constexpr int kWindowShortcutsVersion = 1;
    if (settings.value("Lingmo/WindowShortcutsVersion", 0).toInt() < kWindowShortcutsVersion) {
        applyWindowShortcuts();
        settings.setValue("Lingmo/WindowShortcutsVersion", kWindowShortcutsVersion);
    }
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
