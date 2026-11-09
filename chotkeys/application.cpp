#include "application.h"
#include <QAction>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QGuiApplication>
#include <KGlobalAccel>
#include <QDebug>
#include <QKeySequence>
#include <QMap>
#include <QProcess>
#include <QRegularExpression>

static QString runOutput(const QString &program, const QStringList &args)
{
    QProcess process;
    process.start(program, args);
    if (!process.waitForFinished(2000) || process.exitCode() != 0)
        return QString();
    return QString::fromUtf8(process.readAllStandardOutput()).trimmed();
}

// Wayland: Guake runs on XWayland and only sees the pointer while it is over an X
// window, so "appear on the mouse display" picks a stale monitor. KWin knows the
// active output: point Guake at it for this show, then put its setting back
static bool toggleGuakeOnActiveOutput()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    const QString service = QStringLiteral("org.guake3.RemoteControl");
    if (!bus.interface()->isServiceRegistered(service))
        return false;
    if (runOutput(QStringLiteral("gsettings"), {QStringLiteral("get"), QStringLiteral("org.guake.general"),
                                                 QStringLiteral("mouse-display")}) != QLatin1String("true"))
        return false;

    QDBusMessage active = bus.call(QDBusMessage::createMethodCall(QStringLiteral("org.kde.KWin"), QStringLiteral("/KWin"),
                                                                  QStringLiteral("org.kde.KWin"),
                                                                  QStringLiteral("activeOutputName")), QDBus::Block, 1000);
    if (active.type() != QDBusMessage::ReplyMessage || active.arguments().isEmpty())
        return false;
    const QString output = active.arguments().first().toString();

    // GDK numbers X monitors in RandR order, the order xrandr lists them
    // (" 0: +*DP-1 1920/598x1080/336+1440+0  DP-1")
    int index = -1;
    const QStringList monitors = runOutput(QStringLiteral("xrandr"), {QStringLiteral("--listmonitors")}).split('\n');
    for (const QString &line : monitors) {
        const QStringList fields = line.split(' ', Qt::SkipEmptyParts);
        if (fields.size() >= 2 && fields.first().endsWith(':') && fields.last() == output) {
            index = fields.first().chopped(1).toInt();
            break;
        }
    }
    if (index < 0)
        return false;

    auto setGuake = [](const QString &key, const QString &value) {
        runOutput(QStringLiteral("gsettings"), {QStringLiteral("set"), QStringLiteral("org.guake.general"), key, value});
    };
    setGuake(QStringLiteral("display-n"), QString::number(index));
    setGuake(QStringLiteral("mouse-display"), QStringLiteral("false"));
    // Guake reads both keys while showing, before it answers
    bus.call(QDBusMessage::createMethodCall(service, QStringLiteral("/org/guake3/RemoteControl"), service,
                                            QStringLiteral("show_hide")), QDBus::Block, 3000);
    setGuake(QStringLiteral("mouse-display"), QStringLiteral("true"));
    return true;
}

// Exec lines carry arguments ("guake -t"): Qt 6 no longer splits them for us
static void runCommand(const QString &command)
{
    QStringList args = QProcess::splitCommand(command);
    if (args.isEmpty())
        return;

    if (QFileInfo(args.first()).fileName() == QLatin1String("guake")
            && (args.size() == 1 || args.at(1) == QLatin1String("-t"))
            && QGuiApplication::platformName() != QLatin1String("xcb")
            && toggleGuakeOnActiveOutput())
        return;

    // Wayland: lock with KWin's screen locker (kscreenlocker with the Lingmo
    // theme); lingmo-screenlocker (Meta+L by default) is the X11 locker
    if (QFileInfo(args.first()).fileName() == QLatin1String("lingmo-screenlocker")
            && QGuiApplication::platformName() != QLatin1String("xcb")) {
        QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"),
                                                              QStringLiteral("/ScreenSaver"),
                                                              QStringLiteral("org.freedesktop.ScreenSaver"),
                                                              QStringLiteral("Lock"));
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message));
        QObject::connect(watcher, &QDBusPendingCallWatcher::finished, [](QDBusPendingCallWatcher *call) {
            if (call->isError())
                QProcess::startDetached(QStringLiteral("loginctl"), {QStringLiteral("lock-session")});
            call->deleteLater();
        });
        return;
    }

    QProcess::startDetached(args.takeFirst(), args);
}

Application::Application(QObject *parent)
    : QObject{parent}
{
    // X11: grabs on the X server (QHotkey) and XInput 2 for Meta alone. Wayland:
    // only the compositor sees the keyboard, so KGlobalAccel (inside KWin) and
    // KWin's modifier-only shortcut calling metaTapped()
    m_wayland = QGuiApplication::platformName() != QLatin1String("xcb");

    QDBusConnection::sessionBus().registerService(QStringLiteral("com.lingmo.Chotkeys"));
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Chotkeys"), this, QDBusConnection::ExportScriptableSlots);

    if (!m_wayland) {
        m_metaTap = new MetaTap(this);
        connect(m_metaTap, &MetaTap::tapped, this, [this] {
            if (!m_metaExec.isEmpty())
                runCommand(m_metaExec);
        });
    }
    initSetting();
    const QString configFile = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)+"/lingmoglobalshortcutsrc";
    QFileSystemWatcher *m_FileWatcher = new QFileSystemWatcher(this);
    m_FileWatcher->addPath(configFile);
    connect(m_FileWatcher, &QFileSystemWatcher::fileChanged, this, [this, m_FileWatcher, configFile] {
        // QSettings saves by replacing the file, which drops it from the watcher
        m_FileWatcher->addPath(configFile);
        initSetting();
    });
}

