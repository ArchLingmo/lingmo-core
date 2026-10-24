/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// Renames and merges the XDG user folders (Documents, Music, ...) when the
// language changes, and merges duplicate folders left by other tools.
//
// Safety rules:
//  - files are only ever moved (rename(2)), never copied and kept twice;
//  - a move never replaces anything: renameat2(RENAME_NOREPLACE), and a name
//    clash keeps both entries, the moved one gets a " (2)" suffix;
//  - every step is written to an undo journal before it happens;
//  - across filesystems an entry is copied next to its destination under a
//    temporary name, renamed into place and only then removed at the source;
//    the move is refused up front when the destination lacks space.

#ifndef LINGMO_USERDIRS_H
#define LINGMO_USERDIRS_H

#include <QHash>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>

#include <functional>

namespace UserDirs {

// Where things live. Everything is resolved from here so tests can point it at
// a scratch home.
struct Context {
    QString home;
    QString configHome;
    QString dataHome;
    QStringList localeDirs;     // gettext roots holding <lang>/LC_MESSAGES/xdg-user-dirs.mo
    QStringList defaultsFiles;  // user-dirs.defaults candidates, first existing wins

    static Context fromEnvironment();
    QString userDirsFile() const { return configHome + QStringLiteral("/user-dirs.dirs"); }
    QString userDirsLocaleFile() const { return configHome + QStringLiteral("/user-dirs.locale"); }
    QString stateFile() const { return configHome + QStringLiteral("/lingmoos/userdirs.conf"); }
    QString undoRoot() const { return dataHome + QStringLiteral("/lingmo-userdirs/undo"); }
};

// ------------------------------------------------------------ names

// Reads a GNU gettext .mo catalogue (no plural/context support needed).
QHash<QString, QString> readMoFile(const QString &path, bool *ok = nullptr);

// "pt_BR.UTF-8@x" -> {"pt_BR.UTF-8@x", "pt_BR@x", "pt_BR", "pt"}
QStringList localeFallbacks(const QString &locale);
// "pt_BR.UTF-8" -> "pt_BR"
QString normalizeLocale(const QString &locale);

// Folder names as xdg-user-dirs knows them: the defaults file gives the
// English names, its gettext catalogue the translations.
class XdgNames
{
public:
    explicit XdgNames(const Context &ctx);

    QStringList keys() const { return m_keys; }               // DESKTOP, DOWNLOAD, ...
    QString defaultName(const QString &key) const { return m_defaults.value(key); }
    QString translated(const QString &key, const QString &locale) const;
    // English + every installed translation of this folder
    QStringList knownNames(const QString &key) const;
    QStringList installedLocales() const;

private:
    const QHash<QString, QString> &catalogue(const QString &locale) const;

    Context m_ctx;
    QStringList m_keys;
    QHash<QString, QString> m_defaults;
    mutable QHash<QString, QHash<QString, QString>> m_catalogues;
    mutable QStringList m_installed;
    mutable bool m_scanned = false;
};

// ------------------------------------------------------------ config

class UserDirsConfig
{
public:
    bool load(const QString &file, const QString &home);
    bool save(const QString &file, QString *error = nullptr) const;

    QStringList keys() const;                        // in file order
    QString path(const QString &key) const;          // absolute, "" when unset
    void setPath(const QString &key, const QString &absolutePath);
    bool exists() const { return m_exists; }
    QString text() const;

