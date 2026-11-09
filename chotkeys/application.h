#ifndef APPLICATION_H
#define APPLICATION_H

#include <QObject>
#include "QHotkey/qhotkey.h"
#include "hotkeys.h"
#include "metatap.h"
#include <QSettings>
#include <QStandardPaths>
#include <QProcess>
#include <QList>
#include <QFile>

class QAction;

class Application : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.lingmo.Chotkeys")
public:
    explicit Application(QObject *parent = nullptr);

public slots:
    // Meta pressed and released on its own. Wayland: KWin calls this
    // (kwinrc [ModifierOnlyShortcuts] Meta=, set by lingmo-session); on X11
    // MetaTap sees the tap itself and this does nothing.
    Q_SCRIPTABLE void metaTapped();

private slots:
    void initSetting();

private:
    void cleanSetting();
    void importForeignShortcuts(QSettings &setting);
    QStringList all;
    QStringList allexec;
    QList<QHotkey*> allkey;
    // Wayland: shortcuts registered with KGlobalAccel (kglobalaccel lives in KWin)
    QList<QAction *> m_actions;
    bool m_wayland = false;
    MetaTap *m_metaTap = nullptr;
    QString m_metaExec;   // command bound to Meta on its own, if any
    QSettings setting;
};

#endif // APPLICATION_H
