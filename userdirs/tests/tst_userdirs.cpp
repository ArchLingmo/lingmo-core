/*
 * Copyright (C) 2026 LingmoOS Team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 */

// Unit tests for the folder rename/merge engine. Everything happens in
// throw-away homes under $TMPDIR; /dev/shm (another tmpfs) stands in for a
// second filesystem.

#include <QCryptographicHash>
#include <QDirIterator>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <sys/stat.h>
#include <unistd.h>

#include "engine/userdirs.h"

using namespace UserDirs;

namespace {

void writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY2(f.open(QIODevice::WriteOnly | QIODevice::Truncate), qPrintable(path));
    f.write(data);
}

QByteArray readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QByteArray("<missing>");
    return f.readAll();
}

// relative path -> "type:content-hash-or-link:mode"
QMap<QString, QString> snapshot(const QString &root)
{
    QMap<QString, QString> map;
    QDirIterator it(root, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QFileInfo fi(path);
        struct stat st;
        ::lstat(QFile::encodeName(path).constData(), &st);
        QString value;
        if (fi.isSymLink())
            value = QStringLiteral("link:") + fi.symLinkTarget();
        else if (fi.isDir())
            value = QStringLiteral("dir");
        else
            value = QStringLiteral("file:")
                + QString::fromLatin1(QCryptographicHash::hash(readFile(path), QCryptographicHash::Md5).toHex());
        value += QStringLiteral(":%1").arg(st.st_mode & 07777, 0, 8);
        map.insert(path.mid(root.size()), value);
    }
    return map;
}

struct Home {
    QTemporaryDir tmp;
    Context ctx;

    Home()
    {
        ctx.home = tmp.path() + QStringLiteral("/home");
        ctx.configHome = ctx.home + QStringLiteral("/.config");
        ctx.dataHome = ctx.home + QStringLiteral("/.local/share");
        ctx.localeDirs = {QStringLiteral("/usr/share/locale")};
        ctx.defaultsFiles = {QStringLiteral("/etc/xdg/user-dirs.defaults")};
        QDir().mkpath(ctx.configHome);
        QDir().mkpath(ctx.dataHome);
    }

    QString p(const QString &rel) const { return ctx.home + QLatin1Char('/') + rel; }

    void englishConfig(const QString &extra = QString())
    {
        writeFile(ctx.userDirsFile(),
                  "# This file is written by xdg-user-dirs-update\n"
                  "XDG_DESKTOP_DIR=\"$HOME/Desktop\"\n"
                  "XDG_DOWNLOAD_DIR=\"$HOME/Downloads\"\n"
                  "XDG_TEMPLATES_DIR=\"$HOME/Templates\"\n"
                  "XDG_PUBLICSHARE_DIR=\"$HOME/Public\"\n"
                  "XDG_DOCUMENTS_DIR=\"$HOME/Documents\"\n"
                  "XDG_MUSIC_DIR=\"$HOME/Music\"\n"
                  "XDG_PICTURES_DIR=\"$HOME/Pictures\"\n"
                  "XDG_VIDEOS_DIR=\"$HOME/Videos\"\n"
                  "XDG_PROJECTS_DIR=\"$HOME/\"\n"
                  + extra.toUtf8());
        writeFile(ctx.userDirsLocaleFile(), "en_US");
        for (const char *d : {"Desktop", "Downloads", "Templates", "Public", "Documents", "Music", "Pictures", "Videos"})
            QDir().mkpath(p(QString::fromLatin1(d)));
    }

    UserDirsConfig config() const
    {
        UserDirsConfig c;
        c.load(ctx.userDirsFile(), ctx.home);
        return c;
    }
};

const PlanItem *itemFor(const Plan &plan, const QString &key)
{
    for (const PlanItem &i : plan.items)
        if (i.key == key)
            return &i;
    return nullptr;
}

