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

#ifndef HISTORYMODEL_H
#define HISTORYMODEL_H

#include <QAbstractListModel>
#include "notification.h"

class HistoryModel : public QAbstractListModel
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

    static HistoryModel* self();
    explicit HistoryModel(QObject *parent = nullptr);

    QVariant data(const QModelIndex &index, int role) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Grouped by app: the newest notification goes on top of its app's group and
    // that group moves to the top of the list
    Q_INVOKABLE void add(const Notification &notification);
    Q_INVOKABLE void remove(int index);
    // Click on a past notification: its default action while the app still runs,
    // else bring the app forward
    Q_INVOKABLE void activate(int index);
    Q_INVOKABLE void invokeAction(int index, const QString &actionKey);
    Q_INVOKABLE void reply(int index, const QString &text);
    Q_INVOKABLE void clearGroup(const QString &group);
    // Icon to show next to a group's name
    Q_INVOKABLE QString groupIcon(const QString &group) const;
    Q_INVOKABLE void clearAll();
    Q_INVOKABLE void save();

    void initDatas();
    void updateTime();

private:
    QVector<Notification> m_notifications;
};

#endif // HISTORYMODEL_H
