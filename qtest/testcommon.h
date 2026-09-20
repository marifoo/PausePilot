#ifndef TESTCOMMON_H
#define TESTCOMMON_H

#include <QtTest>
#include <deque>
#include <iterator>
#include <cmath>
#include <vector>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtDebug>

#include "timetracker.h"
#include "lockstatewatcher.h"
#include "sqlitesessionstore.h"

#include "settings.h"
#include "timeline.h"

namespace TestCommon {

// Factory function for creating TimeDuration objects from millisecond timestamps
TimeDuration mk(DurationType type, qint64 startMs, qint64 endMs);

// Creates a test settings file with specified history days retention
QString createSettingsFile(const QString& dirPath, int historyDays);

// Sums all durations of a specific type from a deque
qint64 sumDurations(const std::deque<TimeDuration>& d, DurationType type);

/// D15 (F1): a stand-in for QDateTime::currentDateTime() in fixtures that
/// only need "some recent instant" to build a duration via addSecs() offsets.
/// Real wall-clock time crosses local midnight for roughly nine minutes a
/// day; a fixture built from it then intermittently produces a cross-day
/// interval, which Timeline's assertSameDayInvariant() (timeline.cpp) aborts
/// the whole qtest binary over. Local noon is at least 9.5 hours from
/// midnight in every real zone and never inside a DST transition, so it is
/// safe for offsets much larger than any fixture here uses. Deliberately
/// Qt::LocalTime, not Qt::UTC: a fixed UTC instant's local time-of-day still
/// depends on the zone the test runs in and can itself land near midnight.
/// The date component tracks QDate::currentDate() (only the time-of-day is
/// pinned) so it stays "today" for fixtures that also do day-relative
/// lookups (e.g. History's page-0-is-today grouping).
QDateTime fixedNow();

/// Same instant as fixedNow(), re-expressed with a Qt::UTC timeSpec, for
/// fixtures that specifically exercise UTC storage/round-trip behavior and
/// would otherwise call QDateTime::currentDateTimeUtc(). isCrossMidnight()
/// compares the underlying instant regardless of timeSpec, so this remains
/// exactly as midnight-safe as fixedNow() itself.
QDateTime fixedNowUtc();

/// D1 (R16-002/R08-004/R13-002/R04-004): one shared data table for day-
/// membership/overlap semantics. Consumed directly by the primitive
/// (test_dayvalidity.cpp, every row). The store's
/// test_hasEntriesForDate_exact_midnight_endpoint() (test_database.cpp) pulls
/// the two exact-midnight rows out of this table by name for its seed data.
/// The tracker (test_timer.cpp) and History (test_historydialog.cpp) layers
/// do not consume this table yet -- they pin day-membership behaviour with
/// their own hand-built scenarios instead.
///
/// All date/time fields are local wall-clock components under `tz` (the TZ
/// environment value the test must set -- via a TzGuard -- before
/// constructing any QDateTime from this row, since Qt::LocalTime depends on
/// the process's current zone). `expectedOverlap` is
/// DayValidity::overlapsLocalDate(start, end, queryDate); `expectedCrossDay`
/// is the free function isCrossMidnight(start, end) (types.h) i.e. whether
/// this exact interval would be rejected as DayValidity::Violation::CrossDay /
/// DurationCreateError::CrossMidnight at a write boundary.
struct DayOverlapRow {
    const char* name;
    const char* tz;
    int startY, startMo, startD, startH, startMi, startS;
    int endY,   endMo,   endD,   endH,   endMi,   endS;
    int queryY, queryMo, queryD;
    bool expectedOverlap;
    bool expectedCrossDay;
};

std::vector<DayOverlapRow> dayOverlapRows();

/// D13a: points AppPaths::dataDirectory() (via the UTIMER_DATA_DIR override
/// apppaths.h already honors for C1's subprocess tests) at dirPath, so a
/// suite's SqliteSessionStore/Logger calls use a private database file that
/// cannot collide with another suite's. Call from initTestCase() with a
/// QTemporaryDir member's path(); call clearDataDirOverride() from
/// cleanupTestCase().
void setDataDirOverride(const QString& dirPath);
void clearDataDirOverride();

} // namespace TestCommon

/// Normalizes the duration list in-place (merging adjacent same-type segments)
/// and returns the segment_id strings of any segments merged away.
/// Delegates to Timeline::normalizedWithRemovedIds().
/// Test-only helper; production code uses Timeline::normalizedWithRemovedIds() directly.
std::vector<QString> cleanDurations(std::deque<TimeDuration>* pDurations);

#endif // TESTCOMMON_H
