#include "test_historyeditsession.h"
#include <QtTest>
#include <algorithm>

#include "../historyeditsession.h"
#include "../timetracker.h"

#include "fakesessionstore.h"
#include "fakeclock.h"
#include "testcommon.h"

using TestCommon::createSettingsFile;
using TestCommon::mk;

namespace {

// Fixed civil anchor -- NOT the real "today". The old helpers paired the
// real local calendar date (QDate::currentDate()) with a Qt::UTC time
// spec: at a nonzero UTC offset that pairing names a different local day
// than the literal date field, which corrupted the local-day bucketing
// buildFromTimer() performs (historyeditsession.cpp:62,
// d.startTime.toLocalTime().date()) and, when the resulting rows shared a
// page, could feed a cross-midnight pair into Timeline's constructor
// (timeline.cpp assertSameDayInvariant(), a Q_ASSERT).
//
// todayUTC()/yesterdayUTC() below construct via Qt::LocalTime, not
// Qt::UTC, despite the name (kept to avoid renaming ~100 call sites) --
// deliberately so. A UTC-spec construction combined with an hour drawn
// from a fixed civil date still needs a UTC->local conversion to know
// which local day it lands on, and that conversion's boundary moves with
// the offset: the file uses hours 8-11 across its fixtures, and the
// gate's own zones place a local-midnight crossing inside that exact
// band (Kiritimati +14 and Honolulu -10 both cross at UTC 10:00,
// Marquesas -9:30 at UTC 09:30) -- so no single fixed UTC hour or
// UTC-derived "today" is safe for every test in this file. Building the
// instant directly as local wall-clock time sidesteps the conversion
// entirely: there is nothing to cross.
const QDate kFixedDate(2025, 6, 1);

QDateTime todayUTC(int hour, int minute = 0, int second = 0) {
    return QDateTime(kFixedDate, QTime(hour, minute, second), Qt::LocalTime);
}

QDateTime yesterdayUTC(int hour, int minute = 0, int second = 0) {
    return QDateTime(kFixedDate.addDays(-1), QTime(hour, minute, second), Qt::LocalTime);
}

// The fixed local calendar date every todayUTC()/yesterdayUTC() row is
// anchored to -- never the real clock.
QDate fixedToday() {
    return kFixedDate;
}

} // namespace

void HistoryEditSessionTest::initTestCase()
{
    // No DB setup required — tests use FakeSessionStore.
}

void HistoryEditSessionTest::cleanupTestCase()
{
}

void HistoryEditSessionTest::resetDatabaseFile() const
{
    // No-op: this suite uses FakeSessionStore, not SQLite.
}

// ---------------------------------------------------------------------------
// Page creation
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_build_today_page_only_from_memory()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QCOMPARE(session.pages().size(), static_cast<size_t>(1));
    QCOMPARE(session.pages()[0].isCurrent, true);
    QCOMPARE(session.pages()[0].pageDate, fixedToday());
    // pendingTimelines[0] has 1 completed row (the memory row)
    QCOMPARE(session.pendingTimelines().size(), static_cast<size_t>(1));
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QVERIFY(!session.pendingTimelines()[0].ongoing().has_value());
}

void HistoryEditSessionTest::test_build_today_page_memory_plus_db()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Seed a today DB row in the fake store.
    const QDateTime dbStart = todayUTC(8, 0);
    const QDateTime dbEnd   = todayUTC(8, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, dbStart, dbEnd));

    TimeTracker tracker(settings, fakeDb);

    // Memory row (different time, different segment_id)
    const QDateTime memStart = todayUTC(9, 0);
    const QDateTime memEnd   = todayUTC(9, 30);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Only 1 page: today.
    QCOMPARE(session.pages().size(), static_cast<size_t>(1));
    QCOMPARE(session.pages()[0].isCurrent, true);
    // 2 completed rows: memory row + db today row.
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
}

void HistoryEditSessionTest::test_build_historical_page_separate_from_today()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // A yesterday DB row.
    const QDateTime histStart = yesterdayUTC(10, 0);
    const QDateTime histEnd   = yesterdayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, histStart, histEnd));

    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // 2 pages: today (empty, isCurrent=true) + yesterday (1 entry, isCurrent=false).
    QCOMPARE(session.pages().size(), static_cast<size_t>(2));
    QCOMPARE(session.pages()[0].isCurrent, true);
    QCOMPARE(session.pages()[1].isCurrent, false);
    QCOMPARE(session.pages()[1].pageDate, fixedToday().addDays(-1));
    QCOMPARE(session.pendingTimelines()[1].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_build_deduplicates_db_row_matching_memory_segment_id()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const SegmentId sharedId = SegmentId::mint();
    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);

    // Memory row with sharedId
    TimeTracker tracker(settings, fakeDb);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, start, end, sharedId));

    // DB row with the same segment_id — should be deduped out.
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, start.addMSecs(-2), end, sharedId));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Only the memory row survives (DB duplicate is excluded).
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(session.pendingTimelines()[0].completed()[0].segment_id, sharedId);
}

// ---------------------------------------------------------------------------
// Origin routing
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_origin_memory_rows_marked_true()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const QString segId = session.pendingTimelines()[0].completed()[0].segment_id.toString();
    QVERIFY(session.originIsMemory().value(segId, false));
}

void HistoryEditSessionTest::test_origin_db_rows_marked_false()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime dbStart = yesterdayUTC(10, 0);
    const QDateTime dbEnd   = yesterdayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, dbStart, dbEnd));

    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const QString segId = session.pendingTimelines()[1].completed()[0].segment_id.toString();
    QCOMPARE(session.originIsMemory().value(segId, true), false);
}

