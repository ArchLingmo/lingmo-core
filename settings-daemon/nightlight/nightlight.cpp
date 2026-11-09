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

#include "nightlight.h"
#include "nightlightadaptor.h"

#include <QDBusConnection>
#include <QDebug>
#include <QGuiApplication>

#include <KConfig>
#include <KConfigGroup>

#ifdef HAVE_X11
#include <xcb/randr.h>
#endif
#include <cmath>

static const int s_minTemperature = 2500;
static const int s_maxTemperature = 6500;
static const int s_defaultTemperature = 4000;
static const char *s_timeFormat = "HH:mm";

// Planckian locus (Kim et al. cubic spline) to linear sRGB, brightest channel = 1
static void planckToLinearRgb(double t, double &r, double &g, double &b)
{
    t = qBound(1667.0, t, 25000.0);
    const double t2 = t * t, t3 = t2 * t;

    double x;
    if (t <= 4000)
        x = -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910;
    else
        x = -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;

    const double x2 = x * x, x3 = x2 * x;
    double y;
    if (t <= 2222)
        y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
    else if (t <= 4000)
        y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
    else
        y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;

    const double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
    r = qMax(0.0, 3.2406 * X - 1.5372 * Y - 0.4986 * Z);
    g = qMax(0.0, -0.9689 * X + 1.8758 * Y + 0.0415 * Z);
    b = qMax(0.0, 0.0557 * X - 0.2040 * Y + 1.0570 * Z);

    const double m = qMax(r, qMax(g, b));
    r /= m; g /= m; b /= m;
}

void NightLight::whitePoint(double kelvin, double &r, double &g, double &b)
{
    if (kelvin >= s_maxTemperature) {
        r = g = b = 1.0;
        return;
    }

    // Relative to the 6500 K white so that the neutral point is exactly 1,1,1
    double r0, g0, b0;
    planckToLinearRgb(s_maxTemperature, r0, g0, b0);
    planckToLinearRgb(kelvin, r, g, b);
    r /= r0; g /= g0; b /= b0;

    const double m = qMax(r, qMax(g, b));
    // The ramps hold gamma-encoded values
    r = std::pow(r / m, 1.0 / 2.2);
    g = std::pow(g / m, 1.0 / 2.2);
    b = std::pow(b / m, 1.0 / 2.2);
}

NightLight::NightLight(QObject *parent)
    : QObject(parent)
    , m_settings(QStringLiteral("lingmoos"), QStringLiteral("nightlight"))
{
    m_enabled = m_settings.value("Enabled", false).toBool();
    m_temperature = qBound(s_minTemperature, m_settings.value("Temperature", s_defaultTemperature).toInt(), s_maxTemperature);
    m_mode = m_settings.value("Mode", Always).toInt() == Schedule ? Schedule : Always;
    m_start = QTime::fromString(m_settings.value("StartTime", "19:00").toString(), s_timeFormat);
    m_end = QTime::fromString(m_settings.value("EndTime", "07:00").toString(), s_timeFormat);
    if (!m_start.isValid())
        m_start = QTime(19, 0);
    if (!m_end.isValid())
        m_end = QTime(7, 0);

    m_wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"))
        || qgetenv("XDG_SESSION_TYPE") == "wayland";

#ifdef HAVE_X11
    int screen = 0;
    // On Wayland this would reach Xwayland, whose gamma isn't the screens'
    m_connection = m_wayland ? nullptr : xcb_connect(nullptr, &screen);
    if (!m_connection) {
        // Wayland: KWin's Night Light (applyKWin)
    } else if (xcb_connection_has_error(m_connection)) {
        xcb_disconnect(m_connection);
        m_connection = nullptr;
    } else {
        xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
        for (int i = 0; i < screen && it.rem; ++i)
            xcb_screen_next(&it);
        m_root = it.data ? it.data->root : 0;

        // Gamma per CRTC needs RandR 1.2
        xcb_randr_query_version_reply_t *version =
                xcb_randr_query_version_reply(m_connection, xcb_randr_query_version(m_connection, 1, 2), nullptr);
        if (!m_root || !version || (version->major_version == 1 && version->minor_version < 2)) {
            qWarning() << "NightLight: RandR 1.2 is not available, night light disabled";
            xcb_disconnect(m_connection);
            m_connection = nullptr;
        }
        free(version);
    }
#endif

    m_animation.setEasingCurve(QEasingCurve::InOutQuad);
    connect(&m_animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        apply(value.toDouble());
    });
    connect(&m_animation, &QVariantAnimation::finished, this, [this] {
        if (m_current >= s_maxTemperature)
            restore();
    });

    // Schedule edges and monitors plugged in later
    m_scheduleTimer.setInterval(30 * 1000);
    connect(&m_scheduleTimer, &QTimer::timeout, this, [this] { evaluate(2000); });
    m_scheduleTimer.start();

    new NightLightAdaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/NightLight"), this);

    evaluate(1500);
}