void Application::metaTapped()
{
    if (m_wayland && !m_metaExec.isEmpty())
        runCommand(m_metaExec);
}

void Application::initSetting()
{
    cleanSetting();
    QFile file(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)+"/lingmoglobalshortcutsrc");
    QSettings setting(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation)+"/lingmoglobalshortcutsrc", QSettings::IniFormat);
    if(!file.exists())
    {
        setting.beginGroup("Ctrl+Alt+A");
        setting.setValue("Comment","Screenshot");
        setting.setValue("Exec","lingmo-screenshot");
        setting.endGroup();

        setting.beginGroup("Meta+L");
        setting.setValue("Comment","Screenlocker");
        setting.setValue("Exec","lingmo-screenlocker");
        setting.endGroup();

        setting.beginGroup("Ctrl+Alt+T");
        setting.setValue("Comment","Terminal");
        setting.setValue("Exec","lingmo-terminal");
        setting.endGroup();

        setting.beginGroup("Meta");
        setting.setValue("Comment","Launcher");
        setting.setValue("Exec","lingmo-launcher");
        setting.endGroup();

        setting.beginGroup("Ctrl+Alt+F");
        setting.setValue("Comment","Filemanager");
        setting.setValue("Exec","lingmo-filemanager");
        setting.endGroup();

        setting.beginGroup("Ctrl+Alt+I");
        setting.setValue("Comment","Debinstaller");
        setting.setValue("Exec","lingmo-debinstaller");
        setting.endGroup();

    }

    // Before the late defaults, so a Print key bound to Flameshot in Cinnamon/GNOME wins
    importForeignShortcuts(setting);

    // Shortcuts that came later than the defaults above: add them to existing configs
    // too, unless the user already bound the command or the key to something else
    static const QList<QStringList> lateDefaults = {
        {"Meta+Space", "Spotlight", "lingmo-spotlight"},
        {"Meta+R", "Reload desktop", "lingmo-reload"},
        {"Meta+V", "Clipboard history", "lingmo-clipboard --history"},
        {"Meta+I", "Assistant", "lingmo-assistant"},
        // Push-to-talk: a second press stops listening
        {"Meta+Shift+I", "Ask the assistant by voice", "lingmo-assistant --voice"},
        {"Print", "Screenshot", "lingmo-screenshot gui"},
        {"Shift+Print", "Screenshot (all screens)", "lingmo-screenshot full"},
        {"Ctrl+Print", "Screen recording", "lingmo-screenshot record"},
        // Circle (or select) anything on the screen and ask the local AI about it
        {"Meta+S", "Ask about the screen", "lingmo-screenshot ask"},
        // Live translation on/off with the last settings (Meta+T is KWin's tile editor)
        {"Meta+Shift+T", "Live translation", "lingmo-translate --toggle"},
        // Voice keyboard: type by voice in any app (lingmo-voicestudio); a second press stops.
        // Meta+D is KWin's "Show desktop"; Meta+H is free in KWin and here.
        {"Meta+H", "Dictation", "lingmo-dictation --toggle"},
        // File actions: the conversion wheel for the files selected in Lingmo Files, or the
        // files/picture on the clipboard (lingmo-fileaction; dragging + Shift/Ctrl/Alt also opens it)
        {"Meta+Shift+A", "File actions", "lingmo-fileaction --keyboard"},
    };
    for (const QStringList &shortcut : lateDefaults) {
        bool bound = setting.childGroups().contains(shortcut[0]);
        for (const QString &group : setting.childGroups())
            bound |= setting.value(group + "/Exec").toString().contains(shortcut[2]);
        if (!bound) {
            setting.beginGroup(shortcut[0]);
            setting.setValue("Comment", shortcut[1]);
            setting.setValue("Exec", shortcut[2]);
            setting.endGroup();
            setting.sync();
        }
    }

    all = setting.childGroups();
    for (int i = 0; i < all.size() ; ++i)
    {
        setting.beginGroup(all.at(i));
        allexec<<setting.value("Exec").toString();

        // A lone modifier can't be grabbed as a hotkey: MetaTap watches for Meta taps
        const QKeySequence sequence(all.at(i));
        if (!sequence.isEmpty() && sequence[0].key() == Qt::Key_Meta
            && sequence[0].keyboardModifiers() == Qt::NoModifier) {
            m_metaExec = allexec.last();
            allkey << nullptr;
            setting.endGroup();
            continue;
        }

        if (m_wayland) {
            allkey << nullptr;
            // One action per key; its name is the key so a new key is a new action
            auto *action = new QAction(this);
            action->setObjectName(all.at(i));
            action->setText(setting.value("Comment", all.at(i)).toString());
            action->setProperty("componentName", QStringLiteral("lingmo-chotkeys"));
            action->setProperty("componentDisplayName", QStringLiteral("Lingmo"));
            KGlobalAccel::self()->setDefaultShortcut(action, {sequence}, KGlobalAccel::NoAutoloading);
            KGlobalAccel::self()->setShortcut(action, {sequence}, KGlobalAccel::NoAutoloading);
            const QString exec = allexec.at(i);
            connect(action, &QAction::triggered, this, [exec] { runCommand(exec); });
            m_actions << action;
            setting.endGroup();
            continue;
        }

        allkey << new QHotkey(sequence, true, this);
        qDebug() << "Is segistered:" << allkey[i]->isRegistered();
        QObject::connect(allkey.at(i), &QHotkey::activated,[=](){
            qDebug() << i;
            qDebug() << allexec.at(i);
            runCommand(allexec.at(i));
        });
        setting.endGroup();
    }
}
void Application::cleanSetting()
{
    for (QAction *action : std::as_const(m_actions)) {
        KGlobalAccel::self()->removeAllShortcuts(action);
        delete action;
    }
    m_actions.clear();
    all.clear();
    allexec.clear();
    m_metaExec.clear();
    foreach(QHotkey *tmp,allkey)
    {
        if(tmp)
        {
            allkey.removeOne(tmp);
            tmp->disconnect();
            delete tmp;
            tmp = nullptr;
        }
    }
    allkey.clear();
}

