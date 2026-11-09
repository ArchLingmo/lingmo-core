/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * Author:     devalexandre <alexandre@dev2learn.com>
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

#ifndef NIGHTLIGHT_H
#define NIGHTLIGHT_H

#include <QObject>
#include <QSettings>
#include <QTimer>
#include <QTime>
#include <QHash>
#include <QVector>
#include <QVariantAnimation>

#ifdef HAVE_X11
#include <xcb/xcb.h>
#endif

// Blue light filter.
//   X11: tints every CRTC through its RandR gamma ramp.
//   Wayland: KWin's Night Light does the tinting (only the compositor can):
//     kwinrc [NightColor] Active/Mode=Constant/NightTemperature, written with
//     KConfig notify so KWin picks it up at once. The schedule stays ours.
class NightLight : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(int temperature READ temperature WRITE setTemperature NOTIFY temperatureChanged)
    Q_PROPERTY(int minTemperature READ minTemperature CONSTANT)
    Q_PROPERTY(int maxTemperature READ maxTemperature CONSTANT)
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY modeChanged)
    Q_PROPERTY(QString startTime READ startTime NOTIFY scheduleChanged)
    Q_PROPERTY(QString endTime READ endTime NOTIFY scheduleChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(bool supported READ supported CONSTANT)

public:
    enum Mode {
        Always = 0,
        Schedule = 1
    };

    explicit NightLight(QObject *parent = nullptr);
    ~NightLight();

    bool enabled() const { return m_enabled; }
    void setEnabled(bool enabled);

    int temperature() const { return m_temperature; }
    void setTemperature(int temperature);

    int minTemperature() const;
    int maxTemperature() const;

    int mode() const { return m_mode; }
    void setMode(int mode);

    QString startTime() const;
    QString endTime() const;
    void setSchedule(const QString &startTime, const QString &endTime);

    bool active() const { return m_active; }
    bool supported() const;

    void toggle();

    // Puts the untouched ramps back (disable, daemon exit)
    void restore();

    // RGB multipliers (0..1) for a colour temperature, 6500 K = no change
    static void whitePoint(double kelvin, double &r, double &g, double &b);

signals:
    void enabledChanged(bool enabled);
    void temperatureChanged(int temperature);
    void modeChanged(int mode);
    void scheduleChanged();
    void activeChanged(bool active);

private:
    struct Ramp {
        QVector<uint16_t> red, green, blue;
    };

    void evaluate(int duration);
    bool inSchedule() const;
    void updateCrtcs();
    void apply(double kelvin);
    // Wayland: hand the target to KWin
    void applyKWin(bool active);

private:
    QSettings m_settings;
    bool m_wayland = false;
#ifdef HAVE_X11
    xcb_connection_t *m_connection = nullptr;
    xcb_window_t m_root = 0;
#endif

    bool m_enabled;
    int m_temperature;
    int m_mode;
    QTime m_start;
    QTime m_end;
    bool m_active = false;

    // Original ramps per CRTC, taken the first time each CRTC is seen
    QHash<uint32_t, Ramp> m_original;
    double m_current = 6500.0;
    bool m_tinted = false;

    QVariantAnimation m_animation;
    QTimer m_scheduleTimer;
};

#endif // NIGHTLIGHT_H
