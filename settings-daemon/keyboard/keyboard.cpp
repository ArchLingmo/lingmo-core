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

#include "keyboard.h"
#include "keyboardadaptor.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusReply>
#include <QDBusVariant>
#include <QGuiApplication>
#include <QSocketNotifier>
#include <QProcess>
#include <QFile>
#include <QDebug>

#include <KConfig>
#include <KConfigGroup>

#ifdef HAVE_X11
// xkb.h uses "explicit" as a field name
#define explicit explicit_
#include <xcb/xkb.h>
#undef explicit
#endif
#include <libintl.h>

// KWin's keyboard layouts (Wayland): org.kde.keyboard on KWin's connection
static const QString s_kwinService = QStringLiteral("org.kde.keyboard");
static const QString s_layoutsPath = QStringLiteral("/Layouts");
static const QString s_layoutsInterface = QStringLiteral("org.kde.KeyboardLayouts");

static const QString s_defaultSwitchOption = QStringLiteral("grp:alt_shift_toggle");

static QString layoutOf(const QString &entry)
{
    return entry.section('(', 0, 0);
}

static QString variantOf(const QString &entry)
{
    return entry.contains('(') ? entry.section('(', 1).chopped(1) : QString();
}

Keyboard::Keyboard(QObject *parent)
    : QObject(parent)
    , m_settings(QStringLiteral("lingmoos"), QStringLiteral("keyboard"))
{
    m_switchOption = m_settings.value("SwitchOption", s_defaultSwitchOption).toString();

    loadDescriptions();

    new KeyboardAdaptor(this);
    QDBusConnection::sessionBus().registerObject(QStringLiteral("/Keyboard"), this);

    m_wayland = QGuiApplication::platformName().startsWith(QLatin1String("wayland"))
        || qgetenv("XDG_SESSION_TYPE") == "wayland";

    if (m_wayland) {
        initWayland();
        return;
    }

#ifdef HAVE_X11
    if (!initX11())
        return;

    readRulesNames();
    readState();

    // Apply what the user chose at login; otherwise keep the system default
    const QStringList saved = m_settings.value("Layouts").toStringList();
    if (!saved.isEmpty()) {
        m_layouts = saved;
        emit layoutsChanged();
        apply();
    } else if (m_settings.contains("SwitchOption")) {
        apply();
    }

    QMetaObject::invokeMethod(this, &Keyboard::processEvents, Qt::QueuedConnection);
#endif
}

#ifdef HAVE_X11
bool Keyboard::initX11()
{
    m_connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(m_connection)) {
        xcb_disconnect(m_connection);
        m_connection = nullptr;
        return false;
    }

    m_root = xcb_setup_roots_iterator(xcb_get_setup(m_connection)).data->root;

    xcb_xkb_use_extension_reply_t *xkb =
            xcb_xkb_use_extension_reply(m_connection, xcb_xkb_use_extension(m_connection, 1, 0), nullptr);
    const bool supported = xkb && xkb->supported;
    free(xkb);
    if (!supported) {
        qWarning() << "Keyboard: the X server has no XKB";
        xcb_disconnect(m_connection);
        m_connection = nullptr;
        return false;
    }
    m_xkbEvent = xcb_get_extension_data(m_connection, &xcb_xkb_id)->first_event;

    // Group changes, from any client or the switch shortcut
    const xcb_xkb_select_events_details_t details = {};
    xcb_xkb_select_events(m_connection, XCB_XKB_ID_USE_CORE_KBD,
                          XCB_XKB_EVENT_TYPE_STATE_NOTIFY, 0, XCB_XKB_EVENT_TYPE_STATE_NOTIFY,
                          0, 0, &details);

    // setxkbmap (ours or someone else's) rewrites _XKB_RULES_NAMES on the root window
    xcb_intern_atom_reply_t *atom = xcb_intern_atom_reply(m_connection,
            xcb_intern_atom(m_connection, 0, strlen("_XKB_RULES_NAMES"), "_XKB_RULES_NAMES"), nullptr);
    m_rulesAtom = atom ? atom->atom : 0;
    free(atom);

    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(m_connection, m_root, XCB_CW_EVENT_MASK, &mask);
    xcb_flush(m_connection);

    m_notifier = new QSocketNotifier(xcb_get_file_descriptor(m_connection), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &Keyboard::processEvents);
    return true;
}
#endif

