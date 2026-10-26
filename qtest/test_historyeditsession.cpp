#include "test_historyeditsession.h"
#include <QtTest>
#include <algorithm>

#include "../historyeditsession.h"

#include "testcommon.h"
#ifdef Q_OS_UNIX
#include <time.h>
#endif

using TestCommon::mk;

namespace {

// Fixed civil anchor -- NOT the real "today". The old helpers paired the
// real local calendar date (QDate::currentDate()) with a Qt::UTC time
// spec: at a nonzero UTC offset that pairing names a different local day
// than the literal date field, which corrupted the local-day bucketing
// buildFromContext() performs (historyeditsession.cpp,
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

// H5/WP4: a plain local -> raw-text StoredHistoryRow projection, for building
// HistoryEditContext values directly (no TimeTracker/store involved -- see
// makeContext() below). Mirrors fakesessionstore.cpp's own
// toStoredHistoryRow() closely enough for these tests' purposes: only
// finalized()/parsed matter to buildFromContext(), so the exact raw text is
// not asserted anywhere here.
StoredHistoryRow storedRow(const TimeDuration& d, bool finalized = true)
{
    StoredHistoryRow row;
    row.segmentId = d.segment_id.toString();
    row.type = QString::number(static_cast<int>(d.type));
    row.startUtc = d.startTime.toUTC().toString(Qt::ISODateWithMs);
    row.endUtc = d.endTime.toUTC().toString(Qt::ISODateWithMs);
    row.isFinalized = finalized ? "1" : "0";
    row.parsed = d;
    return row;
}

// Same fixed-anchor convention as todayUTC()/yesterdayUTC(), for an arbitrary
// calendar date (H5/WP4 tests need dates other than "today"/"yesterday",
// e.g. a future-dated page or a two-days-ago page).
QDateTime atLocal(const QDate& date, int hour, int minute = 0, int second = 0) {
    return QDateTime(date, QTime(hour, minute, second), Qt::LocalTime);
}

int pageIndexForDate(const HistoryEditSession& session, const QDate& date) {
    for (size_t i = 0; i < session.pageCount(); ++i)
        if (session.pages()[i].pageDate == date)
            return static_cast<int>(i);
    return -1;
}

// WP4 plan (B7): builds a HistoryEditContext directly, exactly as
// TimeTracker::beginExclusiveEdit() would have captured it, without needing a
// TimeTracker or a store.
HistoryEditContext makeContext(const QDate& today,
                                const Timeline& live,
                                const std::vector<StoredHistoryRow>& rows,
                                const std::deque<TimeDuration>& pending,
                                bool stopped,
                                SessionStoreResult::Category storedStatus = SessionStoreResult::Success)
{
    HistoryEditContext ctx;
    ctx.today = today;
    ctx.openedAt = QDateTime(today, QTime(12, 0), Qt::LocalTime);
    ctx.stored.status = storedStatus;
    ctx.stored.rows = rows;
    ctx.live = live;
    ctx.pendingCommit = pending;
    ctx.stopped = stopped;
    return ctx;
}

} // namespace

void HistoryEditSessionTest::initTestCase()
{
    // No DB setup required — tests build HistoryEditContext directly.
}

void HistoryEditSessionTest::cleanupTestCase()
{
}

void HistoryEditSessionTest::resetDatabaseFile() const
{
    // No-op: this suite never touches a database.
}

// ---------------------------------------------------------------------------
// Page creation
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_build_today_page_only_from_memory()
{
    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);
    const TimeDuration memRow = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({memRow}, std::nullopt), {}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    QCOMPARE(session.pages().size(), static_cast<size_t>(1));
    QCOMPARE(session.pages()[0].isCurrent, true);
    QCOMPARE(session.pages()[0].pageDate, fixedToday());
    QCOMPARE(session.pendingTimelines().size(), static_cast<size_t>(1));
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QVERIFY(!session.pendingTimelines()[0].ongoing().has_value());
}

void HistoryEditSessionTest::test_build_today_page_memory_plus_db()
{
    // A stored row and a memory row, both today, with different segment ids.
    const TimeDuration dbRow = TimeDuration::fromPersistedRow(
        DurationType::Pause, todayUTC(8, 0), todayUTC(8, 30));
    const TimeDuration memRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, todayUTC(9, 0), todayUTC(9, 30));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({memRow}, std::nullopt), {storedRow(dbRow)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    // Only 1 page: today.
    QCOMPARE(session.pages().size(), static_cast<size_t>(1));
    QCOMPARE(session.pages()[0].isCurrent, true);
    // 2 completed rows: memory row + stored today row.
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
}

