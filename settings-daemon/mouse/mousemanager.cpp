/*
 * Copyright (C) 2023-2024 LingmoOS Team.
 *
 * Author:     Reion Wong <reionwong@gmail.com>
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

#include "mousemanager.h"
#include "mouseadaptor.h"
#include "libinputsettings.h"
#include "../input/kwininputdevices.h"

#include <QDebug>

#ifdef HAVE_X11
#include "x11libinputdummydevice.h"
#endif

// Keys shared with the X11 path (x11libinputdummydevice.h)
static const QString s_leftHanded = QStringLiteral("XLbInptLeftHanded");
static const QString s_flat = QStringLiteral("XLbInptAccelProfileFlat");
static const QString s_naturalScroll = QStringLiteral("XLbInptNaturalScroll");
static const QString s_acceleration = QStringLiteral("XLbInptPointerAcceleration");

Mouse::Mouse(QObject *parent)
    : QObject(parent)
{
    // init dbus
    new MouseAdaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Mouse"), this);

#ifdef HAVE_X11
    if (QX11Info::isPlatformX11() && QX11Info::display()) {
        m_inputDummydevice = new X11LibinputDummyDevice(this, QX11Info::display());
        connect(m_inputDummydevice, &X11LibinputDummyDevice::leftHandedChanged, this, &Mouse::leftHandedChanged);
        connect(m_inputDummydevice, &X11LibinputDummyDevice::pointerAccelerationProfileChanged, this, &Mouse::accelerationChanged);
        connect(m_inputDummydevice, &X11LibinputDummyDevice::naturalScrollChanged, this, &Mouse::naturalScrollChanged);
        connect(m_inputDummydevice, &X11LibinputDummyDevice::pointerAccelerationChanged, this, &Mouse::pointerAccelerationChanged);
        return;
    }
#endif

    LibinputSettings settings;
    m_leftHanded = settings.load(s_leftHanded, false);
    m_flat = settings.load(s_flat, false);
    m_naturalScroll = settings.load(s_naturalScroll, false);
    m_pointerAcceleration = settings.load(s_acceleration, qreal(0));

    if (!KWinInputDevices::isWayland()) {
        qWarning() << "Mouse: neither X11 nor Wayland, settings are only stored";
        return;
    }

    m_kwin = new KWinInputDevices(this);
    connect(m_kwin, &KWinInputDevices::devicesChanged, this, &Mouse::applyWayland);
    applyWayland();
}

Mouse::~Mouse()
{
#ifdef HAVE_X11
    delete m_inputDummydevice;
#endif
}

void Mouse::applyWayland()
{
    if (!m_kwin)
        return;
    m_kwin->setValue(KWinInputDevices::Mouse, QStringLiteral("leftHanded"), m_leftHanded);
    m_kwin->setValue(KWinInputDevices::Mouse, QStringLiteral("pointerAccelerationProfileFlat"), m_flat);
    m_kwin->setValue(KWinInputDevices::Mouse, QStringLiteral("pointerAccelerationProfileAdaptive"), !m_flat);
    m_kwin->setValue(KWinInputDevices::Mouse, QStringLiteral("naturalScroll"), m_naturalScroll);
    m_kwin->setValue(KWinInputDevices::Mouse, QStringLiteral("pointerAcceleration"), m_pointerAcceleration);
}

bool Mouse::leftHanded() const
{
#ifdef HAVE_X11
    if (m_inputDummydevice)
        return m_inputDummydevice->isLeftHanded();
#endif
    return m_leftHanded;
}

void Mouse::setLeftHanded(bool enabled)
{
#ifdef HAVE_X11
    if (m_inputDummydevice) {
        m_inputDummydevice->setLeftHanded(enabled);
        m_inputDummydevice->applyConfig();
        return;
    }
#endif
    if (m_leftHanded == enabled)
        return;
    m_leftHanded = enabled;
    LibinputSettings().save(s_leftHanded, enabled);
    applyWayland();
    emit leftHandedChanged();
}

bool Mouse::acceleration() const
{
#ifdef HAVE_X11
    if (m_inputDummydevice)
        return m_inputDummydevice->pointerAccelerationProfileFlat();
#endif
    return m_flat;
}

void Mouse::setAcceleration(bool enabled)
{
#ifdef HAVE_X11
    if (m_inputDummydevice) {
        m_inputDummydevice->setPointerAccelerationProfileFlat(enabled);
        m_inputDummydevice->applyConfig();
        return;
    }
#endif
    if (m_flat == enabled)
        return;
    m_flat = enabled;
    LibinputSettings().save(s_flat, enabled);
    applyWayland();
    emit accelerationChanged();
}

bool Mouse::naturalScroll() const
{
#ifdef HAVE_X11
    if (m_inputDummydevice)
        return m_inputDummydevice->isNaturalScroll();
#endif
    return m_naturalScroll;
}

void Mouse::setNaturalScroll(bool enabled)
{
#ifdef HAVE_X11
    if (m_inputDummydevice) {
        m_inputDummydevice->setNaturalScroll(enabled);
        m_inputDummydevice->applyConfig();
        return;
    }
#endif
    if (m_naturalScroll == enabled)
        return;
    m_naturalScroll = enabled;
    LibinputSettings().save(s_naturalScroll, enabled);
    applyWayland();
    emit naturalScrollChanged();
}

qreal Mouse::pointerAcceleration() const
{
#ifdef HAVE_X11
    if (m_inputDummydevice)
        return m_inputDummydevice->pointerAcceleration();
#endif
    return m_pointerAcceleration;
}

void Mouse::setPointerAcceleration(qreal value)
{
#ifdef HAVE_X11
    if (m_inputDummydevice) {
        m_inputDummydevice->setPointerAcceleration(value);
        m_inputDummydevice->applyConfig();
        return;
    }
#endif
    value = qBound(qreal(-1), value, qreal(1));
    if (qFuzzyCompare(m_pointerAcceleration + 2, value + 2))
        return;
    m_pointerAcceleration = value;
    LibinputSettings().save(s_acceleration, value);
    applyWayland();
    emit pointerAccelerationChanged();
}
