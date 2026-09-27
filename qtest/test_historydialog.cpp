#include "test_historydialog.h"
#include <QtTest>
#include <QTableWidget>
#include <QCheckBox>
#include <QLabel>
#include <QMessageBox>
#include <QTimer>
#include <QApplication>
#include <QSqlDatabase>
#include <QSqlQuery>

#include "../historydialog.h"
#include "../historyeditsession.h"
#include "../shutdowncoordinator.h"
#include "fakesessionstore.h"
#include "fakeclock.h"

using TestCommon::createSettingsFile;
using TestCommon::fixedNow;
using TestCommon::mk;
using TestCommon::sumDurations;

namespace {
QDateTime makeTime(qint64 ms) {
    return QDateTime::fromMSecsSinceEpoch(ms, Qt::UTC);
}

/// H5/WP5: runs one raw SQL statement against the database file on its own
/// connection -- how a test changes the store behind an open dialog's back.
bool execRaw(const QString& dbPath, const QString& sql)
{
    const QString connName = "historydialog_exec_raw";
    bool ok = false;
    {
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(dbPath);
        if (rawDb.open()) {
            QSqlQuery q(rawDb);
            ok = q.exec(sql);
            rawDb.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return ok;
}

/// H5/WP5: inserts one row via raw SQL, bypassing every uTimer write path
/// (for legacy/out-of-contract shapes: cross-midnight, multi-boundary, bad type).
bool insertRaw(const QString& dbPath, const QString& segmentId, int type,
               const QDateTime& start, const QDateTime& end, bool finalized)
{
    return execRaw(dbPath, QString(
        "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
        "VALUES ('%1', %2, '%3', '%4', %5)")
        .arg(segmentId).arg(type)
        .arg(start.toUTC().toString(Qt::ISODateWithMs), end.toUTC().toString(Qt::ISODateWithMs))
        .arg(finalized ? 1 : 0));
}

/// H5/WP5: the raw stored text of one row ("type|start_utc|end_utc|is_finalized"),
/// or an empty string if no row has that segment_id -- a byte-identical check.
QString rowFingerprint(const QString& dbPath, const QString& segmentId)
{
    const QString connName = "historydialog_row_fingerprint";
    QString out;
    {
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(dbPath);
        if (rawDb.open()) {
            QSqlQuery q(rawDb);
            q.prepare("SELECT type, start_utc, end_utc, is_finalized FROM durations WHERE segment_id = :sid");
            q.bindValue(":sid", segmentId);
            if (q.exec() && q.next()) {
                out = QStringList{q.value(0).toString(), q.value(1).toString(),
                                  q.value(2).toString(), q.value(3).toString()}.join('|');
            }
            rawDb.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return out;
}

/// H5/WP5: total number of stored rows.
int storedRowCount(const QString& dbPath)
{
    const QString connName = "historydialog_row_count";
    int count = -1;
    {
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(dbPath);
        if (rawDb.open()) {
            QSqlQuery q(rawDb);
            if (q.exec("SELECT COUNT(*) FROM durations") && q.next())
                count = q.value(0).toInt();
            rawDb.close();
        }
    }
    QSqlDatabase::removeDatabase(connName);
    return count;
}

/// H5/WP5: index of the page for `date`, or -1. Pages are newest-first, so
/// tests look a date up rather than assuming a fixed index.
int pageIndexFor(const HistoryEditSession& session, const QDate& date)
{
    for (size_t i = 0; i < session.pageCount(); ++i)
        if (session.pages()[i].pageDate == date)
            return static_cast<int>(i);
    return -1;
}

/// H5/WP5: index of the Today page (isCurrent), or -1.
int currentPageIndex(const HistoryEditSession& session)
{
    for (size_t i = 0; i < session.pageCount(); ++i)
        if (session.pages()[i].isCurrent)
            return static_cast<int>(i);
    return -1;
}
}

void HistoryDialogTest::initTestCase()
{
    QVERIFY(data_dir_.isValid());
    TestCommon::setDataDirOverride(data_dir_.path());
    db_path_ = QDir(data_dir_.path()).filePath("uTimer.sqlite");
}

void HistoryDialogTest::cleanupTestCase()
{
    TestCommon::clearDataDirOverride();
}

void HistoryDialogTest::test_historydialog_createPages_includes_current_db_ongoing()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    QDateTime now = fixedNow();
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(now);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-120), now.addSecs(-60)));

    // Save a DB entry for today
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-50), now.addSecs(-40)));
    TestCommon::seedStoredRows(db, dbDurations, true);

    // Start ongoing activity
    tracker.useTimerViaButton(Button::Start);
    // Advance the fake clock (was QTest::qWait(5)) so the ongoing segment's
    // duration is strictly positive, deterministically -- see
    // ongoing()->duration() > 0 below.
    clock.advance(5);

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pages()[0].isCurrent, true);
    // completed=1 (DB row), ongoing=1; total display rows=2
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing().has_value());
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing()->duration() > 0);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
}

void HistoryDialogTest::test_historydialog_createPages_dedups_db_row_with_small_time_drift()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime now = fixedNow();
    const QDateTime memoryStart = now.addSecs(-40).addMSecs(2);
    const QDateTime memoryEnd = now.addSecs(-10);
    const SegmentId segmentId = SegmentId::mint();
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memoryStart, memoryEnd, segmentId));

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, memoryStart.addMSecs(-2), memoryEnd, segmentId));
    TestCommon::seedStoredRows(db, dbDurations, true);

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].startTime, memoryStart);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].endTime, memoryEnd);
}

void HistoryDialogTest::test_historydialog_createPages_groups_unsplit_cross_midnight_row_by_start_date()
{
    // Cross-midnight rows are loaded (via fromPersistedRow) and bucketed on BOTH
    // the start-date page (canonical) and the end-date page (continuation/display-only).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime crossMidnightStart(yesterday, QTime(23, 59, 50));
    const QDateTime crossMidnightEnd = crossMidnightStart.addSecs(20); // ends today

    // Trigger DB creation by saving a valid same-day row for yesterday.
    {
        const QDateTime validStart(yesterday, QTime(10, 0, 0));
        const QDateTime validEnd(yesterday, QTime(10, 30, 0));
        auto validSeg = TimeDuration::create(DurationType::Activity, validStart, validEnd);
        QVERIFY(validSeg.has_value());
        std::deque<TimeDuration> init;
        init.push_back(std::move(*validSeg));
        TestCommon::seedStoredRows(db, init, true);
    }

    // Insert the cross-midnight row directly via SQL (bypassing same-day invariant).
    {
        const QString connName = QString("test_cross_midnight_%1").arg(
            reinterpret_cast<quintptr>(&db));
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations "
            "(segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :start_utc, :end_utc, 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId().toString());
        q.bindValue(":start_utc", crossMidnightStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":end_utc", crossMidnightEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    // Cross-midnight row is loaded. It appears as crossMidnight on yesterday's
    // page (display-only canonical) and as a continuation on today's page.
    HistoryDialog dialog(tracker, settings);

    // 2 pages: today (isCurrent) and yesterday.
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pages()[0].isCurrent, true);
    QCOMPARE(dialog.editSession_dbg().pages()[1].isCurrent, false);

    // Yesterday: 1 same-day canonical row + 1 cross-midnight (display-only).
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(1)); // valid same-day
    QCOMPARE(dialog.editSession_dbg().pages()[1].crossMidnight.size(), static_cast<size_t>(1)); // cross-midnight canonical
    QCOMPARE(dialog.editSession_dbg().pages()[1].continuations.size(), static_cast<size_t>(0));

    // Today's page has the cross-midnight row as a continuation (display-only).
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations[0].startTime.date(), yesterday);
    QCOMPARE(dialog.editSession_dbg().pages()[0].crossMidnight.size(), static_cast<size_t>(0));
}

void HistoryDialogTest::test_historydialog_checkbox_toggle_updates_pending_and_totals()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QDateTime now = fixedNow().addDays(-1);
    TestCommon::seedStoredRows(db, {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50))}, true);

    HistoryDialog dialog(tracker, settings);
    dialog.onOlder_dbg();  // page 1: yesterday

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed()[0].type, DurationType::Activity);
    box->setChecked(false);

    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed()[0].type, DurationType::Pause);
    QVERIFY(dialog.pageLabel_dbg()->text().contains("Pause"));
}

void HistoryDialogTest::test_historydialog_split_action_splits_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QDateTime start = fixedNow().addDays(-1).addSecs(-10);
    QDateTime end = fixedNow().addDays(-1).addSecs(-4);
    const TimeDuration seeded = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);
    TestCommon::seedStoredRows(db, {seeded}, true);
    const SegmentId originalSegmentId = seeded.segment_id;

    HistoryDialog dialog(tracker, settings);

    // Bypass SplitDialog::exec() to avoid offscreen QPA propagateSizeHints() warnings.
    // The split types are supplied directly; logic under test is the model update (withSplit,
    // origin registration, segment_id preservation) — not the dialog UI itself.
    dialog.applySplit_dbg(0, 1, DurationType::Activity, DurationType::Pause);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(2));
    const auto& first = dialog.editSession_dbg().pendingTimelines()[1].completed()[0];
    const auto& second = dialog.editSession_dbg().pendingTimelines()[1].completed()[1];
    QCOMPARE(first.startTime, start);
    QCOMPARE(second.endTime, end);
    QCOMPARE(first.endTime, second.startTime);
    QCOMPARE(first.type, DurationType::Activity);
    QCOMPARE(second.type, DurationType::Pause);
    QCOMPARE(first.segment_id, originalSegmentId);
    QVERIFY(!second.segment_id.isEmpty());
    QVERIFY(second.segment_id != originalSegmentId);
}

void HistoryDialogTest::test_historydialog_split_non_today_db_row_survives_save_roundtrip()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime start(QDate::currentDate().addDays(-1), QTime(10, 0, 0));
    const QDateTime end = start.addSecs(8);

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));
    TestCommon::seedStoredRows(db, dbDurations, true);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));

    // Bypass SplitDialog::exec() to avoid offscreen QPA propagateSizeHints() warnings.
    dialog.applySplit_dbg(0, 1, DurationType::Activity, DurationType::Pause);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(2));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    dialog.done(QDialog::Accepted);
    QVERIFY(dialog.saveChanges());

    SqliteSessionStore db2(settings);
    auto loaded = db2.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(2));
    QCOMPARE(loaded[0].startTime, start);
    QCOMPARE(loaded[1].endTime, end);
    QCOMPARE(loaded[0].endTime, loaded[1].startTime);
}

