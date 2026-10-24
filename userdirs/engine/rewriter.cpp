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
#include <QFileInfo>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace UserDirs {

// Characters that may come right before an absolute path in a config line:
// "key=/home/u", "Exec=app /home/u", "\"/home/u\"", "a,/home/u", "<x>/home/u"
static bool startsPath(QChar prev)
{
    static const QString allowed = QStringLiteral(" \t\r\n=\"',;:>(");
    return allowed.contains(prev);
}

// What may follow a folder path for it to still be that folder. No space: a
// raw path in a text file can't tell "/home/u/Videos (old)" from
// "/home/u/Videos" followed by text, so only clear separators count.
static bool endsPath(const QString &text, qsizetype pos)
{
    if (pos >= text.size())
        return true;
    static const QString allowed = QStringLiteral("/\r\n\t\"',;<>&)]");
    return allowed.contains(text.at(pos));
}

static bool urlTokenChar(QChar c)
{
    return !(c.isSpace() || c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('<')
             || c == QLatin1Char('>'));
}

QString rewritePaths(const QString &text, const QList<PathMapping> &mappings, int *replacements)
{
    if (replacements)
        *replacements = 0;
    if (mappings.isEmpty())
        return text;

    // Longest first: "/h/Documentos/a.txt" wins over "/h/Documentos"
    QList<PathMapping> maps;
    for (const PathMapping &m : mappings) {
        const QString from = QDir::cleanPath(m.from);
        // Identity mappings are kept on purpose: they shield entries that
        // stayed behind from the mapping of their parent folder
        if (from.size() > 1)
            maps << PathMapping{from, QDir::cleanPath(m.to)};
    }
    std::stable_sort(maps.begin(), maps.end(), [](const PathMapping &a, const PathMapping &b) {
        return a.from.size() > b.from.size();
    });

    static const QString filePrefix = QStringLiteral("file://");
    QString out;
    out.reserve(text.size() + 64);
    qsizetype i = 0;
    const qsizetype n = text.size();
    while (i < n) {
        const QChar prev = i > 0 ? text.at(i - 1) : QLatin1Char('\n');

        // file:///home/u/V%C3%ADdeos/x.mp4
        if (text.at(i) == QLatin1Char('f') && text.mid(i, 8) == QLatin1String("file:///")) {
            qsizetype end = i;
            while (end < n && urlTokenChar(text.at(end)))
                ++end;
            const QString token = text.mid(i, end - i);
            const QString path = QUrl::fromEncoded(token.toUtf8()).toLocalFile();
            bool done = false;
            if (!path.isEmpty()) {
                for (const PathMapping &m : std::as_const(maps)) {
                    if (path != m.from && !path.startsWith(m.from + QLatin1Char('/')))
                        continue;
                    if (m.from == m.to)
                        break;
                    // Keep the rest of the URL exactly as it was written: skip
                    // as many '/' as the folder path has ('/' is never encoded)
                    const int slashes = m.from.count(QLatin1Char('/'));
                    qsizetype cut = filePrefix.size();
                    int seen = 0;
                    while (cut < token.size()) {
                        if (token.at(cut) == QLatin1Char('/') && ++seen > slashes)
                            break;
                        ++cut;
                    }
                    out += QString::fromUtf8(QUrl::fromLocalFile(m.to).toEncoded()) + token.mid(cut);
                    if (replacements)
                        ++*replacements;
                    done = true;
                    break;
                }
            }
            if (!done)
                out += token;
            i = end;
            continue;
        }

        if (text.at(i) == QLatin1Char('/') && startsPath(prev)) {
            bool done = false;
            for (const PathMapping &m : std::as_const(maps)) {
                if (text.size() - i >= m.from.size() && QStringView(text).mid(i, m.from.size()) == m.from
                    && endsPath(text, i + m.from.size())) {
                    out += m.to;
                    i += m.from.size();
                    if (replacements && m.from != m.to)
                        ++*replacements;
                    done = true;
                    break;
                }
            }
            if (done)
                continue;
        }
        out += text.at(i);
        ++i;
    }
    return out;
}

QString dedupeLines(const QString &text)
{
    QStringList lines = text.split(QLatin1Char('\n'));
    QSet<QString> seen;
    QStringList out;
    for (const QString &line : lines) {
        // Bookmarks are "URL label": the URL identifies it
        const QString id = line.section(QLatin1Char(' '), 0, 0).trimmed();
        if (!id.isEmpty()) {
            if (seen.contains(id))
                continue;
            seen.insert(id);
        }
        out << line;
    }
    return out.join(QLatin1Char('\n'));
}

QStringList pathHoldingFiles(const Context &ctx, const QString &desktopDir)
{
    QStringList files;
    auto add = [&files](const QString &path) {
        const QFileInfo fi(path);
        // Regular files only, and nothing huge
        if (fi.exists() && fi.isFile() && !fi.isSymLink() && fi.size() < 32 * 1024 * 1024
            && !files.contains(fi.absoluteFilePath()))
            files << fi.absoluteFilePath();
    };
    add(ctx.configHome + QStringLiteral("/gtk-3.0/bookmarks"));
    add(ctx.configHome + QStringLiteral("/gtk-4.0/bookmarks"));
    add(ctx.dataHome + QStringLiteral("/user-places.xbel"));
    add(ctx.dataHome + QStringLiteral("/recently-used.xbel"));

    // Lingmo settings: file manager favourites (lingmo-filemanager-places.conf),
    // dock, assistant folders, players' recent lists...
    const QDir lingmo(ctx.configHome + QStringLiteral("/lingmoos"));
    for (const QString &f : lingmo.entryList({QStringLiteral("*.conf")}, QDir::Files))
        if (f != QLatin1String("userdirs.conf"))
            add(lingmo.filePath(f));

    // Launchers on the desktop and in the menu that point into the folders
    // (the dock pins .desktop paths)
    for (const QString &dir : {desktopDir, ctx.dataHome + QStringLiteral("/applications")}) {
        if (dir.isEmpty())
            continue;
        const QDir d(dir);
        for (const QString &f : d.entryList({QStringLiteral("*.desktop")}, QDir::Files))
            add(d.filePath(f));
    }
    return files;
}

} // namespace UserDirs
