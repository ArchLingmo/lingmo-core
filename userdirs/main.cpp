/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// lingmo-userdirs: keeps the XDG user folders in the session language.
//
//   --login-check            at login (autostart): ask once per locale when the
//                            folders were made for another language, or offer
//                            to merge duplicate folders
//   --language-changed CODE  Settings › Language changed the language
//   --merge-duplicates       Settings › Language › Merge duplicate folders
//   --force                  ask even when the user said "don't ask again"
//
// Without a window:
//   --dry-run [--locale CODE] [--duplicates [--into-localized]]
//   --apply   [--locale CODE] [--duplicates [--into-localized]]
//   --count-duplicates, --undo [JOURNAL], --list-undo

#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QLocale>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QSettings>
#include <QTranslator>

#include <cstdio>

#include "backend.h"
#include "engine/userdirs.h"

using namespace UserDirs;

static QString sessionLanguage()
{
    QSettings settings(QSettings::UserScope, QStringLiteral("lingmoos"), QStringLiteral("language"));
    const QString code = settings.value(QStringLiteral("language")).toString();
    if (!code.isEmpty())
        return code;
    const QString lang = qEnvironmentVariable("LANG");
    return lang.isEmpty() ? QLocale::system().name() : normalizeLocale(lang);
}

static void out(const QString &text)
{
    std::fputs(qPrintable(text + QLatin1Char('\n')), stdout);
}

static const char *actionName(Action a)
{
    switch (a) {
    case Action::Rename: return "rename";
    case Action::Merge: return "merge";
    case Action::ConfigOnly: return "config";
    case Action::Skip: return "skip";
    }
    return "?";
}

static int runCli(const QCommandLineParser &parser, const Context &ctx, const QString &locale)
{
    if (parser.isSet(QStringLiteral("list-undo"))) {
        for (const QString &j : journals(ctx))
            out(j);
        return 0;
    }
    if (parser.isSet(QStringLiteral("undo"))) {
        QString dir = parser.value(QStringLiteral("undo"));
        if (dir.isEmpty() || dir == QLatin1String("last")) {
            const QStringList list = journals(ctx);
            if (list.isEmpty()) {
                out(QStringLiteral("nothing to undo"));
                return 1;
            }
            dir = list.first();
        }
        QStringList messages;
        const bool ok = undo(ctx, dir, &messages);
        for (const QString &m : messages)
            out(m);
        return ok ? 0 : 1;
    }

    const XdgNames names(ctx);
    UserDirsConfig config;
    if (!config.load(ctx.userDirsFile(), ctx.home)) {
        out(QStringLiteral("no %1").arg(ctx.userDirsFile()));
        return 1;
    }

    if (parser.isSet(QStringLiteral("count-duplicates"))) {
        int n = 0;
        for (const DuplicateGroup &g : findDuplicates(ctx, names, config))
            n += g.others.size();
        out(QString::number(n));
        return 0;
    }

    const bool duplicates = parser.isSet(QStringLiteral("duplicates"));
    const Plan plan = duplicates
        ? planDuplicates(ctx, names, config, locale,
                         parser.isSet(QStringLiteral("into-localized")) ? MergeTarget::Localized : MergeTarget::Configured)
        : planLanguageChange(ctx, names, config, locale);

    out(QStringLiteral("# %1 plan for %2").arg(duplicates ? QStringLiteral("duplicates") : QStringLiteral("language"), plan.locale));
    for (const PlanItem &item : plan.items) {
        out(QStringLiteral("%1 %2 %3 -> %4 entries=%5 clashes=%6%7%8")
                .arg(item.enabled ? QStringLiteral("[x]") : QStringLiteral("[ ]"), QString::fromLatin1(actionName(item.action)),
                     item.from, item.to)
                .arg(item.entries)
                .arg(item.clashes)
                .arg(item.crossDevice ? QStringLiteral(" cross-device") : QString())
                .arg(item.action == Action::Skip ? QStringLiteral(" reason=%1").arg(int(item.reason)) : QString()));
    }

    const bool dryRun = !parser.isSet(QStringLiteral("apply"));
    Executor executor(ctx);
    const Summary summary = executor.run(plan, dryRun);
    out(dryRun ? QStringLiteral("# would do:") : QStringLiteral("# done:"));
    for (const QString &a : summary.actions)
        out(QStringLiteral("  ") + a);
    for (const QString &e : summary.errors)
        out(QStringLiteral("  error: ") + e);
    out(QStringLiteral("# folders renamed=%1 merged=%2 items=%3 clashes=%4 failed=%5")
            .arg(summary.renamedFolders)
            .arg(summary.mergedFolders)
            .arg(summary.movedItems)
            .arg(summary.renamedClashes)
            .arg(summary.failedItems));
    if (!summary.journalDir.isEmpty())
        out(QStringLiteral("# undo: lingmo-userdirs --undo \"%1\"").arg(summary.journalDir));
    return summary.failedItems > 0 ? 2 : 0;
}

static void loadTranslation(QCoreApplication &app, const QString &locale)
{
    QStringList dirs;
    const QString dev = qEnvironmentVariable("LINGMO_USERDIRS_TRANSLATIONS");
    if (!dev.isEmpty())
        dirs << dev;
    dirs << QStringLiteral("/usr/share/lingmo-userdirs/translations");
    for (const QString &l : localeFallbacks(locale)) {
        for (const QString &dir : dirs) {
            const QString qm = dir + QLatin1Char('/') + l + QStringLiteral(".qm");
            if (!QFile::exists(qm))
                continue;
            auto *translator = new QTranslator(&app);
            if (translator->load(qm)) {
                app.installTranslator(translator);
                return;
            }
            delete translator;
        }
    }
}