void HistoryDialogTest::test_historydialog_save_unrelated_edit_preserves_row_and_creates_new_checkpoint()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db2(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db2);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    const QDateTime historicalStart(QDate::currentDate().addDays(-1), QTime(10, 0, 0));
    const QDateTime historicalEnd = historicalStart.addSecs(300);
    std::deque<TimeDuration> historicalRows;
    historicalRows.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, historicalStart, historicalEnd));
    TestCommon::seedStoredRows(db2, historicalRows, true);


    tracker.useTimerViaButton(Button::Start);
    // Advance the fake clock (was QTest::qWait(20)) so saveCheckpoint_locked's
    // wallClockMs != 0 guard (timetracker.cpp:1653-1656) does not skip the
    // checkpoint write.
    clock.advance(20);
    tracker.triggerCheckpoint_dbg();
    const auto ongoingBeforeSave = tracker.getOngoingDuration();
    QVERIFY(ongoingBeforeSave.has_value());
    const SegmentId oldCheckpointSegmentId = ongoingBeforeSave->segment_id;

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));
        dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Activity));
        QVERIFY(dialog.editSession_dbg().hasEdits());
        dialog.done(QDialog::Accepted);
        QVERIFY(dialog.saveChanges());
    }

    // (!segment_id.isEmpty() is not asserted here: getOngoingDuration()
    // returning a value already implies a minted, non-empty segment_id --
    // TimeDuration's only ctor is private and mints one whenever the id
    // passed in is empty (types.h:150) -- so the check could not fail.)
    const auto ongoingAfterSave = tracker.getOngoingDuration();
    QVERIFY(ongoingAfterSave.has_value());
    QCOMPARE(ongoingAfterSave->segment_id.toString(), oldCheckpointSegmentId.toString());

    clock.advance(20);
    tracker.triggerCheckpoint_dbg();

    const QString connName = "historydialog_t6_case1";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery historicalQuery(db);
        historicalQuery.prepare(
            "SELECT type FROM durations WHERE start_utc = :start_utc AND end_utc = :end_utc AND is_finalized = 1"
        );
        historicalQuery.bindValue(":start_utc", historicalStart.toUTC().toString(Qt::ISODateWithMs));
        historicalQuery.bindValue(":end_utc", historicalEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(historicalQuery.exec());
        QVERIFY(historicalQuery.next());
        QCOMPARE(historicalQuery.value(0).toInt(), static_cast<int>(DurationType::Activity));

        QSqlQuery checkpointQuery(db);
        checkpointQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0");
        QVERIFY(checkpointQuery.next());
        QCOMPARE(checkpointQuery.value(0).toInt(), 1);

        QSqlQuery checkpointIdQuery(db);
        checkpointIdQuery.prepare("SELECT segment_id FROM durations WHERE is_finalized = 0");
        QVERIFY(checkpointIdQuery.exec());
        QVERIFY(checkpointIdQuery.next());
        QCOMPARE(checkpointIdQuery.value(0).toString(), oldCheckpointSegmentId.toString());

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_historydialog_checkpoint_timer_keeps_running_for_dialog_lifetime()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db2(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db2);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    // Advance the fake clock (was QTest::qWait(20)) so triggerCheckpoint_dbg()
    // below has a nonzero wallClockMs and is not skipped by
    // saveCheckpoint_locked's wallClockMs != 0 guard.
    clock.advance(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        // WP6: an open History dialog no longer stops or reanchors the live
        // checkpoint timer -- it keeps running, with its phase unchanged.
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(tracker.isCheckpointTimerActive_dbg());

        tracker.triggerCheckpoint_dbg();

        const QString connName = "historydialog_t6_case2_open";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
            db.setDatabaseName(db_path_);
            QVERIFY(db.open());

            QSqlQuery countQuery(db);
            QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
            QVERIFY(countQuery.next());
            QCOMPARE(countQuery.value(0).toInt(), 1);

            db.close();
        }
        QSqlDatabase::removeDatabase(connName);
    }

    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.triggerCheckpoint_dbg();

    const QString connName = "historydialog_t6_case2_closed";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery countQuery(db);
        QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(countQuery.next());
        QCOMPARE(countQuery.value(0).toInt(), 1);

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_historydialog_save_keeps_db_rows_for_history_plus_current_session()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime historicalStart(QDate::currentDate().addDays(-1), QTime(9, 0, 0));
    const QDateTime historicalEnd = historicalStart.addSecs(60);
    std::deque<TimeDuration> historicalRows;
    historicalRows.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, historicalStart, historicalEnd));
    TestCommon::seedStoredRows(db, historicalRows, true);

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-30), now.addSecs(-20)));
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-20), now.addSecs(-10)));

    HistoryDialog dialog(tracker, settings);

    // A10: an editless OK is a no-op, so make a genuine edit (toggle the
    // historical row's type on its page) to exercise the save path.
    QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));
    dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Activity));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    dialog.done(QDialog::Accepted);
    QVERIFY(dialog.saveChanges());

    // H5/WP5: the date-scoped save writes only the edited date. The historical
    // row is persisted with its new type; the two current-session rows are
    // neither written nor dropped -- they stay in the live session and
    // persist on the tracker's own commit path, not via History.
    const int expectedRows = 1;
    const auto completedAfterSave = tracker.completedTimeline();
    QCOMPARE(completedAfterSave.completed().size(), static_cast<size_t>(2));

    const QString connName = "historydialog_t7_row_count";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery totalQuery(db);
        QVERIFY(totalQuery.exec("SELECT COUNT(*) FROM durations"));
        QVERIFY(totalQuery.next());
        QCOMPARE(totalQuery.value(0).toInt(), expectedRows);

        QSqlQuery typeQuery(db);
        QVERIFY(typeQuery.exec("SELECT type FROM durations"));
        QVERIFY(typeQuery.next());
        QCOMPARE(typeQuery.value(0).toInt(), static_cast<int>(DurationType::Activity));

        QSqlQuery finalizedQuery(db);
        QVERIFY(finalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 1"));
        QVERIFY(finalizedQuery.next());
        QCOMPARE(finalizedQuery.value(0).toInt(), expectedRows);

        QSqlQuery unfinalizedQuery(db);
        QVERIFY(unfinalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(unfinalizedQuery.next());
        QCOMPARE(unfinalizedQuery.value(0).toInt(), 0);

        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_splitdialog_default_types_and_bounds()
{
    QDateTime start = makeTime(0);
    QDateTime end = makeTime(10 * 1000);
    SplitDialog dialog(start, end);

    QCOMPARE(dialog.getFirstSegmentType(), DurationType::Pause);
    QCOMPARE(dialog.getSecondSegmentType(), DurationType::Activity);
    QCOMPARE(dialog.slider_dbg()->minimum(), 1);
    QCOMPARE(dialog.slider_dbg()->maximum(), 9);
    QCOMPARE(dialog.getSplitTime(), start.addSecs(dialog.slider_dbg()->value()));
}

void HistoryDialogTest::test_splitdialog_short_duration_disables_slider()
{
    QDateTime start = makeTime(0);
    QDateTime end = makeTime(2 * 1000);
    SplitDialog dialog(start, end);

    QCOMPARE(dialog.slider_dbg()->minimum(), 1);
    QCOMPARE(dialog.slider_dbg()->maximum(), 1);
    QCOMPARE(dialog.slider_dbg()->value(), 1);
    QVERIFY(!dialog.slider_dbg()->isEnabled());
}

void HistoryDialogTest::test_splitdialog_minimum_duration_boundaries()
{
    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(2 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(!dialog.slider_dbg()->isEnabled());
        QCOMPARE(dialog.slider_dbg()->minimum(), 1);
        QCOMPARE(dialog.slider_dbg()->maximum(), 1);
    }

    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(3 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(dialog.slider_dbg()->isEnabled());
        QCOMPARE(dialog.slider_dbg()->minimum(), 1);
        QCOMPARE(dialog.slider_dbg()->maximum(), 2);
    }

    {
        QDateTime start = makeTime(0);
        QDateTime end = makeTime(4 * 1000);
        SplitDialog dialog(start, end);
        QVERIFY(dialog.slider_dbg()->isEnabled());
        QCOMPARE(dialog.slider_dbg()->minimum(), 1);
        QCOMPARE(dialog.slider_dbg()->maximum(), 3);
    }
}

void HistoryDialogTest::test_splitdialog_setters_affect_types()
{
    QDateTime start = makeTime(0);
    QDateTime end = makeTime(8 * 1000);
    SplitDialog dialog(start, end);

    dialog.setFirstSegmentType(DurationType::Activity);
    QCOMPARE(dialog.getFirstSegmentType(), DurationType::Activity);
    QCOMPARE(dialog.getSecondSegmentType(), DurationType::Pause);

    dialog.setSecondSegmentType(DurationType::Activity);
    QCOMPARE(dialog.getFirstSegmentType(), DurationType::Pause);
    QCOMPARE(dialog.getSecondSegmentType(), DurationType::Activity);
}

// ============================================================================
// Test R — round-trip type toggle via accept updates the store
// ============================================================================

void HistoryDialogTest::test_R_round_trip_type_toggle_via_accept()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime now = fixedNow().addDays(-1);
    TestCommon::seedStoredRows(db,
        {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50))}, true);

    {
        HistoryDialog dialog(tracker, settings);
        QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed()[0].type, DurationType::Activity);

        // Toggle to Pause via Timeline API
        dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause));
        QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed()[0].type, DurationType::Pause);
        QVERIFY(dialog.editSession_dbg().hasEdits());

        dialog.done(QDialog::Accepted);
        QVERIFY(dialog.saveChanges());
    }

    // The store reflects the change
    SqliteSessionStore db2(settings);
    const auto loaded = db2.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded.durations[0].type, DurationType::Pause);
}

// ============================================================================
// Test S — cancel preserves state
// ============================================================================