void HistoryEditSessionTest::test_build_historical_page_separate_from_today()
{
    const TimeDuration histRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, yesterdayUTC(10, 0), yesterdayUTC(10, 30));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(histRow)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    // 2 pages: today (empty, isCurrent=true) + yesterday (1 entry, isCurrent=false).
    QCOMPARE(session.pages().size(), static_cast<size_t>(2));
    QCOMPARE(session.pages()[0].isCurrent, true);
    QCOMPARE(session.pages()[1].isCurrent, false);
    QCOMPARE(session.pages()[1].pageDate, fixedToday().addDays(-1));
    QCOMPARE(session.pendingTimelines()[1].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_build_deduplicates_db_row_matching_memory_segment_id()
{
    const SegmentId sharedId = SegmentId::mint();
    const QDateTime start = todayUTC(10, 0);
    const QDateTime end   = todayUTC(10, 30);

    const TimeDuration memRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, start, end, sharedId);
    // Stored row with the same segment_id but a slightly different start --
    // should be deduped out; the memory row wins (B4).
    const TimeDuration dbRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, start.addMSecs(-2), end, sharedId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({memRow}, std::nullopt), {storedRow(dbRow)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    // Only the memory row survives (the stored duplicate is excluded).
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(session.pendingTimelines()[0].completed()[0].segment_id, sharedId);
    QCOMPARE(session.pendingTimelines()[0].completed()[0].startTime, start);
}

// ---------------------------------------------------------------------------
// Type toggle
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_type_toggle_updates_pending_timeline()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(idx)].completed()[0].type, DurationType::Activity);

    QVERIFY(session.setPageTimeline(static_cast<size_t>(idx),
        session.pendingTimelines()[static_cast<size_t>(idx)].withSegmentType(0, DurationType::Pause)));

    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(idx)].completed()[0].type, DurationType::Pause);
    QVERIFY(session.hasEdits());
}

// ---------------------------------------------------------------------------
// Cross-midnight rows are display-only, and absent from the scoped save
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_WP4_cross_midnight_absent_from_scopedSave_unrelatedEditSucceeds()
{
    // MUST-FIX1 (user decision, review packet C3), carried into H5/WP4: a
    // legacy single-boundary cross-midnight row is display-only (still shown
    // on both pages) but must never appear in the scoped save diff -- not as
    // an upsert, not as a delete. An unrelated edit's save must still succeed.
    const QDate cmDay = fixedToday().addDays(-2);
    const QDateTime cmStart = atLocal(cmDay, 23, 50);
    const QDateTime cmEnd = atLocal(cmDay.addDays(1), 0, 10);
    const SegmentId cmId = SegmentId::mint();
    const TimeDuration crossMidnight = TimeDuration::fromPersistedRow(
        DurationType::Activity, cmStart, cmEnd, cmId);

    // An ordinary row on a different, unrelated historical day.
    const QDate ordinaryDay = fixedToday().addDays(-3);
    const SegmentId ordinaryId = SegmentId::mint();
    const TimeDuration ordinary = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(ordinaryDay, 9, 0), atLocal(ordinaryDay, 9, 30), ordinaryId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt),
        {storedRow(crossMidnight), storedRow(ordinary)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int ordinaryIdx = pageIndexForDate(session, ordinaryDay);
    QVERIFY(ordinaryIdx >= 0);

    QVERIFY(session.setCompletedType(static_cast<size_t>(ordinaryIdx), ordinaryId, DurationType::Pause));
    QVERIFY(session.hasEdits());

    const auto result = session.buildScopedSave();

    // The save is not rejected: the cross-midnight row must not block it.
    QCOMPARE(result.violation, DayValidity::Violation::None);

    // Only the ordinary day's edit is present.
    QCOMPARE(result.days.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].date, ordinaryDay);

    // The cross-midnight row's id is absent from every day's diff entirely.
    for (const auto& change : result.days) {
        for (const auto& u : change.upserts)
            QVERIFY(u.segment_id != cmId);
        for (const auto& d : change.deletedIds)
            QVERIFY(d != cmId);
    }
}

// ---------------------------------------------------------------------------
// F1: a failed history load must be remembered so saving can be refused
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_F1_historyLoadFailed_true_on_transient_error()
{
    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {}, {}, /*stopped=*/true,
        SessionStoreResult::TransientError);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    QVERIFY(session.historyLoadFailed());
}

void HistoryEditSessionTest::test_F1_historyLoadFailed_false_when_disabled()
{
    // Disabled history storage is a legitimate empty state, not a failure.
    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {}, {}, /*stopped=*/true,
        SessionStoreResult::Disabled);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    QVERIFY(!session.historyLoadFailed());
}

// D2/MEDIUM-C: buildFromContext() must use ctx.today, not the real system
// date, to decide page grouping/editability. Pin a date deliberately far
// from the real system date and assert it is classified as the CURRENT
// page -- a regression back to QDate::currentDate() would fail this on any
// day that isn't 2019-03-15.
void HistoryEditSessionTest::test_D2_buildFromContext_uses_ctx_today_not_real_date()
{
    const QDate pinnedDate(2019, 3, 15);
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, QDateTime(pinnedDate, QTime(10, 0), Qt::UTC),
        QDateTime(pinnedDate, QTime(10, 30), Qt::UTC));

    const HistoryEditContext ctx = makeContext(
        pinnedDate, Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    // Assert: the pinned date's page is the CURRENT page -- only true if
    // buildFromContext() actually used ctx.today rather than the real date.
    QVERIFY(!session.pages().empty());
    QCOMPARE(session.pages()[0].pageDate, pinnedDate);
    QCOMPARE(session.pages()[0].isCurrent, true);
}

