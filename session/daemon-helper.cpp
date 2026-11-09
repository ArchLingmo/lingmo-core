#include <QStandardPaths>
#include <QFileInfo>
#include <QDateTime>
/**
 * @name daemon-helper.cpp
 * @author Elysia <c.elysia@foxmail.com>
 **/
#include "daemon-helper.h"

#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <QDebug>
#include <QPair>
#include <QString>
#include <QPointer>

namespace LINGMO_SESSION {
Daemon::Daemon(const QList<QPair<QString, QStringList>> &processList, bool _enableAutoStart, QObject *parent)
    : QObject(parent), m_processList(processList), m_enableAutoRestart(_enableAutoStart) {
  for (const auto &processInfo : m_processList) {
    startProcess(processInfo);
  }
}

// lingmo-reload touches $XDG_RUNTIME_DIR/lingmo-reload-<display> right before it stops
// the desktop components of that session (<display>: WAYLAND_DISPLAY in a Wayland
// session, whose DISPLAY is Xwayland's and may match another session's)
static bool reloadRequested() {
  const QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
  const QByteArray display = qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ? qgetenv("DISPLAY") : qgetenv("WAYLAND_DISPLAY");
  const QFileInfo stamp(runtimeDir + "/lingmo-reload-" + QString::fromLocal8Bit(display));
  return stamp.exists() && stamp.lastModified().secsTo(QDateTime::currentDateTime()) <= 10;
}

// A process that stays up this long is considered healthy again.
static constexpr qint64 kStableUptimeMs = 30 * 1000;
static constexpr int kMaxRestarts = 5;
// A crash this soon after starting counts towards the software-rendering fallback.
static constexpr qint64 kQuickCrashMs = 10 * 1000;

// Desktop components never quit on their own while the session runs. On Wayland they
// do when their outputs go away (layer-shell surfaces closed: a VT switch on NVIDIA, all
// monitors off), exiting normally, and only crashes used to bring them back.
static const QSet<QString> s_keepAlive = {
  QStringLiteral("lingmo-statusbar"), QStringLiteral("lingmo-dock"), QStringLiteral("lingmo-filemanager"),
  QStringLiteral("lingmo-launcher"), QStringLiteral("lingmo-notificationd")};

void Daemon::onProcessError(QProcess::ProcessError error) {
  const QPointer process = qobject_cast<QProcess *>(sender());

  if (!process)
    return;

  QString program = process->program();
  qDebug() << "Process error:" << program << "Error:" << error;
  process->deleteLater();
  scheduleRestart(program, error == QProcess::Crashed);
}

void Daemon::restart(const QStringList &programs) {
  for (const auto &processInfo : m_processList) {
    if (!programs.contains(processInfo.first))
      continue;
    m_restartCount[processInfo.first] = 0;
    QProcess *process = m_processes.value(processInfo.first);
    if (process && process->state() != QProcess::NotRunning) {
      qDebug() << "Restarting" << processInfo.first;
      m_expectedExit.insert(processInfo.first);
      process->terminate();
    } else {
      startProcess(processInfo);
    }
  }
}

void Daemon::scheduleRestart(const QString &program, bool crashed) {
  for (const auto &processInfo : m_processList) {
    if (processInfo.first == program) {
      // Stopped by restart(): straight back
      if (m_expectedExit.remove(program)) {
        QTimer::singleShot(300, this, [this, processInfo]() {
          startProcess(processInfo);
        });
        return;
      }

      // Two crashes in a row within seconds of starting: most likely OpenGL can't
      // create a context (driver updated without a reboot, broken GPU setup). Bring
      // the component back with the software renderer instead of losing the desktop.
      const bool quickCrash = crashed && m_uptime.value(program).isValid()
          && m_uptime.value(program).elapsed() < kQuickCrashMs;
      m_quickCrashes[program] = quickCrash ? m_quickCrashes.value(program) + 1 : 0;
      if (m_quickCrashes.value(program) >= 2 && !m_softwareRendering.contains(program)) {
        qWarning() << program << "keeps crashing at start: restarting it with QtQuick's software renderer";
        m_softwareRendering.insert(program);
        m_restartCount[program] = 0;
        QTimer::singleShot(300, this, [this, processInfo]() {
          startProcess(processInfo);
        });
        return;
      }

      // lingmo-reload stops components on purpose: bring them straight back and
      // don't count it as a failure
      if (reloadRequested()) {
        m_restartCount[program] = 0;
        QTimer::singleShot(300, this, [this, processInfo]() {
          startProcess(processInfo);
        });
        return;
      }

      if (m_uptime.value(program).isValid() && m_uptime.value(program).elapsed() > kStableUptimeMs)
        m_restartCount[program] = 0;

      const int attempt = ++m_restartCount[program];
      if (attempt > kMaxRestarts) {
        qWarning() << "Giving up on" << program << "after" << kMaxRestarts << "failed restarts";
        return;
      }

      // Exponential backoff: 1s, 2s, 4s, 8s, 16s
      const int delayMs = 1000 << (attempt - 1);
      qDebug() << "Restarting process due to error:" << program << "in" << delayMs << "ms";
      QTimer::singleShot(delayMs, this, [this, processInfo]() {
        startProcess(processInfo);
      });
      return;
    }
  }
}

void Daemon::startProcess(const QPair<QString, QStringList> &processInfo) {
  const QPointer process = new QProcess(this);

  if (this->m_enableAutoRestart) {
    connect(process, &QProcess::errorOccurred,
            this, &Daemon::onProcessError);
    connect(process, &QProcess::finished, this, [this, process](int exitCode, QProcess::ExitStatus status) {
      if (!process || status != QProcess::NormalExit)
        return; // crashes go through errorOccurred
      const QString program = process->program();
      if (!s_keepAlive.contains(program) && !m_expectedExit.contains(program))
        return;
      qDebug() << "Process exited:" << program << "code" << exitCode;
      process->deleteLater();
      scheduleRestart(program, false);
    });
  }
  m_processes[processInfo.first] = process;

  // Let child output reach the session log (~/.xsession-errors) instead of being discarded
  process->setProcessChannelMode(QProcess::ForwardedChannels);
  if (m_softwareRendering.contains(processInfo.first)) {
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("QT_QUICK_BACKEND"), QStringLiteral("software"));
    process->setProcessEnvironment(env);
  }
  process->start(processInfo.first, processInfo.second);
  m_uptime[processInfo.first].start();
  if (process->waitForStarted()) {
    qDebug() << "Process started:" << processInfo.first << "PID:" << process->processId();
  } else {
    qDebug() << "Failed to start process:" << processInfo.first << process->errorString();
  }
}
} // namespace LINGMO_SESSION