void HistoryDialogTest::test_S_cancel_preserves_state()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime now = fixedNow().addDays(-1);
    TestCommon::seedStoredRows(db,
        {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50))}, true);

    {
        HistoryDialog dialog(tracker, settings);
        // Toggle to Pause but cancel
        dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause));
        QVERIFY(dialog.editSession_dbg().hasEdits());

        dialog.done(QDialog::Rejected);
        dialog.saveChanges();  // saveChanges checks result() and returns early when Rejected
    }

    // The store is unchanged — still Activity
    SqliteSessionStore db2(settings);
    const auto loaded = db2.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(1));
    QCOMPARE(loaded.durations[0].type, DurationType::Activity);
}

void HistoryDialogTest::test_saveChanges_noop_save_unchanged()
{
    // A10: an editless OK is a true no-op — it must not rewrite the DB. The
    // pre-existing history row is left byte-for-byte untouched, and the unpersisted
    // in-memory session row stays in the live session (it persists on stop, not
    // via an editless dialog visit).
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate today = QDate::currentDate();
    const QDateTime dbStart(today, QTime(8, 0, 0), Qt::UTC);
    const QDateTime dbEnd(today, QTime(8, 30, 0), Qt::UTC);
    const QDateTime memStart(today, QTime(9, 0, 0), Qt::UTC);
    const QDateTime memEnd(today, QTime(9, 30, 0), Qt::UTC);

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, dbStart, dbEnd));
    TestCommon::seedStoredRows(db, dbDurations, true);

    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryDialog dialog(tracker, settings);
    QVERIFY(!dialog.editSession_dbg().hasEdits());
    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    // The DB is unchanged: only the original history Pause row, untouched.
    const QString connName = "historydialog_noop_save";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());

        QSqlQuery countQuery(sqlDb);
        QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations"));
        QVERIFY(countQuery.next());
        QCOMPARE(countQuery.value(0).toInt(), 1);

        QSqlQuery pauseQuery(sqlDb);
        QVERIFY(pauseQuery.exec("SELECT start_utc, end_utc FROM durations WHERE type = " +
            QString::number(static_cast<int>(DurationType::Pause))));
        QVERIFY(pauseQuery.next());
        QCOMPARE(QDateTime::fromString(pauseQuery.value(0).toString(), Qt::ISODateWithMs).toUTC().time(), QTime(8, 0, 0));
        QCOMPARE(QDateTime::fromString(pauseQuery.value(1).toString(), Qt::ISODateWithMs).toUTC().time(), QTime(8, 30, 0));

        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // The in-memory session row is retained in the live session (unpersisted).
    const auto completedAfterSave = tracker.completedTimeline();
    QCOMPARE(completedAfterSave.completed().size(), static_cast<size_t>(1));
    QCOMPARE(completedAfterSave.completed()[0].startTime.toUTC().time(), QTime(9, 0, 0));
}

// A10: an editless OK must be a true no-op. Two adjacent same-type DB rows with
// a sub-500 ms gap (which normalization would merge) must survive an editless OK
// byte-for-byte — no merge, no delete-all rewrite.
void HistoryDialogTest::test_saveChanges_editless_ok_leaves_mergeable_rows_untouched()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Two adjacent Activity rows on a past day with a 100 ms gap (< 500 ms).
    const QDate day = QDate::currentDate().addDays(-1);
    const QDateTime aStart(day, QTime(10, 0, 0, 0), Qt::UTC);
    const QDateTime aEnd(day, QTime(10, 0, 5, 0), Qt::UTC);
    const QDateTime bStart(day, QTime(10, 0, 5, 100), Qt::UTC); // 100 ms gap
    const QDateTime bEnd(day, QTime(10, 0, 10, 0), Qt::UTC);
    std::deque<TimeDuration> dbRows;
    dbRows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, aStart, aEnd));
    dbRows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, bStart, bEnd));
    TestCommon::seedStoredRows(db, dbRows, true);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(!dialog.editSession_dbg().hasEdits());
    dialog.done(QDialog::Accepted);
    // If the no-op guard were absent, saveChanges() would normalize and merge,
    // prompting for confirmation; supply "Yes" so that path would proceed.
    dialog.debugOverlapAnswer_ = QMessageBox::Yes;
    dialog.saveChanges();

    // Both rows must survive byte-for-byte (no merge occurred).
    SqliteSessionStore verify(settings);
    auto loaded = verify.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(2));
    QCOMPARE(loaded.durations[0].startTime.toUTC(), aStart);
    QCOMPARE(loaded.durations[0].endTime.toUTC(), aEnd);
    QCOMPARE(loaded.durations[1].startTime.toUTC(), bStart);
    QCOMPARE(loaded.durations[1].endTime.toUTC(), bEnd);
}

void HistoryDialogTest::test_H4_overlap_cancel_aborts_save()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // WP4b: not Today-specific -- both rows moved onto a past-date page
    // seeded in the store (page 1; page 0 is the now-empty Today page),
    // rather than today's DB row plus a memory row.
    const QDate pastDate = QDate::currentDate().addDays(-1);
    const QDateTime dbStart(pastDate, QTime(11, 0, 0), Qt::UTC);
    const QDateTime dbEnd(pastDate, QTime(11, 30, 0), Qt::UTC);
    const QDateTime secondStart(pastDate, QTime(11, 15, 0), Qt::UTC);
    const QDateTime secondEnd(pastDate, QTime(11, 45, 0), Qt::UTC);

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, dbStart, dbEnd));
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, secondStart, secondEnd));
    TestCommon::seedStoredRows(db, dbDurations, true);

    HistoryDialog dialog(tracker, settings);

    // An editless OK is a no-op that never reaches the merge prompt. Make a
    // genuine same-type edit (split row 0 into two Activity halves) so the
    // save runs and normalization asks to merge the overlapping cluster.
    dialog.applySplit_dbg(0, 1, DurationType::Activity, DurationType::Activity);
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // Bypass QMessageBox::question() to avoid offscreen QPA propagateSizeHints() warnings.
    // Supplies "No" — same as user declining the merge — so saveChanges() aborts.
    dialog.debugOverlapAnswer_ = QMessageBox::No;
    dialog.done(QDialog::Accepted);
    QVERIFY(!dialog.saveChanges());

    // Save aborted — DB should still have exactly 2 finalized rows (original)
    const QString connName = "historydialog_h4_cancel";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());
        QSqlQuery q(sqlDb);
        QVERIFY(q.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 1"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 2);
        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_H2_midnight_crossing_shown_on_both_pages()
{
    // A cross-midnight segment (starts 23:30 yesterday, ends 00:30 today) must appear
    // on BOTH day pages. The start-day (yesterday) page holds the canonical (editable)
    // copy; the end-day (today) page holds a display-only continuation copy.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime segStart(yesterday, QTime(23, 30, 0));
    const QDateTime segEnd = segStart.addSecs(3600); // ends at 00:30 today

    // Insert cross-midnight segment directly via SQL.
    {
        const QString connName = "h2_test_setup";
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        // DB may not exist yet — seed it via seedStoredRows first.
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }
    // Seed DB to create the schema.
    TestCommon::seedStoredRows(db, {}, true);
    {
        const QString connName = "h2_insert";
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :s, :e, 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId().toString());
        q.bindValue(":s", segStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", segEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    HistoryDialog dialog(tracker, settings);

    // Two pages: today (isCurrent) and yesterday.
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pages()[0].isCurrent, true);
    QCOMPARE(dialog.editSession_dbg().pages()[1].isCurrent, false);

    // Yesterday page: cross-midnight row is in crossMidnight (display-only canonical),
    // not in pendingTimelines() (which holds only same-day editable rows).
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(0));
    QCOMPARE(dialog.editSession_dbg().pages()[1].crossMidnight.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pages()[1].crossMidnight[0].startTime.date(), yesterday);
    QCOMPARE(dialog.editSession_dbg().pages()[1].continuations.size(), static_cast<size_t>(0));

    // Today page: continuation copy (display-only).
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations.size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations[0].startTime.date(), yesterday);
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations[0].endTime.date(), QDate::currentDate());
    QCOMPARE(dialog.editSession_dbg().pages()[0].crossMidnight.size(), static_cast<size_t>(0));

    // Both show the same segment_id.
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations[0].segment_id,
             dialog.editSession_dbg().pages()[1].crossMidnight[0].segment_id);

    // Cross-midnight row is NOT in pendingTimelines_ (Timeline rejects cross-midnight).
    // It is display-only (see pages[]/crossMidnight above) and, H5/WP5, never
    // part of a scoped save's diff, so a save leaves it untouched.
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(0));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(0));
}

void HistoryDialogTest::test_H14_cross_midnight_totals_and_counts()
{
    // Seed a finalized cross-midnight Activity row: 23:50 yesterday → 00:10 today (20 min total).
    // Start-day (yesterday) page should show 10 min Activity in totals and count the row.
    // End-day (today) page (continuation) should show 10 min Activity in totals.
    // Both portions together sum to 20 min — no double-counting.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime segStart(yesterday, QTime(23, 50, 0), Qt::LocalTime);
    const QDateTime segEnd(QDate::currentDate(), QTime(0, 10, 0), Qt::LocalTime); // 20 min total

    // Seed DB schema, then insert cross-midnight row directly via SQL.
    TestCommon::seedStoredRows(db, {}, true);
    {
        const QString connName = "h14_insert";
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :s, :e, 1)"
        );
        q.bindValue(":sid", TimeDuration::createSegmentId().toString());
        q.bindValue(":s", segStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":e", segEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    HistoryDialog dialog(tracker, settings);

    // Two pages: today (isCurrent, index 0) and yesterday (index 1).
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pages()[0].isCurrent, true);
    QCOMPARE(dialog.editSession_dbg().pages()[1].isCurrent, false);

    // Yesterday page: has the crossMidnight row (display-only) — count must be 1, not 0.
    QCOMPARE(dialog.editSession_dbg().pages()[1].crossMidnight.size(), static_cast<size_t>(1));
    QVERIFY(dialog.editSession_dbg().pageTitle(1).contains("entries: 1"));

    // Today page: has the continuation row — count must be 1, not 0.
    QCOMPARE(dialog.editSession_dbg().pages()[0].continuations.size(), static_cast<size_t>(1));
    QVERIFY(dialog.editSession_dbg().pageTitle(0).contains("entries: 1"));

    // Today page is page 0 — pageLabel_ is populated in the constructor via updateTable(0).
    // Reading its text does not require the dialog to be shown.
    const QString todayLabel = dialog.pageLabel_dbg()->text();
    QVERIFY2(todayLabel.contains("00:10:00"),
             qPrintable("Today page label should contain 00:10:00 but got: " + todayLabel));

    // Navigate to yesterday's page and verify its totals.
    dialog.updateTotalsLabel_dbg(1);
    const QString yesterdayLabel = dialog.pageLabel_dbg()->text();
    QVERIFY2(yesterdayLabel.contains("00:10:00"),
             qPrintable("Yesterday page label should contain 00:10:00 but got: " + yesterdayLabel));

    // The two portions together must sum to the full 20-minute span.
    // Extract activity from each page's pending + display-only totals.
    // Today: continuation portion = 00:10:00, Yesterday: crossMidnight portion = 00:10:00.
    // Neither page should show 00:00:00 Activity (which was the pre-fix bug).
    QVERIFY(!todayLabel.contains("Activity: 00:00:00"));
    QVERIFY(!yesterdayLabel.contains("Activity: 00:00:00"));
}

