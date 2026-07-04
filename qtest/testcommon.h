#ifndef TESTCOMMON_H
#define TESTCOMMON_H

#include <QtTest>
#include <deque>
#include <iterator>
#include <cmath>
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

} // namespace TestCommon

/// Normalizes the duration list in-place (merging adjacent same-type segments)
/// and returns the segment_id strings of any segments merged away.
/// Delegates to Timeline::normalizedWithRemovedIds().
/// Test-only helper; production code uses Timeline::normalizedWithRemovedIds() directly.
std::vector<QString> cleanDurations(std::deque<TimeDuration>* pDurations);

#endif // TESTCOMMON_H