// ---------------------------------------------------------------------------
// Split op
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_split_registers_child_with_same_origin()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 0, 8); // 8-second segment
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const QString origId = session.pendingTimelines()[0].completed()[0].segment_id.toString();
    QCOMPARE(session.originIsMemory().value(origId, false), true);

    // Split at 4 s
    const QDateTime splitAt = todayUTC(10, 0, 4);
    session.setPageTimeline(0, session.pendingTimelines()[0].withSplit(
        0, splitAt, DurationType::Activity, DurationType::Pause));

    // The second half gets a fresh segment_id; register it with the same origin.
    const auto& newComp = session.pendingTimelines()[0].completed();
    QCOMPARE(newComp.size(), static_cast<size_t>(2));
    const QString childId = newComp[1].segment_id.toString();
    session.registerSplitChild(childId, true /*wasMemory*/);

    QCOMPARE(session.originIsMemory().value(childId, false), true);
}

void HistoryEditSessionTest::test_split_history_row_child_is_false_origin()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    FakeSessionStore fakeDb;

    const QDateTime histStart = yesterdayUTC(10, 0);
    const QDateTime histEnd   = yesterdayUTC(10, 0, 8);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, histStart, histEnd));

    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QCOMPARE(session.pages().size(), static_cast<size_t>(2));

    const QString origId = session.pendingTimelines()[1].completed()[0].segment_id.toString();
    QCOMPARE(session.originIsMemory().value(origId, true), false);

    const QDateTime splitAt = yesterdayUTC(10, 0, 4);
    session.setPageTimeline(1, session.pendingTimelines()[1].withSplit(
        0, splitAt, DurationType::Activity, DurationType::Pause));

    const auto& newComp = session.pendingTimelines()[1].completed();
    QCOMPARE(newComp.size(), static_cast<size_t>(2));
    const QString childId = newComp[1].segment_id.toString();
    session.registerSplitChild(childId, false /*wasMemory*/);

    QCOMPARE(session.originIsMemory().value(childId, true), false);
}

// ---------------------------------------------------------------------------
// Type toggle
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_type_toggle_updates_pending_timeline()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QCOMPARE(session.pendingTimelines()[0].completed()[0].type, DurationType::Activity);

    session.setPageTimeline(0, session.pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    QCOMPARE(session.pendingTimelines()[0].completed()[0].type, DurationType::Pause);
}

// ---------------------------------------------------------------------------
// Merge detection
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_no_merge_needed_for_non_overlapping()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime dbStart = todayUTC(8, 0);
    const QDateTime dbEnd   = todayUTC(8, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, dbStart, dbEnd));

    TimeTracker tracker(settings, fakeDb);

    const QDateTime memStart = todayUTC(9, 0);
    const QDateTime memEnd   = todayUTC(9, 30);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto payload = session.buildSavePayload();
    QVERIFY(!payload.needsMergeConfirmation);
}

void HistoryEditSessionTest::test_merge_needed_for_overlapping_rows()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // DB row 10:00–10:30, memory row 10:15–10:45 → overlap
    const QDateTime dbStart = todayUTC(10, 0);
    const QDateTime dbEnd   = todayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, dbStart, dbEnd));

    TimeTracker tracker(settings, fakeDb);

    const QDateTime memStart = todayUTC(10, 15);
    const QDateTime memEnd   = todayUTC(10, 45);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto payload = session.buildSavePayload();
    QVERIFY(payload.needsMergeConfirmation);
}

// ---------------------------------------------------------------------------
// Cross-midnight preservation
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_cross_midnight_dropped_from_payload()
{
    // MUST-FIX1 (user decision, review packet C3): a legacy single-boundary
    // cross-midnight row is display-only (still shown on both pages -- see
    // HistoryDialog's H2/H14 tests) but must be silently DROPPED from the
    // save payload, not preserved and not rejected. An unrelated edit's save
    // must still succeed (violation == None) with the cross-midnight row's
    // segment_id simply absent from historyTimeline, while an ordinary
    // same-day row saved alongside it survives untouched.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Cross-midnight: 23:50 yesterday → 00:10 today (same-day invariant bypass)
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime cmStart(yesterday, QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime cmEnd(fixedToday(), QTime(0, 10, 0), Qt::LocalTime);
    const SegmentId cmId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, cmStart, cmEnd, cmId));

    // An ordinary same-day row on a different historical day, so the save
    // is non-empty and clearly unrelated to the cross-midnight row.
    const QDateTime ordinaryStart(yesterday.addDays(-1), QTime(9, 0, 0), Qt::LocalTime);
    const QDateTime ordinaryEnd(yesterday.addDays(-1), QTime(9, 30, 0), Qt::LocalTime);
    const SegmentId ordinaryId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, ordinaryStart, ordinaryEnd, ordinaryId));

    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto payload = session.buildSavePayload();

    // The save is not rejected: dropping the legacy row must not fail it.
    QCOMPARE(payload.violation, DayValidity::Violation::None);

    // The cross-midnight row's id is absent from the payload entirely...
    const auto& hist = payload.historyTimeline.completed();
    const bool cmStillPresent = std::any_of(hist.begin(), hist.end(),
        [&cmId](const TimeDuration& d) { return d.segment_id == cmId; });
    QVERIFY(!cmStillPresent);

    // ...while the unrelated ordinary row survives untouched.
    const bool ordinaryPresent = std::any_of(hist.begin(), hist.end(),
        [&ordinaryId](const TimeDuration& d) { return d.segment_id == ordinaryId; });
    QVERIFY(ordinaryPresent);
}