void HistoryDialogTest::test_H6_split_dialog_preset_from_source_row_type()
{
    // onSplitRow() presets SplitDialog's first-segment type to match the
    // source row's type. An Activity source row presets Activity→Pause;
    // a Pause source row presets Pause→Activity.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime now = fixedNow().addDays(-1);
    // Row 0 is Pause, row 1 is Activity — exercise both directions of the preset.
    TestCommon::seedStoredRows(db, {
        TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-20), now.addSecs(-14)),
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-12), now.addSecs(-4))}, true);

    HistoryDialog dialog(tracker, settings);

    // splitDialogPresetFirstType_dbg() runs onSplitRow()'s real preset step
    // (dlg.setFirstSegmentType(duration.type)) without realizing the dialog, so this
    // asserts the production source-row-type -> preset wiring, not a value we supply.
    QCOMPARE(dialog.splitDialogPresetFirstType_dbg(0, 1), DurationType::Pause);
    QCOMPARE(dialog.splitDialogPresetFirstType_dbg(1, 1), DurationType::Activity);
}

// ---------------------------------------------------------------------------
// F1: a failed history load must refuse destructive edited saves
// ---------------------------------------------------------------------------

void HistoryDialogTest::test_F1_saveChanges_refuses_when_history_load_failed()
{
    // Money test: a failed history load must never allow saveChanges() to
    // reach saveHistoryEdit(). H5/WP5: under the date-scoped model a failed
    // open-time load also makes every page read-only (HistoryEligibility),
    // so no edit can be made at all -- the refusal is observable because an
    // editless save would otherwise return true (a no-op success), while
    // the historyLoadFailed() gate runs before the no-edit early return.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.loadDurationsResult.status = SessionStoreResult::TransientError;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().historyLoadFailed());
    QVERIFY(dialog.editSession_dbg().pageCount() >= 1);
    for (size_t i = 0; i < dialog.editSession_dbg().pageCount(); ++i)
        QVERIFY(!dialog.editSession_dbg().pageEditable(i));
    // An attempted edit is refused, so the save below is editless.
    QVERIFY(!dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause)));
    QVERIFY(!dialog.editSession_dbg().hasEdits());

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);

    QVERIFY(!dialog.saveChanges());
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 0);
}

void HistoryDialogTest::test_F1_banner_shows_when_history_load_failed()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.loadDurationsResult.status = SessionStoreResult::TransientError;
    TimeTracker tracker(settings, fakeDb);

    HistoryDialog dialog(tracker, settings);

    QVERIFY(dialog.getLoadReconciliationMessage().contains("could not be loaded"));

    // MEDIUM-1 (D11 review): also cover the loadReconciliationLabel_dbg()
    // display wiring -- the pure string builder alone doesn't prove
    // setupUI() actually wires the message into the visible banner label.
    QVERIFY(dialog.loadReconciliationLabel_dbg() != nullptr);
    QVERIFY(dialog.loadReconciliationLabel_dbg()->text().contains("could not be loaded"));
    QVERIFY(!dialog.loadReconciliationLabel_dbg()->isHidden());
}

void HistoryDialogTest::test_C3_saveChanges_refuses_when_edit_creates_violation()
{
    // The dialog's scoped.violation refusal branch (historydialog.cpp) must
    // still block a save when the user's OWN edit creates the invalid shape
    // -- MUST-FIX2 only narrows the pre-existing, unedited case.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime now = fixedNow().addDays(-1);
    const SegmentId activityId = SegmentId::mint();
    const SegmentId pauseId = SegmentId::mint();
    // Originally non-overlapping.
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-400), now.addSecs(-300), activityId));
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-200), now.addSecs(-100), pauseId));

    TimeTracker tracker(settings, fakeDb);
    HistoryDialog dialog(tracker, settings);

    // Edit: move the Pause row's start so it now overlaps the untouched
    // Activity row (same id -- still "the same" row, just moved by the edit).
    std::deque<TimeDuration> edited{
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-400), now.addSecs(-300), activityId),
        TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-350), now.addSecs(-250), pauseId)
    };
    dialog.editSession_dbg().setPageTimeline(1, Timeline(edited, std::nullopt));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);

    QVERIFY(!dialog.saveChanges());
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 0);
}

// ============================================================================
// D5 review fix (finding 1/2, R15-002): HistoryDialog's shutdown-refusal
// gate is per-dialog (TimeTracker::shutdownEpoch()), not global
// (wasShutDown()) -- cancelShutdown() must not unprotect a dialog that
// spanned the shutdown, but must allow one opened only afterward.
// ============================================================================

void HistoryDialogTest::test_D5_dialog_opened_after_cancelShutdown_saves_successfully()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    // A prior session is shut down (e.g. WM_QUERYENDSESSION), then the
    // shutdown/logoff is cancelled (WM_ENDSESSION(FALSE)) -- all before this
    // dialog is ever opened.
    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);
    coordinator.run();
    coordinator.cancelShutdown();
    QVERIFY(!tracker.wasShutDown());

    // A completed segment to edit -- withSegmentType(0, ...) below needs a
    // completed() entry at index 0. No new Start here: D5 requires History
    // to work immediately after cancellation, not after a fresh session.
    const QDateTime now = fixedNow().addDays(-1);
    TestCommon::seedStoredRows(fakeDb,
        {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-200), now.addSecs(-150))}, true);

    // Act: the dialog is opened only now, strictly after the shutdown/
    // cancellation above -- D5's own requirement (History works immediately,
    // no new Start needed) is exercised by the Start above already having
    // succeeded; this proves the dialog itself is not still refused.
    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    fakeDb.callLog.clear();
    dialog.done(QDialog::Accepted);

    // Assert
    QVERIFY(dialog.saveChanges());
    QVERIFY(fakeDb.callLog.contains("saveHistoryDays"));
}

void HistoryDialogTest::test_D5_dialog_spanning_cancelled_shutdown_refuses_save()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);
    // A completed segment to edit -- withSegmentType(0, ...) below needs a
    // completed() entry at index 0, not just the live ongoing segment.
    const QDateTime now = fixedNow().addDays(-1);
    TestCommon::seedStoredRows(fakeDb,
        {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-200), now.addSecs(-150))}, true);

    // The dialog opens while the session is still live -- its snapshot
    // predates the shutdown that is about to run.
    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // WM_QUERYENDSESSION: the OS runs the Direct shutdown while the dialog
    // is still open (a nested modal event loop does not block nativeEvent
    // delivery). It stops and finalizes the live segment the dialog never saw.
    coordinator.run();
    QVERIFY(tracker.wasShutDown());
    // WM_ENDSESSION(FALSE): the shutdown/logoff is cancelled.
    coordinator.cancelShutdown();
    QVERIFY(!tracker.wasShutDown());

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);

    // Assert: even though wasShutDown() is now false, this dialog's own
    // snapshot spans the shutdown, so it must still be refused -- otherwise
    // the segment the shutdown just committed would be silently dropped.
    QVERIFY(!dialog.saveChanges());
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 0);
}

// ============================================================================
// D9 (R14-002): dialog is a renderer/controller of typed model operations.
// ============================================================================

void HistoryDialogTest::test_D9_split_updates_table_rowcount_and_title_and_highlights()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QDateTime start = fixedNow().addDays(-1).addSecs(-10);
    QDateTime end = fixedNow().addDays(-1).addSecs(-2);
    TestCommon::seedStoredRows(db, {TimeDuration::fromPersistedRow(DurationType::Activity, start, end)}, true);

    HistoryDialog dialog(tracker, settings);
    dialog.onOlder_dbg();  // page 1: yesterday

    QCOMPARE(dialog.table_dbg()->rowCount(), 1);
    QVERIFY(dialog.editSession_dbg().pageTitle(1).contains("entries: 1"));
    QVERIFY(!dialog.editSession_dbg().pageModified(1));

    // D9 review (finding 2): applySplit_dbg() must share onSplitRow()'s
    // production tail (session_.splitRow() + updateTable()) so a debug-only
    // path cannot silently diverge from what a real split renders.
    dialog.applySplit_dbg(0, 1, DurationType::Activity, DurationType::Pause);

    // Model side: count/title/dirty state update immediately.
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(2));
    QVERIFY(dialog.editSession_dbg().pageTitle(1).contains("entries: 2"));
    QVERIFY(dialog.editSession_dbg().pageModified(1));

    // Rendering side: the table itself must reflect the split and be highlighted.
    QCOMPARE(dialog.table_dbg()->rowCount(), 2);
    QVERIFY(dialog.pageLabel_dbg()->text().contains("entries: 2"));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background().color(), QColor(180, 216, 228, 255));
    QCOMPARE(dialog.table_dbg()->item(1, 0)->background().color(), QColor(180, 216, 228, 255));
}

void HistoryDialogTest::test_D9_checkbox_toggle_highlights_row_and_marks_dirty()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QDateTime now = fixedNow().addDays(-1);
    TestCommon::seedStoredRows(db, {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50))}, true);

    HistoryDialog dialog(tracker, settings);
    dialog.onOlder_dbg();  // page 1: yesterday

    QVERIFY(!dialog.editSession_dbg().pageModified(1));

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);
    box->setChecked(false);

    QVERIFY(dialog.editSession_dbg().pageModified(1));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background().color(), QColor(180, 216, 228, 255));
    QCOMPARE(dialog.table_dbg()->item(0, 1)->background().color(), QColor(180, 216, 228, 255));
    QCOMPARE(dialog.table_dbg()->item(0, 2)->background().color(), QColor(180, 216, 228, 255));
}