// D2 test list ("History rejection"): a user edit that produces a reversed
// interval (end before start) must be flagged by buildScopedSave(), never
// silently written -- the History boundary's own guard
// (DayValidity::validatePayload(), via HistoryEditSession) is what D2's
// "reject at Timeline, History, and SessionStore boundaries" step already
// relies on for this case; SessionStore's own final guard is covered
// separately by C3 (test_C3_saveCheckpoint_rejects_nonpositive_interval
// etc. in test_database.cpp).
void HistoryEditSessionTest::test_D2_scopedSave_flags_reversed_interval_created_by_edit()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const SegmentId activityId = SegmentId::mint();
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30), activityId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    // Act: edit the row to a reversed interval (end before start), same id.
    std::deque<TimeDuration> edited{
        TimeDuration::fromPersistedRow(DurationType::Activity,
            atLocal(yesterday, 10, 30), atLocal(yesterday, 10, 0), activityId)
    };
    QVERIFY(session.setPageTimeline(static_cast<size_t>(idx), Timeline(edited, std::nullopt)));
    QVERIFY(session.hasEdits());

    const auto result = session.buildScopedSave();

    // Assert: rejected, not silently written.
    QCOMPARE(result.violation, DayValidity::Violation::NonPositive);
}

// ---------------------------------------------------------------------------
// D9 (R14-002): split/type-change operations, dynamic display count, and
// per-page dirty state are owned by HistoryEditSession.
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_D9_displayCount_reflects_split_immediately()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0, 0), atLocal(yesterday, 10, 0, 8));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QCOMPARE(session.displayCount(static_cast<size_t>(idx)), 1);

    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        atLocal(yesterday, 10, 0, 4), DurationType::Activity, DurationType::Pause);
    QVERIFY(outcome.applied);

    // The count reflects the split immediately -- no frozen build-time title.
    QCOMPARE(session.displayCount(static_cast<size_t>(idx)), 2);
}

void HistoryEditSessionTest::test_D9_pageTitle_reflects_dynamic_count()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0, 0), atLocal(yesterday, 10, 0, 8));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QVERIFY(session.pageTitle(static_cast<size_t>(idx)).contains("entries: 1"));

    session.splitRow(static_cast<size_t>(idx), 0,
        atLocal(yesterday, 10, 0, 4), DurationType::Activity, DurationType::Pause);

    QVERIFY(session.pageTitle(static_cast<size_t>(idx)).contains("entries: 2"));
}

void HistoryEditSessionTest::test_D9_splitRow_returns_original_and_new_ids()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const SegmentId origId = SegmentId::mint();
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0, 0), atLocal(yesterday, 10, 0, 8), origId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        atLocal(yesterday, 10, 0, 4), DurationType::Activity, DurationType::Pause);

    QVERIFY(outcome.applied);
    QCOMPARE(outcome.firstId, origId);
    QVERIFY(!outcome.secondId.isEmpty());
    QVERIFY(outcome.secondId != origId);
}

void HistoryEditSessionTest::test_D9_splitRow_out_of_range_reports_not_applied()
{
    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

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
    const QDate yesterday = fixedToday().addDays(-1);
    const SegmentId segId = SegmentId::mint();
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30), segId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QVERIFY(!session.pageModified(static_cast<size_t>(idx)));

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), segId, DurationType::Pause));

    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(idx)].completed()[0].type, DurationType::Pause);
    QVERIFY(session.pageModified(static_cast<size_t>(idx)));
    QVERIFY(session.hasEdits());
}

void HistoryEditSessionTest::test_D9_setCompletedType_unknown_id_returns_false()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QVERIFY(!session.setCompletedType(static_cast<size_t>(idx), SegmentId::mint(), DurationType::Pause));
    // No-op: nothing was modified.
    QVERIFY(!session.pageModified(static_cast<size_t>(idx)));
}

void HistoryEditSessionTest::test_D9_pageModified_false_before_any_edit()
{
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, yesterdayUTC(10, 0), yesterdayUTC(10, 30));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    QCOMPARE(session.pages().size(), static_cast<size_t>(2));
    QVERIFY(!session.pageModified(0));
    QVERIFY(!session.pageModified(1));
    QVERIFY(!session.hasEdits());
}

void HistoryEditSessionTest::test_D9_pageModified_scoped_to_single_page()
{
    const SegmentId histId = SegmentId::mint();
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, yesterdayUTC(10, 0), yesterdayUTC(10, 30), histId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    QCOMPARE(session.pages().size(), static_cast<size_t>(2));

    QVERIFY(session.setCompletedType(1, histId, DurationType::Pause));

    // Only page 1 (yesterday) is dirty; page 0 (empty Today) is untouched.
    QVERIFY(!session.pageModified(0));
    QVERIFY(session.pageModified(1));
}

void HistoryEditSessionTest::test_D9_page_has_one_ongoing_representation()
{
    // R14-002: the ongoing segment must not be counted both inside the page's
    // completed rows and again as a separate value -- displayCount() must
    // count it exactly once.
    const TimeDuration completedRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, todayUTC(9, 0), todayUTC(9, 30));
    const TimeDuration ongoing = TimeDuration::fromLiveSession(
        DurationType::Pause, todayUTC(9, 30), todayUTC(9, 35));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({completedRow}, ongoing), {}, {}, /*stopped=*/false);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    // 1 completed row + 1 ongoing row.
    QCOMPARE(session.pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QVERIFY(session.pendingTimelines()[0].ongoing().has_value());
    QCOMPARE(session.displayCount(0), 2);
}