void Keyboard::initWayland()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(s_kwinService, s_layoutsPath, s_layoutsInterface, QStringLiteral("layoutChanged"),
                this, SLOT(onKWinLayoutChanged(uint)));
    bus.connect(s_kwinService, s_layoutsPath, s_layoutsInterface, QStringLiteral("layoutListChanged"),
                this, SLOT(onKWinLayoutListChanged()));

    // What the user chose (in either session) wins; else kxkbrc as it is; else the
    // system keymap (localectl), which X11 got from xorg.conf.d but KWin doesn't read
    QStringList layouts = m_settings.value("Layouts").toStringList();
    const KConfig kxkbrc(QStringLiteral("kxkbrc"));
    const KConfigGroup kxkb = kxkbrc.group(QStringLiteral("Layout"));
    for (const QString &option : kxkb.readEntry("Options", QString()).split(',', Qt::SkipEmptyParts)) {
        if (!option.startsWith("grp:"))
            m_options << option;
    }
    bool write = !layouts.isEmpty() || m_settings.contains("SwitchOption");

    if (layouts.isEmpty()) {
        const QStringList list = kxkb.readEntry("LayoutList", QString()).split(',', Qt::SkipEmptyParts);
        const QStringList variants = kxkb.readEntry("VariantList", QString()).split(',');
        for (int i = 0; i < list.size(); ++i) {
            const QString variant = variants.value(i);
            layouts << (variant.isEmpty() ? list.at(i) : QString("%1(%2)").arg(list.at(i), variant));
        }
    }

    if (layouts.isEmpty()) {
        auto locale1 = [](const QString &name) {
            QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.locale1"),
                                                               QStringLiteral("/org/freedesktop/locale1"),
                                                               QStringLiteral("org.freedesktop.DBus.Properties"),
                                                               QStringLiteral("Get"));
            call << QStringLiteral("org.freedesktop.locale1") << name;
            const QDBusReply<QDBusVariant> reply = QDBusConnection::systemBus().call(call, QDBus::Block, 2000);
            return reply.isValid() ? reply.value().variant().toString() : QString();
        };
        const QStringList list = locale1(QStringLiteral("X11Layout")).split(',', Qt::SkipEmptyParts);
        const QStringList variants = locale1(QStringLiteral("X11Variant")).split(',');
        for (int i = 0; i < list.size(); ++i) {
            const QString variant = variants.value(i);
            layouts << (variant.isEmpty() ? list.at(i) : QString("%1(%2)").arg(list.at(i), variant));
        }
        for (const QString &option : locale1(QStringLiteral("X11Options")).split(',', Qt::SkipEmptyParts)) {
            if (!option.startsWith("grp:"))
                m_options << option;
        }
        write = !layouts.isEmpty();
    }

    if (layouts.size() > 4)
        layouts = layouts.mid(0, 4);
    if (!layouts.isEmpty() && layouts != m_layouts) {
        m_layouts = layouts;
        emit layoutsChanged();
    }

    if (write)
        apply();
    readKWinLayout();
}

void Keyboard::readKWinLayout()
{
    QDBusMessage call = QDBusMessage::createMethodCall(s_kwinService, s_layoutsPath, s_layoutsInterface,
                                                       QStringLiteral("getLayout"));
    const QDBusReply<uint> reply = QDBusConnection::sessionBus().call(call);
    if (reply.isValid())
        onKWinLayoutChanged(reply.value());
}

void Keyboard::onKWinLayoutChanged(uint index)
{
    if (int(index) != m_currentIndex) {
        m_currentIndex = int(index);
        emit currentIndexChanged(m_currentIndex);
    }
}

void Keyboard::onKWinLayoutListChanged()
{
    readKWinLayout();
}