bool havePortuguese()
{
    return QFile::exists(QStringLiteral("/usr/share/locale/pt_BR/LC_MESSAGES/xdg-user-dirs.mo"))
        && QFile::exists(QStringLiteral("/etc/xdg/user-dirs.defaults"));
}

} // namespace

class TestUserDirs : public QObject
{
    Q_OBJECT

private slots:
    void numberedNames_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("isDir");
        QTest::addColumn<int>("n");
        QTest::addColumn<QString>("expected");
        QTest::newRow("file") << "foto.jpg" << false << 2 << "foto (2).jpg";
        QTest::newRow("third") << "foto.jpg" << false << 3 << "foto (3).jpg";
        QTest::newRow("no ext") << "README" << false << 2 << "README (2)";
        QTest::newRow("dotfile") << ".bashrc" << false << 2 << ".bashrc (2)";
        QTest::newRow("tar.gz") << "backup.tar.gz" << false << 2 << "backup (2).tar.gz";
        QTest::newRow("dir with dot") << "my.project" << true << 2 << "my.project (2)";
        QTest::newRow("sentence") << "Mr. Smith notes" << false << 2 << "Mr. Smith notes (2)";
    }

    void numberedNames()
    {
        QFETCH(QString, name);
        QFETCH(bool, isDir);
        QFETCH(int, n);
        QFETCH(QString, expected);
        QCOMPARE(Fs::numberedName(name, n, isDir), expected);
    }

    void localeFallbackChain()
    {
        QCOMPARE(localeFallbacks(QStringLiteral("pt_BR.UTF-8")),
                 QStringList({QStringLiteral("pt_BR.UTF-8"), QStringLiteral("pt_BR"), QStringLiteral("pt")}));
        QCOMPARE(normalizeLocale(QStringLiteral("sr_RS.UTF-8@latin")), QStringLiteral("sr_RS"));
    }

    void translationsFromCatalogue()
    {
        if (!havePortuguese())
            QSKIP("xdg-user-dirs pt_BR catalogue not installed");
        Home h;
        const XdgNames names(h.ctx);
        QCOMPARE(names.translated(QStringLiteral("DOCUMENTS"), QStringLiteral("pt_BR")), QStringLiteral("Documentos"));
        QCOMPARE(names.translated(QStringLiteral("VIDEOS"), QStringLiteral("pt_BR.UTF-8")), QStringLiteral("Vídeos"));
        QCOMPARE(names.translated(QStringLiteral("DESKTOP"), QStringLiteral("pt_BR")), QStringLiteral("Área de trabalho"));
        QCOMPARE(names.translated(QStringLiteral("MUSIC"), QStringLiteral("en_US")), QStringLiteral("Music"));
        QCOMPARE(names.translated(QStringLiteral("MUSIC"), QStringLiteral("C")), QStringLiteral("Music"));
        QVERIFY(names.knownNames(QStringLiteral("MUSIC")).contains(QStringLiteral("Músicas")));
    }

    void configRoundTrip()
    {
        Home h;
        h.englishConfig(QStringLiteral("XDG_WEIRD_DIR=\"/mnt/data/My \\\"Stuff\\\"\"\n"));
        UserDirsConfig c = h.config();
        QCOMPARE(c.path(QStringLiteral("MUSIC")), h.p(QStringLiteral("Music")));
        QCOMPARE(c.path(QStringLiteral("PROJECTS")), h.ctx.home);
        QCOMPARE(c.path(QStringLiteral("WEIRD")), QStringLiteral("/mnt/data/My \"Stuff\""));
        c.setPath(QStringLiteral("MUSIC"), h.p(QStringLiteral("Músicas")));
        QVERIFY(c.save(h.ctx.userDirsFile()));
        const QString text = QString::fromUtf8(readFile(h.ctx.userDirsFile()));
        QVERIFY(text.contains(QStringLiteral("XDG_MUSIC_DIR=\"$HOME/Músicas\"")));
        QVERIFY(text.startsWith(QStringLiteral("# This file is written by xdg-user-dirs-update")));
        QVERIFY(text.contains(QStringLiteral("XDG_PROJECTS_DIR=\"$HOME/\"")));  // untouched
        QCOMPARE(h.config().path(QStringLiteral("WEIRD")), QStringLiteral("/mnt/data/My \"Stuff\""));
    }

    void dryRunTouchesNothing()
    {
        if (!havePortuguese())
            QSKIP("xdg-user-dirs pt_BR catalogue not installed");
        Home h;
        h.englishConfig();
        writeFile(h.p(QStringLiteral("Videos/a.mp4")), "video");
        writeFile(h.p(QStringLiteral("Vídeos/a.mp4")), "other video");
        writeFile(h.ctx.configHome + QStringLiteral("/gtk-3.0/bookmarks"), "file://" + h.p(QStringLiteral("Videos")).toUtf8() + "\n");
        const auto before = snapshot(h.tmp.path());

        const XdgNames names(h.ctx);
        const Plan plan = planLanguageChange(h.ctx, names, h.config(), QStringLiteral("pt_BR"));
        const Summary s = Executor(h.ctx).run(plan, true);

        QCOMPARE(snapshot(h.tmp.path()), before);
        QVERIFY(s.dryRun);
        QVERIFY(s.journalDir.isEmpty());
        QVERIFY(s.actions.contains(QStringLiteral("rename \"%1\" -> \"%2\"").arg(h.p(QStringLiteral("Music")), h.p(QStringLiteral("Músicas")))));
        QVERIFY(s.actions.contains(QStringLiteral("move \"%1\" -> \"%2\"").arg(h.p(QStringLiteral("Videos/a.mp4")), h.p(QStringLiteral("Vídeos/a (2).mp4")))));
        QVERIFY(s.rewrittenFiles.contains(h.ctx.configHome + QStringLiteral("/gtk-3.0/bookmarks")));
    }

    void languagePlanRules()
    {
        if (!havePortuguese())
            QSKIP("xdg-user-dirs pt_BR catalogue not installed");
        Home h;
        h.englishConfig(QStringLiteral("XDG_TEMPLATES_DIR=\"$HOME/Meus modelos\"\n"));
        QDir().mkpath(h.p(QStringLiteral("Meus modelos")));
        writeFile(h.p(QStringLiteral("Público")), "a file where the folder would go");
        writeFile(h.p(QStringLiteral("Documentos/x.txt")), "x");
        QDir().mkpath(h.p(QStringLiteral("Imagens")));  // empty: plain rename

        const XdgNames names(h.ctx);
        const Plan plan = planLanguageChange(h.ctx, names, h.config(), QStringLiteral("pt_BR"));
        QCOMPARE(itemFor(plan, QStringLiteral("DOWNLOAD"))->reason, SkipReason::AlreadyNamed);
        QCOMPARE(itemFor(plan, QStringLiteral("PROJECTS"))->reason, SkipReason::PointsAtHome);
        QCOMPARE(itemFor(plan, QStringLiteral("TEMPLATES"))->reason, SkipReason::CustomName);
        QCOMPARE(itemFor(plan, QStringLiteral("PUBLICSHARE"))->reason, SkipReason::TargetNotFolder);
        QCOMPARE(itemFor(plan, QStringLiteral("DOCUMENTS"))->action, Action::Merge);
        QCOMPARE(itemFor(plan, QStringLiteral("PICTURES"))->action, Action::Rename);
        QCOMPARE(itemFor(plan, QStringLiteral("MUSIC"))->action, Action::Rename);
        QVERIFY(!itemFor(plan, QStringLiteral("PUBLICSHARE"))->enabled);
    }

    // The heart of it: merge never overwrites, clashes keep both, undo puts
    // every byte back
    void mergeNeverOverwritesAndUndo()
    {
        if (!havePortuguese())
            QSKIP("xdg-user-dirs pt_BR catalogue not installed");
        Home h;
        h.englishConfig();
        // Source (configured, English)
        writeFile(h.p(QStringLiteral("Documents/report.pdf")), "english report");
        writeFile(h.p(QStringLiteral("Documents/only-en.txt")), "only en");
        writeFile(h.p(QStringLiteral("Documents/.hidden")), "hidden en");
        writeFile(h.p(QStringLiteral("Documents/Projects/deep/nested/file.txt")), "deep en");
        writeFile(h.p(QStringLiteral("Documents/readonly.txt")), "read only");
        ::chmod(QFile::encodeName(h.p(QStringLiteral("Documents/readonly.txt"))).constData(), 0444);
        writeFile(h.p(QStringLiteral("Documents/locked/inside.txt")), "inside locked dir");
        ::chmod(QFile::encodeName(h.p(QStringLiteral("Documents/locked"))).constData(), 0555);
        QVERIFY(QFile::link(QStringLiteral("only-en.txt"), h.p(QStringLiteral("Documents/link-to-en"))));
        QVERIFY(QFile::link(QStringLiteral("/nonexistent/target"), h.p(QStringLiteral("Documents/broken-link"))));
        // Target (localized) with clashing names
        writeFile(h.p(QStringLiteral("Documentos/report.pdf")), "portuguese report");
        writeFile(h.p(QStringLiteral("Documentos/report (2).pdf")), "already a (2)");
        writeFile(h.p(QStringLiteral("Documentos/Projects/pt.txt")), "pt project");
        writeFile(h.p(QStringLiteral("Documentos/.hidden")), "hidden pt");

        const auto before = snapshot(h.tmp.path());
        const QByteArray configBefore = readFile(h.ctx.userDirsFile());

        const XdgNames names(h.ctx);
        const Plan plan = planLanguageChange(h.ctx, names, h.config(), QStringLiteral("pt_BR"));
        const PlanItem *docs = itemFor(plan, QStringLiteral("DOCUMENTS"));
        QCOMPARE(docs->action, Action::Merge);
        QCOMPARE(docs->clashes, 3);

        const Summary s = Executor(h.ctx).run(plan);
        QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QLatin1Char('\n'))));
        QCOMPARE(s.failedItems, 0);

        // Nothing in the target was overwritten
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/report.pdf"))), QByteArray("portuguese report"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/report (2).pdf"))), QByteArray("already a (2)"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/Projects/pt.txt"))), QByteArray("pt project"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/.hidden"))), QByteArray("hidden pt"));
        // ...and both versions survive
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/report (3).pdf"))), QByteArray("english report"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/.hidden (2)"))), QByteArray("hidden en"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/Projects (2)/deep/nested/file.txt"))), QByteArray("deep en"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/only-en.txt"))), QByteArray("only en"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/readonly.txt"))), QByteArray("read only"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/locked/inside.txt"))), QByteArray("inside locked dir"));
        QCOMPARE(QFileInfo(h.p(QStringLiteral("Documentos/link-to-en"))).symLinkTarget(), h.p(QStringLiteral("Documentos/only-en.txt")));
        QVERIFY(QFileInfo(h.p(QStringLiteral("Documentos/broken-link"))).isSymLink());
        QVERIFY(!QFileInfo::exists(h.p(QStringLiteral("Documents"))));
        QCOMPARE(s.renamedClashes, 3);

        // Whole-folder renames
        QVERIFY(QFileInfo(h.p(QStringLiteral("Músicas"))).isDir());
        QVERIFY(QFileInfo(h.p(QStringLiteral("Área de trabalho"))).isDir());
        QVERIFY(!QFileInfo::exists(h.p(QStringLiteral("Music"))));

        // Config follows
        const UserDirsConfig after = h.config();
        QCOMPARE(after.path(QStringLiteral("DOCUMENTS")), h.p(QStringLiteral("Documentos")));
        QCOMPARE(after.path(QStringLiteral("DESKTOP")), h.p(QStringLiteral("Área de trabalho")));
        QCOMPARE(after.path(QStringLiteral("DOWNLOAD")), h.p(QStringLiteral("Downloads")));
        QCOMPARE(after.path(QStringLiteral("PROJECTS")), h.ctx.home);
        QCOMPARE(UserDirsConfig::readLocale(h.ctx.userDirsLocaleFile()), QStringLiteral("pt_BR"));

        // Undo journal
        QCOMPARE(journals(h.ctx).value(0), s.journalDir);
        QStringList messages;
        QVERIFY2(undo(h.ctx, s.journalDir, &messages), qPrintable(messages.join(QLatin1Char('\n'))));
        auto restored = snapshot(h.tmp.path());
        // The journal itself is new
        for (auto it = restored.begin(); it != restored.end();)
            it = it.key().contains(QLatin1String("/lingmo-userdirs")) ? restored.erase(it) : std::next(it);
        auto expected = before;
        for (auto it = expected.begin(); it != expected.end();)
            it = it.key().contains(QLatin1String("/lingmo-userdirs")) ? expected.erase(it) : std::next(it);
        QCOMPARE(restored, expected);
        QCOMPARE(readFile(h.ctx.userDirsFile()), configBefore);
        QCOMPARE(readFile(h.ctx.userDirsLocaleFile()), QByteArray("en_US"));
        QVERIFY(journals(h.ctx).isEmpty());

        ::chmod(QFile::encodeName(h.p(QStringLiteral("Documents/locked"))).constData(), 0755);
    }

    void targetFilledAfterPlanning()
    {
        if (!havePortuguese())
            QSKIP("xdg-user-dirs pt_BR catalogue not installed");
        Home h;
        h.englishConfig();
        writeFile(h.p(QStringLiteral("Music/song.ogg")), "en song");
        const XdgNames names(h.ctx);
        const Plan plan = planLanguageChange(h.ctx, names, h.config(), QStringLiteral("pt_BR"));
        QCOMPARE(itemFor(plan, QStringLiteral("MUSIC"))->action, Action::Rename);
        // Someone creates the folder between the dialog and the click
        writeFile(h.p(QStringLiteral("Músicas/song.ogg")), "pt song");
        const Summary s = Executor(h.ctx).run(plan);
        QCOMPARE(readFile(h.p(QStringLiteral("Músicas/song.ogg"))), QByteArray("pt song"));
        QCOMPARE(readFile(h.p(QStringLiteral("Músicas/song (2).ogg"))), QByteArray("en song"));
        QVERIFY(s.errors.isEmpty());
    }

    void rewriteBookmarks()
    {
        const QString home = QStringLiteral("/home/u");
        const QList<PathMapping> maps = {
            {home + QStringLiteral("/Videos"), home + QStringLiteral("/Vídeos")},
            {home + QStringLiteral("/Documentos"), home + QStringLiteral("/Documents")},
            {home + QStringLiteral("/Documentos/a.txt"), home + QStringLiteral("/Documents/a (2).txt")},
            {home + QStringLiteral("/Documentos/stuck"), home + QStringLiteral("/Documentos/stuck")},
        };
        // GTK bookmarks: encoded URLs, labels kept
        QCOMPARE(rewritePaths(QStringLiteral("file:///home/u/Videos Filmes\nfile:///home/u/Videos2\n"), maps),
                 QStringLiteral("file:///home/u/V%C3%ADdeos Filmes\nfile:///home/u/Videos2\n"));
        // The rest of a URL stays as written
        QCOMPARE(rewritePaths(QStringLiteral("file:///home/u/Videos/F%C3%A9rias%202020/a.mp4"), maps),
                 QStringLiteral("file:///home/u/V%C3%ADdeos/F%C3%A9rias%202020/a.mp4"));
        // Encoded source folder
        QCOMPARE(rewritePaths(QStringLiteral("<bookmark href=\"file:///home/u/Documentos/b.txt\">"), maps),
                 QStringLiteral("<bookmark href=\"file:///home/u/Documents/b.txt\">"));
        // Longest mapping: a clash-renamed entry
        QCOMPARE(rewritePaths(QStringLiteral("recent=/home/u/Documentos/a.txt"), maps),
                 QStringLiteral("recent=/home/u/Documents/a (2).txt"));
        QCOMPARE(rewritePaths(QStringLiteral("file:///home/u/Documentos/a.txt"), maps),
                 QStringLiteral("file:///home/u/Documents/a%20(2).txt"));
        // An entry that could not move keeps its path
        QCOMPARE(rewritePaths(QStringLiteral("x=/home/u/Documentos/stuck/f"), maps),
                 QStringLiteral("x=/home/u/Documentos/stuck/f"));
        // Raw paths in ini lists, not prefixes of other names
        QCOMPARE(rewritePaths(QStringLiteral("favorites=/home/u/Videos, /home/u/Videos-old, /home/u/Videos/x"), maps),
                 QStringLiteral("favorites=/home/u/Vídeos, /home/u/Videos-old, /home/u/Vídeos/x"));
        QCOMPARE(rewritePaths(QStringLiteral("p=/other/home/u/Videos"), maps), QStringLiteral("p=/other/home/u/Videos"));
        // Dedupe after two folders became one
        QCOMPARE(dedupeLines(QStringLiteral("file:///a A\nfile:///b\nfile:///a Again\n")),
                 QStringLiteral("file:///a A\nfile:///b\n"));
    }

    void rewriteFilesThroughRun()
    {
        if (!havePortuguese())
            QSKIP("xdg-user-dirs pt_BR catalogue not installed");
        Home h;
        h.englishConfig();
        const QByteArray url = QUrl::fromLocalFile(h.p(QStringLiteral("Videos"))).toEncoded();
        writeFile(h.ctx.configHome + QStringLiteral("/gtk-3.0/bookmarks"),
                  url + " Filmes\n" + QUrl::fromLocalFile(h.p(QStringLiteral("Vídeos"))).toEncoded() + "\n");
        writeFile(h.ctx.dataHome + QStringLiteral("/user-places.xbel"),
                  "<xbel><bookmark href=\"" + url + "\"><title>Videos</title></bookmark></xbel>\n");
        // File manager favourites as QSettings writes them
        {
            QSettings fav(h.ctx.configHome + QStringLiteral("/lingmoos/lingmo-filemanager-places.conf"), QSettings::IniFormat);
            fav.setValue(QStringLiteral("favorites"), QStringList{h.p(QStringLiteral("Videos")), h.p(QStringLiteral("Music/Rock"))});
        }
        writeFile(h.p(QStringLiteral("Desktop/clip.desktop")),
                  "[Desktop Entry]\nType=Link\nURL=" + url + "/clip.mp4\n");
        QDir().mkpath(h.p(QStringLiteral("Vídeos")));
        writeFile(h.p(QStringLiteral("Videos/clip.mp4")), "clip");

        const XdgNames names(h.ctx);
        const Plan plan = planLanguageChange(h.ctx, names, h.config(), QStringLiteral("pt_BR"));
        const Summary s = Executor(h.ctx).run(plan);
        QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QLatin1Char('\n'))));

        const QByteArray newUrl = QUrl::fromLocalFile(h.p(QStringLiteral("Vídeos"))).toEncoded();
        // Both bookmarks now point at the same folder: one line left
        QCOMPARE(readFile(h.ctx.configHome + QStringLiteral("/gtk-3.0/bookmarks")), newUrl + " Filmes\n");
        QVERIFY(readFile(h.ctx.dataHome + QStringLiteral("/user-places.xbel")).contains("href=\"" + newUrl + "\""));
        QSettings fav(h.ctx.configHome + QStringLiteral("/lingmoos/lingmo-filemanager-places.conf"), QSettings::IniFormat);
        QCOMPARE(fav.value(QStringLiteral("favorites")).toStringList(),
                 QStringList({h.p(QStringLiteral("Vídeos")), h.p(QStringLiteral("Músicas/Rock"))}));
        // The desktop entry moved with the Desktop folder and was rewritten
        QVERIFY(readFile(h.p(QStringLiteral("Área de trabalho/clip.desktop"))).contains("URL=" + newUrl + "/clip.mp4"));

        // Undo restores the files we wrote
        QVERIFY(undo(h.ctx, s.journalDir, nullptr));
        QCOMPARE(readFile(h.ctx.configHome + QStringLiteral("/gtk-3.0/bookmarks")),
                 url + " Filmes\n" + QUrl::fromLocalFile(h.p(QStringLiteral("Vídeos"))).toEncoded() + "\n");
    }

    void undoLeavesLaterEditsAlone()
    {
        if (!havePortuguese())
            QSKIP("xdg-user-dirs pt_BR catalogue not installed");
        Home h;
        h.englishConfig();
        const QString bookmarks = h.ctx.configHome + QStringLiteral("/gtk-3.0/bookmarks");
        writeFile(bookmarks, QUrl::fromLocalFile(h.p(QStringLiteral("Music"))).toEncoded() + "\n");
        const XdgNames names(h.ctx);
        const Summary s = Executor(h.ctx).run(planLanguageChange(h.ctx, names, h.config(), QStringLiteral("pt_BR")));
        writeFile(bookmarks, "file:///somewhere/else\n");  // the user edits it afterwards
        QStringList messages;
        undo(h.ctx, s.journalDir, &messages);
        QCOMPARE(readFile(bookmarks), QByteArray("file:///somewhere/else\n"));
        QVERIFY(QFileInfo(h.p(QStringLiteral("Music"))).isDir());
    }

    void duplicates()
    {
        if (!havePortuguese())
            QSKIP("xdg-user-dirs pt_BR catalogue not installed");
        Home h;
        h.englishConfig();
        writeFile(h.p(QStringLiteral("Documentos/a.txt")), "pt");
        writeFile(h.p(QStringLiteral("Documents/a.txt")), "en");
        writeFile(h.p(QStringLiteral("Músicas/x.ogg")), "x");
        writeFile(h.p(QStringLiteral("Vídeos/v.mp4")), "v");
        QDir().mkpath(h.p(QStringLiteral("Dokumente")));  // German: listed, unticked
        const XdgNames names(h.ctx);

        const auto groups = findDuplicates(h.ctx, names, h.config());
        QStringList keys;
        for (const auto &g : groups)
            keys << g.key;
        QCOMPARE(keys, QStringList({QStringLiteral("DOCUMENTS"), QStringLiteral("MUSIC"), QStringLiteral("VIDEOS")}));

        // Into the configured (English) folders
        Plan plan = planDuplicates(h.ctx, names, h.config(), QStringLiteral("pt_BR"), MergeTarget::Configured);
        for (const PlanItem &i : plan.items) {
            QVERIFY(!i.setsConfig);
            QCOMPARE(i.enabled, !i.from.endsWith(QLatin1String("Dokumente")));
        }
        const Summary dry = Executor(h.ctx).run(plan, true);
        QVERIFY(dry.actions.contains(QStringLiteral("move \"%1\" -> \"%2\"").arg(h.p(QStringLiteral("Documentos/a.txt")), h.p(QStringLiteral("Documents/a (2).txt")))));

        // Into the localized ones: config follows
        plan = planDuplicates(h.ctx, names, h.config(), QStringLiteral("pt_BR"), MergeTarget::Localized);
        const Summary s = Executor(h.ctx).run(plan);
        QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QLatin1Char('\n'))));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/a.txt"))), QByteArray("pt"));
        QCOMPARE(readFile(h.p(QStringLiteral("Documentos/a (2).txt"))), QByteArray("en"));
        QVERIFY(QFileInfo(h.p(QStringLiteral("Dokumente"))).isDir());  // unticked: kept
        QCOMPARE(h.config().path(QStringLiteral("DOCUMENTS")), h.p(QStringLiteral("Documentos")));
        QCOMPARE(h.config().path(QStringLiteral("MUSIC")), h.p(QStringLiteral("Músicas")));
        QCOMPARE(h.config().path(QStringLiteral("VIDEOS")), h.p(QStringLiteral("Vídeos")));
        QCOMPARE(UserDirsConfig::readLocale(h.ctx.userDirsLocaleFile()), QStringLiteral("en_US"));  // not touched
    }

    // /dev/shm is a different tmpfs from $TMPDIR on this kind of system
    void crossDeviceMerge()
    {
        QTemporaryDir other(QStringLiteral("/dev/shm/lingmo-userdirs-test-XXXXXX"));
        Home h;
        if (!other.isValid() || Fs::sameDevice(other.path(), h.ctx.home))
            QSKIP("no second filesystem available");
        writeFile(other.path() + QStringLiteral("/src/big.bin"), QByteArray(3 * 1024 * 1024, 'x'));
        writeFile(other.path() + QStringLiteral("/src/clash.txt"), "from other fs");
        writeFile(other.path() + QStringLiteral("/src/sub/nested.txt"), "nested");
        QVERIFY(QFile::link(QStringLiteral("big.bin"), other.path() + QStringLiteral("/src/rel-link")));
        ::chmod(QFile::encodeName(other.path() + QStringLiteral("/src/clash.txt")).constData(), 0400);
        writeFile(h.p(QStringLiteral("dst/clash.txt")), "already here");

        Plan plan;
        plan.mode = Plan::Duplicates;
        plan.writeLocale = false;
        PlanItem item;
        item.key = QStringLiteral("TEST");
        item.from = other.path() + QStringLiteral("/src");
        item.to = h.p(QStringLiteral("dst"));
        item.action = Action::Merge;
        item.setsConfig = false;
        measure(item);
        QVERIFY(item.crossDevice);
        QVERIFY(item.bytes >= 3 * 1024 * 1024);
        plan.items << item;

        qint64 lastDone = -1, total = 0;
        const Summary s = Executor(h.ctx).run(plan, false, [&](const Progress &p) {
            QVERIFY(p.done >= lastDone);
            lastDone = p.done;
            total = p.total;
        });
        QVERIFY2(s.errors.isEmpty(), qPrintable(s.errors.join(QLatin1Char('\n'))));
        QCOMPARE(lastDone, total);
        QCOMPARE(readFile(h.p(QStringLiteral("dst/clash.txt"))), QByteArray("already here"));
        QCOMPARE(readFile(h.p(QStringLiteral("dst/clash (2).txt"))), QByteArray("from other fs"));
        QCOMPARE(QFileInfo(h.p(QStringLiteral("dst/clash (2).txt"))).permissions() & QFileDevice::WriteOwner, QFileDevice::Permissions());
        QCOMPARE(readFile(h.p(QStringLiteral("dst/sub/nested.txt"))), QByteArray("nested"));
        QCOMPARE(readFile(h.p(QStringLiteral("dst/big.bin"))).size(), 3 * 1024 * 1024);
        QCOMPARE(QFile::symLinkTarget(h.p(QStringLiteral("dst/rel-link"))), h.p(QStringLiteral("dst/big.bin")));
        QVERIFY(!QFileInfo::exists(other.path() + QStringLiteral("/src")));
        // No temporary copies left behind
        QVERIFY(QDir(h.p(QStringLiteral("dst"))).entryList({QStringLiteral(".lingmo-userdirs-*")}, QDir::AllEntries | QDir::Hidden).isEmpty());

        // And back across the filesystems
        QVERIFY(undo(h.ctx, s.journalDir, nullptr));
        QCOMPARE(readFile(other.path() + QStringLiteral("/src/clash.txt")), QByteArray("from other fs"));
        QCOMPARE(readFile(h.p(QStringLiteral("dst/clash.txt"))), QByteArray("already here"));
        QVERIFY(!QFileInfo::exists(h.p(QStringLiteral("dst/clash (2).txt"))));
    }
};

QTEST_GUILESS_MAIN(TestUserDirs)
#include "tst_userdirs.moc"
