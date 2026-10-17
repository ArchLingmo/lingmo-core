/*
 * SPDX-FileCopyrightText: 2023-2024 Reion Wong <reion@lingmoos.com>
 * SPDX-FileCopyrightText: 2018-2019 Kai Uwe Broulik <kde@privat.broulik.de>
 * SPDX-License-Identifier: LGPL-2.1-only OR LGPL-3.0-only OR LicenseRef-KDE-Accepted-LGPL
*/

#ifndef NOTIFICATIONSMODEL_H
#define NOTIFICATIONSMODEL_H

#include <QAbstractListModel>
#include <QVector>
#include <QTimer>

#include "notificationserver.h"

class Notification;
class NotificationsModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        SummaryRole = Qt::DisplayRole,
        ImageRole = Qt::DecorationRole,
        CreatedRole,
        UpdatedRole,
        BodyRole,
        IconNameRole,
        HasDefaultActionRole,
        AppNameRole,
        ActionsRole,        // [{key, label}] without default / inline-reply
        HasReplyRole,
        ReplyPlaceholderRole,
        ReplySubmitTextRole,
        GroupRole           // app the notification is grouped under
    };
    Q_ENUM(Roles)

    static NotificationsModel *self();
    explicit NotificationsModel(QObject *parent = nullptr);

    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QHash<int, QByteArray> roleNames() const override;

    Q_INVOKABLE void expired(uint id);

    Q_INVOKABLE void close(uint id);
    Q_INVOKABLE void invokeDefaultAction(uint id);
    // Click on a notification: its default action, or else bring its app forward
    Q_INVOKABLE void activate(uint id);
    // One of the notification's buttons; closes it unless it is resident
    Q_INVOKABLE void invokeAction(uint id, const QString &actionKey);
    // Inline reply typed in the notification
    Q_INVOKABLE void reply(uint id, const QString &text);

    // Name the notification is grouped under in the notification center
    static QString groupOf(const Notification &notification);

    int rowOfNotification(uint id) const;
    void removeRows(const QVector<int> &rows);

private slots:
    void onNotificationAdded(const Notification &notification);
    void onNotificationReplaced(uint replacedId, const Notification &notification);
    void onNotificationRemoved(uint notificationId, NotificationServer::CloseReason reason);

private:
    QVector<Notification> m_notifications;
    QVector<uint /*notificationId*/> m_pendingRemovals;
    QTimer m_pendingRemovalTimer;
};

#endif // NOTIFICATIONSMODEL_H
