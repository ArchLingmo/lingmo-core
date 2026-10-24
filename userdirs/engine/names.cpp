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
#include <QRegularExpression>
#include <QtEndian>

namespace UserDirs {

Context Context::fromEnvironment()
{
    Context ctx;
    ctx.home = QDir::homePath();
    ctx.configHome = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (ctx.configHome.isEmpty())
        ctx.configHome = ctx.home + QStringLiteral("/.config");
    ctx.dataHome = qEnvironmentVariable("XDG_DATA_HOME");
    if (ctx.dataHome.isEmpty())
        ctx.dataHome = ctx.home + QStringLiteral("/.local/share");

    // Tests and development builds can point at another catalogue
    const QString localeOverride = qEnvironmentVariable("LINGMO_USERDIRS_LOCALEDIR");
    if (!localeOverride.isEmpty())
        ctx.localeDirs << localeOverride;
    ctx.localeDirs << QStringLiteral("/usr/share/locale") << QStringLiteral("/usr/local/share/locale");

    QString configDirs = qEnvironmentVariable("XDG_CONFIG_DIRS");
    if (configDirs.isEmpty())
        configDirs = QStringLiteral("/etc/xdg");
    for (const QString &dir : configDirs.split(QLatin1Char(':'), Qt::SkipEmptyParts))
        ctx.defaultsFiles << dir + QStringLiteral("/user-dirs.defaults");
    if (!ctx.defaultsFiles.contains(QStringLiteral("/etc/xdg/user-dirs.defaults")))
        ctx.defaultsFiles << QStringLiteral("/etc/xdg/user-dirs.defaults");
    return ctx;
}

QHash<QString, QString> readMoFile(const QString &path, bool *ok)
{
    QHash<QString, QString> table;
    if (ok)
        *ok = false;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return table;
    const QByteArray data = file.readAll();
    if (data.size() < 28)
        return table;

    const auto *raw = reinterpret_cast<const uchar *>(data.constData());
    const quint32 magic = qFromLittleEndian<quint32>(raw);
    bool little;
    if (magic == 0x950412de)
        little = true;
    else if (qFromBigEndian<quint32>(raw) == 0x950412de)
        little = false;
    else
        return table;

    auto word = [&](qsizetype offset) -> quint32 {
        if (offset < 0 || offset + 4 > data.size())
            return 0;
        return little ? qFromLittleEndian<quint32>(raw + offset) : qFromBigEndian<quint32>(raw + offset);
    };

    const quint32 count = word(8);
    const quint32 origTable = word(12);
    const quint32 transTable = word(16);
    for (quint32 i = 0; i < count; ++i) {
        const quint32 olen = word(origTable + i * 8);
        const quint32 ooff = word(origTable + i * 8 + 4);
        const quint32 tlen = word(transTable + i * 8);
        const quint32 toff = word(transTable + i * 8 + 4);
        if (qint64(ooff) + olen > data.size() || qint64(toff) + tlen > data.size())
            return table;
        if (olen == 0)
            continue; // header entry
        QByteArray orig = data.mid(ooff, olen);
        QByteArray trans = data.mid(toff, tlen);
        if (orig.contains('\x04'))
            continue; // msgctxt entries are not used by xdg-user-dirs
        // plural forms: keep the singular
        orig = orig.left(orig.indexOf('\0') >= 0 ? orig.indexOf('\0') : orig.size());
        trans = trans.left(trans.indexOf('\0') >= 0 ? trans.indexOf('\0') : trans.size());
        if (!trans.isEmpty())
            table.insert(QString::fromUtf8(orig), QString::fromUtf8(trans));
    }
    if (ok)
        *ok = true;
    return table;
}

QString normalizeLocale(const QString &locale)
{
    QString l = locale.trimmed();
    const int at = l.indexOf(QLatin1Char('@'));
    if (at >= 0)
        l.truncate(at);
    const int dot = l.indexOf(QLatin1Char('.'));
    if (dot >= 0)
        l.truncate(dot);
    return l;
}

QStringList localeFallbacks(const QString &locale)
{
    QStringList list;
    auto add = [&list](const QString &l) {
        if (!l.isEmpty() && !list.contains(l))
            list << l;
    };
    const QString l = locale.trimmed();
    add(l);
    QString modifier;
    QString base = l;
    const int at = base.indexOf(QLatin1Char('@'));
    if (at >= 0) {
        modifier = base.mid(at);
        base.truncate(at);
    }
    const int dot = base.indexOf(QLatin1Char('.'));
    if (dot >= 0)
        base.truncate(dot);
    if (!modifier.isEmpty())
        add(base + modifier);
    add(base);
    const int us = base.indexOf(QLatin1Char('_'));
    if (us > 0) {
        if (!modifier.isEmpty())
            add(base.left(us) + modifier);
        add(base.left(us));
    }
    return list;
}

XdgNames::XdgNames(const Context &ctx)
    : m_ctx(ctx)
{
    for (const QString &path : ctx.defaultsFiles) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        while (!file.atEnd()) {
            const QString line = QString::fromUtf8(file.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
                continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0)
                continue;
            const QString key = line.left(eq).trimmed().toUpper();
            QString value = line.mid(eq + 1).trimmed();
            if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                value = value.mid(1, value.size() - 2);
            if (!m_keys.contains(key)) {
                m_keys << key;
                m_defaults.insert(key, value);
            }
        }
        if (!m_keys.isEmpty())
            break;
    }

