/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "userdirs.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <cerrno>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

namespace UserDirs {

static const QString JournalName = QStringLiteral("journal.jsonl");
static const QString UndoneName = QStringLiteral("journal.undone");

static QString errText(int err)
{
    return QString::fromLocal8Bit(std::strerror(err));
}

static QByteArray sha(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

static QByteArray readAll(const QString &path, bool *ok = nullptr)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (ok)
            *ok = false;
        return {};
    }
    if (ok)
        *ok = true;
    return f.readAll();
}

static bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, f.errorString());
        return false;
    }
    f.write(data);
    if (!f.commit()) {
        if (error)
            *error = QStringLiteral("%1: %2").arg(path, f.errorString());
        return false;
    }
    return true;
}

static mode_t modeOf(const QString &path)
{
    struct stat st;
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return 0;
    return st.st_mode;
}

static bool ownedDir(const QString &path)
{
    struct stat st;
    return ::lstat(QFile::encodeName(path).constData(), &st) == 0 && S_ISDIR(st.st_mode)
        && st.st_uid == ::geteuid();
}

// ------------------------------------------------------------ journal

class Journal
{
public:
    bool open(const QString &root, QString *error)
    {
        const QString stamp = QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
        QString dir = root + QLatin1Char('/') + stamp;
        for (int n = 2; QFileInfo::exists(dir); ++n)
            dir = root + QLatin1Char('/') + stamp + QStringLiteral("-%1").arg(n);
        if (!QDir().mkpath(dir + QStringLiteral("/backup"))) {
            *error = QStringLiteral("%1: cannot create the undo journal").arg(dir);
            return false;
        }
        m_dir = dir;
        m_file.setFileName(dir + QLatin1Char('/') + JournalName);
        if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            *error = QStringLiteral("%1: %2").arg(m_file.fileName(), m_file.errorString());
            return false;
        }
        return true;
    }

    void write(const QJsonObject &op)
    {
        if (!m_file.isOpen())
            return;
        m_file.write(QJsonDocument(op).toJson(QJsonDocument::Compact) + '\n');
        m_file.flush();
        ::fsync(m_file.handle());
    }

    QString dir() const { return m_dir; }

    QString backup(const QString &path, const QByteArray &data)
    {
        const QString name = QStringLiteral("%1-%2").arg(++m_backups).arg(QFileInfo(path).fileName());
        const QString target = m_dir + QStringLiteral("/backup/") + name;
        QString err;
        writeFile(target, data, &err);
        return target;
    }

private:
    QString m_dir;
    QFile m_file;
    int m_backups = 0;
};

// ------------------------------------------------------------ the run

namespace {

struct Run {
    Context ctx;
    bool dryRun = false;
    Journal journal;
    Summary summary;
    std::function<void(const Progress &)> progressFn;
    Progress progress;
    QList<PathMapping> entryMappings;   // per entry: renamed on a clash, or left behind
    QSet<QString> claimed;              // dry run: names taken by earlier moves
    qint64 pendingBytes = 0;            // copied bytes not yet reported (1 unit = 1 MiB)

    void report(const QString &current, qint64 units = 0)
    {
        progress.done += units;
        if (!current.isEmpty())
            progress.current = current;
        if (progressFn)
            progressFn(progress);
    }

    bool exists(const QString &path) const
    {
        return claimed.contains(path) || QFileInfo::exists(path) || QFileInfo(path).isSymLink();
    }

    void logMove(const QString &from, const QString &to)
    {
        journal.write({{QStringLiteral("op"), QStringLiteral("move")},
                       {QStringLiteral("from"), from},
                       {QStringLiteral("to"), to}});
    }