// GTK accelerator ("<Primary><Alt>t", "F12") to a QKeySequence string ("Ctrl+Alt+T")
static QString gtkAccelToSequence(QString accel)
{
    static const QList<QPair<QString, QString>> modifiers = {
        {"<primary>", "Ctrl+"}, {"<control>", "Ctrl+"}, {"<ctrl>", "Ctrl+"}, {"<alt>", "Alt+"},
        {"<super>", "Meta+"}, {"<meta>", "Meta+"}, {"<shift>", "Shift+"},
    };
    QString sequence;
    for (bool found = true; found && accel.startsWith('<');) {
        found = false;
        for (const auto &mod : modifiers) {
            if (accel.startsWith(mod.first, Qt::CaseInsensitive)) {
                sequence += mod.second;
                accel.remove(0, mod.first.size());
                found = true;
            }
        }
        if (!found)
            return {};   // modifier we don't know (<Hyper>, ...)
    }
    if (accel.isEmpty())
        return {};
    if (accel.size() == 1)
        accel = accel.toUpper();
    const QKeySequence key(sequence + accel);
    return key.isEmpty() || key[0].key() == Qt::Key_unknown ? QString() : key.toString();
}

// Users coming from Cinnamon or GNOME keep their custom shortcuts (F12 for Guake, Print
// for Flameshot, ...): import them once, never replacing a key Lingmo already uses
void Application::importForeignShortcuts(QSettings &setting)
{
    QSettings state(QStringLiteral("lingmoos"), QStringLiteral("chotkeys"));
    if (state.value(QStringLiteral("ForeignShortcutsImported"), false).toBool())
        return;
    state.setValue(QStringLiteral("ForeignShortcutsImported"), true);

    const QStringList sources = {
        QStringLiteral("/org/cinnamon/desktop/keybindings/custom-keybindings/"),
        QStringLiteral("/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/"),
    };
    static const QRegularExpression quoted(QStringLiteral("'((?:[^'\\\\]|\\\\.)*)'"));

    for (const QString &source : sources) {
        QProcess dconf;
        dconf.start(QStringLiteral("dconf"), {QStringLiteral("dump"), source});
        if (!dconf.waitForFinished(3000) || dconf.exitCode() != 0)
            continue;

        // [customN] sections with binding=['F12'] (Cinnamon) or binding='F12' (GNOME)
        QMap<QString, QString> entry;
        auto flush = [&] {
            const QString command = entry.value(QStringLiteral("command"));
            const QString sequence = gtkAccelToSequence(entry.value(QStringLiteral("binding")));
            if (!command.isEmpty() && !sequence.isEmpty() && !setting.childGroups().contains(sequence)) {
                setting.beginGroup(sequence);
                setting.setValue(QStringLiteral("Comment"), entry.value(QStringLiteral("name"), command));
                setting.setValue(QStringLiteral("Exec"), command);
                setting.endGroup();
                qDebug() << "Imported shortcut" << sequence << command;
            }
            entry.clear();
        };
        const QStringList lines = QString::fromUtf8(dconf.readAllStandardOutput()).split('\n');
        for (const QString &line : lines) {
            if (line.startsWith('[')) {
                flush();
                continue;
            }
            const int eq = line.indexOf('=');
            const auto match = quoted.match(line, eq + 1);
            if (eq > 0 && match.hasMatch())
                entry.insert(line.left(eq), match.captured(1).replace(QStringLiteral("\\'"), QStringLiteral("'")));
        }
        flush();
    }
    setting.sync();
}
