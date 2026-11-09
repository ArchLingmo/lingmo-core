/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "kwininputdevices.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDebug>
#include <QGuiApplication>
#include <QTimer>

static const QString s_service = QStringLiteral("org.kde.KWin");
static const QString s_managerPath = QStringLiteral("/org/kde/KWin/InputDevice");
static const QString s_managerInterface = QStringLiteral("org.kde.KWin.InputDeviceManager");
static const QString s_deviceInterface = QStringLiteral("org.kde.KWin.InputDevice");
static const QString s_properties = QStringLiteral("org.freedesktop.DBus.Properties");

bool KWinInputDevices::isWayland()
{
    return QGuiApplication::platformName().startsWith(QLatin1String("wayland"))
        || qgetenv("XDG_SESSION_TYPE") == "wayland";
}

KWinInputDevices::KWinInputDevices(QObject *parent)
    : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(s_service, s_managerPath, s_managerInterface, QStringLiteral("deviceAdded"),
                this, SLOT(onDeviceAdded(QString)));
    bus.connect(s_service, s_managerPath, s_managerInterface, QStringLiteral("deviceRemoved"),
                this, SLOT(onDeviceRemoved(QString)));
    reload();
}

void KWinInputDevices::reload()
{
    m_mice.clear();
    m_touchpads.clear();

    QDBusMessage call = QDBusMessage::createMethodCall(s_service, s_managerPath, s_properties, QStringLiteral("Get"));
    call << s_managerInterface << QStringLiteral("devicesSysNames");
    const QDBusReply<QDBusVariant> reply = QDBusConnection::sessionBus().call(call);
    if (!reply.isValid()) {
        qWarning() << "KWin input devices unavailable:" << reply.error().message();
        return;
    }

    const QStringList names = reply.value().variant().toStringList();
    for (const QString &name : names) {
        if (property(name, QStringLiteral("touchpad")).toBool())
            m_touchpads << name;
        else if (property(name, QStringLiteral("pointer")).toBool()
                 && !property(name, QStringLiteral("tabletTool")).toBool()
                 && !property(name, QStringLiteral("touch")).toBool())
            m_mice << name;
    }
}

void KWinInputDevices::onDeviceAdded(const QString &)
{
    // KWin announces the device before its properties settle
    QTimer::singleShot(200, this, [this] {
        reload();
        emit devicesChanged();
    });
}

void KWinInputDevices::onDeviceRemoved(const QString &)
{
    reload();
    emit devicesChanged();
}

QStringList KWinInputDevices::devices(Kind kind) const
{
    return kind == Touchpad ? m_touchpads : m_mice;
}

QVariant KWinInputDevices::property(const QString &sysName, const QString &name) const
{
    QDBusMessage call = QDBusMessage::createMethodCall(s_service, s_managerPath + QLatin1Char('/') + sysName,
                                                       s_properties, QStringLiteral("Get"));
    call << s_deviceInterface << name;
    const QDBusReply<QDBusVariant> reply = QDBusConnection::sessionBus().call(call);
    return reply.isValid() ? reply.value().variant() : QVariant();
}

bool KWinInputDevices::setProperty(const QString &sysName, const QString &name, const QVariant &value)
{
    QDBusMessage call = QDBusMessage::createMethodCall(s_service, s_managerPath + QLatin1Char('/') + sysName,
                                                       s_properties, QStringLiteral("Set"));
    call << s_deviceInterface << name << QVariant::fromValue(QDBusVariant(value));
    const QDBusMessage reply = QDBusConnection::sessionBus().call(call);
    if (reply.type() == QDBusMessage::ErrorMessage) {
        qWarning() << "KWin input device" << sysName << name << reply.errorMessage();
        return false;
    }
    return true;
}

bool KWinInputDevices::supports(const QString &sysName, const QString &property) const
{
    // "naturalScroll" -> "supportsNaturalScroll"; properties without a supports*
    // twin (enabled, tapToClick) are attempted and KWin ignores what it can't do
    QString name = property;
    name[0] = name.at(0).toUpper();
    if (property == QLatin1String("tapToClick"))
        return this->property(sysName, QStringLiteral("tapFingerCount")).toInt() > 0;
    if (property == QLatin1String("enabled"))
        return this->property(sysName, QStringLiteral("supportsDisableEvents")).toBool();
    const QVariant supported = this->property(sysName, QStringLiteral("supports") + name);
    return !supported.isValid() || supported.toBool();
}

QVariant KWinInputDevices::value(Kind kind, const QString &property, const QVariant &defaultValue) const
{
    for (const QString &device : devices(kind)) {
        const QVariant v = this->property(device, property);
        if (v.isValid())
            return v;
    }
    return defaultValue;
}

void KWinInputDevices::setValue(Kind kind, const QString &property, const QVariant &value)
{
    for (const QString &device : devices(kind)) {
        if (supports(device, property))
            setProperty(device, property, value);
    }
}
