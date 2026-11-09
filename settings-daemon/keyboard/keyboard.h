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

#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <QObject>
#include <QSettings>
#include <QStringList>
#include <QHash>

#ifdef HAVE_X11
#include <xcb/xcb.h>
#endif

class QSocketNotifier;

// Keyboard layouts. The list always goes to kxkbrc ([Layout], what KWin reads)
// with org.kde.keyboard reloadConfig, so both sessions share it.
//   X11: applied with setxkbmap; the active XKB group is followed, whoever
//        changes it (switch shortcut, other tools).
//   Wayland: KWin owns the keymap; the active layout comes from (and is set
//        through) KWin's org.kde.keyboard /Layouts.
class Keyboard : public QObject
{
    Q_OBJECT
    // Entries are "layout" or "layout(variant)", e.g. "br", "us(intl)"
    Q_PROPERTY(QStringList layouts READ layouts NOTIFY layoutsChanged)
    Q_PROPERTY(QStringList shortNames READ shortNames NOTIFY layoutsChanged)
    Q_PROPERTY(QStringList descriptions READ descriptions NOTIFY layoutsChanged)
    Q_PROPERTY(QString switchOption READ switchOption NOTIFY switchOptionChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)

public:
    explicit Keyboard(QObject *parent = nullptr);
    ~Keyboard();

    QStringList layouts() const { return m_layouts; }
    QStringList shortNames() const;
    QStringList descriptions() const;
    QString switchOption() const { return m_switchOption; }
    int currentIndex() const { return m_currentIndex; }

    void setLayouts(const QStringList &layouts);
    void setSwitchOption(const QString &option);
    void setCurrentIndex(int index);
    void nextLayout();

signals:
    void layoutsChanged();
    void switchOptionChanged();
    void currentIndexChanged(int index);

private slots:
    // KWin (Wayland)
    void onKWinLayoutChanged(uint index);
    void onKWinLayoutListChanged();

private:
    void apply();
    void writeKxkbrc();
    void loadDescriptions();
    // Wayland
    void initWayland();
    void readKWinLayout();
#ifdef HAVE_X11
    bool initX11();
    void readRulesNames();
    void readState();
    void processEvents();
#endif

private:
    QSettings m_settings;
    bool m_wayland = false;
#ifdef HAVE_X11
    xcb_connection_t *m_connection = nullptr;
    xcb_window_t m_root = 0;
    xcb_atom_t m_rulesAtom = 0;
    uint8_t m_xkbEvent = 0;
#endif
    QSocketNotifier *m_notifier = nullptr;

    QStringList m_layouts;
    QStringList m_options;
    QString m_switchOption;
    int m_currentIndex = 0;

    QHash<QString, QString> m_descriptions;
};

#endif // KEYBOARD_H