// ---------------------------------------------------------------------------
// D9 review (finding 3): splitRow() owns the minimum-length rules with a
// typed reason -- pin each boundary.
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_D9_splitRow_rejects_source_row_under_3_seconds_as_TooShort()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime start = atLocal(yesterday, 10, 0, 0);
    const QDateTime end = start.addMSecs(2999); // just under the 3s minimum
    const TimeDuration row = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        start.addMSecs(1500), DurationType::Activity, DurationType::Pause);

    QVERIFY(!outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::TooShort);
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(idx)].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_D9_splitRow_accepts_source_row_at_exactly_3_seconds()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime start = atLocal(yesterday, 10, 0, 0);
    const QDateTime end = start.addMSecs(3000); // exactly at the 3s minimum
    const TimeDuration row = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        start.addMSecs(1500), DurationType::Activity, DurationType::Pause);

    QVERIFY(outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::None);
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(idx)].completed().size(), static_cast<size_t>(2));
}

void HistoryEditSessionTest::test_D9_splitRow_rejects_first_half_under_1_second_as_HalfTooShort()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime start = atLocal(yesterday, 10, 0, 0);
    const QDateTime end = start.addMSecs(5000);
    const TimeDuration row = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    // First half would be 999ms -- just under the 1s-per-half minimum.
    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        start.addMSecs(999), DurationType::Activity, DurationType::Pause);

    QVERIFY(!outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::HalfTooShort);
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(idx)].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_D9_splitRow_rejects_second_half_under_1_second_as_HalfTooShort()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime start = atLocal(yesterday, 10, 0, 0);
    const QDateTime end = start.addMSecs(5000);
    const TimeDuration row = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    // Second half would be 999ms -- just under the 1s-per-half minimum.
    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        start.addMSecs(4001), DurationType::Activity, DurationType::Pause);

    QVERIFY(!outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::HalfTooShort);
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(idx)].completed().size(), static_cast<size_t>(1));
}

void HistoryEditSessionTest::test_D9_splitRow_accepts_first_half_at_exactly_1_second()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime start = atLocal(yesterday, 10, 0, 0);
    const QDateTime end = start.addMSecs(3000);
    const TimeDuration row = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    // First half is exactly 1000ms -- at the boundary, must be accepted.
    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        start.addMSecs(1000), DurationType::Activity, DurationType::Pause);

    QVERIFY(outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::None);
    const auto& comp = session.pendingTimelines()[static_cast<size_t>(idx)].completed();
    QCOMPARE(comp.size(), static_cast<size_t>(2));
    QCOMPARE(comp[0].startTime.msecsTo(comp[0].endTime), static_cast<qint64>(1000));
    QCOMPARE(comp[1].startTime.msecsTo(comp[1].endTime), static_cast<qint64>(2000));
}

void HistoryEditSessionTest::test_D9_splitRow_accepts_second_half_at_exactly_1_second()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime start = atLocal(yesterday, 10, 0, 0);
    const QDateTime end = start.addMSecs(3000);
    const TimeDuration row = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    // Second half is exactly 1000ms (split at end - 1000ms) -- at the
    // boundary, must be accepted. Pins the second-half comparison
    // independently of test_D9_splitRow_accepts_first_half_at_exactly_1_second.
    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        end.addMSecs(-1000), DurationType::Activity, DurationType::Pause);

    QVERIFY(outcome.applied);
    QCOMPARE(outcome.error, HistoryEditSession::SplitError::None);
    const auto& comp = session.pendingTimelines()[static_cast<size_t>(idx)].completed();
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
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime start = atLocal(yesterday, 10, 0, 0);
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, start, start.addMSecs(3000));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QCOMPARE(session.splitRejection(static_cast<size_t>(idx), 0), HistoryEditSession::SplitError::None);
}

void HistoryEditSessionTest::test_D9_splitRejection_TooShort_for_short_row()
{
    const QDate yesterday = fixedToday().addDays(-1);
    const QDateTime start = atLocal(yesterday, 10, 0, 0);
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, start, start.addMSecs(2999));

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QCOMPARE(session.splitRejection(static_cast<size_t>(idx), 0), HistoryEditSession::SplitError::TooShort);
}

void HistoryEditSessionTest::test_D9_splitRejection_OutOfRange_for_bad_indices()
{
    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    // No rows on page 0.
    QCOMPARE(session.splitRejection(0, 0), HistoryEditSession::SplitError::OutOfRange);
    QCOMPARE(session.splitRejection(5, 0), HistoryEditSession::SplitError::OutOfRange);
}