    // Moves `from` to `to`, which must not exist. 0, EEXIST or an errno.
    int movePath(const QString &from, const QString &to, QString *error)
    {
        int r = Fs::renameNoReplace(from, to);
        if (r == EACCES && ownedDir(from)) {
            // Moving a folder to another parent rewrites its "..": a read-only
            // folder of ours needs write permission for that moment
            const mode_t mode = modeOf(from);
            ::chmod(QFile::encodeName(from).constData(), (mode & 07777) | S_IWUSR);
            r = Fs::renameNoReplace(from, to);
            ::chmod(QFile::encodeName(r == 0 ? to : from).constData(), mode & 07777);
        }
        if (r == 0 || r == EEXIST)
            return r;
        if (r != EXDEV) {
            *error = QStringLiteral("%1: %2").arg(from, errText(r));
            return r;
        }

        // Another filesystem: copy next to the destination, rename into place,
        // then remove the source
        QString blocker;
        if (!Fs::canRemoveTree(from, &blocker)) {
            *error = QStringLiteral("%1: %2").arg(blocker, errText(EACCES));
            return EACCES;
        }
        const QString parent = QFileInfo(to).absolutePath();
        const qint64 size = Fs::treeSize(from);
        const qint64 free = Fs::freeSpace(parent);
        if (free >= 0 && size > free) {
            *error = QStringLiteral("%1: %2").arg(to, errText(ENOSPC));
            return ENOSPC;
        }
        const QString tmp = parent + QStringLiteral("/.lingmo-userdirs-")
            + QUuid::createUuid().toString(QUuid::Id128).left(12) + QStringLiteral(".part");
        if (!Fs::copyTree(from, tmp, error, [this](qint64 bytes) {
                pendingBytes += bytes;
                report(QString(), pendingBytes >> 20);
                pendingBytes &= (1 << 20) - 1;
            })) {
            QString ignored;
            Fs::removeTree(tmp, &ignored);
            return EIO;
        }
        r = Fs::renameNoReplace(tmp, to);
        if (r != 0) {
            QString ignored;
            Fs::removeTree(tmp, &ignored);
            if (r != EEXIST)
                *error = QStringLiteral("%1: %2").arg(to, errText(r));
            return r;
        }
        QString removeError;
        if (!Fs::removeTree(from, &removeError)) {
            // Nothing is lost: the copy is complete, the source only partly gone
            summary.errors << QStringLiteral("%1 (copied, but the original could not be fully removed)").arg(removeError);
        }
        return 0;
    }

