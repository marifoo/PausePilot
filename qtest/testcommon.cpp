#include "testcommon.h"
#include "timeline.h"

std::vector<QString> cleanDurations(std::deque<TimeDuration>* pDurations)
{
    Timeline t(*pDurations, std::nullopt);
    auto [normed, removed] = t.normalizedWithRemovedIds();
    *pDurations = normed.completed();
    return removed;
}

namespace TestCommon {

TimeDuration mk(DurationType type, qint64 startMs, qint64 endMs)
{
    QDateTime start = QDateTime::fromMSecsSinceEpoch(startMs, Qt::UTC);
    QDateTime end = QDateTime::fromMSecsSinceEpoch(endMs, Qt::UTC);
    return TimeDuration::fromPersistedRow(type, start, end);
}

QString createSettingsFile(const QString& dirPath, int historyDays)
{
    QString settingsPath = QDir(dirPath).filePath("user-settings.ini");
    QSettings seed(settingsPath, QSettings::IniFormat);
    seed.setValue("PausePilot/history_days_to_keep", historyDays);
    seed.setValue("PausePilot/debug_log_to_file", false);
    seed.sync();
    return settingsPath;
}

qint64 sumDurations(const std::deque<TimeDuration>& d, DurationType type)
{
    qint64 total = 0;
    for (const auto& t : d) {
        if (t.type == type) total += t.duration();
    }
    return total;
}

QDateTime fixedNow()
{
    // Today's real date, but pinned to local noon: History pages group by
    // real calendar date (e.g. "page 0 is today"), so the date component
    // must track QDate::currentDate() for fixtures that mix this with
    // day-relative lookups. Only the time-of-day needs pinning away from
    // midnight -- see the header comment for why.
    return LocalTime::at(LocalTime::fromInstant(QDateTime::currentDateTime()).date(), QTime(12, 0, 0));
}

QDateTime fixedNowUtc()
{
    return fixedNow().toUTC();
}

void setDataDirOverride(const QString& dirPath)
{
    qputenv("PAUSEPILOT_DATA_DIR", dirPath.toUtf8());
}

void clearDataDirOverride()
{
    qunsetenv("PAUSEPILOT_DATA_DIR");
}

void seedStoredRows(SqliteSessionStore& db, const std::deque<TimeDuration>& rows, bool finalized)
{
    QVERIFY(db.ensureOpen_dbg());
    QSqlQuery query(db.rawDb_dbg());
    query.prepare(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES (:segment_id, :type, :start_utc, :end_utc, :is_finalized)");
    for (const auto& d : rows) {
        query.bindValue(":segment_id", d.segment_id.toString());
        query.bindValue(":type", static_cast<int>(d.type));
        query.bindValue(":start_utc", d.startTime.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(":end_utc", d.endTime.toUTC().toString(Qt::ISODateWithMs));
        query.bindValue(":is_finalized", finalized ? 1 : 0);
        QVERIFY(query.exec());
    }
}

void seedStoredRows(FakeSessionStore& db, const std::deque<TimeDuration>& rows, bool finalized)
{
    if (finalized) {
        for (const auto& d : rows) db.loadDurationsResult.durations.push_back(d);
    } else {
        for (const auto& d : rows) db.historyUnfinalizedRows.push_back(d);
    }
}

std::vector<DayOverlapRow> dayOverlapRows()
{
    return {
        // name, tz,
        //   startY,Mo,D,H,Mi,S,   endY,Mo,D,H,Mi,S,   queryY,Mo,D,  expectedOverlap, expectedCrossDay
        {"same_day", "UTC",
         2025, 6, 15, 9, 0, 0,   2025, 6, 15, 17, 0, 0,   2025, 6, 15,  true, false},

        // A row spanning exactly one local midnight overlaps BOTH days it
        // touches -- including the query for the day it started on (D1's
        // "retained prior-day memory" case).
        {"single_midnight_overlaps_start_day", "UTC",
         2025, 6, 15, 23, 0, 0,   2025, 6, 16, 1, 0, 0,   2025, 6, 15,  true, true},
        {"single_midnight_overlaps_next_day", "UTC",
         2025, 6, 15, 23, 0, 0,   2025, 6, 16, 1, 0, 0,   2025, 6, 16,  true, true},

        // A row spanning more than one boundary (rejected the same way as a
        // single-midnight row, but reaches farther).
        {"multiple_midnights", "UTC",
         2025, 6, 14, 23, 0, 0,   2025, 6, 16, 1, 0, 0,   2025, 6, 15,  true, true},

        // Exact-midnight endpoint: half-open semantics mean the interval
        // contributes nothing to the day it merely touches at the boundary,
        // and is itself same-day (legal, not CrossDay) -- it never reaches
        // the following day's start.
        {"exact_midnight_endpoint_same_day", "UTC",
         2025, 6, 15, 22, 0, 0,   2025, 6, 16, 0, 0, 0,   2025, 6, 15,  true, false},
        {"exact_midnight_endpoint_no_spill_to_next_day", "UTC",
         2025, 6, 15, 22, 0, 0,   2025, 6, 16, 0, 0, 0,   2025, 6, 16,  false, false},

        // UTC/local conversion: local Asia/Tokyo midnight does not line up
        // with UTC midnight, so a naive UTC-date grouping would misfile this.
        {"tokyo_local_midnight_boundary_same_day", "Asia/Tokyo",
         2025, 1, 1, 0, 15, 0,   2025, 1, 1, 0, 45, 0,   2025, 1, 1,  true, false},
        {"tokyo_local_midnight_boundary_prev_day", "Asia/Tokyo",
         2025, 1, 1, 0, 15, 0,   2025, 1, 1, 0, 45, 0,   2024, 12, 31,  false, false},

        // Europe/Berlin DST spring-forward (2025-03-30, local day is 23h).
        {"berlin_spring_forward_same_day", "Europe/Berlin",
         2025, 3, 30, 1, 30, 0,   2025, 3, 30, 22, 0, 0,   2025, 3, 30,  true, false},
        {"berlin_spring_forward_crosses_into_next_day", "Europe/Berlin",
         2025, 3, 30, 23, 0, 0,   2025, 3, 31, 1, 0, 0,   2025, 3, 31,  true, true},

        // Europe/Berlin DST fall-back (2025-10-26, local day is 25h).
        {"berlin_fall_back_same_day", "Europe/Berlin",
         2025, 10, 26, 1, 0, 0,   2025, 10, 26, 23, 30, 0,   2025, 10, 26,  true, false},
        {"berlin_fall_back_prev_day_not_included", "Europe/Berlin",
         2025, 10, 26, 1, 0, 0,   2025, 10, 26, 23, 30, 0,   2025, 10, 25,  false, false},

        // America/Santiago spring-forward (2025-09-07): local clocks jump
        // from 23:59:59 (Sep 6) to 01:00:00 (Sep 7), so local midnight of
        // Sep 7 does not exist as a wall-clock instant. Qt normalizes the
        // nonexistent QDateTime(2025-09-07, 00:00, LocalTime) forward to the
        // first valid instant, 01:00 local -- see the localDayEnd 23-hour
        // assertion below. This row pins that a query for the gap day itself
        // still gets a correct overlap answer.
        {"santiago_spring_forward_gap_day_overlap", "America/Santiago",
         2025, 9, 7, 1, 30, 0,   2025, 9, 7, 10, 0, 0,   2025, 9, 7,  true, false},

        // America/Santiago, the day BEFORE the spring-forward gap: this row's
        // isCrossMidnight/overlapsLocalDate computation needs localDayEnd(D)
        // = localDayStart(D+1), and D+1 (09-07) is the gap day itself, so
        // this row is the one that exercises the invalid-midnight fallback
        // inside localDayStart (D1 round 2, MEDIUM-3).
        {"santiago_day_before_gap", "America/Santiago",
         2025, 9, 6, 9, 0, 0,   2025, 9, 6, 17, 0, 0,   2025, 9, 6,  true, false},

        // A non-DST zone (Asia/Kolkata never observes DST).
        {"kolkata_non_dst_same_day", "Asia/Kolkata",
         2025, 6, 15, 9, 0, 0,   2025, 6, 15, 17, 0, 0,   2025, 6, 15,  true, false},
        {"kolkata_non_dst_unrelated_day", "Asia/Kolkata",
         2025, 6, 15, 9, 0, 0,   2025, 6, 15, 17, 0, 0,   2025, 6, 16,  false, false},
    };
}

} // namespace TestCommon