void HistoryEditSessionTest::test_M3_timezone_change_makes_row_single_boundary_cross_midnight()
{
    // M3: an ordinary timezone change can turn a valid same-day UTC row into
    // a single-boundary cross-midnight row on reload -- no edit, no DST, no
    // manual DB tampering involved.
    // Scoped RAII guard: restores the local calendar
    // on destruction, so this test cannot leak its timezone into any test
    // that runs after it.
    TestCommon::TzGuard tzGuard;

    const SegmentId rowId = SegmentId::mint();
    const QDateTime start(QDate(2025, 6, 14), QTime(22, 30, 0), Qt::UTC);
    const QDateTime end(QDate(2025, 6, 14), QTime(23, 30, 0), Qt::UTC);
    const TimeDuration row = TimeDuration::fromPersistedRow(DurationType::Activity, start, end, rowId);

    tzGuard.set("UTC");
    {
        // Optional sanity check: under UTC, this is an ordinary same-day
        // pending row, not display-only.
        const HistoryEditContext ctx = makeContext(
            QDate(2025, 6, 14), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);
        HistoryEditSession session;
        session.buildFromContext(ctx);
        bool foundPending = false;
        for (const auto& tl : session.pendingTimelines())
            for (const auto& d : tl.completed())
                if (d.segment_id == rowId) foundPending = true;
        QVERIFY(foundPending);
    }

    // POSIX sign inversion: the "Etc/GMT-1" (and "UTC-1") zone names mean
    // +01:00, not -01:00 -- POSIX TZ offsets are inverted from the
    // conventional UTC+/-N naming. Never write "UTC+1" for this zone.
    tzGuard.set("Etc/GMT-1");
    {
        // 22:30Z -> 23:30 local (14th); 23:30Z -> 00:30 local (15th): a
        // single-boundary cross-midnight row under this offset.
        const HistoryEditContext ctx = makeContext(
            QDate(2025, 6, 15), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);
        HistoryEditSession session;
        session.buildFromContext(ctx);

        int crossMidnightCount = 0;
        for (const auto& page : session.pages())
            for (const auto& d : page.crossMidnight)
                if (d.segment_id == rowId) ++crossMidnightCount;
        QCOMPARE(crossMidnightCount, 1);

        int pendingCount = 0;
        for (const auto& tl : session.pendingTimelines())
            for (const auto& d : tl.completed())
                if (d.segment_id == rowId) ++pendingCount;
        QCOMPARE(pendingCount, 0);
    }
}

// ---------------------------------------------------------------------------
// H5/WP4 -- buildFromContext() page building
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_WP4_pastMemoryRow_isReadOnly_todayUnaffected()
{
    // Arrange: a memory (live) row sits on yesterday; Today has its own
    // finalized row and the tracker is stopped, so Today should be editable
    // regardless of what yesterday's memory row does to yesterday's page.
    const QDate yesterday = fixedToday().addDays(-1);
    const TimeDuration memRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30));
    const TimeDuration todayRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, todayUTC(9, 0), todayUTC(9, 30));

    const HistoryEditContext ctx = makeContext(
        fixedToday(),
        Timeline({memRow}, std::nullopt),
        {storedRow(todayRow)},
        {},
        /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    const int yesterdayIdx = pageIndexForDate(session, yesterday);
    const int todayIdx = pageIndexForDate(session, fixedToday());
    QVERIFY(yesterdayIdx >= 0);
    QVERIFY(todayIdx >= 0);

    // Act/Assert: yesterday's page is read-only (a live row overlaps it);
    // Today's permission and content are unaffected.
    QCOMPARE(session.pageEditable(static_cast<size_t>(yesterdayIdx)), false);
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(yesterdayIdx)].completed().size(),
        static_cast<size_t>(1));
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(yesterdayIdx)].completed()[0].segment_id,
        memRow.segment_id);

    QCOMPARE(session.pageEditable(static_cast<size_t>(todayIdx)), true);
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(todayIdx)].completed().size(),
        static_cast<size_t>(1));
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(todayIdx)].completed()[0].segment_id,
        todayRow.segment_id);
}

void HistoryEditSessionTest::test_WP4_ongoingRow_onToday_notEditable()
{
    // Arrange: the tracker is running, so ctx carries an ongoing row on Today.
    const TimeDuration ongoing = TimeDuration::fromLiveSession(
        DurationType::Activity, todayUTC(9, 0), todayUTC(9, 5));

    const HistoryEditContext ctx = makeContext(
        fixedToday(),
        Timeline({}, ongoing),
        {},
        {},
        /*stopped=*/false);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    const int todayIdx = pageIndexForDate(session, fixedToday());
    QVERIFY(todayIdx >= 0);

    // Act/Assert
    QVERIFY(session.pendingTimelines()[static_cast<size_t>(todayIdx)].ongoing().has_value());
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(todayIdx)].ongoing()->segment_id,
        ongoing.segment_id);
    QCOMPARE(session.pageEditable(static_cast<size_t>(todayIdx)), false);
}

