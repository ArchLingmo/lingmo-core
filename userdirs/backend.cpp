/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#include "backend.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QThread>
#include <QVariantMap>

using namespace UserDirs;

Backend::Backend(const Context &ctx, Mode mode, const QString &locale, QObject *parent)
    : QObject(parent)
    , m_ctx(ctx)
    , m_names(ctx)
    , m_mode(mode)
    , m_locale(normalizeLocale(locale))
{
    replan();
}

QString Backend::mode() const
{
    return m_mode == LanguageChange ? QStringLiteral("language") : QStringLiteral("duplicates");
}

QString Backend::languageName() const
{
    QString name = QLocale(m_locale).nativeLanguageName();
    if (name.isEmpty())
        name = m_locale;
    // "português" -> "Português": it starts the question's object
    return name.left(1).toUpper() + name.mid(1);
}

void Backend::replan()
{
    UserDirsConfig config;
    config.load(m_ctx.userDirsFile(), m_ctx.home);
    if (m_mode == LanguageChange)
        m_plan = planLanguageChange(m_ctx, m_names, config, m_locale);
    else
        m_plan = planDuplicates(m_ctx, m_names, config, m_locale,
                                m_intoLocalized ? MergeTarget::Localized : MergeTarget::Configured);

    m_visible.clear();
    for (int i = 0; i < m_plan.items.size(); ++i) {
        const PlanItem &item = m_plan.items.at(i);
        if (item.action != Action::Skip) {
            m_visible << i;
            continue;
        }
        // Problems worth telling; folders that simply stay as they are aren't
        switch (item.reason) {
        case SkipReason::NotEnoughSpace:
        case SkipReason::TargetNotFolder:
        case SkipReason::SourceIsLink:
        case SkipReason::TargetInUse:
            m_visible << i;
            break;
        default:
            break;
        }
    }
    emit itemsChanged();
}

void Backend::setMergeIntoLocalized(bool value)
{
    if (m_intoLocalized == value)
        return;
    m_intoLocalized = value;
    replan();
}

QString Backend::displayName(const QString &path) const
{
    const QString home = QDir::cleanPath(m_ctx.home);
    if (QFileInfo(path).absolutePath() == home)
        return QFileInfo(path).fileName();
    if (path.startsWith(home + QLatin1Char('/')))
        return QStringLiteral("~") + path.mid(home.size());
    return path;
}

QString Backend::describe(const PlanItem &item) const
{
    QString text;
    switch (item.action) {
    case Action::Rename:
        text = tr("Rename");
        break;
    case Action::Merge:
        if (item.entries == 0)
            text = tr("Empty: it is removed");
        else
            text = tr("Merge %n item(s) into the existing folder", "", int(item.entries));
        if (item.clashes > 0)
            text += QStringLiteral(" · ") + tr("%n with the same name get “(2)”", "", int(item.clashes));
        break;
    case Action::ConfigOnly:
        text = tr("Use the folder that already exists");
        break;
    case Action::Skip:
        switch (item.reason) {
        case SkipReason::NotEnoughSpace:
            return tr("Not enough free space on the disk: kept");
        case SkipReason::TargetNotFolder:
            return tr("A file already uses this name: kept");
        case SkipReason::SourceIsLink:
            return tr("Link to another place: kept");
        case SkipReason::TargetInUse:
            return tr("Name used by another folder: kept");
        default:
            return tr("Kept");
        }
    }
    if (item.crossDevice)
        text += QStringLiteral(" · ") + tr("on another disk, files are moved one by one");
    return text;
}

QVariantList Backend::items() const
{
    QVariantList list;
    for (const int i : m_visible) {
        const PlanItem &item = m_plan.items.at(i);
        QVariantMap map;
        map[QStringLiteral("key")] = item.key;
        map[QStringLiteral("fromName")] = displayName(item.from);
        map[QStringLiteral("toName")] = displayName(item.to);
        map[QStringLiteral("from")] = item.from;
        map[QStringLiteral("to")] = item.to;
        map[QStringLiteral("detail")] = describe(item);
        map[QStringLiteral("checked")] = item.enabled && item.action != Action::Skip;
        map[QStringLiteral("checkable")] = item.action != Action::Skip;
        map[QStringLiteral("merge")] = item.action == Action::Merge;
        map[QStringLiteral("warning")] = item.action == Action::Skip;
        list << map;
    }
    return list;
}