void HistoryDialogTest::test_D9_navigation_preserves_per_page_dirty_state()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime start(QDate::currentDate().addDays(-1), QTime(10, 0, 0));
    const QDateTime end = start.addSecs(300);

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));
    TestCommon::seedStoredRows(db, dbDurations, true);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));

    dialog.onOlder_dbg();  // page 1: yesterday
    QVERIFY(!dialog.editSession_dbg().pageModified(1));

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);
    box->setChecked(false);
    QVERIFY(dialog.editSession_dbg().pageModified(1));

    dialog.onNewer_dbg();  // back to page 0 (untouched)
    QVERIFY(!dialog.editSession_dbg().pageModified(0));

    dialog.onOlder_dbg();  // page 1 again — dirty state and highlight survive re-render
    QVERIFY(dialog.editSession_dbg().pageModified(1));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background().color(), QColor(180, 216, 228, 255));
}

void HistoryDialogTest::test_D9_cancel_after_split_and_toggle_preserves_runtime_state()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    QDateTime start = fixedNow().addDays(-1).addSecs(-10);
    QDateTime end = fixedNow().addDays(-1).addSecs(-2);
    TestCommon::seedStoredRows(fakeDb, {TimeDuration::fromPersistedRow(DurationType::Activity, start, end)}, true);

    {
        HistoryDialog dialog(tracker, settings);
        dialog.onOlder_dbg();  // page 1: yesterday
        dialog.applySplit_dbg(0, 1, DurationType::Activity, DurationType::Pause);
        QVERIFY(dialog.editSession_dbg().pageModified(1));

        // Also toggle the first half's type -- named "split and toggle",
        // exercise both operations before cancelling.
        QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
        QVERIFY(box != nullptr);
        box->setChecked(false);

        fakeDb.callLog.clear();
        dialog.done(QDialog::Rejected);
        dialog.saveChanges();  // returns false immediately: result() != Accepted
    }

    // No store writes occurred for either the split or the toggle.
    QVERIFY(!fakeDb.callLog.contains("saveHistoryDays"));

    // The store is unchanged — still the original single Activity row.
    const auto loaded = fakeDb.loadDurations();
    QCOMPARE(loaded.durations.size(), static_cast<size_t>(1));
    QCOMPARE(loaded.durations[0].type, DurationType::Activity);
    QCOMPARE(loaded.durations[0].startTime, start);
    QCOMPARE(loaded.durations[0].endTime, end);
}

// ---------------------------------------------------------------------------
// D9 review (finding 2 (g)): sequence tests where hasEdits()/pageModified()
// and the rendered highlight must always agree -- these are the regression
// tests for finding 1 (a single, model-sourced highlight rule).
// ---------------------------------------------------------------------------

void HistoryDialogTest::test_D9_toggle_off_then_on_clears_highlight_and_dirty_state()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QDateTime now = fixedNow().addDays(-1);
    TestCommon::seedStoredRows(db,
        {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50))}, true);

    HistoryDialog dialog(tracker, settings);
    dialog.onOlder_dbg();  // page 1: yesterday

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);

    box->setChecked(false); // Activity -> Pause
    box->setChecked(true);  // Pause -> Activity: back to the original value

    QVERIFY(!dialog.editSession_dbg().pageModified(1));
    QVERIFY(!dialog.editSession_dbg().hasEdits());
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background(), QBrush());
}

void HistoryDialogTest::test_D9_toggle_twice_clears_highlight()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QDateTime now = fixedNow().addDays(-1);
    TestCommon::seedStoredRows(db,
        {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50))}, true);

    HistoryDialog dialog(tracker, settings);
    dialog.onOlder_dbg();  // page 1: yesterday

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);

    box->setChecked(false);
    QVERIFY(dialog.editSession_dbg().pageModified(1));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background().color(), QColor(180, 216, 228, 255));

    box->setChecked(true); // same net effect as the pair above, checked separately
    QVERIFY(!dialog.editSession_dbg().pageModified(1));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background(), QBrush());
}

void HistoryDialogTest::test_D9_split_too_short_row_does_not_open_modal()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Anchored to a fixed local noon-ish time on yesterday's date (not real
    // now) so the row can never straddle local midnight -- see the house
    // convention at test_timer.cpp:78 and git show 5e63eae. Yesterday is
    // page 1: pendingTimelines()[1] below.
    const QDateTime end(QDate::currentDate().addDays(-1), QTime(12, 0, 0), Qt::LocalTime);
    const QDateTime start = end.addMSecs(-2500); // 2.5s -- under the 3s minimum
    TestCommon::seedStoredRows(db,
        {TimeDuration::fromPersistedRow(DurationType::Activity, start, end)}, true);

    HistoryDialog dialog(tracker, settings);
    dialog.debugSkipSplitMessages_ = true;

    // Hang guard: if the too-short-row refusal ever regresses, SplitDialog
    // would open a real modal under offscreen QPA and dlg.exec() below would
    // block this test forever. Auto-close any modal that appears so the
    // assertion fails cleanly instead of hanging the whole suite.
    QTimer::singleShot(200, &dialog, []() {
        if (QWidget* w = QApplication::activeModalWidget())
            w->close();
    });

    dialog.setContextMenuTarget_dbg(0, 1);
    dialog.onSplitRow_dbg();

    // D9 review (finding 1, round 3): refused before SplitDialog was ever
    // constructed -- before this fix, the modal always opened here (with a
    // disabled slider but live type radios and OK) and the row was only
    // rejected after the user pressed OK. Asserting TooShortInfo (rather
    // than just "not DialogOpened") distinguishes this outcome from
    // SplitDialog having opened.
    QCOMPARE(dialog.debugLastSplitUi_, HistoryDialog::DebugSplitUi::TooShortInfo);
    // D11 HIGH-1 fix: debugLastSplitMessage_ is captured from the same local
    // variable passed to the (suppressed) QMessageBox::information() call,
    // so this also asserts on the actual message content that would be
    // shown, not just an opaque branch tag.
    QVERIFY(dialog.debugLastSplitMessage_.contains("too short to split meaningfully"));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(1));
    QVERIFY(!dialog.editSession_dbg().hasEdits());
}

void HistoryDialogTest::test_split_ongoing_row_entry_point_is_inert()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // A running Today page: an ongoing (unfinished) segment on page 0.
    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);

    HistoryDialog dialog(tracker, settings);
    const auto& todayTimeline = dialog.editSession_dbg().pendingTimelines()[0];
    QVERIFY(todayTimeline.ongoing().has_value());

    // Target the ongoing row directly via the debug seams -- this bypasses
    // showContextMenu()'s own guard (historydialog.cpp: "Don't allow split
    // on the ongoing row"), so this proves onSplitRow() itself refuses it
    // defensively, not just the context-menu wiring.
    dialog.setContextMenuTarget_dbg(static_cast<int>(todayTimeline.completed().size()), 0);
    dialog.onSplitRow_dbg();

    QCOMPARE(dialog.debugLastSplitUi_, HistoryDialog::DebugSplitUi::None);
    QVERIFY(!dialog.editSession_dbg().hasEdits());

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryDialogTest::test_revoked_dialog_refuses_save_and_leaves_store_untouched()
{
    // WP1 A1: the dialog refuses to save when the edit is revoked, and the store is untouched.
    // Per the plan's spec: real SQLite with one historical Pause row, falsifiable
    // 'closed' assertion via QSignalSpy, and segment_id tracking.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    const QDateTime now = fixedNow();

    // Save a Pause row to the DB (historical, yesterday)
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        now.addDays(-1).addSecs(-200), now.addDays(-1).addSecs(-150)));

    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(now);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Write the historical row to the DB
    TestCommon::seedStoredRows(db, dbDurations, true);

    // Start the timer to create the ongoing segment
    tracker.useTimerViaButton(Button::Start);
    clock.advance(5); // Ensure positive-length ongoing

    // Capture the segment_id before the revoke
    auto ongoingBefore = tracker.getOngoingDuration();
    QVERIFY(ongoingBefore.has_value());
    const SegmentId originalSegmentId = ongoingBefore->segment_id;

    // Construct the dialog
    HistoryDialog dialog(tracker, settings);

    // Flip the Pause row to Activity (an edit)
    dialog.editSession_dbg().setPageTimeline(1,
        dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Activity));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // Arrange: create the finished spy before Act (per MEDIUM-1 rule)
    QSignalSpy finished(&dialog, &QDialog::finished);

    // Act: emit the revoke signal
    emit tracker.exclusiveEditRevoked();

    // Assert: the spy reaches count 1 with last() == Rejected
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).toInt(), int(QDialog::Rejected));

    // Retry via setResult(Accepted) + saveChanges(), which exercises the editRevoked_ check
    dialog.setResult(QDialog::Accepted);
    QVERIFY(!dialog.saveChanges());

    // Assert: the spy count is unchanged (no new finished emission)
    QCOMPARE(finished.count(), 1);

    // Assert: SQLite still has the row as Pause
    {
        const QString connName = QString("test_revoked_a1_%1").arg(reinterpret_cast<quintptr>(&db));
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());

        QSqlQuery q(rawDb);
        q.prepare("SELECT type FROM durations WHERE is_finalized = 1");
        QVERIFY(q.exec());
        QVERIFY(q.next());
        // type 0 is Activity, 1 is Pause (per DurationType enum)
        QCOMPARE(q.value(0).toInt(), 1); // Pause

        rawDb.close();
    }
    QSqlDatabase::removeDatabase(QString("test_revoked_a1_%1").arg(reinterpret_cast<quintptr>(&db)));

    // Assert: tracker is Active with the same segment_id
    QVERIFY(tracker.isActive());
    auto ongoingAfter = tracker.getOngoingDuration();
    QVERIFY(ongoingAfter.has_value());
    QCOMPARE(ongoingAfter->segment_id, originalSegmentId);
}