void HistoryEditSessionTest::test_WP4_mutationsOnReadOnlyPage_leaveItUnchanged()
{
    // Arrange: Today is read-only (running, with an ongoing row) and also
    // has a finalized completed row, so there is something to try to mutate.
    const SegmentId rowId = SegmentId::mint();
    const TimeDuration completedRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, todayUTC(8, 0), todayUTC(8, 30), rowId);
    const TimeDuration ongoing = TimeDuration::fromLiveSession(
        DurationType::Activity, todayUTC(9, 0), todayUTC(9, 5));

    const HistoryEditContext ctx = makeContext(
        fixedToday(),
        Timeline({}, ongoing),
        {storedRow(completedRow)},
        {},
        /*stopped=*/false);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    const int todayIdx = pageIndexForDate(session, fixedToday());
    QVERIFY(todayIdx >= 0);
    QCOMPARE(session.pageEditable(static_cast<size_t>(todayIdx)), false);

    // Act: every mutation on this read-only page must refuse.
    const bool typeChanged = session.setCompletedType(static_cast<size_t>(todayIdx), rowId, DurationType::Pause);
    const auto splitOutcome = session.splitRow(static_cast<size_t>(todayIdx), 0,
        todayUTC(8, 15), DurationType::Activity, DurationType::Pause);
    const bool timelineSet = session.setPageTimeline(static_cast<size_t>(todayIdx),
        Timeline({}, std::nullopt));

    // Assert: refused, and the page's rows are byte-identical to before.
    QCOMPARE(typeChanged, false);
    QCOMPARE(splitOutcome.applied, false);
    QCOMPARE(splitOutcome.error, HistoryEditSession::SplitError::ReadOnly);
    QCOMPARE(timelineSet, false);

    const auto& completed = session.pendingTimelines()[static_cast<size_t>(todayIdx)].completed();
    QCOMPARE(completed.size(), static_cast<size_t>(1));
    QCOMPARE(completed[0].segment_id, rowId);
    QCOMPARE(completed[0].type, DurationType::Activity);
    QCOMPARE(completed[0].startTime, completedRow.startTime);
    QCOMPARE(completed[0].endTime, completedRow.endTime);
    QVERIFY(session.pendingTimelines()[static_cast<size_t>(todayIdx)].ongoing().has_value());
    QCOMPARE(session.pendingTimelines()[static_cast<size_t>(todayIdx)].ongoing()->segment_id,
        ongoing.segment_id);
}

void HistoryEditSessionTest::test_WP4_rowsOutOfOrder_sortedOnPage()
{
    // Arrange: three stored rows for yesterday, supplied out of chronological
    // order.
    const QDate yesterday = fixedToday().addDays(-1);
    const TimeDuration rowA = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 5));
    const TimeDuration rowB = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 12, 0), atLocal(yesterday, 12, 5));
    const TimeDuration rowC = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 15, 0), atLocal(yesterday, 15, 5));

    const HistoryEditContext ctx = makeContext(
        fixedToday(),
        Timeline({}, std::nullopt),
        {storedRow(rowC), storedRow(rowA), storedRow(rowB)},
        {},
        /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    // Act/Assert: the page's rows come back sorted by start time.
    const auto& completed = session.pendingTimelines()[static_cast<size_t>(idx)].completed();
    QCOMPARE(completed.size(), static_cast<size_t>(3));
    QCOMPARE(completed[0].segment_id, rowA.segment_id);
    QCOMPARE(completed[1].segment_id, rowB.segment_id);
    QCOMPARE(completed[2].segment_id, rowC.segment_id);
}

void HistoryEditSessionTest::test_WP4_futureDatedPage_isEditable()
{
    // Arrange (U2): a page dated after Today, with no live/pending overlap.
    const QDate future = fixedToday().addDays(3);
    const TimeDuration futureRow = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(future, 10, 0), atLocal(future, 10, 30));

    const HistoryEditContext ctx = makeContext(
        fixedToday(),
        Timeline({}, std::nullopt),
        {storedRow(futureRow)},
        {},
        /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    const int idx = pageIndexForDate(session, future);
    QVERIFY(idx >= 0);

    // Act/Assert
    QCOMPARE(session.pageEditable(static_cast<size_t>(idx)), true);
}

void HistoryEditSessionTest::test_WP4_daySpan_matchesLocalDayStartEnd()
{
    // Arrange
    const QDate yesterday = fixedToday().addDays(-1);
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30));

    const HistoryEditContext ctx = makeContext(
        fixedToday(),
        Timeline({}, std::nullopt),
        {storedRow(row)},
        {},
        /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);

    // Act/Assert: every page's span matches localDayStart/End(pageDate).
    for (size_t i = 0; i < session.pageCount(); ++i) {
        const auto span = session.daySpan(i);
        QCOMPARE(span.startUtc, DayValidity::localDayStart(session.pages()[i].pageDate).toUTC());
        QCOMPARE(span.endUtc, DayValidity::localDayEnd(session.pages()[i].pageDate).toUTC());
    }
}

// ---------------------------------------------------------------------------
// H5/WP4 -- buildScopedSave() scoped diff
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_WP4_scopedSave_toggle_yieldsOneUpsert()
{
    // Arrange
    const QDate yesterday = fixedToday().addDays(-1);
    const SegmentId rowId = SegmentId::mint();
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30), rowId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), rowId, DurationType::Pause));
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.violation, DayValidity::Violation::None);
    QCOMPARE(result.needsMergeConfirmation, false);
    QCOMPARE(result.days.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].date, yesterday);
    QCOMPARE(result.days[0].deletedIds.size(), static_cast<size_t>(0));
    QCOMPARE(result.days[0].upserts.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].upserts[0].segment_id, rowId);
    QCOMPARE(result.days[0].upserts[0].type, DurationType::Pause);
}