void Keyboard::writeKxkbrc()
{
    QStringList layouts, variants;
    for (const QString &entry : m_layouts) {
        layouts << layoutOf(entry);
        variants << variantOf(entry);
    }
    QStringList options = m_options;
    if (!m_switchOption.isEmpty() && m_layouts.size() > 1)
        options << m_switchOption;

    // KConfig with Notify, and the signal KWin's keyboard layout module listens for
    KConfig config(QStringLiteral("kxkbrc"));
    KConfigGroup group = config.group(QStringLiteral("Layout"));
    group.writeEntry("Use", true, KConfig::Notify);
    group.writeEntry("LayoutList", layouts.join(','), KConfig::Notify);
    group.writeEntry("VariantList", variants.join(','), KConfig::Notify);
    group.writeEntry("Options", options.join(','), KConfig::Notify);
    group.writeEntry("ResetOldOptions", !options.isEmpty(), KConfig::Notify);
    if (!group.hasKey("SwitchMode"))
        group.writeEntry("SwitchMode", QStringLiteral("Global"));
    config.sync();

    QDBusConnection::sessionBus().send(
        QDBusMessage::createSignal(s_layoutsPath, QStringLiteral("org.kde.keyboard"), QStringLiteral("reloadConfig")));
}

Keyboard::~Keyboard()
{
#ifdef HAVE_X11
    if (m_connection)
        xcb_disconnect(m_connection);
#endif
}

QStringList Keyboard::shortNames() const
{
    // "US", and "US₂" for a second US variant
    static const QStringList subscripts = { "", "", "₂", "₃", "₄", "₅", "₆", "₇", "₈", "₉" };
    QHash<QString, int> seen;
    QStringList names;

    for (const QString &entry : m_layouts) {
        const QString code = layoutOf(entry).toUpper();
        const int n = ++seen[code];
        names << code + (n < subscripts.size() ? subscripts.at(n) : QString::number(n));
    }

    return names;
}

QStringList Keyboard::descriptions() const
{
    QStringList list;

    for (const QString &entry : m_layouts)
        list << m_descriptions.value(entry, entry);

    return list;
}

void Keyboard::setLayouts(const QStringList &layouts)
{
    QStringList list;
    for (const QString &entry : layouts) {
        const QString e = entry.trimmed();
        if (!e.isEmpty() && !list.contains(e))
            list << e;
    }

    // XKB holds at most four groups
    if (list.isEmpty() || list.size() > 4)
        return;

    m_settings.setValue("Layouts", list);

    if (m_layouts != list) {
        m_layouts = list;
        emit layoutsChanged();
    }

    apply();
}

void Keyboard::setSwitchOption(const QString &option)
{
    if (!option.isEmpty() && !option.startsWith("grp:"))
        return;

    m_settings.setValue("SwitchOption", option);

    if (m_switchOption != option) {
        m_switchOption = option;
        emit switchOptionChanged();
    }

    apply();
}

void Keyboard::setCurrentIndex(int index)
{
    if (index < 0 || index >= m_layouts.size())
        return;

    if (m_wayland) {
        QDBusMessage call = QDBusMessage::createMethodCall(s_kwinService, s_layoutsPath, s_layoutsInterface,
                                                           QStringLiteral("setLayout"));
        call << uint(index);
        QDBusConnection::sessionBus().asyncCall(call);
        return;
    }

#ifdef HAVE_X11
    if (!m_connection)
        return;

    xcb_xkb_latch_lock_state(m_connection, XCB_XKB_ID_USE_CORE_KBD, 0, 0, 1, index, 0, 0, 0);
    xcb_flush(m_connection);
#endif
}

void Keyboard::nextLayout()
{
    if (m_layouts.size() > 1)
        setCurrentIndex((m_currentIndex + 1) % m_layouts.size());
}

void Keyboard::apply()
{
    if (m_layouts.isEmpty())
        return;

    // Both sessions: KWin (Wayland) applies it; on X11 it keeps the two in step
    writeKxkbrc();

#ifdef HAVE_X11
    if (m_wayland || !m_connection)
        return;

    QStringList layouts, variants;
    for (const QString &entry : m_layouts) {
        layouts << layoutOf(entry);
        variants << variantOf(entry);
    }

    // Keep the user's other XKB options (caps:ctrl_modifier…), replace the switch key
    QStringList options = m_options;
    if (!m_switchOption.isEmpty() && m_layouts.size() > 1)
        options << m_switchOption;

    QStringList args = { "-layout", layouts.join(','), "-variant", variants.join(','), "-option", "" };
    if (!options.isEmpty())
        args << "-option" << options.join(',');

    if (!QProcess::startDetached("setxkbmap", args))
        qWarning() << "Keyboard: could not run setxkbmap";
#endif
}

