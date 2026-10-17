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

#include "historymodel.h"
#include "appactivator.h"
#include "datehelper.h"
#include "notificationsmodel.h"
#include "notificationserver.h"

#include <QSettings>
#include <algorithm>
#include <QDataStream>
#include <QIODevice>
#include <QMetaEnum>
#include <qflags.h>

static HistoryModel *s_historyModel = nullptr;

HistoryModel *HistoryModel::self()
{
    if (!s_historyModel)
        s_historyModel = new HistoryModel;

    return s_historyModel;
}

HistoryModel::HistoryModel(QObject *parent)
    : QAbstractListModel(parent)
{
    initDatas();
}

QVariant HistoryModel::data(const QModelIndex &index, int role) const
{
    const Notification &notification = m_notifications.at(index.row());

    switch (role) {
    case HistoryModel::IdRole:
        return notification.id;
    case HistoryModel::SummaryRole:
        return notification.summary;
    case HistoryModel::ImageRole:
        return "";
    case HistoryModel::CreatedRole:
        return DateHelper::friendlyTime(notification.created);
    case HistoryModel::UpdatedRole:
        return DateHelper::friendlyTime(notification.updated);
    case HistoryModel::BodyRole:
        return notification.body;
    case HistoryModel::IconNameRole:
        return notification.appIcon;
    case HistoryModel::HasDefaultActionRole:
        return notification.hasDefaultAction();
    case HistoryModel::AppNameRole:
        return notification.appName;
    case HistoryModel::ActionsRole:
        return notification.actionList();
    case HistoryModel::HasReplyRole:
        return notification.hasReply();
    case HistoryModel::ReplyPlaceholderRole:
        return notification.replyPlaceholder;
    case HistoryModel::ReplySubmitTextRole:
        return notification.replySubmitText;
    case HistoryModel::GroupRole:
        return NotificationsModel::groupOf(notification);
    default:
        break;
    }

    return QVariant();
}

int HistoryModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);

    return m_notifications.size();
}

QHash<int, QByteArray> HistoryModel::roleNames() const
{
    static QHash<int, QByteArray> s_roles;

    if (s_roles.isEmpty()) {
        // This generates role names from the Roles enum in the form of: FooRole -> foo
        const QMetaEnum e = QMetaEnum::fromType<HistoryModel::Roles>();

        // Qt built-in roles we use
        s_roles.insert(Qt::DisplayRole, QByteArrayLiteral("display"));
        s_roles.insert(Qt::DecorationRole, QByteArrayLiteral("decoration"));

        for (int i = 0; i < e.keyCount(); ++i) {
            const int value = e.value(i);

            QByteArray key(e.key(i));
            key[0] = key[0] + 32; // lower case first letter
            key.chop(4); // strip "Role" suffix

            s_roles.insert(value, key);
        }

        s_roles.insert(HistoryModel::IdRole, QByteArrayLiteral("notificationId")); // id is QML-reserved
    }

    return s_roles;
}

static const int s_historyLimit = 200;

static QString groupOf(const Notification &notification)
{
    return NotificationsModel::groupOf(notification);
}

void HistoryModel::add(const Notification &notification)
{
    const QString group = groupOf(notification);

    // Each app's notifications are always kept together, so the group is either
    // missing, already on top, or somewhere below
    const bool groupExists = std::any_of(m_notifications.cbegin(), m_notifications.cend(),
                                         [&](const Notification &n) { return groupOf(n) == group; });

    if (!groupExists || groupOf(m_notifications.first()) == group) {
        // Common case: the group is already on top (or new)
        beginInsertRows(QModelIndex(), 0, 0);
        m_notifications.prepend(notification);
        endInsertRows();
    } else {
        beginResetModel();
        QVector<Notification> same, others;
        for (const Notification &n : std::as_const(m_notifications))
            (groupOf(n) == group ? same : others).append(n);
        m_notifications = QVector<Notification>{notification} + same + others;
        endResetModel();
    }

    if (m_notifications.size() > s_historyLimit) {
        beginRemoveRows(QModelIndex(), s_historyLimit, m_notifications.size() - 1);
        m_notifications.resize(s_historyLimit);
        endRemoveRows();
    }

    save();
}

