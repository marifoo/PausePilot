#include "test_renames.h"
#include <QtTest>
#include <QDir>
#include <QFile>
#include <QStringList>

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
