#include "touchpadmanager.h"
#include "touchpadadaptor.h"
#include "../input/kwininputdevices.h"

#include <QDebug>
#include <QSettings>

#ifdef HAVE_X11
#include "x11/xlibbackend.h"
#include <QtGui/private/qtx11extras_p.h>
#endif

// Keys here (enabled, tapToClick, naturalScroll, pointerAcceleration) are also the
// names of KWin's org.kde.KWin.InputDevice properties
TouchpadManager::TouchpadManager(QObject *parent)
    : QObject(parent)
{
    // init dbus
    new TouchpadAdaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Touchpad"), this);

#ifdef HAVE_X11
    if (QX11Info::isPlatformX11()) {
        // Null without an X server (initialize() gives up): available() is then false
        m_backend = XlibBackend::initialize(this);
        if (m_backend) {
            m_backend->getConfig();
            m_backend->applyConfig();
        }
        return;
    }
#endif

    if (KWinInputDevices::isWayland()) {
        m_kwin = new KWinInputDevices(this);
        connect(m_kwin, &KWinInputDevices::devicesChanged, this, &TouchpadManager::applyWayland);
        applyWayland();
    }
}

QVariant TouchpadManager::waylandValue(const QString &key) const
{
    if (!m_kwin)
        return QVariant();
    const QString device = m_kwin->devices(KWinInputDevices::Touchpad).value(0);
    if (device.isEmpty())
        return QVariant();
    const QVariant current = m_kwin->value(KWinInputDevices::Touchpad, key);
    QSettings config(QStringLiteral("lingmoos"), QStringLiteral("touchpadxlibinputrc"));
    // The group is the device's name, as the X11 backend writes it
    const QString name = m_kwin->value(KWinInputDevices::Touchpad, QStringLiteral("name")).toString();
    config.beginGroup(name);
    return config.value(key, current);
}

void TouchpadManager::setWaylandValue(const QString &key, const QVariant &value)
{
    if (!m_kwin)
        return;
    const QString name = m_kwin->value(KWinInputDevices::Touchpad, QStringLiteral("name")).toString();
    if (!name.isEmpty()) {
        QSettings config(QStringLiteral("lingmoos"), QStringLiteral("touchpadxlibinputrc"));
        config.beginGroup(name);
        config.setValue(key, value);
        config.endGroup();
        config.sync();
    }
    m_kwin->setValue(KWinInputDevices::Touchpad, key, value);
}

void TouchpadManager::applyWayland()
{
    if (!m_kwin || !m_kwin->available(KWinInputDevices::Touchpad))
        return;
    for (const char *key : {"enabled", "tapToClick", "naturalScroll", "pointerAcceleration"}) {
        const QString k = QString::fromLatin1(key);
        const QVariant v = waylandValue(k);
        if (v.isValid())
            m_kwin->setValue(KWinInputDevices::Touchpad, k,
                             k == QLatin1String("pointerAcceleration") ? QVariant(v.toDouble()) : QVariant(v.toBool()));
    }
}

bool TouchpadManager::available() const
{
#ifdef HAVE_X11
    if (m_backend)
        return m_backend->isTouchpadAvailable();
#endif
    return m_kwin && m_kwin->available(KWinInputDevices::Touchpad);
}

bool TouchpadManager::enabled() const
{
#ifdef HAVE_X11
    if (m_backend)
        return m_backend->isTouchpadEnabled();
#endif
    return waylandValue(QStringLiteral("enabled")).toBool();
}

void TouchpadManager::setEnabled(bool enabled)
{
#ifdef HAVE_X11
    if (m_backend) {
        m_backend->setTouchpadEnabled(enabled);
        m_backend->applyConfig();
        return;
    }
#endif
    setWaylandValue(QStringLiteral("enabled"), enabled);
}

bool TouchpadManager::tapToClick() const
{
#ifdef HAVE_X11
    if (m_backend)
        return m_backend->tapToClick();
#endif
    return waylandValue(QStringLiteral("tapToClick")).toBool();
}

void TouchpadManager::setTapToClick(bool value)
{
#ifdef HAVE_X11
    if (m_backend) {
        m_backend->setTapToClick(value);
        m_backend->applyConfig();
        return;
    }
#endif
    setWaylandValue(QStringLiteral("tapToClick"), value);
}

bool TouchpadManager::naturalScroll() const
{
#ifdef HAVE_X11
    if (m_backend)
        return m_backend->naturalScroll();
#endif
    return waylandValue(QStringLiteral("naturalScroll")).toBool();
}

void TouchpadManager::setNaturalScroll(bool naturalScroll)
{
#ifdef HAVE_X11
    if (m_backend) {
        m_backend->setNaturalScroll(naturalScroll);
        m_backend->applyConfig();
        return;
    }
#endif
    setWaylandValue(QStringLiteral("naturalScroll"), naturalScroll);
}

qreal TouchpadManager::pointerAcceleration() const
{
#ifdef HAVE_X11
    if (m_backend)
        return m_backend->pointerAcceleration();
#endif
    return waylandValue(QStringLiteral("pointerAcceleration")).toDouble();
}

void TouchpadManager::setPointerAcceleration(qreal value)
{
#ifdef HAVE_X11
    if (m_backend) {
        m_backend->setPointerAcceleration(value);
        m_backend->applyConfig();
        return;
    }
#endif
    setWaylandValue(QStringLiteral("pointerAcceleration"), qBound(qreal(-1), value, qreal(1)));
}