NightLight::~NightLight()
{
    m_animation.stop();
    restore();

#ifdef HAVE_X11
    if (m_connection)
        xcb_disconnect(m_connection);
#endif
}

bool NightLight::supported() const
{
    if (m_wayland)
        return true;
#ifdef HAVE_X11
    return m_connection != nullptr;
#else
    return false;
#endif
}

void NightLight::applyKWin(bool active)
{
    // KWin animates the change itself
    KConfig config(QStringLiteral("kwinrc"));
    KConfigGroup group = config.group(QStringLiteral("NightColor"));
    if (group.readEntry("Active", false) == active && group.readEntry("Mode", QString()) == QLatin1String("Constant")
        && group.readEntry("NightTemperature", 0) == m_temperature)
        return;
    group.writeEntry("Active", active, KConfig::Notify);
    group.writeEntry("Mode", QStringLiteral("Constant"), KConfig::Notify);
    group.writeEntry("NightTemperature", m_temperature, KConfig::Notify);
    config.sync();
}

void NightLight::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;

    m_enabled = enabled;
    m_settings.setValue("Enabled", enabled);
    emit enabledChanged(enabled);
    evaluate(1500);
}

void NightLight::toggle()
{
    setEnabled(!m_enabled);
}

void NightLight::setTemperature(int temperature)
{
    temperature = qBound(s_minTemperature, temperature, s_maxTemperature);
    if (m_temperature == temperature)
        return;

    m_temperature = temperature;
    m_settings.setValue("Temperature", temperature);
    emit temperatureChanged(temperature);
    // Short, so a slider being dragged follows the finger
    evaluate(250);
}

int NightLight::minTemperature() const
{
    return s_minTemperature;
}

int NightLight::maxTemperature() const
{
    return s_maxTemperature;
}

void NightLight::setMode(int mode)
{
    mode = mode == Schedule ? Schedule : Always;
    if (m_mode == mode)
        return;

    m_mode = mode;
    m_settings.setValue("Mode", mode);
    emit modeChanged(mode);
    evaluate(1500);
}

QString NightLight::startTime() const
{
    return m_start.toString(s_timeFormat);
}

QString NightLight::endTime() const
{
    return m_end.toString(s_timeFormat);
}

void NightLight::setSchedule(const QString &startTime, const QString &endTime)
{
    const QTime start = QTime::fromString(startTime, s_timeFormat);
    const QTime end = QTime::fromString(endTime, s_timeFormat);
    if (!start.isValid() || !end.isValid() || (start == m_start && end == m_end))
        return;

    m_start = start;
    m_end = end;
    m_settings.setValue("StartTime", startTime);
    m_settings.setValue("EndTime", endTime);
    emit scheduleChanged();
    evaluate(1500);
}

bool NightLight::inSchedule() const
{
    const QTime now = QTime::currentTime();

    if (m_start == m_end)
        return true;
    if (m_start < m_end)
        return now >= m_start && now < m_end;
    // Crosses midnight, e.g. 19:00 - 07:00
    return now >= m_start || now < m_end;
}

void NightLight::evaluate(int duration)
{
    const bool active = m_enabled && (m_mode == Always || inSchedule());
    if (active != m_active) {
        m_active = active;
        emit activeChanged(active);
    }

    if (m_wayland) {
        applyKWin(active);
        return;
    }

#ifdef HAVE_X11
    if (!m_connection)
        return;

    const bool newCrtcs = [this] {
        const int before = m_original.size();
        updateCrtcs();
        return m_original.size() > before;
    }();

    const double target = active ? m_temperature : s_maxTemperature;

    if (m_animation.state() == QAbstractAnimation::Running) {
        if (m_animation.endValue().toDouble() == target)
            return;
        m_animation.stop();
    } else if (m_current == target) {
        // Tint screens that showed up since the last change
        if (newCrtcs && m_current < s_maxTemperature)
            apply(m_current);
        return;
    }

    m_animation.setStartValue(m_current);
    m_animation.setEndValue(target);
    m_animation.setDuration(qMax(1, duration));
    m_animation.start();
#else
    Q_UNUSED(duration)
#endif
}