// ---------------------------------------------------------------------------
// Ongoing refresh / clear behaviour
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_refresh_ongoing_updates_end_time()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Start the timer so there is an ongoing segment.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(50);

    HistoryEditSession session;
    session.buildFromTimer(tracker, QDate::currentDate());

    QVERIFY(session.pendingTimelines()[0].ongoing().has_value());
    const QDateTime snapshotEnd = session.pendingTimelines()[0].ongoing()->endTime;

    // Wait so the engine's ongoing end-time advances.
    QTest::qWait(100);

    // refreshOngoing() should update the end-time to the newer value.
    session.refreshOngoing(tracker);

    const QDateTime refreshedEnd = session.pendingTimelines()[0].ongoing()->endTime;
    QVERIFY2(refreshedEnd > snapshotEnd,
             "refreshed end-time must be strictly later than snapshot end-time");

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryEditSessionTest::test_refresh_ongoing_advances_end_time_and_preserves_user_type_edit()
{
    // After the user edits the ongoing type (e.g. Activity → Pause), refreshOngoing()
    // must advance the end time from the engine while keeping the user's type edit.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(30);

    HistoryEditSession session;
    session.buildFromTimer(tracker, QDate::currentDate());

    QVERIFY(session.pendingTimelines()[0].ongoing().has_value());
    QCOMPARE(session.pendingTimelines()[0].ongoing()->type, DurationType::Activity);
    const QDateTime snapshotEnd = session.pendingTimelines()[0].ongoing()->endTime;

    // Simulate the user toggling the ongoing type to Pause.
    QVERIFY(session.setOngoingType(0, DurationType::Pause));

    QTest::qWait(100);

    const QDateTime before = QDateTime::currentDateTime();
    session.refreshOngoing(tracker);
    const QDateTime after = QDateTime::currentDateTime();

    // Type edit must be preserved.
    QCOMPARE(session.pendingTimelines()[0].ongoing()->type, DurationType::Pause);
    // End time must have advanced beyond the dialog-open snapshot.
    QVERIFY2(session.pendingTimelines()[0].ongoing()->endTime > snapshotEnd,
             "end-time must advance past the dialog-open snapshot");
    // End time must lie within [before, after] (wall-clock window around refresh).
    QVERIFY2(session.pendingTimelines()[0].ongoing()->endTime >= before,
             "end-time must not be earlier than the refresh call");
    QVERIFY2(session.pendingTimelines()[0].ongoing()->endTime <= after,
             "end-time must not be later than the refresh call");

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryEditSessionTest::test_refresh_ongoing_clears_when_engine_has_no_ongoing()
{
    // If the engine no longer has an ongoing segment (e.g. engine stopped before
    // save), refreshOngoing() must clear the session's ongoing to avoid anchoring
    // a stale start time.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    HistoryEditSession session;
    session.buildFromTimer(tracker, QDate::currentDate());

    QVERIFY(session.pendingTimelines()[0].ongoing().has_value());

    // Stop the engine (simulates a deferred midnight stop or similar).
    tracker.useTimerViaButton(Button::Stop);

    // refreshOngoing() must clear the ongoing in the session.
    session.refreshOngoing(tracker);

    QVERIFY(!session.pendingTimelines()[0].ongoing().has_value());
}

// ---------------------------------------------------------------------------
// Ongoing type toggle + refresh: end time advances, type preserved in payload
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_toggle_activity_to_pause_payload_end_utc_advances()
{
    // Arrange: running Activity session.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Deterministic clock (D15/F1): buildSavePayload() below places the
    // ongoing segment into a Timeline whose constructor asserts same-day, so
    // a real-clock start/refresh pair straddling local midnight would abort
    // the whole qtest binary rather than fail this one test. See
    // test_backward_clock_jump_keeps_engine_running() above for the
    // declared-before-tracker ordering rationale.
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(12, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(30);

    HistoryEditSession session;
    session.buildFromTimer(tracker, clock.now().date());

    QVERIFY(session.pendingTimelines()[0].ongoing().has_value());
    QCOMPARE(session.pendingTimelines()[0].ongoing()->type, DurationType::Activity);
    const QDateTime dialogOpenEnd = session.pendingTimelines()[0].ongoing()->endTime;

    // Act: user toggles ongoing type to Pause.
    QVERIFY(session.setOngoingType(0, DurationType::Pause));

    // Advance clock so the engine snapshot is later than dialogOpenEnd.
    clock.advance(100);

    const QDateTime before = clock.now();
    session.refreshOngoing(tracker);
    const QDateTime after = clock.now();

    const auto payload = session.buildSavePayload();

    // The ongoing is placed in memoryTimeline.ongoing() and also as the last
    // element of sessionTimeline.completed() (for DB persistence as unfinalized).
    QVERIFY(payload.memoryTimeline.ongoing().has_value());
    // Type in payload must be Pause (user's edit preserved).
    QCOMPARE(payload.memoryTimeline.ongoing()->type, DurationType::Pause);

    // end_utc must lie in [before, after] — definitely later than dialog-open snapshot.
    const QDateTime payloadEnd = payload.memoryTimeline.ongoing()->endTime;
    QVERIFY2(payloadEnd > dialogOpenEnd,
             "payload end_utc must be later than the dialog-open snapshot");
    QVERIFY2(payloadEnd >= before, "payload end_utc must not precede the refresh call");
    QVERIFY2(payloadEnd <= after,  "payload end_utc must not exceed the refresh call");

    // sessionTimeline.completed() must also contain the ongoing with the Pause type.
    const auto& sessionComp = payload.sessionTimeline.completed();
    QVERIFY(!sessionComp.empty());
    QCOMPARE(sessionComp.back().type, DurationType::Pause);
    QVERIFY2(sessionComp.back().endTime > dialogOpenEnd,
             "session bucket end_utc must be later than the dialog-open snapshot");

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryEditSessionTest::test_toggle_pause_to_activity_payload_end_utc_advances()
{
    // Arrange: running Pause session (timer paused after Start).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Deterministic clock (D15/F1): see
    // test_toggle_activity_to_pause_payload_end_utc_advances() above for why
    // buildSavePayload()'s ongoing-segment Timeline makes a real clock here
    // unsafe near local midnight.
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(12, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);
    tracker.useTimerViaButton(Button::Pause);
    clock.advance(20);

    HistoryEditSession session;
    session.buildFromTimer(tracker, clock.now().date());

    QVERIFY(session.pendingTimelines()[0].ongoing().has_value());
    QCOMPARE(session.pendingTimelines()[0].ongoing()->type, DurationType::Pause);
    const QDateTime dialogOpenEnd = session.pendingTimelines()[0].ongoing()->endTime;

    // Act: user toggles ongoing type back to Activity.
    QVERIFY(session.setOngoingType(0, DurationType::Activity));

    clock.advance(100);

    const QDateTime before = clock.now();
    session.refreshOngoing(tracker);
    const QDateTime after = clock.now();

    const auto payload = session.buildSavePayload();

    // Type must be Activity (user's edit preserved).
    QVERIFY(payload.memoryTimeline.ongoing().has_value());
    QCOMPARE(payload.memoryTimeline.ongoing()->type, DurationType::Activity);

    // end_utc must lie in [before, after].
    const QDateTime payloadEnd = payload.memoryTimeline.ongoing()->endTime;
    QVERIFY2(payloadEnd > dialogOpenEnd,
             "payload end_utc must be later than the dialog-open snapshot");
    QVERIFY2(payloadEnd >= before, "payload end_utc must not precede the refresh call");
    QVERIFY2(payloadEnd <= after,  "payload end_utc must not exceed the refresh call");

    // sessionTimeline.completed() must also contain the ongoing with the Activity type.
    const auto& sessionComp = payload.sessionTimeline.completed();
    QVERIFY(!sessionComp.empty());
    QCOMPARE(sessionComp.back().type, DurationType::Activity);
    QVERIFY2(sessionComp.back().endTime > dialogOpenEnd,
             "session bucket end_utc must be later than the dialog-open snapshot");

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// Payload correctness
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_payload_history_vs_session_buckets()
{
    // A9: completed rows are finalized on save regardless of origin.
    // DB row (yesterday) and memory row (today) both go to historyTimeline
    // (finalized). The session bucket stays empty when there is no ongoing.
    // The memory row also appears in memoryTimeline (for the live-session commit).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime histStart = yesterdayUTC(10, 0);
    const QDateTime histEnd   = yesterdayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, histStart, histEnd));

    TimeTracker tracker(settings, fakeDb);

    const QDateTime memStart = todayUTC(9, 0);
    const QDateTime memEnd   = todayUTC(9, 30);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto payload = session.buildSavePayload();

    // Both completed rows are finalized; no unfinalized session rows (no ongoing).
    QCOMPARE(payload.historyTimeline.completed().size(), static_cast<size_t>(2));
    QCOMPARE(payload.sessionTimeline.completed().size(), static_cast<size_t>(0));
    // The memory-origin row is still committed into the live session.
    QCOMPARE(payload.memoryTimeline.completed().size(), static_cast<size_t>(1));
    QCOMPARE(payload.memoryTimeline.completed()[0].startTime.time(), QTime(9, 0, 0));
    QVERIFY(!payload.needsMergeConfirmation);
}

void HistoryEditSessionTest::test_payload_memory_timeline_for_commit()
{
    // memoryTimeline must contain only the memory-origin rows (+ ongoing if present),
    // ready for TimeTracker::commitEditedTimeline().
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // DB row today: should end up in historyTimeline, NOT in memoryTimeline.
    const QDateTime dbStart = todayUTC(8, 0);
    const QDateTime dbEnd   = todayUTC(8, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, dbStart, dbEnd));

    TimeTracker tracker(settings, fakeDb);

    // Memory row today.
    const QDateTime memStart = todayUTC(9, 0);
    const QDateTime memEnd   = todayUTC(9, 30);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto payload = session.buildSavePayload();

    // memoryTimeline has only the memory row (for the live-session commit).
    QCOMPARE(payload.memoryTimeline.completed().size(), static_cast<size_t>(1));
    QCOMPARE(payload.memoryTimeline.completed()[0].startTime.time(), QTime(9, 0, 0));
    // A9: both completed rows (DB + memory) are finalized in the history bucket.
    QCOMPARE(payload.historyTimeline.completed().size(), static_cast<size_t>(2));
}

void HistoryEditSessionTest::test_payload_memory_absorbs_merged_memory_row()
{
    // When a DB row and a memory row overlap and are merged during normalization,
    // the merged row is finalized in the history bucket (A9: completed rows are
    // finalized on save) while still being committed into the live session via
    // memoryTimeline because it absorbed current-session time.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // DB Activity row 10:00–10:30 (today)
    const QDateTime dbStart = todayUTC(10, 0);
    const QDateTime dbEnd   = todayUTC(10, 30);
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, dbStart, dbEnd));

    TimeTracker tracker(settings, fakeDb);

    // Memory Activity row 10:15–10:45 (overlaps DB row)
    const QDateTime memStart = todayUTC(10, 15);
    const QDateTime memEnd   = todayUTC(10, 45);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto payload = session.buildSavePayload();

    // After normalization: exactly 1 merged row covering 10:00–10:45.
    QVERIFY(payload.needsMergeConfirmation);
    // The merged completed row is finalized in the history bucket.
    QCOMPARE(payload.historyTimeline.completed().size(), static_cast<size_t>(1));
    QCOMPARE(payload.historyTimeline.completed()[0].startTime.time(), QTime(10, 0, 0));
    QCOMPARE(payload.historyTimeline.completed()[0].endTime.time(), QTime(10, 45, 0));
    // No unfinalized session rows (no ongoing).
    QCOMPARE(payload.sessionTimeline.completed().size(), static_cast<size_t>(0));
    // Memory timeline still has the merged row (absorbed current-session time).
    QCOMPARE(payload.memoryTimeline.completed().size(), static_cast<size_t>(1));
}