    // Moves every entry of `from` into the existing folder `to`, keeping both
    // on a name clash. Removes `from` when it ends up empty.
    void mergeInto(const QString &from, const QString &to)
    {
        const QStringList kids = QDir(from).entryList(QDir::AllEntries | QDir::Hidden | QDir::System
                                                      | QDir::NoDotAndDotDot, QDir::Name);
        // Entries can't leave a read-only folder; ours can be opened up for it
        const mode_t fromMode = modeOf(from);
        const bool reopened = !dryRun && ::access(QFile::encodeName(from).constData(), W_OK) != 0 && ownedDir(from);
        if (reopened)
            ::chmod(QFile::encodeName(from).constData(), (fromMode & 07777) | S_IWUSR | S_IXUSR);

        int left = 0;
        for (const QString &name : kids) {
            const QString src = from + QLatin1Char('/') + name;
            const bool isDir = Fs::isRealDir(src);
            report(src);
            QString dest = to + QLatin1Char('/') + name;
            int n = 1;
            bool moved = false;
            while (n < 10000) {
                if (dryRun) {
                    if (exists(dest)) {
                        dest = to + QLatin1Char('/') + Fs::numberedName(name, ++n, isDir);
                        continue;
                    }
                    claimed.insert(dest);
                    moved = true;
                    break;
                }
                QString error;
                const int r = movePath(src, dest, &error);
                if (r == 0) {
                    logMove(src, dest);
                    moved = true;
                    break;
                }
                if (r == EEXIST) {
                    dest = to + QLatin1Char('/') + Fs::numberedName(name, ++n, isDir);
                    continue;
                }
                summary.errors << error;
                break;
            }
            if (moved) {
                ++summary.movedItems;
                summary.actions << QStringLiteral("move \"%1\" -> \"%2\"").arg(src, dest);
                if (n > 1) {
                    ++summary.renamedClashes;
                    entryMappings << PathMapping{src, dest};
                }
            } else {
                ++summary.failedItems;
                ++left;
                // Links into this entry must keep pointing at it
                entryMappings << PathMapping{src, src};
            }
            report(QString(), 1);
        }

        if (dryRun) {
            summary.actions << QStringLiteral("remove empty folder \"%1\"").arg(from);
            return;
        }
        if (left == 0 && QDir(from).isEmpty(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) {
            if (::rmdir(QFile::encodeName(from).constData()) == 0) {
                journal.write({{QStringLiteral("op"), QStringLiteral("rmdir")},
                               {QStringLiteral("path"), from},
                               {QStringLiteral("mode"), int(fromMode & 07777)}});
                summary.actions << QStringLiteral("remove empty folder \"%1\"").arg(from);
                return;
            }
            // A mount point stays (EBUSY); it is empty now
            summary.errors << QStringLiteral("%1: %2").arg(from, errText(errno));
        }
        if (reopened)
            ::chmod(QFile::encodeName(from).constData(), fromMode & 07777);
    }

    // Returns true when `item.to` holds the folder afterwards
    bool runItem(const PlanItem &item)
    {
        switch (item.action) {
        case Action::Skip:
            return false;
        case Action::ConfigOnly:
            summary.actions << QStringLiteral("point XDG_%1_DIR at \"%2\"").arg(item.key, item.to);
            return true;
        case Action::Rename:
        case Action::Merge:
            break;
        }

        const QFileInfo fromInfo(item.from);
        if (!fromInfo.exists() && !fromInfo.isSymLink()) {
            summary.errors << QStringLiteral("%1: %2").arg(item.from, errText(ENOENT));
            return Fs::isRealDir(item.to);
        }

        const bool targetPresent = exists(item.to);
        const bool targetEmpty = !dryRun ? Fs::isEmptyDir(item.to) : (!claimed.contains(item.to) && Fs::isEmptyDir(item.to));
        const bool sameFs = Fs::sameDevice(item.from, QFileInfo(item.to).absolutePath());

        // One rename(2) for the whole folder when nothing is in the way
        if ((!targetPresent || targetEmpty) && (sameFs || fromInfo.isSymLink())) {
            report(item.from);
            if (dryRun) {
                summary.actions << QStringLiteral("rename \"%1\" -> \"%2\"").arg(item.from, item.to);
                claimed.insert(item.to);
                ++summary.renamedFolders;
                report(QString(), 1);
                return true;
            }
            if (targetEmpty) {
                // Only an empty folder is ever removed, and it comes back on undo
                const mode_t mode = modeOf(item.to);
                if (::rmdir(QFile::encodeName(item.to).constData()) == 0)
                    journal.write({{QStringLiteral("op"), QStringLiteral("rmdir")},
                                   {QStringLiteral("path"), item.to},
                                   {QStringLiteral("mode"), int(mode & 07777)}});
            }
            QString error;
            const int r = movePath(item.from, item.to, &error);
            if (r == 0) {
                logMove(item.from, item.to);
                summary.actions << QStringLiteral("rename \"%1\" -> \"%2\"").arg(item.from, item.to);
                ++summary.renamedFolders;
                report(QString(), 1);
                return true;
            }
            if (r != EEXIST) {
                summary.errors << error;
                ++summary.failedItems;
                if (targetEmpty && !QFileInfo::exists(item.to))
                    QDir().mkdir(item.to);
                return false;
            }
            // Something appeared at the target meanwhile: merge into it
        }

        if (item.action == Action::Rename && !exists(item.to)) {
            // Another filesystem: create the folder and move entry by entry
            if (!dryRun) {
                if (!QDir().mkdir(item.to)) {
                    summary.errors << QStringLiteral("%1: %2").arg(item.to, errText(errno));
                    ++summary.failedItems;
                    return false;
                }
                ::chmod(QFile::encodeName(item.to).constData(), modeOf(item.from) & 07777);
                journal.write({{QStringLiteral("op"), QStringLiteral("mkdir")}, {QStringLiteral("path"), item.to}});
            }
            summary.actions << QStringLiteral("create folder \"%1\"").arg(item.to);
        }

        const int failedBefore = summary.failedItems;
        mergeInto(item.from, item.to);
        if (summary.failedItems == failedBefore)
            ++summary.mergedFolders;
        return Fs::isRealDir(item.to) || dryRun;
    }

    // Rewrites one file through the journal
    void rewriteFile(const QString &path, const QList<PathMapping> &mappings, bool dedupe)
    {
        bool ok = false;
        const QByteArray old = readAll(path, &ok);
        if (!ok)
            return;
        const QString text = QString::fromUtf8(old);
        if (text.toUtf8() != old)
            return; // not UTF-8 text: leave it alone
        int count = 0;
        QString updated = rewritePaths(text, mappings, &count);
        if (dedupe)
            updated = dedupeLines(updated);
        if (updated == text)
            return;
        summary.actions << QStringLiteral("update %1 path(s) in \"%2\"").arg(count).arg(path);
        summary.rewrittenFiles << path;
        if (dryRun)
            return;
        writeTracked(path, old, true, updated.toUtf8());
    }

    void writeTracked(const QString &path, const QByteArray &old, bool existed, const QByteArray &data)
    {
        const QString backup = existed ? journal.backup(path, old) : QString();
        journal.write({{QStringLiteral("op"), QStringLiteral("file")},
                       {QStringLiteral("path"), path},
                       {QStringLiteral("backup"), backup},
                       {QStringLiteral("existed"), existed},
                       {QStringLiteral("sha"), QString::fromLatin1(sha(data))}});
        QString error;
        if (!writeFile(path, data, &error))
            summary.errors << error;
    }
};

} // namespace

Executor::Executor(const Context &ctx)
    : m_ctx(ctx)
{
}

Summary Executor::run(const Plan &plan, bool dryRun, const std::function<void(const Progress &)> &progress)
{
    Run run;
    run.ctx = m_ctx;
    run.dryRun = dryRun;
    run.progressFn = progress;
    run.summary.dryRun = dryRun;

    for (const PlanItem &item : plan.items) {
        if (!item.enabled || item.action == Action::Skip)
            continue;
        run.progress.total += 1 + item.entries + (item.crossDevice ? item.bytes / (1 << 20) : 0);
    }

    if (!dryRun) {
        QString error;
        if (!run.journal.open(m_ctx.undoRoot(), &error)) {
            // No journal, no changes
            run.summary.errors << error;
            return run.summary;
        }
        run.summary.journalDir = run.journal.dir();
        run.journal.write({{QStringLiteral("op"), QStringLiteral("begin")},
                           {QStringLiteral("mode"), plan.mode == Plan::LanguageChange ? QStringLiteral("language")
                                                                                        : QStringLiteral("duplicates")},
                           {QStringLiteral("locale"), plan.locale},
                           {QStringLiteral("time"), QDateTime::currentDateTime().toString(Qt::ISODate)}});
    }

    // 1. Folders
    UserDirsConfig config;
    config.load(m_ctx.userDirsFile(), m_ctx.home);
    const QByteArray oldConfig = readAll(m_ctx.userDirsFile());
    QList<PathMapping> mappings;
    bool configChanged = false;
    for (const PlanItem &item : plan.items) {
        if (!item.enabled || item.action == Action::Skip)
            continue;
        if (!run.runItem(item))
            continue;
        if (item.action != Action::ConfigOnly)
            mappings << PathMapping{item.from, item.to};
        if (item.setsConfig && config.path(item.key) != item.to) {
            config.setPath(item.key, item.to);
            configChanged = true;
        }
        run.report(QString(), 0);
    }
    mappings = run.entryMappings + mappings;
    run.summary.mappings = mappings;

    // 2. user-dirs.dirs and user-dirs.locale
    if (configChanged) {
        run.summary.actions << QStringLiteral("update \"%1\"").arg(m_ctx.userDirsFile());
        if (!dryRun)
            run.writeTracked(m_ctx.userDirsFile(), oldConfig, config.exists(), config.text().toUtf8());
    }
    if (plan.writeLocale && !plan.locale.isEmpty()
        && UserDirsConfig::readLocale(m_ctx.userDirsLocaleFile()) != plan.locale) {
        run.summary.actions << QStringLiteral("write \"%1\" to \"%2\"").arg(plan.locale, m_ctx.userDirsLocaleFile());
        if (!dryRun) {
            bool existed = false;
            const QByteArray old = readAll(m_ctx.userDirsLocaleFile(), &existed);
            QDir().mkpath(m_ctx.configHome);
            run.writeTracked(m_ctx.userDirsLocaleFile(), old, existed, plan.locale.toUtf8());
        }
    }

    // 3. Bookmarks, favourites, recent files, launchers
    if (!mappings.isEmpty()) {
        const QString desktop = config.path(QStringLiteral("DESKTOP"));
        for (const QString &file : pathHoldingFiles(m_ctx, desktop)) {
            const bool gtkBookmarks = file.endsWith(QLatin1String("/bookmarks"));
            run.rewriteFile(file, mappings, gtkBookmarks);
        }
    }

    if (!dryRun)
        run.journal.write({{QStringLiteral("op"), QStringLiteral("end")}});
    run.progress.done = run.progress.total;
    run.report(QString(), 0);
    return run.summary;
}

// ------------------------------------------------------------ undo

QStringList journals(const Context &ctx)
{
    QStringList list;
    const QDir root(ctx.undoRoot());
    for (const QString &d : root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed)) {
        if (QFileInfo::exists(root.filePath(d) + QLatin1Char('/') + JournalName))
            list << root.filePath(d);
    }
    return list;
}