void NightLight::updateCrtcs()
{
#ifdef HAVE_X11
    xcb_randr_get_screen_resources_current_reply_t *resources =
            xcb_randr_get_screen_resources_current_reply(m_connection,
                xcb_randr_get_screen_resources_current(m_connection, m_root), nullptr);
    if (!resources)
        return;

    const xcb_randr_crtc_t *crtcs = xcb_randr_get_screen_resources_current_crtcs(resources);
    const int count = xcb_randr_get_screen_resources_current_crtcs_length(resources);

    QHash<uint32_t, Ramp> ramps;
    for (int i = 0; i < count; ++i) {
        if (m_original.contains(crtcs[i])) {
            ramps.insert(crtcs[i], m_original.value(crtcs[i]));
            continue;
        }

        xcb_randr_get_crtc_gamma_reply_t *gamma =
                xcb_randr_get_crtc_gamma_reply(m_connection, xcb_randr_get_crtc_gamma(m_connection, crtcs[i]), nullptr);
        if (!gamma)
            continue;

        if (gamma->size > 0) {
            Ramp ramp;
            const uint16_t *red = xcb_randr_get_crtc_gamma_red(gamma);
            const uint16_t *green = xcb_randr_get_crtc_gamma_green(gamma);
            const uint16_t *blue = xcb_randr_get_crtc_gamma_blue(gamma);
            ramp.red = QVector<uint16_t>(red, red + gamma->size);
            ramp.green = QVector<uint16_t>(green, green + gamma->size);
            ramp.blue = QVector<uint16_t>(blue, blue + gamma->size);

            // Some servers report an empty (all zero) ramp: start from linear then.
            // Same when a previous run died while tinting (the "Tinted" mark is still
            // set): the ramp on screen is ours, not the original one.
            const bool leftTinted = m_settings.value("Tinted", false).toBool();
            if (leftTinted || (ramp.red.last() == 0 && ramp.green.last() == 0 && ramp.blue.last() == 0)) {
                for (int j = 0; j < gamma->size; ++j)
                    ramp.red[j] = ramp.green[j] = ramp.blue[j] = uint16_t(j * 65535.0 / qMax(1, gamma->size - 1));
            }

            ramps.insert(crtcs[i], ramp);
        }
        free(gamma);
    }

    free(resources);
    m_original = ramps;
#endif
}

void NightLight::apply(double kelvin)
{
#ifdef HAVE_X11
    if (!m_connection)
        return;

    double r, g, b;
    whitePoint(kelvin, r, g, b);

    for (auto it = m_original.constBegin(); it != m_original.constEnd(); ++it) {
        const Ramp &orig = it.value();
        const int size = orig.red.size();
        QVector<uint16_t> red(size), green(size), blue(size);

        for (int i = 0; i < size; ++i) {
            red[i] = uint16_t(orig.red[i] * r);
            green[i] = uint16_t(orig.green[i] * g);
            blue[i] = uint16_t(orig.blue[i] * b);
        }

        xcb_randr_set_crtc_gamma(m_connection, it.key(), size, red.constData(), green.constData(), blue.constData());
    }

    xcb_flush(m_connection);
    m_current = kelvin;
    if (!m_tinted) {
        // Survives a crash: the next start then won't take the tinted ramp as original
        m_settings.setValue("Tinted", true);
        m_settings.sync();
    }
    m_tinted = true;
#else
    Q_UNUSED(kelvin)
#endif
}

void NightLight::restore()
{
#ifdef HAVE_X11
    if (!m_connection || !m_tinted)
        return;

    for (auto it = m_original.constBegin(); it != m_original.constEnd(); ++it) {
        const Ramp &orig = it.value();
        xcb_randr_set_crtc_gamma(m_connection, it.key(), orig.red.size(),
                                 orig.red.constData(), orig.green.constData(), orig.blue.constData());
    }

    xcb_flush(m_connection);
    m_current = s_maxTemperature;
    m_tinted = false;
    m_settings.setValue("Tinted", false);
    m_settings.sync();
#endif
}