// ---------------------------------------------------------------------------
// A8: backward clock jump must not silently stop the engine on save
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_backward_clock_jump_keeps_engine_running()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Deterministic clock; start a running Activity session at 12:00.
    // Declared before the TimeTracker that captures it by reference: the
    // now-provider lambda holds &clock, and locals destruct in reverse
    // declaration order, so a clock declared after tracker would be
    // destroyed first and ~TimeTracker()'s drain would read freed memory.
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(12, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(30000); // 12:00:30 — ongoing has a positive length at dialog-open

    HistoryEditSession session;
    session.buildFromTimer(tracker, QDate(2025, 6, 1));
    QVERIFY(session.pendingTimelines()[0].ongoing().has_value());

    // The wall clock jumps backward 2 minutes (NTP/DST): now < segment_start.
    clock.advance(-120000); // 11:58:30

    QSignalSpy stoppedSpy(&tracker, &TimeTracker::stopped);

    // Drive the dialog-OK flow at the engine/session level (the editless no-op
    // gate is A10's concern; here there is an in-flight save through commit).
    session.refreshOngoing(tracker);
    const auto payload = session.buildSavePayload();

    // The pending ongoing must NOT be cleared by the backward jump.
    QVERIFY(payload.memoryTimeline.ongoing().has_value());

    tracker.commitEditedTimeline(payload.memoryTimeline);

    // The engine must still be running — no stopped(EditApplied).
    QVERIFY(!tracker.isStopped());
    QCOMPARE(stoppedSpy.count(), 0);

    // Cleanup: restore the clock ahead of the segment start before stopping.
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(12, 5, 0), Qt::LocalTime));
    tracker.useTimerViaButton(Button::Stop);
}

