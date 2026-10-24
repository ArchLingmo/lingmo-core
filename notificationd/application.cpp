/*
 * Copyright (C) 2023-2024 LingmoOS Team.
 *
 * Author:     Reion Wong <reion@lingmoos.com>
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
#include "notificationsmodel.h"
#include "screenhelper.h"
#include "notificationadaptor.h"
#include "historymodel.h"
#include "notificationpopup.h"

#include <QCommandLineParser>
#include <QQmlApplicationEngine>
#include <QQmlContext>

#include <QDBusInterface>
#include <QDBusPendingCall>
#include <QDBusConnection>
#include <QPixmapCache>
#include <QTranslator>
#include <QFileInfo>
#include <QIcon>
#include <QDir>

#include <QDebug>
#ifdef HAVE_LINGMO_INTENTS
#include <LingmoIntents/intentservice.h>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif

Application::Application(int& argc, char** argv)
    : QApplication(argc, argv)
    , m_notificationServer(NotificationServer::self())
    , m_model(NotificationsModel::self())
    , m_window(nullptr)
    , m_settings(Settings::self())
    , m_instance(false)
{
    if (QDBusConnection::sessionBus().registerService("com.lingmo.Notification")) {
        setOrganizationName("lingmoos");

        // Translations
        QLocale locale;
        QString qmFilePath = QString("%1/%2.qm").arg("/usr/share/lingmo-notificationd/translations/").arg(locale.name());
        if (QFile::exists(qmFilePath)) {
            QTranslator *translator = new QTranslator(this);
            if (translator->load(qmFilePath)) {
                installTranslator(translator);
            } else {
                translator->deleteLater();
            }
        }

        new NotificationAdaptor(this);
        QDBusConnection::sessionBus().registerObject("/Notification", this);
#ifdef HAVE_LINGMO_INTENTS
        publishIntents();
#endif

        HistoryModel *history = HistoryModel::self();
        for (auto sig : {&HistoryModel::rowsInserted, &HistoryModel::rowsRemoved})
            connect(history, sig, this, &Application::countChanged);
        connect(history, &HistoryModel::modelReset, this, &Application::countChanged);

        qmlRegisterType<NotificationsModel>("Lingmo.Notification", 1, 0, "NotificationsModel");
        qmlRegisterType<HistoryModel>("Lingmo.Notification", 1, 0, "HistoryModel");
        qmlRegisterType<ScreenHelper>("Lingmo.Notification", 1, 0, "ScreenHelper");
        qmlRegisterType<NotificationPopup>("Lingmo.Notification", 1, 0, "NotificationPopup");

        m_instance = true;
    }
}

#ifdef HAVE_LINGMO_INTENTS
// notify.show (Lingmo Intents) on com.lingmo.Notification /Intents
void Application::publishIntents()
{
    using LingmoIntents::IntentCall;
    using LingmoIntents::IntentResult;
    auto *intents = new LingmoIntents::IntentService(QStringLiteral("lingmo-notificationd"), this);
    intents->handleAsync(QStringLiteral("notify.show"), [this](const IntentCall &call) {
        const QVariantMap p = call.params();
        // Who asked is shown as the app name (never taken from the parameters)
        static const QHash<QString, QString> names{{QStringLiteral("assistant"), tr("Assistant")},
                                                   {QStringLiteral("flows"), tr("Flows")},
                                                   {QStringLiteral("spotlight"), tr("Spotlight")}};
        const QString app = names.value(call.caller(), QStringLiteral("Lingmo"));
        const QString urgency = p.value(QStringLiteral("urgency")).toString();
        const uchar level = urgency == QLatin1String("low") ? 0 : urgency == QLatin1String("critical") ? 2 : 1;
        // Through the bus, like any other app: the server reads the D-Bus message of each call
        QDBusMessage msg = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                                          QStringLiteral("/org/freedesktop/Notifications"),
                                                          QStringLiteral("org.freedesktop.Notifications"),
                                                          QStringLiteral("Notify"));
        msg << app << uint(0) << p.value(QStringLiteral("icon")).toString() << p.value(QStringLiteral("title")).toString()
            << p.value(QStringLiteral("body")).toString() << QStringList()
            << QVariantMap{{QStringLiteral("urgency"), QVariant::fromValue(level)}} << int(-1);
        auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(msg), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, call] {
            watcher->deleteLater();
            const QDBusPendingReply<uint> reply = *watcher;
            if (reply.isError())
                return call.finish(IntentResult::failed(reply.error().message()));
            call.finish(IntentResult::ok({{QStringLiteral("id"), reply.value()}}));
        });
    });
    intents->publish();
}
#endif

void Application::showWindow()
{
    if (m_window)
        m_window->open();
}

void Application::setDoNotDisturb(bool enabled)
{
    m_settings->setDoNotDisturb(enabled);
}

bool Application::doNotDisturb() const
{
    return m_settings->doNotDisturb();
}

int Application::run()
{
    if (!parseCommandLineArgs())
        return 0;

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("notificationsModel", m_model);
    engine.rootContext()->setContextProperty("Settings", m_settings);
    engine.load(QUrl("qrc:/qml/main.qml"));

    m_window = new NotificationWindow;

    connect(m_settings, &Settings::doNotDisturbChanged, this, &Application::doNotDisturbChanged);

    return QApplication::exec();
}

bool Application::parseCommandLineArgs()
{
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Notification"));
    parser.addHelpOption();

    QCommandLineOption showOption(QStringList() << "s" << "show" << "Show dialog");
    parser.addOption(showOption);

    parser.process(arguments());

    if (m_instance) {
        QPixmapCache::setCacheLimit(2048);
    } else {
        QDBusInterface iface("com.lingmo.Notification",
                             "/Notification",
                             "com.lingmo.Notification",
                             QDBusConnection::sessionBus(), this);
        if (iface.isValid() && parser.isSet(showOption)) {
            iface.call("showWindow");
        }
    }

    return m_instance;
}

int Application::count() const
{
    return HistoryModel::self()->rowCount();
}
