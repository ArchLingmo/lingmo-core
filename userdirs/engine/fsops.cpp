/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "userdirs.h"

#include <QDir>
#include <QFileInfo>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

namespace UserDirs {
namespace Fs {

static QByteArray enc(const QString &path)
{
    return QFile::encodeName(path);
}

static QStringList children(const QString &dir)
{
    QStringList list;
    DIR *d = ::opendir(enc(dir).constData());
    if (!d)
        return list;
    while (dirent *e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
            continue;
        list << QFile::decodeName(e->d_name);
    }
    ::closedir(d);
    list.sort();
    return list;
}

int renameNoReplace(const QString &from, const QString &to)
{
    const QByteArray f = enc(from), t = enc(to);
    if (::renameat2(AT_FDCWD, f.constData(), AT_FDCWD, t.constData(), RENAME_NOREPLACE) == 0)
        return 0;
    const int err = errno;
    if (err != EINVAL && err != ENOSYS)
        return err;

    // Filesystems without RENAME_NOREPLACE (some FUSE/network ones)
    struct stat st;
    if (::lstat(f.constData(), &st) != 0)
        return errno;
    if (!S_ISDIR(st.st_mode)) {
        // link(2) fails atomically when the name exists
        if (::link(f.constData(), t.constData()) == 0) {
            ::unlink(f.constData());
            return 0;
        }
        if (errno == EEXIST)
            return EEXIST;
    }
    struct stat tst;
    if (::lstat(t.constData(), &tst) == 0)
        return EEXIST;
    return ::rename(f.constData(), t.constData()) == 0 ? 0 : errno;
}

QString numberedName(const QString &name, int n, bool isDir)
{
    const QString suffix = QStringLiteral(" (%1)").arg(n);
    if (isDir)
        return name + suffix;
    int dot = name.lastIndexOf(QLatin1Char('.'));
    if (dot <= 0)
        return name + suffix; // no extension, or a dot file (".bashrc")
    const QString ext = name.mid(dot + 1);
    if (ext.isEmpty() || ext.size() > 8 || ext.contains(QLatin1Char(' ')))
        return name + suffix;
    // "backup.tar.gz" -> "backup (2).tar.gz"
    const int tar = name.left(dot).lastIndexOf(QLatin1Char('.'));
    if (tar > 0 && name.mid(tar, dot - tar).compare(QLatin1String(".tar"), Qt::CaseInsensitive) == 0)
        dot = tar;
    return name.left(dot) + suffix + name.mid(dot);
}

static qint64 treeSizeImpl(const QByteArray &path, qint64 *entries, int depth)
{
    struct stat st;
    if (::lstat(path.constData(), &st) != 0)
        return 0;
    if (entries)
        ++*entries;
    if (!S_ISDIR(st.st_mode))
        return S_ISREG(st.st_mode) ? qint64(st.st_size) : 0;
    qint64 total = 4096;
    if (depth > 256)
        return total;
    DIR *d = ::opendir(path.constData());
    if (!d)
        return total;
    while (dirent *e = ::readdir(d)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
            continue;
        total += treeSizeImpl(path + '/' + e->d_name, entries, depth + 1);
    }
    ::closedir(d);
    return total;
}

qint64 treeSize(const QString &path, qint64 *entries)
{
    return treeSizeImpl(enc(path), entries, 0);
}

qint64 freeSpace(const QString &path)
{
    QString p = path;
    while (!p.isEmpty() && !QFileInfo::exists(p))
        p = QFileInfo(p).absolutePath() == p ? QString() : QFileInfo(p).absolutePath();
    struct statvfs vfs;
    if (p.isEmpty() || ::statvfs(enc(p).constData(), &vfs) != 0)
        return -1;
    return qint64(vfs.f_bavail) * qint64(vfs.f_frsize);
}

static bool deviceOf(QString path, dev_t *dev)
{
    struct stat st;
    // A path that does not exist yet lives on its parent's filesystem
    while (!path.isEmpty()) {
        if (::lstat(enc(path).constData(), &st) == 0) {
            *dev = st.st_dev;
            return true;
        }
        const QString parent = QFileInfo(path).absolutePath();
        if (parent == path)
            break;
        path = parent;
    }
    return false;
}

bool sameDevice(const QString &a, const QString &b)
{
    dev_t da, db;
    if (!deviceOf(a, &da) || !deviceOf(b, &db))
        return true;
    return da == db;
}

bool isRealDir(const QString &path)
{
    struct stat st;
    return ::lstat(enc(path).constData(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isEmptyDir(const QString &path)
{
    return isRealDir(path) && children(path).isEmpty();
}

// removeTree() makes the user's own read-only folders writable again
static bool writableOrOwned(const QByteArray &dir)
{
    if (::access(dir.constData(), W_OK | X_OK) == 0)
        return true;
    struct stat st;
    return ::lstat(dir.constData(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::geteuid();
}

bool canRemoveTree(const QString &path, QString *blocker)
{
    struct stat st;
    const QByteArray p = enc(path);
    if (::lstat(p.constData(), &st) != 0)
        return true;
    // The entry itself is removed from its parent
    const QByteArray parent = enc(QFileInfo(path).absolutePath());
    if (!writableOrOwned(parent)) {
        if (blocker)
            *blocker = QFileInfo(path).absolutePath();
        return false;
    }
    if (!S_ISDIR(st.st_mode))
        return true;
    const QStringList kids = children(path);
    if (!kids.isEmpty() && !writableOrOwned(p)) {
        if (blocker)
            *blocker = path;
        return false;
    }
    for (const QString &k : kids) {
        if (!canRemoveTree(path + QLatin1Char('/') + k, blocker))
            return false;
    }
    return true;
}

static QString errText(int err)
{
    return QString::fromLocal8Bit(std::strerror(err));
}

static bool copyFile(const QByteArray &from, const QByteArray &to, const struct stat &st, QString *error,
                     const std::function<void(qint64)> &bytesCopied)
{
    const int in = ::open(from.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (in < 0) {
        *error = QStringLiteral("%1: %2").arg(QFile::decodeName(from), errText(errno));
        return false;
    }
    const int out = ::open(to.constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (out < 0) {
        *error = QStringLiteral("%1: %2").arg(QFile::decodeName(to), errText(errno));
        ::close(in);
        return false;
    }
    bool ok = true;
    char buf[1 << 16];
    for (;;) {
        ssize_t n = ::copy_file_range(in, nullptr, out, nullptr, 1 << 24, 0);
        if (n < 0 && (errno == EXDEV || errno == ENOSYS || errno == EINVAL || errno == EOPNOTSUPP)) {
            n = ::read(in, buf, sizeof buf);
            if (n > 0) {
                ssize_t off = 0;
                while (off < n) {
                    const ssize_t w = ::write(out, buf + off, n - off);
                    if (w < 0) {
                        if (errno == EINTR)
                            continue;
                        n = -1;
                        break;
                    }
                    off += w;
                }
            }
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            *error = QStringLiteral("%1: %2").arg(QFile::decodeName(to), errText(errno));
            ok = false;
            break;
        }
        if (n == 0)
            break;
        if (bytesCopied)
            bytesCopied(n);
    }
    if (ok) {
        ::fchmod(out, st.st_mode & 07777);
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
        ::futimens(out, times);
        // The source is deleted next: the copy must be on disk first
        if (::fsync(out) != 0) {
            *error = QStringLiteral("%1: %2").arg(QFile::decodeName(to), errText(errno));
            ok = false;
        }
    }
    if (::close(out) != 0 && ok) {
        *error = QStringLiteral("%1: %2").arg(QFile::decodeName(to), errText(errno));
        ok = false;
    }
    ::close(in);
    return ok;
}

static bool copyTreeImpl(const QByteArray &from, const QByteArray &to, QString *error,
                         const std::function<void(qint64)> &bytesCopied, int depth)
{
    struct stat st;
    if (::lstat(from.constData(), &st) != 0) {
        *error = QStringLiteral("%1: %2").arg(QFile::decodeName(from), errText(errno));
        return false;
    }
    if (depth > 256) {
        *error = QStringLiteral("%1: too deep").arg(QFile::decodeName(from));
        return false;
    }

    if (S_ISLNK(st.st_mode)) {
        QByteArray target(st.st_size > 0 ? st.st_size + 1 : 4096, '\0');
        const ssize_t n = ::readlink(from.constData(), target.data(), target.size());
        if (n < 0) {
            *error = QStringLiteral("%1: %2").arg(QFile::decodeName(from), errText(errno));
            return false;
        }
        target.truncate(n);
        if (::symlink(target.constData(), to.constData()) != 0) {
            *error = QStringLiteral("%1: %2").arg(QFile::decodeName(to), errText(errno));
            return false;
        }
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
        ::utimensat(AT_FDCWD, to.constData(), times, AT_SYMLINK_NOFOLLOW);
        return true;
    }
    if (S_ISREG(st.st_mode))
        return copyFile(from, to, st, error, bytesCopied);
    if (S_ISDIR(st.st_mode)) {
        if (::mkdir(to.constData(), 0700) != 0) {
            *error = QStringLiteral("%1: %2").arg(QFile::decodeName(to), errText(errno));
            return false;
        }
        DIR *d = ::opendir(from.constData());
        if (!d) {
            *error = QStringLiteral("%1: %2").arg(QFile::decodeName(from), errText(errno));
            return false;
        }
        bool ok = true;
        while (dirent *e = ::readdir(d)) {
            if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
                continue;
            if (!copyTreeImpl(from + '/' + e->d_name, to + '/' + e->d_name, error, bytesCopied, depth + 1)) {
                ok = false;
                break;
            }
        }
        ::closedir(d);
        if (!ok)
            return false;
        ::chmod(to.constData(), st.st_mode & 07777);
        const struct timespec times[2] = {st.st_atim, st.st_mtim};
        ::utimensat(AT_FDCWD, to.constData(), times, 0);
        return true;
    }
    *error = QStringLiteral("%1: not a regular file, folder or link").arg(QFile::decodeName(from));
    return false;
}

bool copyTree(const QString &from, const QString &to, QString *error, const std::function<void(qint64)> &bytesCopied)
{
    QString dummy;
    return copyTreeImpl(enc(from), enc(to), error ? error : &dummy, bytesCopied, 0);
}

static bool removeTreeImpl(const QByteArray &path, QString *error, int depth)
{
    struct stat st;
    if (::lstat(path.constData(), &st) != 0)
        return errno == ENOENT;
    if (S_ISDIR(st.st_mode) && depth < 300) {
        // A read-only folder we created while copying: make it writable again
        if (!(st.st_mode & S_IWUSR))
            ::chmod(path.constData(), (st.st_mode & 07777) | S_IWUSR | S_IXUSR);
        DIR *d = ::opendir(path.constData());
        if (d) {
            bool ok = true;
            while (dirent *e = ::readdir(d)) {
                if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0)
                    continue;
                if (!removeTreeImpl(path + '/' + e->d_name, error, depth + 1))
                    ok = false;
            }
            ::closedir(d);
            if (!ok)
                return false;
        }
        if (::rmdir(path.constData()) != 0) {
            *error = QStringLiteral("%1: %2").arg(QFile::decodeName(path), errText(errno));
            return false;
        }
        return true;
    }
    if (::unlink(path.constData()) != 0) {
        *error = QStringLiteral("%1: %2").arg(QFile::decodeName(path), errText(errno));
        return false;
    }
    return true;
}

bool removeTree(const QString &path, QString *error)
{
    QString dummy;
    return removeTreeImpl(enc(path), error ? error : &dummy, 0);
}

} // namespace Fs
} // namespace UserDirs