void HistoryEditSessionTest::test_C3_payload_backward_clock_jump_not_nonpositive()
{
    // Finding 1 (C3 review, High): the same backward-clock-jump arrangement
    // as test_backward_clock_jump_keeps_engine_running(), but asserting the
    // payload classification directly. A8 deliberately exposes a
    // zero-length ongoing segment (start == end) so a History save
    // re-anchors the live segment; that zero-length row must NOT enter
    // buildSavePayload()'s validation set as an unfinalized DB row (it has
    // nothing to persist), or every History save is blocked with
    // NonPositive until the wall clock catches up.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    // Declared before the TimeTracker for the same reason as
    // test_backward_clock_jump_keeps_engine_running() above.
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(12, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(30000); // 12:00:30

    HistoryEditSession session;
    session.buildFromTimer(tracker, QDate(2025, 6, 1));

    clock.advance(-120000); // 11:58:30 — backward step past segment start.

    // Act
    session.refreshOngoing(tracker);
    const auto payload = session.buildSavePayload();

    // Assert: the zero-length ongoing row must not block the save.
    QCOMPARE(payload.violation, DayValidity::Violation::None);
    QVERIFY(payload.memoryTimeline.ongoing().has_value());
    QCOMPARE(payload.sessionTimeline.completed().size(), static_cast<size_t>(0));

    // Cleanup.
    clock.setNow(QDateTime(QDate(2025, 6, 1), QTime(12, 5, 0), Qt::LocalTime));
    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// F1: a failed history load must be remembered so saving can be refused
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_F1_historyLoadFailed_true_on_transient_error()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.loadDurationsResult.status = SessionStoreResult::TransientError;
    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QVERIFY(session.historyLoadFailed());
}

void HistoryEditSessionTest::test_F1_historyLoadFailed_false_when_disabled()
{
    // Disabled history storage is a legitimate empty state, not a failure.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.loadDurationsResult.status = SessionStoreResult::Disabled;
    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QVERIFY(!session.historyLoadFailed());
}

// ---------------------------------------------------------------------------
// C3 (R13-001/R14-001): buildSavePayload() classification of unsupported rows
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_C3_payload_violation_none_for_ordinary_edit()
{
    // Arrange: an ordinary same-day, non-overlapping DB row plus a memory row.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, todayUTC(8, 0), todayUTC(8, 30)));

    TimeTracker tracker(settings, fakeDb);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, todayUTC(9, 0), todayUTC(9, 30)));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Act
    const auto payload = session.buildSavePayload();

    // Assert
    QCOMPARE(payload.violation, DayValidity::Violation::None);
}

void HistoryEditSessionTest::test_C3_payload_drops_pre_existing_cross_type_overlap()
{
    // MUST-FIX2 (user decision, review packet C3): two genuinely persisted
    // DB rows (an Activity row and a Pause row) that overlap in time, with
    // NEITHER touched by any split/type-toggle/ongoing-conversion this
    // session, must not fail an unrelated save. buildSavePayload() drops the
    // later-starting row (Pause, 10:15) and keeps the earlier one (Activity,
    // 10:00) instead of flagging a violation -- exactly like other
    // out-of-contract data loaded unmodified from the database. Finding 3
    // (C3 review) restricts this drop rule to non-memory-origin rows only,
    // so both rows here are loaded from the DB, not the live session -- see
    // test_C3_payload_flags_memory_origin_overlap_instead_of_dropping for the
    // memory-origin case, which must NOT be silently dropped.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const SegmentId activityId = SegmentId::mint();
    const SegmentId pauseId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, todayUTC(10, 0), todayUTC(10, 30), activityId));
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, todayUTC(10, 15), todayUTC(10, 45), pauseId));

    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Act
    const auto payload = session.buildSavePayload();

    // Assert: no violation, the save succeeds.
    QCOMPARE(payload.violation, DayValidity::Violation::None);
    const auto& hist = payload.historyTimeline.completed();
    const bool activitySurvives = std::any_of(hist.begin(), hist.end(),
        [&activityId](const TimeDuration& d) { return d.segment_id == activityId; });
    const bool pauseSurvives = std::any_of(hist.begin(), hist.end(),
        [&pauseId](const TimeDuration& d) { return d.segment_id == pauseId; });
    QVERIFY(activitySurvives);
    QVERIFY(!pauseSurvives);
    // Finding 3: since neither row is memory-origin, memoryTimeline must be
    // completely unaffected by the drop.
    QCOMPARE(payload.memoryTimeline.completed().size(), static_cast<size_t>(0));
    QVERIFY(!payload.memoryTimeline.ongoing().has_value());
}

