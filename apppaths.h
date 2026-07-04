#ifndef APPPATHS_H
#define APPPATHS_H

#include <QCoreApplication>
#include <QDir>
#include <QString>

/// Central source of truth for all file paths derived from the application directory.
/// Avoids scattering QCoreApplication::applicationDirPath() calls across the codebase.
namespace AppPaths {

inline QString dataDirectory()
{
    return QCoreApplication::applicationDirPath();
}

inline QString databaseFile()
{
    return QDir(dataDirectory()).filePath("uTimer.sqlite");
}

inline QString logFile()
{
    return QDir(dataDirectory()).filePath("uTimer.log");
}

inline QString settingsFile()
{
    return QDir(dataDirectory()).filePath("user-settings.ini");
}

/// Single-instance guard file (see QLockFile in main()).
inline QString instanceLockFile()
{
    return QDir(dataDirectory()).filePath("uTimer.lock");
}

} // namespace AppPaths

#endif // APPPATHS_H
