/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "userdirs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>

namespace UserDirs {

static const QRegularExpression &lineRe()
{
    static const QRegularExpression re(QStringLiteral("^\\s*XDG_([A-Z0-9_]+)_DIR\\s*=\\s*\"(.*)\"\\s*$"));
    return re;
}

static QString unescape(const QString &value)
{
    QString out;
    out.reserve(value.size());
    for (int i = 0; i < value.size(); ++i) {
        if (value.at(i) == QLatin1Char('\\') && i + 1 < value.size())
            ++i;
        out += value.at(i);
    }
    return out;
}

static QString escape(const QString &value)
{
    QString out;
    for (const QChar c : value) {
        if (c == QLatin1Char('"') || c == QLatin1Char('\\') || c == QLatin1Char('$') || c == QLatin1Char('`'))
            out += QLatin1Char('\\');
        out += c;
    }
    return out;
}

bool UserDirsConfig::load(const QString &file, const QString &home)
{
    m_home = QDir::cleanPath(home);
    m_lines.clear();
    m_paths.clear();
    m_exists = false;

    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    m_exists = true;
    const QString text = QString::fromUtf8(f.readAll());
    m_lines = text.split(QLatin1Char('\n'));
    if (!m_lines.isEmpty() && m_lines.last().isEmpty())
        m_lines.removeLast();

    for (const QString &line : std::as_const(m_lines)) {
        const auto m = lineRe().match(line);
        if (!m.hasMatch())
            continue;
        const QString key = m.captured(1);
        const QString raw = m.captured(2);
        QString path;
        if (raw == QLatin1String("$HOME") || raw == QLatin1String("$HOME/"))
            path = m_home;
        else if (raw.startsWith(QLatin1String("$HOME/")))
            path = m_home + QLatin1Char('/') + unescape(raw.mid(6));
        else if (raw.startsWith(QLatin1Char('/')))
            path = unescape(raw);
        else
            continue; // relative paths are invalid per spec
        m_paths.insert(key, QDir::cleanPath(path));
    }
    return true;
}

QStringList UserDirsConfig::keys() const
{
    QStringList list;
    for (const QString &line : m_lines) {
        const auto m = lineRe().match(line);
        if (m.hasMatch() && m_paths.contains(m.captured(1)) && !list.contains(m.captured(1)))
            list << m.captured(1);
    }
    return list;
}

QString UserDirsConfig::path(const QString &key) const
{
    return m_paths.value(key);
}

QString UserDirsConfig::quoteValue(const QString &absolutePath, const QString &home)
{
    const QString p = QDir::cleanPath(absolutePath);
    const QString h = QDir::cleanPath(home);
    if (p == h)
        return QStringLiteral("\"$HOME/\"");
    if (p.startsWith(h + QLatin1Char('/')))
        return QStringLiteral("\"$HOME/") + escape(p.mid(h.size() + 1)) + QLatin1Char('"');
    return QLatin1Char('"') + escape(p) + QLatin1Char('"');
}

void UserDirsConfig::setPath(const QString &key, const QString &absolutePath)
{
    const QString newLine = QStringLiteral("XDG_%1_DIR=%2").arg(key, quoteValue(absolutePath, m_home));
    bool replaced = false;
    for (QString &line : m_lines) {
        const auto m = lineRe().match(line);
        if (m.hasMatch() && m.captured(1) == key) {
            line = newLine;
            replaced = true;
        }
    }
    if (!replaced)
        m_lines << newLine;
    m_paths.insert(key, QDir::cleanPath(absolutePath));
}

QString UserDirsConfig::text() const
{
    return m_lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

bool UserDirsConfig::save(const QString &file, QString *error) const
{
    QDir().mkpath(QFileInfo(file).absolutePath());
    QSaveFile f(file);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    f.write(text().toUtf8());
    if (!f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

QString UserDirsConfig::readLocale(const QString &file)
{
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll()).trimmed();
}

bool writeLocaleFile(const Context &ctx, const QString &locale, QString *error)
{
    QDir().mkpath(ctx.configHome);
    QSaveFile f(ctx.userDirsLocaleFile());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = f.errorString();
        return false;
    }
    // Same format xdg-user-dirs-update and xdg-user-dirs-gtk write: the bare
    // locale, no newline
    f.write(normalizeLocale(locale).toUtf8());
    const bool ok = f.commit();
    if (!ok && error)
        *error = f.errorString();
    return ok;
}

// ------------------------------------------------------------ remembered answers

QString declinedLocale(const Context &ctx)
{
    QSettings s(ctx.stateFile(), QSettings::IniFormat);
    return s.value(QStringLiteral("DeclinedLocale")).toString();
}

void setDeclinedLocale(const Context &ctx, const QString &locale)
{
    QSettings s(ctx.stateFile(), QSettings::IniFormat);
    s.setValue(QStringLiteral("DeclinedLocale"), normalizeLocale(locale));
}

bool duplicatesAsked(const Context &ctx)
{
    QSettings s(ctx.stateFile(), QSettings::IniFormat);
    return s.value(QStringLiteral("DuplicatesAsked"), false).toBool();
}

void setDuplicatesAsked(const Context &ctx, bool asked)
{
    QSettings s(ctx.stateFile(), QSettings::IniFormat);
    s.setValue(QStringLiteral("DuplicatesAsked"), asked);
}

} // namespace UserDirs