#ifdef HAVE_X11
void Keyboard::readRulesNames()
{
    if (!m_rulesAtom)
        return;

    xcb_get_property_reply_t *reply = xcb_get_property_reply(m_connection,
            xcb_get_property(m_connection, 0, m_root, m_rulesAtom, XCB_ATOM_STRING, 0, 1024), nullptr);
    if (!reply)
        return;

    // rules \0 model \0 layout \0 variant \0 options
    const QByteArray data(static_cast<const char *>(xcb_get_property_value(reply)),
                          xcb_get_property_value_length(reply));
    free(reply);

    const QList<QByteArray> fields = data.split('\0');
    if (fields.size() < 3)
        return;

    const QStringList layouts = QString::fromLatin1(fields.value(2)).split(',');
    const QStringList variants = QString::fromLatin1(fields.value(3)).split(',');

    QStringList list;
    for (int i = 0; i < layouts.size(); ++i) {
        if (layouts.at(i).isEmpty())
            continue;
        const QString variant = variants.value(i);
        list << (variant.isEmpty() ? layouts.at(i) : QString("%1(%2)").arg(layouts.at(i), variant));
    }

    m_options.clear();
    for (const QString &option : QString::fromLatin1(fields.value(4)).split(',', Qt::SkipEmptyParts)) {
        if (!option.startsWith("grp:"))
            m_options << option;
        else if (!m_settings.contains("SwitchOption") && m_switchOption != option) {
            m_switchOption = option;
            emit switchOptionChanged();
        }
    }

    if (!list.isEmpty() && list != m_layouts) {
        m_layouts = list;
        emit layoutsChanged();
    }
}

void Keyboard::readState()
{
    xcb_xkb_get_state_reply_t *state =
            xcb_xkb_get_state_reply(m_connection, xcb_xkb_get_state(m_connection, XCB_XKB_ID_USE_CORE_KBD), nullptr);
    if (!state)
        return;

    const int group = state->group;
    free(state);

    if (group != m_currentIndex) {
        m_currentIndex = group;
        emit currentIndexChanged(group);
    }
}

void Keyboard::processEvents()
{
    if (!m_connection)
        return;

    while (xcb_generic_event_t *event = xcb_poll_for_event(m_connection)) {
        const uint8_t type = event->response_type & ~0x80;

        if (type == m_xkbEvent) {
            auto *state = reinterpret_cast<xcb_xkb_state_notify_event_t *>(event);
            if (state->xkbType == XCB_XKB_STATE_NOTIFY && state->group != m_currentIndex) {
                m_currentIndex = state->group;
                emit currentIndexChanged(m_currentIndex);
            }
        } else if (type == XCB_PROPERTY_NOTIFY) {
            if (reinterpret_cast<xcb_property_notify_event_t *>(event)->atom == m_rulesAtom) {
                readRulesNames();
                readState();
            }
        }

        free(event);
    }
}
#endif

void Keyboard::loadDescriptions()
{
    QFile file("/usr/share/X11/xkb/rules/evdev.lst");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    // "! layout":  "  br   Portuguese (Brazil)"
    // "! variant": "  intl us: English (US, intl., with dead keys)"
    QString section;
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.startsWith('!')) {
            section = line.mid(1).trimmed();
            continue;
        }
        if (line.isEmpty())
            continue;

        const QString name = line.section(' ', 0, 0);
        QString description = line.section(' ', 1).trimmed();

        if (section == "layout") {
            m_descriptions.insert(name, QString::fromUtf8(dgettext("xkeyboard-config", description.toUtf8().constData())));
        } else if (section == "variant") {
            const QString layout = description.section(':', 0, 0);
            description = description.section(':', 1).trimmed();
            m_descriptions.insert(QString("%1(%2)").arg(layout, name),
                                  QString::fromUtf8(dgettext("xkeyboard-config", description.toUtf8().constData())));
        }
    }
}