int Backend::checkedCount() const
{
    return m_plan.actionableCount();
}

void Backend::setChecked(int index, bool checked)
{
    if (index < 0 || index >= m_visible.size())
        return;
    PlanItem &item = m_plan.items[m_visible.at(index)];
    if (item.action == Action::Skip)
        return;
    item.enabled = checked;
    emit itemsChanged();
}

void Backend::setPage(const QString &page)
{
    m_page = page;
    emit pageChanged();
}

void Backend::apply()
{
    if (m_page != QLatin1String("prompt"))
        return;
    // The folders may have changed while the dialog was open
    QList<PlanItem> items = m_plan.items;
    for (PlanItem &item : items)
        measure(item);
    m_plan.items = items;

    setPage(QStringLiteral("progress"));
    const Plan plan = m_plan;
    const Context ctx = m_ctx;
    QThread *thread = QThread::create([this, plan, ctx] {
        Executor executor(ctx);
        const Summary summary = executor.run(plan, false, [this](const Progress &p) {
            const double value = p.total > 0 ? double(p.done) / double(p.total) : 0;
            const QString current = p.current;
            QMetaObject::invokeMethod(this, [this, value, current] {
                m_progress = value;
                if (!current.isEmpty())
                    m_currentPath = displayName(current);
                emit progressChanged();
            }, Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(this, [this, summary] { showSummary(summary); }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void Backend::showSummary(const Summary &summary)
{
    QStringList title;
    if (m_mode == LanguageChange) {
        const int folders = summary.renamedFolders + summary.mergedFolders;
        if (folders > 0)
            title << tr("%n folder(s) renamed", "", folders);
        if (summary.movedItems > 0 && summary.mergedFolders > 0)
            title << tr("%n item(s) merged", "", summary.movedItems);
    } else {
        const int folders = summary.renamedFolders + summary.mergedFolders;
        if (folders > 0)
            title << tr("%n folder(s) merged", "", folders);
        if (summary.movedItems > 0)
            title << tr("%n item(s) moved", "", summary.movedItems);
    }
    m_summaryTitle = title.isEmpty() ? tr("No folder was changed") : title.join(QStringLiteral(", "));

    m_summaryDetails.clear();
    if (summary.renamedClashes > 0)
        m_summaryDetails << tr("%n item(s) with a repeated name were kept with a “(2)” suffix", "",
                               summary.renamedClashes);
    if (!summary.rewrittenFiles.isEmpty())
        m_summaryDetails << tr("%n bookmark or settings file(s) now point at the new folders", "",
                               int(summary.rewrittenFiles.size()));
    if (summary.failedItems > 0)
        m_summaryDetails << tr("%n item(s) could not be moved and stayed where they were", "",
                               summary.failedItems);
    if (summary.renamedFolders + summary.mergedFolders > 0)
        m_summaryDetails << tr("Log out and back in so every app sees the new names.");

    m_errors = summary.errors;
    m_journal = summary.journalDir;
    m_progress = 1;
    emit progressChanged();
    setPage(QStringLiteral("summary"));

    // Done for this locale either way
    if (m_mode == Duplicates)
        setDuplicatesAsked(m_ctx, true);
}

void Backend::keep(bool dontAskAgain)
{
    if (m_mode == LanguageChange) {
        // Like xdg-user-dirs-gtk: "don't ask again" records the new locale as
        // the folders' locale; otherwise only this locale's prompt is skipped
        if (dontAskAgain)
            writeLocaleFile(m_ctx, m_locale);
        else
            setDeclinedLocale(m_ctx, m_locale);
    } else {
        setDuplicatesAsked(m_ctx, true);
    }
    close();
}

void Backend::undoLast()
{
    if (m_journal.isEmpty())
        return;
    QStringList messages;
    const bool ok = undo(m_ctx, m_journal, &messages);
    m_journal.clear();
    m_summaryTitle = ok ? tr("The folders are back as they were") : tr("Undone, with some items left in place");
    m_summaryDetails.clear();
    m_errors.clear();
    for (const QString &m : std::as_const(messages)) {
        if (m.startsWith(QLatin1String("kept")) || m.startsWith(QLatin1String("changed since")) || m.contains(QLatin1String(": ")))
            m_errors << m;
    }
    if (m_mode == LanguageChange)
        setDeclinedLocale(m_ctx, m_locale);
    setPage(QStringLiteral("undone"));
}

void Backend::close()
{
    QCoreApplication::quit();
}