int main(int argc, char *argv[])
{
    // The window-less options need no display
    bool cli = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        if (a == "--dry-run" || a == "--apply" || a == "--count-duplicates" || a == "--undo"
            || a.startsWith("--undo=") || a == "--list-undo" || a == "-h" || a == "--help")
            cli = true;
    }

    QScopedPointer<QCoreApplication> app(cli ? new QCoreApplication(argc, argv) : new QGuiApplication(argc, argv));
    app->setApplicationName(QStringLiteral("lingmo-userdirs"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Keeps the XDG user folders in the session language"));
    parser.addHelpOption();
    parser.addOptions({
        {QStringLiteral("login-check"), QStringLiteral("Ask at login when the folders were made for another language")},
        {QStringLiteral("language-changed"), QStringLiteral("The session language changed to CODE"), QStringLiteral("CODE")},
        {QStringLiteral("merge-duplicates"), QStringLiteral("Offer to merge duplicate folders")},
        {QStringLiteral("force"), QStringLiteral("Ask even if the user chose not to be asked again")},
        {QStringLiteral("locale"), QStringLiteral("Target locale (default: the session language)"), QStringLiteral("CODE")},
        {QStringLiteral("duplicates"), QStringLiteral("With --dry-run/--apply: merge duplicates instead of renaming")},
        {QStringLiteral("into-localized"), QStringLiteral("Merge duplicates into the folder named in the session language")},
        {QStringLiteral("dry-run"), QStringLiteral("Print what would happen, change nothing")},
        {QStringLiteral("apply"), QStringLiteral("Do it without asking")},
        {QStringLiteral("count-duplicates"), QStringLiteral("Print how many duplicate folders exist")},
        {QStringLiteral("undo"), QStringLiteral("Undo a run (default: the last one)"), QStringLiteral("JOURNAL"), QString()},
        {QStringLiteral("list-undo"), QStringLiteral("List the runs that can be undone")},
        {QStringLiteral("summary-preview"), QStringLiteral("Show a sample summary page (screenshots)")},
    });
    // --undo without a value
    QStringList args = app->arguments();
    for (int i = 0; i < args.size(); ++i)
        if (args.at(i) == QLatin1String("--undo") && (i + 1 >= args.size() || args.at(i + 1).startsWith(QLatin1String("--"))))
            args.insert(i + 1, QStringLiteral("last"));
    parser.process(args);

    const Context ctx = Context::fromEnvironment();
    QString locale = parser.value(QStringLiteral("locale"));
    if (parser.isSet(QStringLiteral("language-changed")))
        locale = parser.value(QStringLiteral("language-changed"));
    if (locale.isEmpty())
        locale = sessionLanguage();
    locale = normalizeLocale(locale);

    if (cli)
        return runCli(parser, ctx, locale);

    UserDirsConfig config;
    if (!config.load(ctx.userDirsFile(), ctx.home))
        return 0; // xdg-user-dirs-update has not run yet: nothing to rename

    const bool force = parser.isSet(QStringLiteral("force"));
    const QString folderLocale = normalizeLocale(UserDirsConfig::readLocale(ctx.userDirsLocaleFile()));
    Backend::Mode mode = Backend::LanguageChange;

    if (parser.isSet(QStringLiteral("merge-duplicates"))) {
        mode = Backend::Duplicates;
    } else if (parser.isSet(QStringLiteral("language-changed")) || parser.isSet(QStringLiteral("login-check"))) {
        const bool atLogin = !parser.isSet(QStringLiteral("language-changed"));
        bool askRename = force || (folderLocale != locale && !(atLogin && declinedLocale(ctx) == locale));
        if (askRename) {
            const XdgNames names(ctx);
            if (planLanguageChange(ctx, names, config, locale).actionableCount() == 0) {
                // Already right (or nothing we may touch): remember the locale
                // so neither we nor xdg-user-dirs-gtk ask about it again
                if (!force) {
                    writeLocaleFile(ctx, locale);
                    askRename = false;
                }
            }
        }
        if (!askRename) {
            // At login, offer once to merge folders left twice by other tools
            if (!atLogin || duplicatesAsked(ctx))
                return 0;
            const XdgNames names(ctx);
            if (findDuplicates(ctx, names, config).isEmpty())
                return 0;
            mode = Backend::Duplicates;
        }
    }

    // One dialog at a time (login check and a language change can overlap)
    if (!QDBusConnection::sessionBus().registerService(QStringLiteral("com.lingmo.UserDirs")))
        return 0;

    loadTranslation(*app, locale);
    QLocale::setDefault(QLocale(locale));

    Backend backend(ctx, mode, locale);
    if (parser.isSet(QStringLiteral("summary-preview"))) {
        Summary sample;
        sample.renamedFolders = 3;
        sample.mergedFolders = 1;
        sample.movedItems = 12;
        sample.renamedClashes = 2;
        sample.rewrittenFiles = {QStringLiteral("bookmarks"), QStringLiteral("user-places.xbel"), QStringLiteral("recently-used.xbel")};
        sample.journalDir = QStringLiteral("preview");
        backend.showSummary(sample);
    }

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("backend"), &backend);
    const QUrl url(QStringLiteral("qrc:/qml/main.qml"));
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreated, app.data(), [url](QObject *obj, const QUrl &objUrl) {
        if (!obj && url == objUrl)
            QCoreApplication::exit(-1);
    }, Qt::QueuedConnection);
    engine.load(url);

    return app->exec();
}
