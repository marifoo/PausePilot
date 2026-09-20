#include "test_renames.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QRegularExpression>

/**
 * RenamesTest — checks that old class names are not accidentally reintroduced.
 *
 * Reads production headers and asserts the old class names are absent.
 * Catches accidental reintroduction during merges or cherry-picks.
 */
void RenamesTest::test_no_old_names_in_production_headers()
{
    const QString srcDir =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");

    // Production headers to check (excludes design docs and memory files)
    const QStringList headers = {
        "timetracker.h",
        "sessionstore.h",
        "sqlitesessionstore.h",
        "mainwin.h",
        "contentwidget.h",
        "historydialog.h",
        "shutdowncoordinator.h",
        "healthmonitor.h",
        "timeline.h",
        "settings.h",
        "logger.h",
        "types.h",
        "lockstatewatcher.h",
    };

    // Old names that must not appear in any production header
    const QStringList bannedNames = {
        "IDatabaseManager",
        "FakeDatabaseManager",
    };
    // Note: "DatabaseManager" alone would false-positive on "SqliteSessionStore" comment
    // history in sqlitesessionstore.cpp; we check headers only where it was a type name.
    const QStringList bannedNamesHeadersOnly = {
        "class DatabaseManager",
        "DatabaseManager(",
        "DatabaseManager&",
        "DatabaseManager*",
        "DatabaseManager ",
    };

    for (const QString& headerName : headers) {
        const QString path = QDir(srcDir).filePath(headerName);
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable("Cannot open header: " + path));
        const QString content = QString::fromUtf8(file.readAll());
        file.close();

        for (const QString& banned : bannedNames) {
            QVERIFY2(!content.contains(banned),
                     qPrintable(headerName + " still contains old name: " + banned));
        }
        for (const QString& banned : bannedNamesHeadersOnly) {
            QVERIFY2(!content.contains(banned),
                     qPrintable(headerName + " still contains old name: " + banned));
        }
    }
}

/**
 * D14 (R01, part 2 review): checks that identifiers/behaviors known to have
 * been removed or renamed from production do not resurface in the
 * documentation. The doc/ markdown files and README.md describe the current
 * code by identifier name; a doc rewrite that reintroduces one of these
 * stale names is exactly the drift D14 part 2 fixed, so it is pinned here
 * rather than relying on the next reviewer to notice by inspection.
 */
void RenamesTest::test_no_stale_identifiers_in_docs()
{
    const QString srcDir =
        QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("..");

    const QStringList docFiles = {
        "doc/architecture.md",
        "doc/persistence.md",
        "doc/runtime-behaviour.md",
        "doc/README.md",
        "README.md",
    };

    // Stale identifiers/behaviors removed or renamed from production that
    // must not reappear in the docs. "start_date"/"end_date" are checked as
    // whole-word patterns (not e.g. "..._date" ) since "date" alone appears
    // legitimately throughout the prose.
    const QStringList bannedTerms = {
        "recovered_seconds",
        "modeChanged",
        "PauseCause",
        "saveDurations",
        "checkpoint_interval_min_",
        "markUnsaved",
        "buffer_for_lock",
        "buffer_for_unlock",
        "WTSSESSION_CHANGE",
        "LinuxLockMethod",
        "sendButtons",
        "pressedButton",
        "MainWin::update",
        "force_direct",
        "finalizeIfNoOverlap(",
    };
    const QStringList bannedWholeWords = {
        "start_date",
        "end_date",
    };

    for (const QString& docName : docFiles) {
        const QString path = QDir(srcDir).filePath(docName);
        QFile file(path);
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text),
                 qPrintable("Cannot open doc: " + path));
        const QString content = QString::fromUtf8(file.readAll());
        file.close();

        for (const QString& banned : bannedTerms) {
            QVERIFY2(!content.contains(banned),
                     qPrintable(docName + " still contains stale identifier: " + banned));
        }
        for (const QString& banned : bannedWholeWords) {
            const QRegularExpression re("\\b" + QRegularExpression::escape(banned) + "\\b");
            QVERIFY2(!content.contains(re),
                     qPrintable(docName + " still contains stale identifier: " + banned));
        }
    }
}
