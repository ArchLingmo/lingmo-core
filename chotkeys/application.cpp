#include "application.h"
#include <QFileSystemWatcher>
#include <QDebug>
#include <QKeySequence>
#include <QMap>
#include <QRegularExpression>

// Exec lines carry arguments ("guake -t"): Qt 6 no longer splits them for us
static void runCommand(const QString &command)
{
    QStringList args = QProcess::splitCommand(command);
    if (!args.isEmpty())
        QProcess::startDetached(args.takeFirst(), args);
}

Application::Application(QObject *parent)
    : QObject{parent}
{
    m_metaTap = new MetaTap(this);
    connect(m_metaTap, &MetaTap::tapped, this, [this] {
        if (!m_metaExec.isEmpty())
            runCommand(m_metaExec);
    });
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