void HistoryDialogTest::test_revoked_dialog_accept_does_not_commit()
{
    // WP1 A2: after revoke, accept() does not commit edits; editRevoked_
    // check is what refuses (not the result() guard).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime now = fixedNow();
    // Add an Activity row yesterday (so it's on page 1)
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addDays(-1).addSecs(-100), now.addDays(-1).addSecs(-50)));

    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(now);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Start the timer
    tracker.useTimerViaButton(Button::Start);
    clock.advance(5); // Ensure positive-length ongoing

    // Construct the dialog
    HistoryDialog dialog(tracker, settings);

    // Make an edit: toggle the Activity row to Pause
    dialog.editSession_dbg().setPageTimeline(1,
        dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // Arrange the finished spy
    QSignalSpy finished(&dialog, &QDialog::finished);

    // Act: emit the revoke signal
    emit tracker.exclusiveEditRevoked();

    // Wait for the finished signal to reach count 1 (the queued close)
    QTRY_COMPARE(finished.count(), 1);

    // Now call accept(), which sets result to Accepted and calls saveChanges()
    fakeDb.callLog.clear();
    dialog.accept();

    // Assert: saveHistoryDays was never called
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 0);

    // The spy count is still 1 (only the revoke rejection), no new Accepted
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).toInt(), int(QDialog::Rejected));

    // result() is Rejected
    QCOMPARE(dialog.result(), QDialog::Rejected);
}

void HistoryDialogTest::test_revoked_editless_accept_same_turn_never_accepts()
{
    // WP1 H1: an editless dialog must not slip through saveChanges()'s
    // no-edit early return when the edit was revoked in the same event turn
    // as accept() -- before the queued closeRevoked() has run.
    // onExclusiveEditRevoked() sets editRevoked_ synchronously (direct
    // connection) but only queues closeRevoked(); without the early
    // editRevoked_ check, accept() -> saveChanges() would return true at
    // the no-edit early return and finish Accepted before the queued
    // rejection ever ran.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime now = fixedNow();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addDays(-1).addSecs(-100), now.addDays(-1).addSecs(-50)));

    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(now);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(5); // Ensure positive-length ongoing

    HistoryDialog dialog(tracker, settings);
    // No edits made: exercises saveChanges()'s no-edit early return.
    QVERIFY(!dialog.editSession_dbg().hasEdits());

    // Arrange: create the finished spy before Act (per MEDIUM-1 rule).
    QSignalSpy finished(&dialog, &QDialog::finished);
    fakeDb.callLog.clear();

    // Act: revoke and accept in the same event turn, before the queued
    // closeRevoked() runs.
    emit tracker.exclusiveEditRevoked();
    dialog.accept();

    // Assert immediately: accept() must not have finished Accepted.
    QCOMPARE(finished.count(), 0);
    QVERIFY(dialog.result() != QDialog::Accepted);

    // Process the queued close: final state is Rejected.
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).toInt(), int(QDialog::Rejected));
    QCOMPARE(dialog.result(), QDialog::Rejected);

    // No DB writes occurred at any point.
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 0);
    QCOMPARE(fakeDb.callLog.count("commitSession"), 0);
}

void HistoryDialogTest::test_historydialog_construction_loads_store_exactly_once()
{
    // S1 / H5-WP5: the constructor's one store read is the
    // loadHistoryRows() call beginExclusiveEdit() makes for the open-time
    // HistoryEditContext; session_.buildFromContext() does no I/O of its own,
    // and nothing reads the store a second time (no loadDurations()).
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime now = fixedNow();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    TimeTracker tracker(settings, fakeDb);

    fakeDb.callLog.clear();
    HistoryDialog dialog(tracker, settings);

    QCOMPARE(fakeDb.callLog.count("loadHistoryRows"), 1);
    QCOMPARE(fakeDb.callLog.count("loadDurations"), 0);

    // Displayed rows unchanged: the seeded DB row still shows on page 0.
    QVERIFY(dialog.table_dbg()->rowCount() >= 1);
}

// ============================================================================
// WP2 (midnight_close_plan.md): a live scheduled midnight stop revokes an
// open edit and closes the dialog, and the cutoff segment is committed via
// the normal no-dialog stop path.
// ============================================================================

void HistoryDialogTest::test_D1_scheduled_midnight_closes_dialog_and_keeps_cutoff_segment()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);

    const QDate today = QDate::currentDate();
    const QDateTime segStart(today, QTime(23, 50, 0), Qt::LocalTime);

    // Historical Pause row the day before.
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        segStart.addDays(-1).addSecs(-200), segStart.addDays(-1).addSecs(-150)));

    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(segStart);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    TestCommon::seedStoredRows(db, dbDurations, true);

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const SegmentId liveSegmentId = tracker.getOngoingDuration()->segment_id;

    const QDateTime cutoff(today, QTime(23, 59, 59, 500), Qt::LocalTime);

    {
        HistoryDialog dialog(tracker, settings);

        // Flip the historical Pause row to Activity (an edit); no prompt raised.
        dialog.editSession_dbg().setPageTimeline(1,
            dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Activity));
        QVERIFY(dialog.editSession_dbg().hasEdits());

        clock.setNow(cutoff);

        // Arrange: create the finished spy before Act (no done() in the Arrange).
        QSignalSpy dialogFinished(&dialog, &QDialog::finished);

        // Act: the real heartbeat tick fires at the boundary.
        tracker.onTick(cutoff);

        // Assert: closed (fails on HEAD, which defers instead of revoking).
        QTRY_COMPARE(dialogFinished.count(), 1);
        QCOMPARE(dialogFinished.last().at(0).toInt(), int(QDialog::Rejected));

        // Retry refused by editRevoked_, not by the result() guard.
        dialog.setResult(QDialog::Accepted);
        QVERIFY(!dialog.saveChanges());
        QCOMPARE(dialogFinished.count(), 1);
    }
    // dialog destroyed here.

    // Assert: SQLite has one finalized Activity row [segStart, cutoff] with
    // the live segment_id, the historical row is still Pause, and no row is
    // left unfinalized.
    {
        const QString connName = QString("test_D1_%1").arg(reinterpret_cast<quintptr>(&db));
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());

        QSqlQuery unfinalized(rawDb);
        QVERIFY(unfinalized.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(unfinalized.next());
        QCOMPARE(unfinalized.value(0).toInt(), 0);

        QSqlQuery cutoffRow(rawDb);
        cutoffRow.prepare("SELECT type, start_utc, end_utc FROM durations WHERE segment_id = ?");
        cutoffRow.addBindValue(liveSegmentId.toString());
        QVERIFY(cutoffRow.exec());
        QVERIFY(cutoffRow.next());
        QCOMPARE(cutoffRow.value(0).toInt(), 0); // Activity
        QCOMPARE(QDateTime::fromString(cutoffRow.value(1).toString(), Qt::ISODateWithMs).toUTC(),
                 segStart.toUTC());
        QCOMPARE(QDateTime::fromString(cutoffRow.value(2).toString(), Qt::ISODateWithMs).toUTC(),
                 cutoff.toUTC());

        QSqlQuery historicalRow(rawDb);
        QVERIFY(historicalRow.exec("SELECT type FROM durations WHERE segment_id != '"
            + liveSegmentId.toString() + "'"));
        QVERIFY(historicalRow.next());
        QCOMPARE(historicalRow.value(0).toInt(), 1); // still Pause

        rawDb.close();
    }
    QSqlDatabase::removeDatabase(QString("test_D1_%1").arg(reinterpret_cast<quintptr>(&db)));
}

void HistoryDialogTest::test_dialog_closes_on_shutdown_and_stays_closed_after_cancel()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    // FakeClock declared before TimeTracker so it outlives it (lifetime rule).
    FakeClock clock(QDateTime(QDate::currentDate(), QTime(9, 0, 0), Qt::LocalTime));
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);

    // Arrange: a completed segment to edit, on yesterday's page.
    const QDateTime now = clock.now().addDays(-1);
    TestCommon::seedStoredRows(fakeDb,
        {TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-200), now.addSecs(-150))}, true);

    // Open the dialog and begin an edit.
    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(1,
        dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // Create the finished spy AFTER arranging the dialog, BEFORE the shutdown.
    QSignalSpy dialogFinished(&dialog, &QDialog::finished);

    fakeDb.callLog.clear();

    // Act: run shutdown, then cancel it.
    coordinator.run();
    coordinator.cancelShutdown();

    // Assert: the dialog closed (revoked).
    QTRY_COMPARE(dialogFinished.count(), 1);
    QCOMPARE(dialogFinished.last().at(0).toInt(), int(QDialog::Rejected));

    // Assert: retry via accept() is refused by D5's shutdown-epoch check.
    dialog.setResult(QDialog::Accepted);
    QVERIFY(!dialog.saveChanges());
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 0);
    QCOMPARE(dialogFinished.count(), 1); // No finished(Accepted) emitted.
}

// ============================================================================
// H5/WP5: date-scoped History save through the dialog.
// ============================================================================