void HistoryEditSessionTest::test_C3_payload_flags_memory_origin_overlap_instead_of_dropping()
{
    // Finding 3 (C3 review, High): the pre-existing-overlap drop rule must
    // never silently delete a memory-origin (never-persisted current-session)
    // row, even if it is otherwise "unedited" this session. A DB Activity row
    // and a memory-origin Pause row overlap here; buildSavePayload() must
    // leave this as a flagged TypeOverlap violation rather than dropping the
    // memory row the way it would drop a second DB row.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const SegmentId activityId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, todayUTC(10, 0), todayUTC(10, 30), activityId));

    TimeTracker tracker(settings, fakeDb);
    const SegmentId pauseId = SegmentId::mint();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Pause, todayUTC(10, 15), todayUTC(10, 45), pauseId));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Act
    const auto payload = session.buildSavePayload();

    // Assert: flagged, not silently dropped -- the memory row must survive
    // untouched anywhere this save is refused.
    QCOMPARE(payload.violation, DayValidity::Violation::TypeOverlap);
}

void HistoryEditSessionTest::test_C3_payload_drops_pre_existing_overlap_equal_start_tie_break()
{
    // MUST-FIX2 (user decision, review packet C3): on equal starts the
    // lexicographically smaller SegmentId survives -- the same tie-break
    // convention plan section 2 uses for same-type merges, reused here for
    // one uniform rule.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime start = todayUTC(10, 0);
    const SegmentId smallerId = SegmentId::fromString("aaaa-lower-id");
    const SegmentId largerId  = SegmentId::fromString("zzzz-higher-id");

    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, todayUTC(10, 30), largerId));
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, start, todayUTC(10, 20), smallerId));

    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Act
    const auto payload = session.buildSavePayload();

    // Assert
    QCOMPARE(payload.violation, DayValidity::Violation::None);
    const auto& hist = payload.historyTimeline.completed();
    const bool smallerSurvives = std::any_of(hist.begin(), hist.end(),
        [&smallerId](const TimeDuration& d) { return d.segment_id == smallerId; });
    const bool largerSurvives = std::any_of(hist.begin(), hist.end(),
        [&largerId](const TimeDuration& d) { return d.segment_id == largerId; });
    QVERIFY(smallerSurvives);
    QVERIFY(!largerSurvives);
}

void HistoryEditSessionTest::test_C3_payload_flags_overlap_created_by_edit()
{
    // The pre-existing-overlap drop rule must not blur into edit-time
    // prevention (D9/D7 step 4's job, not this method's): when this
    // session's own edit is what created the overlap, it is still a
    // violation rather than being silently dropped.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const SegmentId activityId = SegmentId::mint();
    const SegmentId pauseId = SegmentId::mint();
    // Originally non-overlapping.
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, todayUTC(10, 0), todayUTC(10, 30), activityId));
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, todayUTC(10, 45), todayUTC(11, 15), pauseId));

    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Act: edit the Pause row's start so it now overlaps the untouched
    // Activity row (same id, so it is still the "same" row, just moved).
    std::deque<TimeDuration> edited{
        TimeDuration::fromPersistedRow(DurationType::Activity, todayUTC(10, 0), todayUTC(10, 30), activityId),
        TimeDuration::fromPersistedRow(DurationType::Pause, todayUTC(10, 15), todayUTC(10, 45), pauseId)
    };
    session.setPageTimeline(0, Timeline(edited, std::nullopt));

    const auto payload = session.buildSavePayload();

    // Assert: still flagged, not dropped.
    QCOMPARE(payload.violation, DayValidity::Violation::TypeOverlap);
}

void HistoryEditSessionTest::test_C3_payload_drops_completed_row_conflicting_with_unedited_ongoing()
{
    // Finding 2 (C3 review, High): a pre-existing overlap between a genuinely
    // persisted, unedited DB row and the live, unedited ongoing segment must
    // be resolvable by the same drop rule as two completed rows -- otherwise
    // there is no route to save except Cancel. The ongoing row is NEVER the
    // one dropped; the completed row loses instead.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const SegmentId pauseId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, todayUTC(9, 0), todayUTC(11, 0), pauseId));

    FakeClock clock(QDateTime(kFixedDate, QTime(10, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60000); // 10:01 — ongoing Activity overlaps the Pause row above.

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());
    const SegmentId ongoingId = session.pendingTimelines()[0].ongoing()->segment_id;

    // Act: zero edits.
    session.refreshOngoing(tracker);
    const auto payload = session.buildSavePayload();

    // Assert: no violation; the conflicting Pause row was dropped, the
    // ongoing Activity segment survives untouched.
    QCOMPARE(payload.violation, DayValidity::Violation::None);
    const auto& hist = payload.historyTimeline.completed();
    const bool pauseSurvives = std::any_of(hist.begin(), hist.end(),
        [&pauseId](const TimeDuration& d) { return d.segment_id == pauseId; });
    QVERIFY(!pauseSurvives);
    QVERIFY(payload.memoryTimeline.ongoing().has_value());
    QCOMPARE(payload.memoryTimeline.ongoing()->segment_id, ongoingId);

    // Cleanup.
    tracker.useTimerViaButton(Button::Stop);
}

// D2/MEDIUM-C: buildFromTimer() must use the explicit `today` parameter,
// not the real system date, to decide page grouping/editability. Pin a
// date deliberately far from the real system date and assert it is
// classified as the CURRENT (memory-backed, editable) page -- a regression
// back to QDate::currentDate() would fail this on any day that isn't
// 2019-03-15.
void HistoryEditSessionTest::test_D2_buildFromTimer_uses_explicit_today_not_real_date()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDate pinnedDate(2019, 3, 15);
    const QDateTime start(pinnedDate, QTime(10, 0), Qt::UTC);
    const QDateTime end(pinnedDate, QTime(10, 30), Qt::UTC);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;

    // Act: build using the pinned date as "today".
    session.buildFromTimer(tracker, pinnedDate);

    // Assert: the pinned date's page is the CURRENT page -- only true if
    // buildFromTimer() actually used `today` rather than the real date.
    QVERIFY(!session.pages().empty());
    QCOMPARE(session.pages()[0].pageDate, pinnedDate);
    QCOMPARE(session.pages()[0].isCurrent, true);
}

