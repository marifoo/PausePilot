#ifndef APPPATHS_H
#define APPPATHS_H

#include <QCoreApplication>
#include <QDir>
#include <QString>

/// Central source of truth for all file paths derived from the data directory.
/// Avoids scattering QCoreApplication::applicationDirPath() calls across the codebase.
///
/// Deviation (C1, R15-001): dataDirectory() honors a PAUSEPILOT_DATA_DIR
/// environment override so C1's subprocess tests can start the production
/// binary against an isolated temporary directory instead of its own
/// application directory. This is the minimum override C1 needs for those
/// tests; D13 owns making the rest of the test harness hermetic. The same
/// inline function is compiled into apptest and qtest, so an exported
/// PAUSEPILOT_DATA_DIR is honored by them too -- today the test binaries simply
/// do not set the override for themselves, so each still derives its own
/// data directory from applicationDirPath() by default. Two instances
/// started with different PAUSEPILOT_DATA_DIR values may run concurrently:
/// different data directories mean different lock files, so there is no
/// shared state to race on.
namespace AppPaths {

inline QString dataDirectory()
{
    const QString overridePath = qEnvironmentVariable("PAUSEPILOT_DATA_DIR");
    if (!overridePath.isEmpty())
        return QDir::fromNativeSeparators(overridePath);
    return QCoreApplication::applicationDirPath();
}

inline QString databaseFile()
{
    return QDir(dataDirectory()).filePath("PausePilot.sqlite");
}

inline QString logFile()
{
    return QDir(dataDirectory()).filePath("PausePilot.log");
}

inline QString settingsFile()
{
    return QDir(dataDirectory()).filePath("user-settings.ini");
}

/// Single-instance guard file (see QLockFile in main()).
inline QString instanceLockFile()
{
    return QDir(dataDirectory()).filePath("PausePilot.lock");
}

} // namespace AppPaths

#endif // APPPATHS_H