void HistoryEditSessionTest::test_WP4_scopedSave_split_yieldsOriginalPlusNewId()
{
    // Arrange
    const QDate yesterday = fixedToday().addDays(-1);
    const SegmentId rowId = SegmentId::mint();
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 10), rowId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, /*stopped=*/true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    const auto outcome = session.splitRow(static_cast<size_t>(idx), 0,
        atLocal(yesterday, 10, 5), DurationType::Activity, DurationType::Pause);
    QVERIFY(outcome.applied);
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.days.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].deletedIds.size(), static_cast<size_t>(0));
    QCOMPARE(result.days[0].upserts.size(), static_cast<size_t>(2));
    QSet<QString> upsertIds;
    for (const auto& u : result.days[0].upserts)
        upsertIds.insert(u.segment_id.toString());
    QVERIFY(upsertIds.contains(outcome.firstId.toString()));
    QVERIFY(upsertIds.contains(outcome.secondId.toString()));
}

void HistoryEditSessionTest::test_WP4_scopedSave_toggleNextToSameTypeNeighbour_merges()
{
    // Arrange: two touching rows of different type; toggling A to match B's
    // type makes them a same-type, touching pair with A changed.
    const QDate yesterday = fixedToday().addDays(-1);
    const SegmentId idA = SegmentId::mint();
    const SegmentId idB = SegmentId::mint();
    const TimeDuration rowA = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30), idA);
    const TimeDuration rowB = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 30), atLocal(yesterday, 11, 0), idB);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(rowA), storedRow(rowB)}, {}, true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), idA, DurationType::Activity));
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.needsMergeConfirmation, true);
    QCOMPARE(result.days.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].upserts.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].upserts[0].segment_id, idA);
    QCOMPARE(result.days[0].upserts[0].type, DurationType::Activity);
    QCOMPARE(result.days[0].upserts[0].startTime, rowA.startTime);
    QCOMPARE(result.days[0].upserts[0].endTime, rowB.endTime);
    QCOMPARE(result.days[0].deletedIds.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].deletedIds[0], idB);
}

void HistoryEditSessionTest::test_WP4_scopedSave_untouchedTouchingPair_absentFromDays_idsKept()
{
    // Arrange: an untouched touching same-type pair must not merge just
    // because a third, unrelated row on the same page is edited.
    const QDate yesterday = fixedToday().addDays(-1);
    const SegmentId idP1 = SegmentId::mint();
    const SegmentId idP2 = SegmentId::mint();
    const SegmentId idC = SegmentId::mint();
    const TimeDuration rowP1 = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(yesterday, 8, 0), atLocal(yesterday, 8, 30), idP1);
    const TimeDuration rowP2 = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(yesterday, 8, 30), atLocal(yesterday, 9, 0), idP2);
    const TimeDuration rowC = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30), idC);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt),
        {storedRow(rowP1), storedRow(rowP2), storedRow(rowC)}, {}, true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), idC, DurationType::Pause));
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.needsMergeConfirmation, false);
    QCOMPARE(result.days.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].upserts.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].upserts[0].segment_id, idC);
    QVERIFY(result.days[0].deletedIds.empty());
}

void HistoryEditSessionTest::test_WP4_scopedSave_toggleBackToOriginal_yieldsEmptyDays()
{
    // Arrange: toggle, then toggle back -- net zero change.
    const QDate yesterday = fixedToday().addDays(-1);
    const SegmentId rowId = SegmentId::mint();
    const TimeDuration row = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30), rowId);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(row)}, {}, true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, yesterday);
    QVERIFY(idx >= 0);

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), rowId, DurationType::Activity));
    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), rowId, DurationType::Pause));
    QVERIFY(!session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QVERIFY(result.days.empty());
}

void HistoryEditSessionTest::test_WP4_scopedSave_editsOnTwoDates_yieldTwoDaysEntries()
{
    // Arrange
    const QDate yesterday = fixedToday().addDays(-1);
    const QDate twoDaysAgo = fixedToday().addDays(-2);
    const SegmentId idY = SegmentId::mint();
    const SegmentId idZ = SegmentId::mint();
    const TimeDuration rowY = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(yesterday, 10, 0), atLocal(yesterday, 10, 30), idY);
    const TimeDuration rowZ = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(twoDaysAgo, 10, 0), atLocal(twoDaysAgo, 10, 30), idZ);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt), {storedRow(rowY), storedRow(rowZ)}, {}, true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idxY = pageIndexForDate(session, yesterday);
    const int idxZ = pageIndexForDate(session, twoDaysAgo);
    QVERIFY(idxY >= 0);
    QVERIFY(idxZ >= 0);

    QVERIFY(session.setCompletedType(static_cast<size_t>(idxY), idY, DurationType::Pause));
    QVERIFY(session.setCompletedType(static_cast<size_t>(idxZ), idZ, DurationType::Pause));
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.days.size(), static_cast<size_t>(2));
    QSet<QDate> dates;
    for (const auto& d : result.days)
        dates.insert(d.date);
    QVERIFY(dates.contains(yesterday));
    QVERIFY(dates.contains(twoDaysAgo));
}