bool undo(const Context &ctx, const QString &journalDir, QStringList *messages)
{
    Q_UNUSED(ctx)
    QStringList dummy;
    QStringList &msg = messages ? *messages : dummy;

    QFile file(journalDir + QLatin1Char('/') + JournalName);
    if (!file.open(QIODevice::ReadOnly)) {
        msg << QStringLiteral("%1: %2").arg(file.fileName(), file.errorString());
        return false;
    }
    QList<QJsonObject> ops;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject())
            ops << doc.object();
    }
    file.close();

    Run run;
    run.ctx = ctx;
    bool ok = true;
    for (auto it = ops.crbegin(); it != ops.crend(); ++it) {
        const QJsonObject &op = *it;
        const QString kind = op.value(QStringLiteral("op")).toString();
        if (kind == QLatin1String("move")) {
            const QString from = op.value(QStringLiteral("from")).toString();
            const QString to = op.value(QStringLiteral("to")).toString();
            const QFileInfo now(to);
            if (!now.exists() && !now.isSymLink()) {
                msg << QStringLiteral("not found, skipped: %1").arg(to);
                continue;
            }
            // A folder recreated empty since (xdg-user-dirs-update) may go
            if (Fs::isEmptyDir(from) && Fs::isRealDir(to))
                ::rmdir(QFile::encodeName(from).constData());
            QString error;
            const int r = run.movePath(to, from, &error);
            if (r == EEXIST) {
                msg << QStringLiteral("kept, \"%1\" exists again: %2").arg(from, to);
                ok = false;
            } else if (r != 0) {
                msg << error;
                ok = false;
            } else {
                msg << QStringLiteral("moved back \"%1\" -> \"%2\"").arg(to, from);
            }
        } else if (kind == QLatin1String("rmdir")) {
            const QString path = op.value(QStringLiteral("path")).toString();
            if (!QFileInfo::exists(path)) {
                QDir().mkdir(path);
                const int mode = op.value(QStringLiteral("mode")).toInt(0);
                if (mode)
                    ::chmod(QFile::encodeName(path).constData(), mode_t(mode) | S_IWUSR | S_IXUSR);
                msg << QStringLiteral("recreated \"%1\"").arg(path);
            }
        } else if (kind == QLatin1String("mkdir")) {
            const QString path = op.value(QStringLiteral("path")).toString();
            if (Fs::isEmptyDir(path) && ::rmdir(QFile::encodeName(path).constData()) == 0)
                msg << QStringLiteral("removed empty \"%1\"").arg(path);
        } else if (kind == QLatin1String("file")) {
            const QString path = op.value(QStringLiteral("path")).toString();
            const QByteArray current = readAll(path);
            if (sha(current) != op.value(QStringLiteral("sha")).toString().toLatin1()) {
                msg << QStringLiteral("changed since, left as is: %1").arg(path);
                continue;
            }
            if (op.value(QStringLiteral("existed")).toBool()) {
                QString error;
                if (writeFile(path, readAll(op.value(QStringLiteral("backup")).toString()), &error))
                    msg << QStringLiteral("restored %1").arg(path);
                else {
                    msg << error;
                    ok = false;
                }
            } else {
                QFile::remove(path);
                msg << QStringLiteral("removed %1").arg(path);
            }
        }
    }
    // The read-only folders reopened for the undo get their mode back
    for (const QJsonObject &op : ops) {
        if (op.value(QStringLiteral("op")).toString() == QLatin1String("rmdir")) {
            const int mode = op.value(QStringLiteral("mode")).toInt(0);
            if (mode)
                ::chmod(QFile::encodeName(op.value(QStringLiteral("path")).toString()).constData(), mode_t(mode));
        }
    }
    QFile::rename(journalDir + QLatin1Char('/') + JournalName, journalDir + QLatin1Char('/') + UndoneName);
    msg << run.summary.errors;
    return ok;
}

} // namespace UserDirs