// D2 test list ("History rejection"): a user edit that produces a reversed
// interval (end before start) must be flagged by buildSavePayload(), never
// silently written via replaceAll() -- the History boundary's own guard
// (DayValidity::validatePayload(), via HistoryEditSession) is what D2's
// "reject at Timeline, History, and SessionStore boundaries" step already
// relies on for this case; SessionStore's own final guard is covered
// separately by C3 (test_C3_saveCheckpoint_rejects_nonpositive_interval
// etc. in test_database.cpp).
void HistoryEditSessionTest::test_D2_payload_flags_reversed_interval_created_by_edit()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const SegmentId activityId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, todayUTC(10, 0), todayUTC(10, 30), activityId));

    TimeTracker tracker(settings, fakeDb);
    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Act: edit the row to a reversed interval (end before start).
    std::deque<TimeDuration> edited{
        TimeDuration::fromPersistedRow(DurationType::Activity, todayUTC(10, 30), todayUTC(10, 0), activityId)
    };
    session.setPageTimeline(0, Timeline(edited, std::nullopt));

    const auto payload = session.buildSavePayload();

    // Assert: rejected, not silently written.
    QCOMPARE(payload.violation, DayValidity::Violation::NonPositive);
}

// ---------------------------------------------------------------------------
// D9 (R14-002): split/type-change/ongoing-toggle operations, dynamic display
// count, and per-page dirty state are owned by HistoryEditSession.
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_D9_displayCount_reflects_split_immediately()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 0, 8);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QCOMPARE(session.displayCount(0), 1);

    const auto outcome = session.splitRow(0, 0, todayUTC(10, 0, 4), DurationType::Activity, DurationType::Pause);
    QVERIFY(outcome.applied);

    // The count reflects the split immediately -- no frozen build-time title.
    QCOMPARE(session.displayCount(0), 2);
}

void HistoryEditSessionTest::test_D9_pageTitle_reflects_dynamic_count()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 0, 8);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QVERIFY(session.pageTitle(0).contains("entries: 1"));

    session.splitRow(0, 0, todayUTC(10, 0, 4), DurationType::Activity, DurationType::Pause);

    QVERIFY(session.pageTitle(0).contains("entries: 2"));
}

void HistoryEditSessionTest::test_D9_splitRow_registers_origin_and_returns_ids()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 0, 8);
    const SegmentId origId = SegmentId::mint();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end, origId));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto outcome = session.splitRow(0, 0, todayUTC(10, 0, 4), DurationType::Activity, DurationType::Pause);

    QVERIFY(outcome.applied);
    QCOMPARE(outcome.firstId, origId);
    QVERIFY(!outcome.secondId.isEmpty());
    QVERIFY(outcome.secondId != origId);
    // D9: splitRow() itself owns origin registration -- the caller does not
    // need to call registerSplitChild() separately.
    QCOMPARE(session.originIsMemory().value(outcome.secondId.toString(), false), true);
}

void HistoryEditSessionTest::test_D9_splitRow_out_of_range_reports_not_applied()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // No rows on page 0 -- both the row index and the page index are out of range.
    const auto badRow = session.splitRow(0, 0, QDateTime::currentDateTime(), DurationType::Activity, DurationType::Pause);
    QVERIFY(!badRow.applied);
    QCOMPARE(badRow.error, HistoryEditSession::SplitError::OutOfRange);

    const auto badPage = session.splitRow(5, 0, QDateTime::currentDateTime(), DurationType::Activity, DurationType::Pause);
    QVERIFY(!badPage.applied);
    QCOMPARE(badPage.error, HistoryEditSession::SplitError::OutOfRange);
}

void HistoryEditSessionTest::test_D9_setCompletedType_updates_type_and_marks_page_modified()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    const SegmentId segId = SegmentId::mint();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end, segId));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QVERIFY(!session.pageModified(0));

    QVERIFY(session.setCompletedType(0, segId, DurationType::Pause));

    QCOMPARE(session.pendingTimelines()[0].completed()[0].type, DurationType::Pause);
    QVERIFY(session.pageModified(0));
    QVERIFY(session.hasEdits());
}

void HistoryEditSessionTest::test_D9_setCompletedType_unknown_id_returns_false()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QVERIFY(!session.setCompletedType(0, SegmentId::mint(), DurationType::Pause));
    // No-op: nothing was modified.
    QVERIFY(!session.pageModified(0));
}

void HistoryEditSessionTest::test_D9_setOngoingType_updates_type_and_marks_page_modified()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    HistoryEditSession session;
    session.buildFromTimer(tracker, QDate::currentDate());

    QVERIFY(!session.pageModified(0));
    QCOMPARE(session.pendingTimelines()[0].ongoing()->type, DurationType::Activity);

    QVERIFY(session.setOngoingType(0, DurationType::Pause));

    QCOMPARE(session.pendingTimelines()[0].ongoing()->type, DurationType::Pause);
    QVERIFY(session.pageModified(0));
    QVERIFY(session.hasEdits());

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryEditSessionTest::test_D9_setOngoingType_no_ongoing_returns_false()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // No Start -- page 0 has no ongoing row.
    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QVERIFY(!session.setOngoingType(0, DurationType::Pause));
    QVERIFY(!session.pageModified(0));
}

void HistoryEditSessionTest::test_D9_pageModified_false_before_any_edit()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, yesterdayUTC(10, 0), yesterdayUTC(10, 30)));

    TimeTracker tracker(settings, fakeDb);
    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QCOMPARE(session.pages().size(), static_cast<size_t>(2));
    QVERIFY(!session.pageModified(0));
    QVERIFY(!session.pageModified(1));
    QVERIFY(!session.hasEdits());
}

