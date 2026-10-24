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

namespace UserDirs {

// Room kept free on the destination after a cross-device move
static const qint64 SpaceMargin = 64ll * 1024 * 1024;

int Plan::actionableCount() const
{
    int n = 0;
    for (const PlanItem &item : items)
        if (item.enabled && item.action != Action::Skip)
            ++n;
    return n;
}

static QStringList childNames(const QString &dir)
{
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}

void measure(PlanItem &item)
{
    if (item.action == Action::Skip || item.action == Action::ConfigOnly)
        return;

    if (item.action == Action::Rename && QFileInfo::exists(item.to) && !Fs::isEmptyDir(item.to))
        item.action = Action::Merge;

    const QStringList kids = childNames(item.from);
    item.entries = kids.size();
    item.clashes = 0;
    if (item.action == Action::Merge) {
        const QStringList existing = childNames(item.to);
        for (const QString &k : kids)
            if (existing.contains(k))
                ++item.clashes;
    }

    const QString destParent = item.action == Action::Merge ? item.to : QFileInfo(item.to).absolutePath();
    item.crossDevice = !Fs::sameDevice(item.from, destParent);
    // A link is renamed, never followed
    if (item.crossDevice && !QFileInfo(item.from).isSymLink()) {
        item.bytes = Fs::treeSize(item.from);
        const qint64 free = Fs::freeSpace(destParent);
        if (free >= 0 && item.bytes + SpaceMargin > free) {
            item.action = Action::Skip;
            item.reason = SkipReason::NotEnoughSpace;
            item.enabled = false;
        }
    }
}

static bool directChildOfHome(const QString &path, const QString &home)
{
    return QFileInfo(path).absolutePath() == QDir::cleanPath(home);
}

Plan planLanguageChange(const Context &ctx, const XdgNames &names, const UserDirsConfig &config,
                        const QString &locale)
{
    Plan plan;
    plan.mode = Plan::LanguageChange;
    plan.locale = normalizeLocale(locale);
    const QString home = QDir::cleanPath(ctx.home);

    // Current folders of every key, to avoid renaming onto one of them
    QHash<QString, QString> current;
    for (const QString &key : config.keys())
        current.insert(key, config.path(key));

    QStringList targets;
    for (const QString &key : config.keys()) {
        PlanItem item;
        item.key = key;
        item.from = config.path(key);
        const QString name = names.translated(key, plan.locale);
        item.to = name.isEmpty() ? item.from : home + QLatin1Char('/') + name;

        if (name.isEmpty()) {
            // A folder xdg-user-dirs does not know (XDG_PROJECTS_DIR on older
            // defaults files...)
            item.reason = SkipReason::CustomName;
        } else if (item.from == home) {
            item.reason = SkipReason::PointsAtHome;
        } else if (!directChildOfHome(item.from, home)) {
            item.reason = SkipReason::OutsideHome;
        } else if (item.from == item.to) {
            item.reason = SkipReason::AlreadyNamed;
        } else if (!names.knownNames(key).contains(QFileInfo(item.from).fileName())) {
            item.reason = SkipReason::CustomName;
        } else if (targets.contains(item.to)) {
            item.reason = SkipReason::TargetInUse;
        } else {
            bool usedByOther = false;
            for (auto it = current.cbegin(); it != current.cend(); ++it)
                if (it.key() != key && it.value() == item.to)
                    usedByOther = true;
            const QFileInfo to(item.to);
            const QFileInfo from(item.from);
            if (usedByOther) {
                item.reason = SkipReason::TargetInUse;
            } else if (to.exists() || to.isSymLink()) {
                if (to.isSymLink() || !to.isDir()) {
                    item.reason = SkipReason::TargetNotFolder;
                } else if (!from.exists() && !from.isSymLink()) {
                    item.action = Action::ConfigOnly; // just point at the existing one
                } else if (from.isSymLink() && !Fs::isEmptyDir(item.to)) {
                    item.reason = SkipReason::SourceIsLink;
                } else {
                    item.action = Fs::isEmptyDir(item.to) ? Action::Rename : Action::Merge;
                }
            } else if (!from.exists() && !from.isSymLink()) {
                item.action = Action::ConfigOnly;
            } else {
                item.action = Action::Rename;
            }
        }
        if (item.action == Action::Skip)
            item.enabled = false;
        else
            targets << item.to;
        measure(item);
        plan.items << item;
    }
    return plan;
}

QList<DuplicateGroup> findDuplicates(const Context &ctx, const XdgNames &names, const UserDirsConfig &config)
{
    QList<DuplicateGroup> groups;
    const QString home = QDir::cleanPath(ctx.home);

    QStringList configured;
    for (const QString &key : config.keys())
        configured << config.path(key);

    for (const QString &key : config.keys()) {
        const QString path = config.path(key);
        if (path == home || !directChildOfHome(path, home) || !Fs::isRealDir(path))
            continue;
        // Only folders that still carry a standard name: a renamed one was the
        // user's choice
        const QStringList known = names.knownNames(key);
        if (!known.contains(QFileInfo(path).fileName()))
            continue;

        DuplicateGroup group;
        group.key = key;
        group.configured = path;
        for (const QString &name : known) {
            const QString candidate = home + QLatin1Char('/') + name;
            if (candidate == path || configured.contains(candidate) || !Fs::isRealDir(candidate))
                continue;
            // Case-insensitive filesystems: the same folder under another case
            if (QFileInfo(candidate).canonicalFilePath() == QFileInfo(path).canonicalFilePath())
                continue;
            group.others << candidate;
        }
        if (!group.others.isEmpty())
            groups << group;
    }
    return groups;
}

Plan planDuplicates(const Context &ctx, const XdgNames &names, const UserDirsConfig &config,
                    const QString &locale, MergeTarget target)
{
    Plan plan;
    plan.mode = Plan::Duplicates;
    plan.locale = normalizeLocale(locale);
    plan.writeLocale = false;
    const QString home = QDir::cleanPath(ctx.home);

    for (const DuplicateGroup &group : findDuplicates(ctx, names, config)) {
        QString into = group.configured;
        if (target == MergeTarget::Localized) {
            const QString localized = home + QLatin1Char('/') + names.translated(group.key, plan.locale);
            if (group.others.contains(localized))
                into = localized;
        }
        // Names a user would recognise by default: English, the current
        // language and the one the folders were created in. Folders that only
        // match a third language are listed but left unticked.
        const QString oldLocale = UserDirsConfig::readLocale(ctx.userDirsLocaleFile());
        const QStringList likely = {names.defaultName(group.key), names.translated(group.key, plan.locale),
                                    names.translated(group.key, oldLocale)};

        QStringList members = QStringList{group.configured} + group.others;
        for (const QString &member : std::as_const(members)) {
            if (member == into)
                continue;
            PlanItem item;
            item.key = group.key;
            item.from = member;
            item.to = into;
            item.action = Action::Merge;
            // Only the move of the configured folder changes the config
            item.setsConfig = (member == group.configured);
            item.enabled = member == group.configured || likely.contains(QFileInfo(member).fileName());
            if (Fs::isEmptyDir(item.to))
                item.action = Action::Rename;
            measure(item);
            plan.items << item;
        }
    }
    return plan;
}

} // namespace UserDirs