// ---------------------------------------------------------------------------
// H5/WP4 -- buildScopedSave() violations
// ---------------------------------------------------------------------------

void HistoryEditSessionTest::test_WP4_scopedSave_overlapsRetainedCrossMidnight_givesTypeOverlap()
{
    // Arrange: a single-boundary cross-midnight Pause row is retained
    // context on `day`; a same-day row shares day's type at first, then gets
    // toggled to Activity, creating a NEW overlap with the retained row.
    const QDate day = fixedToday().addDays(-1);
    const TimeDuration crossMidnight = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(day, 23, 0), atLocal(day.addDays(1), 1, 0));
    const SegmentId idX = SegmentId::mint();
    const TimeDuration rowX = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(day, 22, 50), atLocal(day, 23, 5), idX);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt),
        {storedRow(crossMidnight), storedRow(rowX)}, {}, true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, day);
    QVERIFY(idx >= 0);
    // Arrange precondition: the cross-midnight row is display-only context,
    // not an editable candidate on this page.
    QCOMPARE(session.pages()[static_cast<size_t>(idx)].crossMidnight.size(), static_cast<size_t>(1));

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), idX, DurationType::Activity));
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.violation, DayValidity::Violation::TypeOverlap);
}

void HistoryEditSessionTest::test_WP4_scopedSave_overlapsRetainedMultiBoundary_givesTypeOverlap()
{
    // Arrange: a stored row spanning more than one boundary is skipped from
    // the pages entirely, but must still count as retained context for the
    // overlap check.
    const QDate day = fixedToday().addDays(-1);
    const TimeDuration multiBoundary = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(day, 22, 0), atLocal(day.addDays(2), 2, 0));
    const SegmentId idX = SegmentId::mint();
    const TimeDuration rowX = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(day, 23, 0), atLocal(day, 23, 10), idX);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt),
        {storedRow(multiBoundary), storedRow(rowX)}, {}, true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, day);
    QVERIFY(idx >= 0);
    // Arrange precondition: the multi-boundary row produced no page rows of
    // its own (it never appears as an editable row or a display row).
    for (size_t i = 0; i < session.pageCount(); ++i) {
        for (const auto& d : session.pendingTimelines()[i].completed())
            QVERIFY(d.segment_id != multiBoundary.segment_id);
        for (const auto& d : session.pages()[i].crossMidnight)
            QVERIFY(d.segment_id != multiBoundary.segment_id);
        for (const auto& d : session.pages()[i].continuations)
            QVERIFY(d.segment_id != multiBoundary.segment_id);
    }

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), idX, DurationType::Activity));
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.violation, DayValidity::Violation::TypeOverlap);
}

void HistoryEditSessionTest::test_WP4_scopedSave_unrelatedPreExistingOverlap_doesNotBlockOtherEdit()
{
    // Arrange: an untouched cross-type overlap already exists between two
    // retained rows; a third, unrelated row is edited elsewhere on the page.
    const QDate day = fixedToday().addDays(-1);
    const TimeDuration rowP = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(day, 8, 0), atLocal(day, 9, 0));
    const TimeDuration rowA = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(day, 8, 30), atLocal(day, 8, 45));
    const SegmentId idC = SegmentId::mint();
    const TimeDuration rowC = TimeDuration::fromPersistedRow(
        DurationType::Activity, atLocal(day, 12, 0), atLocal(day, 12, 30), idC);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt),
        {storedRow(rowP), storedRow(rowA), storedRow(rowC)}, {}, true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, day);
    QVERIFY(idx >= 0);

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), idC, DurationType::Pause));
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.violation, DayValidity::Violation::None);
    QCOMPARE(result.days.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].upserts.size(), static_cast<size_t>(1));
    QCOMPARE(result.days[0].upserts[0].segment_id, idC);
}

void HistoryEditSessionTest::test_WP4_scopedSave_editedRowOverlapsUntouchedRetained_givesTypeOverlap()
{
    // Arrange (M1): an untouched Pause row stays retained on the page; a
    // second Pause row is toggled to Activity and now overlaps it. This must
    // be caught even though neither row is a cross-midnight/multi-boundary
    // row -- the untouched-rows-into-retained loop in buildScopedSave() is
    // the only thing that can catch it.
    const QDate day = fixedToday().addDays(-1);
    const TimeDuration rowP = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(day, 10, 0), atLocal(day, 11, 0));
    const SegmentId idX = SegmentId::mint();
    const TimeDuration rowX = TimeDuration::fromPersistedRow(
        DurationType::Pause, atLocal(day, 10, 30), atLocal(day, 10, 45), idX);

    const HistoryEditContext ctx = makeContext(
        fixedToday(), Timeline({}, std::nullopt),
        {storedRow(rowP), storedRow(rowX)}, {}, true);

    HistoryEditSession session;
    session.buildFromContext(ctx);
    const int idx = pageIndexForDate(session, day);
    QVERIFY(idx >= 0);

    QVERIFY(session.setCompletedType(static_cast<size_t>(idx), idX, DurationType::Activity));
    QVERIFY(session.hasEdits());

    // Act
    const auto result = session.buildScopedSave();

    // Assert
    QCOMPARE(result.violation, DayValidity::Violation::TypeOverlap);
}