// WP5 acceptance: a save touches only the edited date; every other date's
// rows stay byte-identical (unlike the old whole-table replaceAll()).
void HistoryDialogTest::test_H5_scoped_save_touches_only_edited_date()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDate dayBefore = QDate::currentDate().addDays(-2);
    const TimeDuration edited = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(10, 0, 0)), QDateTime(yesterday, QTime(10, 30, 0)));
    const TimeDuration other = TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(dayBefore, QTime(10, 0, 0)), QDateTime(dayBefore, QTime(10, 30, 0)));
    TestCommon::seedStoredRows(db, {edited, other}, true);
    const QString otherBefore = rowFingerprint(db_path_, other.segment_id.toString());
    const QString editedBefore = rowFingerprint(db_path_, edited.segment_id.toString());
    QVERIFY(!otherBefore.isEmpty());

    HistoryDialog dialog(tracker, settings);
    const int page = pageIndexFor(dialog.editSession_dbg(), yesterday);
    QVERIFY(page >= 0);
    QVERIFY(dialog.editSession_dbg().pageEditable(page));
    QVERIFY(dialog.editSession_dbg().setCompletedType(page, edited.segment_id, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    dialog.done(QDialog::Accepted);
    QVERIFY(dialog.saveChanges());

    // The other date is byte-identical.
    QCOMPARE(rowFingerprint(db_path_, other.segment_id.toString()), otherBefore);
    // The edited row changed type only (same id, same raw start/end text).
    const QStringList before = editedBefore.split('|');
    const QStringList after = rowFingerprint(db_path_, edited.segment_id.toString()).split('|');
    QCOMPARE(after.size(), 4);
    QCOMPARE(after[0].toInt(), static_cast<int>(DurationType::Pause));
    QCOMPARE(after[1], before[1]);
    QCOMPARE(after[2], before[2]);
    QCOMPARE(storedRowCount(db_path_), 2);
}

// WP5 acceptance (replaces the old C3/R13-001/M3 drop tests): a legacy
// single-boundary cross-midnight row, a multi-boundary row and a pre-existing
// Activity/Pause overlap pair -- all sharing the edited date -- stay
// byte-identical, with no prompt, when an unrelated row on that date is saved.
void HistoryDialogTest::test_H5_unrelated_save_keeps_legacy_rows_and_overlap_byte_identical()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate today = QDate::currentDate();
    const QDate yesterday = today.addDays(-1);

    // The unrelated row the user edits.
    const TimeDuration edited = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(14, 0, 0)), QDateTime(yesterday, QTime(14, 30, 0)));
    // A pre-existing cross-type overlap pair on the same date.
    const TimeDuration overlapActivity = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(10, 0, 0)), QDateTime(yesterday, QTime(10, 30, 0)));
    const TimeDuration overlapPause = TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(yesterday, QTime(10, 15, 0)), QDateTime(yesterday, QTime(10, 45, 0)));
    TestCommon::seedStoredRows(db, {edited, overlapActivity, overlapPause}, true);

    // Legacy shapes no uTimer write path produces, inserted via raw SQL:
    // a single-boundary cross-midnight row (yesterday 23:50 -> today 00:10)
    // and a multi-boundary row (three days ago 23:00 -> yesterday 01:00).
    const QString crossMidnightId = SegmentId::mint().toString();
    const QString multiBoundaryId = SegmentId::mint().toString();
    QVERIFY(insertRaw(db_path_, crossMidnightId, 0,
        QDateTime(yesterday, QTime(23, 50, 0)), QDateTime(today, QTime(0, 10, 0)), true));
    QVERIFY(insertRaw(db_path_, multiBoundaryId, 0,
        QDateTime(today.addDays(-3), QTime(23, 0, 0)), QDateTime(yesterday, QTime(1, 0, 0)), true));

    const QStringList untouchedIds{crossMidnightId, multiBoundaryId,
        overlapActivity.segment_id.toString(), overlapPause.segment_id.toString()};
    QStringList before;
    for (const auto& id : untouchedIds)
        before << rowFingerprint(db_path_, id);
    for (const auto& fp : before)
        QVERIFY(!fp.isEmpty());

    HistoryDialog dialog(tracker, settings);
    const int page = pageIndexFor(dialog.editSession_dbg(), yesterday);
    QVERIFY(page >= 0);
    QCOMPARE(dialog.editSession_dbg().pages()[page].crossMidnight.size(), static_cast<size_t>(1));
    QVERIFY(dialog.editSession_dbg().setCompletedType(page, edited.segment_id, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // The diff carries only the edited row: nothing to merge, no violation.
    const auto scoped = dialog.editSession_dbg().buildScopedSave();
    QCOMPARE(scoped.violation, DayValidity::Violation::None);
    QVERIFY(!scoped.needsMergeConfirmation);
    QCOMPARE(scoped.days.size(), static_cast<size_t>(1));
    QCOMPARE(scoped.days[0].upserts.size(), static_cast<size_t>(1));
    QCOMPARE(scoped.days[0].upserts[0].segment_id, edited.segment_id);
    QVERIFY(scoped.days[0].deletedIds.empty());

    int mergePrompts = 0;
    dialog.debugOnMergePromptShown_ = [&mergePrompts]() { ++mergePrompts; };
    dialog.done(QDialog::Accepted);
    QVERIFY(dialog.saveChanges());

    QCOMPARE(mergePrompts, 0);
    QCOMPARE(dialog.debugConflictShownCount_, 0);
    QCOMPARE(dialog.debugCriticalShownCount_, 0);
    for (int i = 0; i < untouchedIds.size(); ++i)
        QCOMPARE(rowFingerprint(db_path_, untouchedIds[i]), before[i]);
    QCOMPARE(rowFingerprint(db_path_, edited.segment_id.toString()).section('|', 0, 0).toInt(),
             static_cast<int>(DurationType::Pause));
    QCOMPARE(storedRowCount(db_path_), 5);
}

// WP5 acceptance: a malformed finalized row on the edited date survives the
// dialog's open and save byte-identical.
void HistoryDialogTest::test_H5_malformed_row_on_edited_date_survives_open_and_save()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const TimeDuration edited = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(10, 0, 0)), QDateTime(yesterday, QTime(10, 30, 0)));
    TestCommon::seedStoredRows(db, {edited}, true);

    // Malformed (invalid type 99, otherwise a well-formed interval on the
    // same date), seeded via raw SQL after the last store write of this
    // Arrange -- no commit follows it that could let the debug cross-layer
    // checker remove it.
    const QString malformedId = SegmentId::mint().toString();
    QVERIFY(insertRaw(db_path_, malformedId, 99,
        QDateTime(yesterday, QTime(14, 0, 0)), QDateTime(yesterday, QTime(14, 30, 0)), true));
    const QString malformedBefore = rowFingerprint(db_path_, malformedId);
    QVERIFY(!malformedBefore.isEmpty());

    HistoryDialog dialog(tracker, settings);
    const int page = pageIndexFor(dialog.editSession_dbg(), yesterday);
    QVERIFY(page >= 0);
    // The malformed row is never displayed.
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[page].completed().size(), static_cast<size_t>(1));
    QVERIFY(dialog.editSession_dbg().setCompletedType(page, edited.segment_id, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    dialog.done(QDialog::Accepted);
    QVERIFY(dialog.saveChanges());

    QCOMPARE(rowFingerprint(db_path_, malformedId), malformedBefore);
    QCOMPARE(rowFingerprint(db_path_, edited.segment_id.toString()).section('|', 0, 0).toInt(),
             static_cast<int>(DurationType::Pause));
    QCOMPARE(storedRowCount(db_path_), 2);
}

// WP5 acceptance: a split plus a confirmed merge on a past date round-trips
// through the store (update + insert + delete) and reloads as saved.
void HistoryDialogTest::test_H5_split_and_confirmed_merge_on_past_date_round_trips()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime t1000(yesterday, QTime(10, 0, 0));
    const QDateTime t1015(yesterday, QTime(10, 15, 0));
    const QDateTime t1030(yesterday, QTime(10, 30, 0));
    const QDateTime t1045(yesterday, QTime(10, 45, 0));
    const TimeDuration splitSource = TimeDuration::fromPersistedRow(DurationType::Activity, t1000, t1030);
    const TimeDuration neighbour = TimeDuration::fromPersistedRow(DurationType::Activity, t1030, t1045);
    TestCommon::seedStoredRows(db, {splitSource, neighbour}, true);

    {
        HistoryDialog dialog(tracker, settings);
        const int page = pageIndexFor(dialog.editSession_dbg(), yesterday);
        QVERIFY(page >= 0);
        QVERIFY(dialog.editSession_dbg().pageEditable(page));

        // Split 10:00-10:30 at its midpoint into Pause | Activity. The new
        // Activity half touches the untouched Activity neighbour, so the
        // scoped diff merges them and asks for confirmation.
        dialog.applySplit_dbg(0, page, DurationType::Pause, DurationType::Activity);
        QCOMPARE(dialog.editSession_dbg().pendingTimelines()[page].completed().size(), static_cast<size_t>(3));
        QVERIFY(dialog.editSession_dbg().hasEdits());
        QVERIFY(dialog.editSession_dbg().buildScopedSave().needsMergeConfirmation);

        dialog.debugOverlapAnswer_ = QMessageBox::Yes;
        dialog.done(QDialog::Accepted);
        QVERIFY(dialog.saveChanges());
    }

    QCOMPARE(storedRowCount(db_path_), 2);
    QVERIFY(rowFingerprint(db_path_, neighbour.segment_id.toString()).isEmpty()); // merged away

    HistoryDialog reopened(tracker, settings);
    const int page = pageIndexFor(reopened.editSession_dbg(), yesterday);
    QVERIFY(page >= 0);
    const auto rows = reopened.editSession_dbg().pendingTimelines()[page].completed();
    QCOMPARE(rows.size(), static_cast<size_t>(2));
    QCOMPARE(rows[0].segment_id, splitSource.segment_id);
    QCOMPARE(rows[0].type, DurationType::Pause);
    QCOMPARE(rows[0].startTime, t1000);
    QCOMPARE(rows[0].endTime, t1015);
    QCOMPARE(rows[1].type, DurationType::Activity);
    QCOMPARE(rows[1].startTime, t1015);
    QCOMPARE(rows[1].endTime, t1045);
    QVERIFY(rows[1].segment_id != splitSource.segment_id);
    QVERIFY(rows[1].segment_id != neighbour.segment_id);
}

void HistoryDialogTest::test_H5_conflict_after_open_keeps_edits_and_writes_nothing_data()
{
    QTest::addColumn<bool>("insertMultiBoundaryRow");
    QTest::newRow("row_on_date_changed") << false;
    QTest::newRow("multi_boundary_row_inserted") << true;
}