void HistoryEditSessionTest::test_D9_pageModified_scoped_to_single_page()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    FakeSessionStore fakeDb;

    const SegmentId histId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, yesterdayUTC(10, 0), yesterdayUTC(10, 30), histId));

    TimeTracker tracker(settings, fakeDb);
    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QCOMPARE(session.pages().size(), static_cast<size_t>(2));

    QVERIFY(session.setCompletedType(1, histId, DurationType::Pause));

    // Only page 1 is dirty; page 0 (empty Today) is untouched.
    QVERIFY(!session.pageModified(0));
    QVERIFY(session.pageModified(1));
}

void HistoryEditSessionTest::test_D9_page_has_one_ongoing_representation()
{
    // R14-002: the ongoing segment must not be counted both inside the page's
    // completed rows and again as a separate value -- displayCount() must
    // count it exactly once.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Start, then Pause: Pause finalizes the Activity segment into
    // session_.durations (1 completed row) and begins a new ongoing Pause
    // segment -- avoids resetForNewSession() clearing a debug-injected
    // completed row, which only Start() from Mode::None does.
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    tracker.useTimerViaButton(Button::Pause);
    QTest::qWait(20);

    HistoryEditSession session;
    session.buildFromTimer(tracker, QDate::currentDate());

    // 1 completed row + 1 ongoing row.
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QVERIFY(session.pendingTimelines()[0].ongoing().has_value());
    QCOMPARE(session.displayCount(0), 2);

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// D9 review (finding 3): splitRow() owns the minimum-length rules with a
// typed reason -- pin each boundary.
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_D9_splitRow_rejects_source_row_under_3_seconds_as_TooShort()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0, 0);
    const QDateTime end = start.addMSecs(2999); // just under the 3s minimum
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto outcome = session.splitRow(0, 0, start.addMSecs(1500), DurationType::Activity, DurationType::Pause);

    QVERIFY(!outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::TooShort);
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_D9_splitRow_accepts_source_row_at_exactly_3_seconds()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0, 0);
    const QDateTime end = start.addMSecs(3000); // exactly at the 3s minimum
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    const auto outcome = session.splitRow(0, 0, start.addMSecs(1500), DurationType::Activity, DurationType::Pause);

    QVERIFY(outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::None);
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
}

void HistoryEditSessionTest::test_D9_splitRow_rejects_first_half_under_1_second_as_HalfTooShort()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0, 0);
    const QDateTime end = start.addMSecs(5000);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // First half would be 999ms -- just under the 1s-per-half minimum.
    const auto outcome = session.splitRow(0, 0, start.addMSecs(999), DurationType::Activity, DurationType::Pause);

    QVERIFY(!outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::HalfTooShort);
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_D9_splitRow_rejects_second_half_under_1_second_as_HalfTooShort()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0, 0);
    const QDateTime end = start.addMSecs(5000);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Second half would be 999ms -- just under the 1s-per-half minimum.
    const auto outcome = session.splitRow(0, 0, start.addMSecs(4001), DurationType::Activity, DurationType::Pause);

    QVERIFY(!outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::HalfTooShort);
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_D9_splitRow_accepts_first_half_at_exactly_1_second()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0, 0);
    const QDateTime end = start.addMSecs(3000);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // First half is exactly 1000ms -- at the boundary, must be accepted.
    const auto outcome = session.splitRow(0, 0, start.addMSecs(1000), DurationType::Activity, DurationType::Pause);

    QVERIFY(outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::None);
    const auto& comp = session.pendingTimelines()[0].completed();
    QCOMPARE(comp.size(), static_cast<size_t>(2));
    QCOMPARE(comp[0].startTime.msecsTo(comp[0].endTime), static_cast<qint64>(1000));
    QCOMPARE(comp[1].startTime.msecsTo(comp[1].endTime), static_cast<qint64>(2000));
}

void HistoryEditSessionTest::test_D9_splitRow_accepts_second_half_at_exactly_1_second()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0, 0);
    const QDateTime end = start.addMSecs(3000);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // Second half is exactly 1000ms (split at end - 1000ms) -- at the
    // boundary, must be accepted. Pins the second-half comparison
    // independently of test_D9_splitRow_accepts_first_half_at_exactly_1_second.
    const auto outcome = session.splitRow(0, 0, end.addMSecs(-1000), DurationType::Activity, DurationType::Pause);

    QVERIFY(outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::None);
    const auto& comp = session.pendingTimelines()[0].completed();
    QCOMPARE(comp.size(), static_cast<size_t>(2));
    QCOMPARE(comp[0].startTime.msecsTo(comp[0].endTime), static_cast<qint64>(2000));
    QCOMPARE(comp[1].startTime.msecsTo(comp[1].endTime), static_cast<qint64>(1000));
}

// ---------------------------------------------------------------------------
// D9 review (finding 1, round 3): splitRejection() -- the shared predicate
// splitRow() and HistoryDialog's pre-dialog check both use.
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_D9_splitRejection_none_for_valid_row()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0, 0);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, start.addMSecs(3000)));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QCOMPARE(session.splitRejection(0, 0), HistoryEditSession::SplitError::None);
}

void HistoryEditSessionTest::test_D9_splitRejection_TooShort_for_short_row()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime start = todayUTC(10, 0, 0);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, start.addMSecs(2999)));

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    QCOMPARE(session.splitRejection(0, 0), HistoryEditSession::SplitError::TooShort);
}

void HistoryEditSessionTest::test_D9_splitRejection_OutOfRange_for_bad_indices()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    HistoryEditSession session;
    session.buildFromTimer(tracker, fixedToday());

    // No rows on page 0.
    QCOMPARE(session.splitRejection(0, 0), HistoryEditSession::SplitError::OutOfRange);
    QCOMPARE(session.splitRejection(5, 0), HistoryEditSession::SplitError::OutOfRange);
}
