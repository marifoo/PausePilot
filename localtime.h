#ifndef LOCALTIME_H
#define LOCALTIME_H

#include <QDateTime>
#include <QTimeZone>
#include <optional>

/// The application's local calendar. Keep native Qt::LocalTime semantics in
/// production; debug fixtures can supply a named zone without changing the
/// desktop's timezone or relying on the platform-specific C runtime TZ syntax.
namespace LocalTime {
#ifndef QT_NO_DEBUG
inline std::optional<QTimeZone> zoneOverride_dbg;
#endif

inline QDateTime at(const QDate& date, const QTime& time)
{
#ifndef QT_NO_DEBUG
    if (zoneOverride_dbg)
        return QDateTime(date, time, *zoneOverride_dbg);
#endif
    return QDateTime(date, time, Qt::LocalTime);
}

inline QDateTime fromInstant(const QDateTime& instant)
{
#ifndef QT_NO_DEBUG
    if (zoneOverride_dbg)
        return instant.toTimeZone(*zoneOverride_dbg);
#endif
    return instant.toLocalTime();
}
} // namespace LocalTime

#endif // LOCALTIME_H