    static QString readLocale(const QString &file);
    static QString quoteValue(const QString &absolutePath, const QString &home);

private:
    QString m_home;
    QStringList m_lines;
    QMap<QString, QString> m_paths;
    bool m_exists = false;
};

// ------------------------------------------------------------ filesystem

namespace Fs {
// rename(2) that never replaces an existing entry. Returns 0 or an errno.
int renameNoReplace(const QString &from, const QString &to);
// "foto.jpg" -> "foto (2).jpg", "pasta" -> "pasta (2)", "a.tar.gz" -> "a (2).tar.gz"
QString numberedName(const QString &name, int n, bool isDir = false);
// Recursive size in bytes (does not follow symlinks)
qint64 treeSize(const QString &path, qint64 *entries = nullptr);
qint64 freeSpace(const QString &path);
bool sameDevice(const QString &a, const QString &b);
bool isRealDir(const QString &path);      // a directory, not a symlink to one
bool isEmptyDir(const QString &path);
// Every directory that would be emptied by moving `path` away is writable
bool canRemoveTree(const QString &path, QString *blocker = nullptr);
// Copies `from` to `to` (which must not exist) keeping modes, times and symlinks
bool copyTree(const QString &from, const QString &to, QString *error,
              const std::function<void(qint64)> &bytesCopied = {});
bool removeTree(const QString &path, QString *error);
}

// ------------------------------------------------------------ rewriting paths

struct PathMapping {
    QString from;
    QString to;
};

// Rewrites absolute paths ("/home/u/Videos/x") and file:// URLs
// ("file:///home/u/V%C3%ADdeos") that point into a mapped folder. The longest
// mapping wins; "/home/u/Videos2" is left alone.
QString rewritePaths(const QString &text, const QList<PathMapping> &mappings, int *replacements = nullptr);
// GTK bookmarks: drop lines repeated after the rewrite (first one wins)
QString dedupeLines(const QString &text);
// Files holding paths that follow the folders: GTK/KDE bookmarks, recent
// files, Lingmo settings (file manager favourites, dock, ...), desktop entries
QStringList pathHoldingFiles(const Context &ctx, const QString &desktopDir);

// ------------------------------------------------------------ plan

enum class Action {
    Rename,       // the folder is renamed; the target does not exist (or is empty)
    Merge,        // the target exists with content: entries are moved into it
    ConfigOnly,   // the folder does not exist yet: only user-dirs.dirs changes
    Skip,
};

enum class SkipReason {
    None,
    AlreadyNamed,       // already uses the target name
    PointsAtHome,       // "$HOME/": the folder is disabled
    OutsideHome,        // the user moved it elsewhere on purpose
    CustomName,         // renamed by the user: not a name xdg-user-dirs knows
    TargetNotFolder,    // a file or a link sits where the folder would go
    TargetInUse,        // another XDG folder already uses the target
    NotEnoughSpace,     // cross-device move that does not fit
    SourceIsLink,       // a link to elsewhere cannot be merged
    Unchecked,          // the user unticked it
};

struct PlanItem {
    QString key;            // DOCUMENTS
    QString from;           // absolute
    QString to;             // absolute
    Action action = Action::Skip;
    SkipReason reason = SkipReason::None;
    bool enabled = true;
    bool crossDevice = false;
    qint64 entries = 0;     // direct children of `from`
    qint64 clashes = 0;     // children whose name exists in `to`
    qint64 bytes = 0;       // only measured for cross-device moves
    bool setsConfig = true; // point user-dirs.dirs at `to` when done
};

struct Plan {
    enum Mode { LanguageChange, Duplicates };
    Mode mode = LanguageChange;
    QString locale;                 // target locale (pt_BR)
    bool writeLocale = true;        // write user-dirs.locale when done
    QList<PlanItem> items;

    int actionableCount() const;
};

Plan planLanguageChange(const Context &ctx, const XdgNames &names, const UserDirsConfig &config,
                        const QString &locale);

struct DuplicateGroup {
    QString key;
    QString configured;      // what user-dirs.dirs points at
    QStringList others;      // other folders with a name of the same kind
};
QList<DuplicateGroup> findDuplicates(const Context &ctx, const XdgNames &names, const UserDirsConfig &config);

enum class MergeTarget { Configured, Localized };
Plan planDuplicates(const Context &ctx, const XdgNames &names, const UserDirsConfig &config,
                    const QString &locale, MergeTarget target);

// Fills entries/clashes/bytes and re-checks space; turns Rename into Merge
// when the target gained content.
void measure(PlanItem &item);

// ------------------------------------------------------------ run

struct Summary {
    int renamedFolders = 0;   // whole folder renamed in one step
    int mergedFolders = 0;    // folder emptied into another one
    int movedItems = 0;       // entries moved into an existing folder
    int renamedClashes = 0;   // entries that got a " (2)" suffix
    int failedItems = 0;
    QStringList errors;
    QStringList rewrittenFiles;
    QStringList actions;      // human readable log ("move A -> B"), also the dry-run output
    QList<PathMapping> mappings;
    QString journalDir;
    bool dryRun = false;
};

struct Progress {
    qint64 done = 0;
    qint64 total = 0;
    QString current;
};

class Executor
{
public:
    explicit Executor(const Context &ctx);

    // Runs the plan. In dry-run mode nothing is touched: `actions` lists what
    // would happen.
    Summary run(const Plan &plan, bool dryRun = false,
                const std::function<void(const Progress &)> &progress = {});

private:
    Context m_ctx;
};

// Undo journal
QStringList journals(const Context &ctx);   // newest first, not yet undone
bool undo(const Context &ctx, const QString &journalDir, QStringList *messages);

// Remembered answers (~/.config/lingmoos/userdirs.conf)
QString declinedLocale(const Context &ctx);
void setDeclinedLocale(const Context &ctx, const QString &locale);
bool duplicatesAsked(const Context &ctx);
void setDuplicatesAsked(const Context &ctx, bool asked);
bool writeLocaleFile(const Context &ctx, const QString &locale, QString *error = nullptr);

} // namespace UserDirs

#endif
