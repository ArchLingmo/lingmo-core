/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef KWININPUTDEVICES_H
#define KWININPUTDEVICES_H

#include <QObject>
#include <QStringList>
#include <QVariant>

// Input devices of KWin on Wayland (org.kde.KWin.InputDevice over D-Bus): there
// the compositor owns libinput, and KWin keeps what is set here in kcminputrc.
class KWinInputDevices : public QObject
{
    Q_OBJECT

public:
    enum Kind {
        Mouse,      // pointers that aren't touchpads
        Touchpad,
    };

    explicit KWinInputDevices(QObject *parent = nullptr);

    // The Wayland session (KWin owns the input devices)
    static bool isWayland();

    QStringList devices(Kind kind) const;
    bool available(Kind kind) const { return !devices(kind).isEmpty(); }

    // First device of that kind that has the property, else defaultValue
    QVariant value(Kind kind, const QString &property, const QVariant &defaultValue = QVariant()) const;
    // On every device of that kind (only where KWin says it is supported)
    void setValue(Kind kind, const QString &property, const QVariant &value);

signals:
    // A device was plugged in or removed: apply the settings again
    void devicesChanged();

private slots:
    void onDeviceAdded(const QString &sysName);
    void onDeviceRemoved(const QString &sysName);

private:
    void reload();
    QVariant property(const QString &sysName, const QString &name) const;
    bool setProperty(const QString &sysName, const QString &name, const QVariant &value);
    bool supports(const QString &sysName, const QString &property) const;

private:
    QStringList m_mice;
    QStringList m_touchpads;
};

#endif // KWININPUTDEVICES_H
