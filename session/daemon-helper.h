#ifndef __DAEMON_HELPER_
#define __DAEMON_HELPER_

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QDebug>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QList>
#include <QHash>
#include <QSet>
#include <QElapsedTimer>
#include <QPointer>

namespace LINGMO_SESSION {
  class Daemon : public QObject {
    Q_OBJECT

  public:
    /**
     * Start all the passed process using daemon.
     * @param processList Process list to start
     * @param parent
     */
    explicit Daemon(const QList<QPair<QString, QStringList>>& processList, bool _enableAutoStart = true, QObject* parent = nullptr);

  public slots:

    /**
     *  Handle the case when the progarm has some errors (i.e. crashed)
     * @param error
     */
    void onProcessError(QProcess::ProcessError error);

    /**
     * Stop and start these programs again (or start them when they were given up on),
     * without counting it as a failure. Used when the outputs come back on Wayland.
     */
    void restart(const QStringList &programs);

  private:
    /**
     * Start a program that went away again, with backoff, unless it keeps failing
     */
    void scheduleRestart(const QString &program, bool crashed);

    /**
     *  Start a given process using daemon helper
     * @brief startProcess
     * @param processInfo
     */
    void startProcess(const QPair<QString, QStringList>& processInfo);

    QList<QPair<QString, QStringList>> m_processList;

    /**
     * @brief Consecutive restarts per program, and how long each has been up.
     * Used to back off and eventually give up on programs that keep failing.
     */
    QHash<QString, int> m_restartCount;
    QHash<QString, QElapsedTimer> m_uptime;

    /**
     * @brief Crashes right after starting, per program, and the programs that now run
     * with QtQuick's software renderer because OpenGL keeps failing for them (e.g. a
     * graphics driver updated but its kernel module not reloaded yet).
     */
    QHash<QString, int> m_quickCrashes;
    QSet<QString> m_softwareRendering;

    /**
     * @brief Whether to enable auto reload when process exited.
     */
    bool m_enableAutoRestart;

    /**
     * @brief The running process of each program, and the ones stopped on purpose by
     * restart() (brought back right away).
     */
    QHash<QString, QPointer<QProcess>> m_processes;
    QSet<QString> m_expectedExit;
  };
}
#endif