// WP5 acceptance: a raw-SQL change on the selected date after open makes the
// save a Conflict -- the "History Changed" message, the dialog stays open,
// the edits are kept, and the dialog writes nothing.
void HistoryDialogTest::test_H5_conflict_after_open_keeps_edits_and_writes_nothing()
{
    QFETCH(bool, insertMultiBoundaryRow);

    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const TimeDuration edited = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(10, 0, 0)), QDateTime(yesterday, QTime(10, 30, 0)));
    const TimeDuration other = TimeDuration::fromPersistedRow(DurationType::Pause,
        QDateTime(yesterday, QTime(11, 0, 0)), QDateTime(yesterday, QTime(11, 30, 0)));
    TestCommon::seedStoredRows(db, {edited, other}, true);
    const QString editedBefore = rowFingerprint(db_path_, edited.segment_id.toString());

    HistoryDialog dialog(tracker, settings);
    const int page = pageIndexFor(dialog.editSession_dbg(), yesterday);
    QVERIFY(page >= 0);
    QVERIFY(dialog.editSession_dbg().setCompletedType(page, edited.segment_id, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // History changes underneath the open dialog, on the selected date.
    QString changedId;
    if (insertMultiBoundaryRow) {
        changedId = SegmentId::mint().toString();
        QVERIFY(insertRaw(db_path_, changedId, 0,
            QDateTime(yesterday.addDays(-2), QTime(23, 0, 0)), QDateTime(yesterday, QTime(1, 0, 0)), true));
    } else {
        changedId = other.segment_id.toString();
        QVERIFY(execRaw(db_path_, QString("UPDATE durations SET end_utc = '%1' WHERE segment_id = '%2'")
            .arg(QDateTime(yesterday, QTime(11, 40, 0)).toUTC().toString(Qt::ISODateWithMs), changedId)));
    }
    const QString changedAfterRawSql = rowFingerprint(db_path_, changedId);
    QVERIFY(!changedAfterRawSql.isEmpty());
    const int rowCountAfterRawSql = storedRowCount(db_path_);

    dialog.debugSkipCritical_ = true;
    dialog.accept();

    QVERIFY(dialog.result() != QDialog::Accepted);
    QVERIFY(dialog.editSession_dbg().hasEdits());
    QCOMPARE(dialog.debugConflictShownCount_, 1);
    QCOMPARE(dialog.debugCriticalShownCount_, 0);
    // The dialog wrote nothing: the store is exactly what the raw SQL left.
    QCOMPARE(rowFingerprint(db_path_, edited.segment_id.toString()), editedBefore);
    QCOMPARE(rowFingerprint(db_path_, changedId), changedAfterRawSql);
    QCOMPARE(storedRowCount(db_path_), rowCountAfterRawSql);
}

// WP5 acceptance: the commit-failure seam -- a locked database fails the
// scoped save with the "Database Error" message, the edits are kept and
// nothing is written; once the lock is released, a retry succeeds.
void HistoryDialogTest::test_H5_commit_failure_keeps_edits_and_retry_succeeds()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const TimeDuration edited = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(10, 0, 0)), QDateTime(yesterday, QTime(10, 30, 0)));
    TestCommon::seedStoredRows(db, {edited}, true);
    const QString editedBefore = rowFingerprint(db_path_, edited.segment_id.toString());

    HistoryDialog dialog(tracker, settings);
    const int page = pageIndexFor(dialog.editSession_dbg(), yesterday);
    QVERIFY(page >= 0);
    QVERIFY(dialog.editSession_dbg().setCompletedType(page, edited.segment_id, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());
    dialog.debugSkipCritical_ = true;

    // Hold an exclusive transaction on a second connection so the scoped
    // save's own transaction fails.
    const QString lockConnName = "historydialog_h5_commit_failure_lock";
    {
        QSqlDatabase lockDb = QSqlDatabase::addDatabase("QSQLITE", lockConnName);
        lockDb.setDatabaseName(db_path_);
        QVERIFY(lockDb.open());
        QSqlQuery lockQuery(lockDb);
        QVERIFY(lockQuery.exec("BEGIN EXCLUSIVE TRANSACTION"));

        dialog.accept();

        QVERIFY(lockQuery.exec("ROLLBACK"));
        lockDb.close();
    }
    QSqlDatabase::removeDatabase(lockConnName);

    QVERIFY(dialog.result() != QDialog::Accepted);
    QCOMPARE(dialog.debugCriticalShownCount_, 1);
    QCOMPARE(dialog.debugConflictShownCount_, 0);
    QVERIFY(dialog.editSession_dbg().hasEdits());
    QCOMPARE(rowFingerprint(db_path_, edited.segment_id.toString()), editedBefore);

    // Retry with the lock released.
    dialog.accept();

    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.debugCriticalShownCount_, 1);
    QCOMPARE(rowFingerprint(db_path_, edited.segment_id.toString()).section('|', 0, 0).toInt(),
             static_cast<int>(DurationType::Pause));
}

// WP5 acceptance: a revoke fired while the save-error message is up (here
// the Conflict one) ends the dialog Rejected, with no further write.
void HistoryDialogTest::test_H5_revoke_from_save_error_ends_rejected_without_write()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const TimeDuration edited = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(10, 0, 0)), QDateTime(yesterday, QTime(10, 30, 0)));
    TestCommon::seedStoredRows(fakeDb, {edited}, true);

    HistoryDialog dialog(tracker, settings);
    const int page = pageIndexFor(dialog.editSession_dbg(), yesterday);
    QVERIFY(page >= 0);
    QVERIFY(dialog.editSession_dbg().setCompletedType(page, edited.segment_id, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    fakeDb.saveHistoryDaysResult = SessionStoreResult::conflict("history changed since open");
    dialog.debugSkipCritical_ = true;
    int errorShown = 0;
    dialog.debugOnSaveErrorShown_ = [&]() {
        ++errorShown;
        emit tracker.exclusiveEditRevoked();
    };
    QSignalSpy finished(&dialog, &QDialog::finished);
    fakeDb.callLog.clear();

    // Act
    dialog.accept();

    QCOMPARE(errorShown, 1);
    QCOMPARE(dialog.debugConflictShownCount_, 1);
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 1);
    // The queued closeRevoked() rejects the dialog.
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).toInt(), int(QDialog::Rejected));
    QCOMPARE(dialog.result(), int(QDialog::Rejected));

    // A further OK is refused by editRevoked_ before any store call.
    dialog.accept();
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QCOMPARE(finished.count(), 1);
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 1);
    QCOMPARE(errorShown, 1);
}

// WP5 acceptance: the late editRevoked_ check -- a revoke fired from inside
// the merge prompt, answered Yes, still refuses: saveHistoryDays() is never
// called and the dialog ends Rejected.
void HistoryDialogTest::test_H5_revoke_during_merge_prompt_never_saves()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDate yesterday = QDate::currentDate().addDays(-1);
    const TimeDuration splitSource = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(10, 0, 0)), QDateTime(yesterday, QTime(10, 30, 0)));
    const TimeDuration neighbour = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(yesterday, QTime(10, 30, 0)), QDateTime(yesterday, QTime(10, 45, 0)));
    TestCommon::seedStoredRows(fakeDb, {splitSource, neighbour}, true);

    HistoryDialog dialog(tracker, settings);
    const int page = pageIndexFor(dialog.editSession_dbg(), yesterday);
    QVERIFY(page >= 0);
    dialog.applySplit_dbg(0, page, DurationType::Pause, DurationType::Activity);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[page].completed().size(), static_cast<size_t>(3));
    QVERIFY(dialog.editSession_dbg().hasEdits());
    QVERIFY(dialog.editSession_dbg().buildScopedSave().needsMergeConfirmation);

    int promptShown = 0;
    dialog.debugOnMergePromptShown_ = [&]() {
        ++promptShown;
        emit tracker.exclusiveEditRevoked();
    };
    dialog.debugOverlapAnswer_ = QMessageBox::Yes;
    QSignalSpy finished(&dialog, &QDialog::finished);
    fakeDb.callLog.clear();

    // Act
    dialog.accept();

    QCOMPARE(promptShown, 1);
    QVERIFY(dialog.result() != QDialog::Accepted);
    QTRY_COMPARE(finished.count(), 1);
    QCOMPARE(finished.last().at(0).toInt(), int(QDialog::Rejected));
    QCOMPARE(dialog.result(), int(QDialog::Rejected));
    QCOMPARE(fakeDb.callLog.count("saveHistoryDays"), 0);
    QCOMPARE(dialog.debugConflictShownCount_, 0);
    QCOMPARE(dialog.debugCriticalShownCount_, 0);
}

// WP5 acceptance: after a Stop, Today is editable and a History edit on it
// saves (real SQLite).
void HistoryDialogTest::test_H5_today_after_stop_saves()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(60000);
    tracker.useTimerViaButton(Button::Stop);
    QVERIFY(!tracker.isActive());

    HistoryDialog dialog(tracker, settings);
    const int today = currentPageIndex(dialog.editSession_dbg());
    QVERIFY(today >= 0);
    QVERIFY(dialog.editSession_dbg().pageEditable(today));
    const auto rows = dialog.editSession_dbg().pendingTimelines()[today].completed();
    QCOMPARE(rows.size(), static_cast<size_t>(1));
    QCOMPARE(rows[0].type, DurationType::Activity);
    const SegmentId rowId = rows[0].segment_id;

    QVERIFY(dialog.editSession_dbg().setCompletedType(today, rowId, DurationType::Pause));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    dialog.done(QDialog::Accepted);
    QVERIFY(dialog.saveChanges());

    QCOMPARE(rowFingerprint(db_path_, rowId.toString()).section('|', 0, 0).toInt(),
             static_cast<int>(DurationType::Pause));
    QCOMPARE(storedRowCount(db_path_), 1);
}

// WP5 acceptance: Today opened while tracking runs is read-only, both through
// the widgets and through the model API.
void HistoryDialogTest::test_H5_today_while_running_is_read_only()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // A finalized stored row fully inside Today, so the only reason Today is
    // read-only is that tracking is running.
    const QDate todayDate = QDate::currentDate();
    const TimeDuration stored = TimeDuration::fromPersistedRow(DurationType::Activity,
        QDateTime(todayDate, QTime(9, 0, 0)), QDateTime(todayDate, QTime(9, 30, 0)));
    TestCommon::seedStoredRows(db, {stored}, true);

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20000);

    HistoryDialog dialog(tracker, settings);
    auto& session = dialog.editSession_dbg();
    const int today = currentPageIndex(session);
    QVERIFY(today >= 0);
    QVERIFY(!session.pageEditable(today));
    for (int i = 0; i < today; ++i)
        dialog.onOlder_dbg();

    // Widgets: read-only title, a disabled type box on the completed row, and
    // no type box at all on the ongoing row, whose Type cell says "(Running)".
    QVERIFY(dialog.pageLabel_dbg()->text().contains("(read-only)"));
    QCOMPARE(session.pendingTimelines()[today].completed().size(), static_cast<size_t>(1));
    QVERIFY(session.pendingTimelines()[today].ongoing().has_value());
    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);
    QVERIFY(!box->isEnabled());
    QVERIFY(dialog.table_dbg()->cellWidget(1, 3) == nullptr);
    QVERIFY(dialog.table_dbg()->item(1, 0)->text().contains("(Running)"));

    // Model API: every mutation on the page is refused.
    QVERIFY(!session.setCompletedType(today, stored.segment_id, DurationType::Pause));
    const HistoryEditSession::SplitOutcome split = session.splitRow(today, 0,
        QDateTime(todayDate, QTime(9, 15, 0)), DurationType::Activity, DurationType::Pause);
    QVERIFY(!split.applied);
    QCOMPARE(split.error, HistoryEditSession::SplitError::ReadOnly);
    QVERIFY(!session.setPageTimeline(today, session.pendingTimelines()[today].withSegmentType(0, DurationType::Pause)));
    QVERIFY(!session.hasEdits());
}
