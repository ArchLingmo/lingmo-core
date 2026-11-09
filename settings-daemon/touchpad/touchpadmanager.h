#ifndef TOUCHPADMANAGER_H
#define TOUCHPADMANAGER_H

#include <QObject>
#include <QVariant>

class XlibBackend;
class KWinInputDevices;

class TouchpadManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled CONSTANT)
    Q_PROPERTY(bool tapToClick READ tapToClick WRITE setTapToClick CONSTANT)
    Q_PROPERTY(bool naturalScroll READ naturalScroll WRITE setNaturalScroll CONSTANT)
    Q_PROPERTY(qreal pointerAcceleration READ pointerAcceleration WRITE setPointerAcceleration CONSTANT)

public:
    explicit TouchpadManager(QObject *parent = nullptr);

    bool available() const;

    bool enabled() const;
    void setEnabled(bool enabled);

    bool tapToClick() const;
    void setTapToClick(bool value);

    bool naturalScroll() const;
    void setNaturalScroll(bool naturalScroll);

    qreal pointerAcceleration() const;
    void setPointerAcceleration(qreal value);

private:
    // Wayland: what the user chose (same file and keys as the X11 libinput backend:
    // lingmoos/touchpadxlibinputrc, one group per device name), else KWin's value
    QVariant waylandValue(const QString &key) const;
    void setWaylandValue(const QString &key, const QVariant &value);
    void applyWayland();

private:
    // X11 session (null when there is no X server, e.g. on Wayland)
    XlibBackend *m_backend = nullptr;
    // Wayland session
    KWinInputDevices *m_kwin = nullptr;
};

#endif // TOUCHPADMANAGER_H