void HistoryModel::remove(int index)
{
    if (index < 0 || index >= m_notifications.size())
        return;

    beginRemoveRows(QModelIndex(), index, index);
    m_notifications.removeAt(index);
    endRemoveRows();
    save();
}

void HistoryModel::clearGroup(const QString &group)
{
    for (int i = m_notifications.size() - 1; i >= 0; --i) {
        if (groupOf(m_notifications.at(i)) == group) {
            beginRemoveRows(QModelIndex(), i, i);
            m_notifications.removeAt(i);
            endRemoveRows();
        }
    }
    save();
}

QString HistoryModel::groupIcon(const QString &group) const
{
    for (const Notification &n : m_notifications) {
        if (groupOf(n) == group) {
            if (!n.appIcon.isEmpty())
                return n.appIcon;
            if (!n.desktopEntry.isEmpty())
                return n.desktopEntry;
        }
    }
    return QString();
}

void HistoryModel::invokeAction(int index, const QString &actionKey)
{
    if (index < 0 || index >= m_notifications.size())
        return;

    const Notification notification = m_notifications.at(index);
    // id 0: loaded from disk, its sender is long gone
    if (notification.id != 0)
        NotificationServer::self()->InvokeAction(notification.id, actionKey);
    if (!notification.resident)
        remove(index);
}

void HistoryModel::reply(int index, const QString &text)
{
    if (index < 0 || index >= m_notifications.size() || text.trimmed().isEmpty())
        return;

    const Notification notification = m_notifications.at(index);
    if (notification.id != 0)
        NotificationServer::self()->reply(notification.id, text);
    remove(index);
}

void HistoryModel::clearAll()
{
    beginResetModel();
    m_notifications.clear();
    endResetModel();
    save();
}

// Version 2 adds the desktop entry; version 1 data (the plain operator<<) is
// still read. Actions and ids are not kept: they mean nothing after a restart.
void HistoryModel::save()
{
    QSettings settings(QSettings::UserScope, "lingmoos", "notifications");
    settings.clear();

    QByteArray datas;
    QDataStream out(&datas, QIODevice::WriteOnly);
    out << qint32(m_notifications.size());
    for (const Notification &n : std::as_const(m_notifications)) {
        out << n.service << n.summary << n.body << n.appName << n.appIcon
            << n.created << n.updated << n.desktopEntry;
    }

    settings.setValue("version", 2);
    settings.setValue("datas", datas);
}

void HistoryModel::initDatas()
{
    QSettings settings(QSettings::UserScope, "lingmoos", "notifications");
    const QByteArray datas = settings.value("datas").toByteArray();
    if (datas.isEmpty())
        return;

    // Read-only stream: the old code opened it write-only and never loaded anything
    QDataStream in(datas);
    if (settings.value("version", 1).toInt() >= 2) {
        qint32 count = 0;
        in >> count;
        for (qint32 i = 0; i < count && in.status() == QDataStream::Ok; ++i) {
            Notification n;
            in >> n.service >> n.summary >> n.body >> n.appName >> n.appIcon
               >> n.created >> n.updated >> n.desktopEntry;
            if (in.status() == QDataStream::Ok)
                m_notifications.append(n);
        }
    } else {
        in >> m_notifications;
        if (in.status() != QDataStream::Ok)
            m_notifications.clear();
    }

    // Keep each app's notifications together, groups in order of their newest one
    QVector<Notification> grouped;
    QStringList order;
    for (const Notification &n : std::as_const(m_notifications))
        if (!order.contains(groupOf(n)))
            order << groupOf(n);
    for (const QString &group : order)
        for (const Notification &n : std::as_const(m_notifications))
            if (groupOf(n) == group)
                grouped.append(n);
    m_notifications = grouped;
}

void HistoryModel::updateTime()
{
    emit layoutChanged();
}

void HistoryModel::activate(int index)
{
    if (index < 0 || index >= m_notifications.size())
        return;

    const Notification &notification = m_notifications.at(index);
    if (notification.id != 0 && notification.hasDefaultAction()) {
        NotificationServer::self()->InvokeAction(notification.id, QStringLiteral("default"));
        return;
    }
    AppActivator::activate(notification.pid, {notification.desktopEntry, notification.appIcon, notification.appName});
}