    // xdg-user-dirs' own fallback when no defaults file exists
    if (m_keys.isEmpty()) {
        const QList<QPair<QString, QString>> builtin = {
            {QStringLiteral("DESKTOP"), QStringLiteral("Desktop")},
            {QStringLiteral("DOWNLOAD"), QStringLiteral("Downloads")},
            {QStringLiteral("TEMPLATES"), QStringLiteral("Templates")},
            {QStringLiteral("PUBLICSHARE"), QStringLiteral("Public")},
            {QStringLiteral("DOCUMENTS"), QStringLiteral("Documents")},
            {QStringLiteral("MUSIC"), QStringLiteral("Music")},
            {QStringLiteral("PICTURES"), QStringLiteral("Pictures")},
            {QStringLiteral("VIDEOS"), QStringLiteral("Videos")},
        };
        for (const auto &p : builtin) {
            m_keys << p.first;
            m_defaults.insert(p.first, p.second);
        }
    }
}

const QHash<QString, QString> &XdgNames::catalogue(const QString &locale) const
{
    auto it = m_catalogues.constFind(locale);
    if (it != m_catalogues.constEnd())
        return it.value();

    QHash<QString, QString> table;
    for (const QString &l : localeFallbacks(locale)) {
        bool found = false;
        for (const QString &root : m_ctx.localeDirs) {
            const QString path = root + QLatin1Char('/') + l + QStringLiteral("/LC_MESSAGES/xdg-user-dirs.mo");
            if (QFile::exists(path)) {
                table = readMoFile(path);
                found = true;
                break;
            }
        }
        if (found)
            break;
    }
    return m_catalogues.insert(locale, table).value();
}

QString XdgNames::translated(const QString &key, const QString &locale) const
{
    const QString english = m_defaults.value(key);
    if (english.isEmpty())
        return QString();
    const QString l = normalizeLocale(locale);
    if (l.isEmpty() || l == QLatin1String("C") || l == QLatin1String("POSIX"))
        return english;
    const QString t = catalogue(locale).value(english);
    // A translation must stay a single folder name
    if (t.isEmpty() || t.contains(QLatin1Char('/')) || t == QLatin1String(".") || t == QLatin1String(".."))
        return english;
    return t;
}

QStringList XdgNames::installedLocales() const
{
    if (m_scanned)
        return m_installed;
    QStringList locales;
    for (const QString &root : m_ctx.localeDirs) {
        const QDir dir(root);
        for (const QString &l : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!locales.contains(l) && QFile::exists(dir.filePath(l + QStringLiteral("/LC_MESSAGES/xdg-user-dirs.mo"))))
                locales << l;
        }
    }
    locales.sort();
    m_installed = locales;
    m_scanned = true;
    return locales;
}

QStringList XdgNames::knownNames(const QString &key) const
{
    QStringList names;
    const QString english = m_defaults.value(key);
    if (english.isEmpty())
        return names;
    names << english;
    for (const QString &l : installedLocales()) {
        const QString t = translated(key, l);
        if (!names.contains(t))
            names << t;
    }
    return names;
}

} // namespace UserDirs
