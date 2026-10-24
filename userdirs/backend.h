/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

#ifndef USERDIRS_BACKEND_H
#define USERDIRS_BACKEND_H

#include <QObject>
#include <QVariantList>

#include "engine/userdirs.h"

class Backend : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString mode READ mode CONSTANT)
    Q_PROPERTY(QString languageName READ languageName CONSTANT)
    Q_PROPERTY(QString page READ page NOTIFY pageChanged)
    Q_PROPERTY(QVariantList items READ items NOTIFY itemsChanged)
    Q_PROPERTY(int checkedCount READ checkedCount NOTIFY itemsChanged)
    Q_PROPERTY(bool mergeIntoLocalized READ mergeIntoLocalized WRITE setMergeIntoLocalized NOTIFY itemsChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString currentPath READ currentPath NOTIFY progressChanged)
    Q_PROPERTY(QString summaryTitle READ summaryTitle NOTIFY pageChanged)
    Q_PROPERTY(QStringList summaryDetails READ summaryDetails NOTIFY pageChanged)
    Q_PROPERTY(QStringList errors READ errors NOTIFY pageChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY pageChanged)

public:
    enum Mode { LanguageChange, Duplicates };

    Backend(const UserDirs::Context &ctx, Mode mode, const QString &locale, QObject *parent = nullptr);

    QString mode() const;
    QString languageName() const;
    QString page() const { return m_page; }
    QVariantList items() const;
    int checkedCount() const;
    bool mergeIntoLocalized() const { return m_intoLocalized; }
    void setMergeIntoLocalized(bool value);
    double progress() const { return m_progress; }
    QString currentPath() const { return m_currentPath; }
    QString summaryTitle() const { return m_summaryTitle; }
    QStringList summaryDetails() const { return m_summaryDetails; }
    QStringList errors() const { return m_errors; }
    bool canUndo() const { return !m_journal.isEmpty(); }

    bool hasWork() const { return m_plan.actionableCount() > 0; }
    // Shows the summary page right away (screenshots, tests)
    void showSummary(const UserDirs::Summary &summary);

    Q_INVOKABLE void setChecked(int index, bool checked);
    Q_INVOKABLE void apply();
    Q_INVOKABLE void keep(bool dontAskAgain);
    Q_INVOKABLE void undoLast();
    Q_INVOKABLE void close();

signals:
    void pageChanged();
    void itemsChanged();
    void progressChanged();

private:
    void replan();
    void setPage(const QString &page);
    QString displayName(const QString &path) const;
    QString describe(const UserDirs::PlanItem &item) const;

    UserDirs::Context m_ctx;
    UserDirs::XdgNames m_names;
    Mode m_mode;
    QString m_locale;
    bool m_intoLocalized = false;
    UserDirs::Plan m_plan;
    QList<int> m_visible;  // plan indexes shown in the list

    QString m_page = QStringLiteral("prompt");
    double m_progress = 0;
    QString m_currentPath;
    QString m_summaryTitle;
    QStringList m_summaryDetails;
    QStringList m_errors;
    QString m_journal;
};

#endif
