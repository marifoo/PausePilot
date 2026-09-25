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
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

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
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pages().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].segment_id.toString()), true);
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
        QVERIFY(tracker.replaceAll(Timeline(std::move(init), std::nullopt), Timeline({}, std::nullopt)).ok());
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

    QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].type, DurationType::Activity);
    box->setChecked(false);

    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].type, DurationType::Pause);
    QVERIFY(dialog.pageLabel_dbg()->text().contains("Pause"));
}

void HistoryDialogTest::test_historydialog_saveChanges_updates_timetracker_and_db()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-200), now.addSecs(-150)));

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-140), now.addSecs(-120)));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    QCOMPARE(tracker.completedTimeline().completed().size(), static_cast<size_t>(1));
    QCOMPARE(tracker.completedTimeline().completed()[0].type, DurationType::Pause);

    SqliteSessionStore db2(settings);
    auto loaded = db2.loadDurations();
    QVERIFY(loaded.size() >= 1);
}

void HistoryDialogTest::test_historydialog_split_action_splits_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    QDateTime start = fixedNow().addSecs(-10);
    QDateTime end = fixedNow().addSecs(-4);
    const TimeDuration seeded = TimeDuration::fromPersistedRow(DurationType::Activity, start, end);
    tracker.seedCompletedDuration_dbg(seeded);
    const SegmentId originalSegmentId = seeded.segment_id;

    HistoryDialog dialog(tracker, settings);

    // Bypass SplitDialog::exec() to avoid offscreen QPA propagateSizeHints() warnings.
    // The split types are supplied directly; logic under test is the model update (withSplit,
    // origin registration, segment_id preservation) — not the dialog UI itself.
    dialog.applySplit_dbg(0, 0, DurationType::Activity, DurationType::Pause);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
    const auto& first = dialog.editSession_dbg().pendingTimelines()[0].completed()[0];
    const auto& second = dialog.editSession_dbg().pendingTimelines()[0].completed()[1];
    QCOMPARE(first.startTime, start);
    QCOMPARE(second.endTime, end);
    QCOMPARE(first.endTime, second.startTime);
    QCOMPARE(first.type, DurationType::Activity);
    QCOMPARE(second.type, DurationType::Pause);
    QCOMPARE(first.segment_id, originalSegmentId);
    QVERIFY(!second.segment_id.isEmpty());
    QVERIFY(second.segment_id != originalSegmentId);
}

void HistoryDialogTest::test_historydialog_split_today_mixed_origins_routes_to_correct_bucket()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime now = fixedNow();
    const QDateTime memStart = now.addSecs(-20);
    const QDateTime memEnd = now.addSecs(-12);
    const QDateTime dbStart = now.addSecs(-11);
    const QDateTime dbEnd = now.addSecs(-6);

    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, dbStart, dbEnd));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[1].segment_id.toString()), false);

    // Bypass SplitDialog::exec() to avoid offscreen QPA propagateSizeHints() warnings.
    dialog.applySplit_dbg(0, 0, DurationType::Activity, DurationType::Pause);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(3));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[1].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[2].segment_id.toString()), false);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    // Bind the Timeline to a named local before indexing into it repeatedly:
    // completedTimeline() returns by value and completed() returns a
    // reference into that temporary.
    const auto completedAfterSave = tracker.completedTimeline();
    QCOMPARE(completedAfterSave.completed().size(), static_cast<size_t>(2));
    QCOMPARE(completedAfterSave.completed()[0].startTime, memStart);
    QCOMPARE(completedAfterSave.completed()[1].endTime, memEnd);
    QCOMPARE(completedAfterSave.completed()[0].endTime, completedAfterSave.completed()[1].startTime);

    // A9: all completed rows are finalized on save (the two memory-origin split
    // halves plus the DB Pause row) — the DB now loads all three.
    SqliteSessionStore db2(settings);
    auto loaded = db2.loadDurations();
    QCOMPARE(loaded.size(), static_cast<size_t>(3));
    bool foundDbPause = false;
    for (size_t i = 0; i < loaded.size(); ++i) {
        if (loaded[i].startTime == dbStart && loaded[i].endTime == dbEnd &&
            loaded[i].type == DurationType::Pause)
            foundDbPause = true;
    }
    QVERIFY(foundDbPause);
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
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));

    // Bypass SplitDialog::exec() to avoid offscreen QPA propagateSizeHints() warnings.
    dialog.applySplit_dbg(0, 1, DurationType::Activity, DurationType::Pause);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[1].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[1].completed()[0].segment_id.toString()), false);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[1].completed()[1].segment_id.toString()), false);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

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
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

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
    QVERIFY(tracker.replaceAll(Timeline(historicalRows, std::nullopt), Timeline({}, std::nullopt)).ok());


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
        dialog.done(QDialog::Accepted);
        dialog.saveChanges();
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

void HistoryDialogTest::test_historydialog_pauses_checkpoint_timer_for_dialog_lifetime()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db2(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db2);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    // Advance the fake clock (was QTest::qWait(20)) so the later
    // triggerCheckpoint_dbg() below (after the dialog closes) has a nonzero
    // wallClockMs and is not skipped by saveCheckpoint_locked's
    // wallClockMs != 0 guard (timetracker.cpp:1653-1656).
    clock.advance(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(!tracker.isCheckpointTimerActive_dbg());

        tracker.triggerCheckpoint_dbg();

        const QString connName = "historydialog_t6_case2_open";
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
            db.setDatabaseName(db_path_);
            QVERIFY(db.open());

            QSqlQuery countQuery(db);
            QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
            QVERIFY(countQuery.next());
            QCOMPARE(countQuery.value(0).toInt(), 0);

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
    QVERIFY(tracker.replaceAll(Timeline(historicalRows, std::nullopt), Timeline({}, std::nullopt)).ok());

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-30), now.addSecs(-20)));
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-20), now.addSecs(-10)));

    HistoryDialog dialog(tracker, settings);

    // A10: an editless OK is a no-op, so make a genuine edit (toggle the
    // historical row's type on its page) to exercise the save path.
    QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));
    dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Activity));

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    // A9: all completed rows are finalized on save (1 historical + 2 current
    // session), and nothing stays unfinalized (there is no ongoing segment).
    const int expectedRows = 3;

    const QString connName = "historydialog_t7_row_count";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery totalQuery(db);
        QVERIFY(totalQuery.exec("SELECT COUNT(*) FROM durations"));
        QVERIFY(totalQuery.next());
        QCOMPARE(totalQuery.value(0).toInt(), expectedRows);

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

void HistoryDialogTest::test_historydialog_save_then_crash_reopen_retains_current_segment_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = createSettingsFile(tempDir.path(), 7);

    {
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

        const QDateTime now = fixedNow();
        tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-15), now.addSecs(-5)));

        HistoryDialog dialog(tracker, settings);
        // A10: an editless OK is a no-op, so make a genuine edit (toggle the
        // row's type) to exercise the save path.
        dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));
        dialog.done(QDialog::Accepted);
        dialog.saveChanges();

        // Simulate crash: skip graceful stop/finalize.
        tracker.simulateCrashWithoutShutdown_dbg();
    }

    {
        Settings settings(settingsPath);
        SqliteSessionStore db(settings);

        // A9: the completed current-session row is finalized on save, so nothing
        // stays unfinalized (no ongoing segment).
        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery unfinalizedQuery(db.rawDb_dbg());
        QVERIFY(unfinalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0"));
        QVERIFY(unfinalizedQuery.next());
        QCOMPARE(unfinalizedQuery.value(0).toInt(), 0);
        db.closeConnection_dbg();

        // The finalized row survives the crash + startup recovery.
        SqliteSessionStore db3(settings);
        TimeTracker reopened(settings, db3);
        reopened.initializeFromStore();
        auto loaded = db.loadDurations();
        QCOMPARE(loaded.size(), static_cast<size_t>(1));
    }
}

// Issue 8: saveChanges() must refresh the ongoing end-time from the engine,
// not preserve the stale snapshot captured at dialog-open time.
void HistoryDialogTest::test_historydialog_save_uses_ongoing_snapshot_endtime_after_wait()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(30);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing().has_value());
    const QDateTime snapshotEnd = dialog.editSession_dbg().pendingTimelines()[0].ongoing()->endTime;
    const QDateTime snapshotStart = dialog.editSession_dbg().pendingTimelines()[0].ongoing()->startTime;

    // A10: an editless OK is a no-op, so make a genuine edit (toggle the ongoing
    // type) to exercise the save path — the end-time refresh still applies.
    QVERIFY(dialog.editSession_dbg().setOngoingType(0, DurationType::Pause));

    // Advance the fake clock (was QTest::qWait(200)) so the engine's ongoing
    // end-time -- read via now_provider_() in getOngoingDuration_locked()
    // (timetracker.cpp:1257) -- deterministically advances past snapshotEnd.
    clock.advance(200);

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    const QString connName = "historydialog_t9_snapshot_endtime";
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", connName);
        db.setDatabaseName(db_path_);
        QVERIFY(db.open());

        QSqlQuery ongoingQuery(db);
        ongoingQuery.prepare(
            "SELECT start_utc, end_utc FROM durations WHERE is_finalized = 0"
        );
        QVERIFY(ongoingQuery.exec());
        QVERIFY(ongoingQuery.next());

        const QDateTime savedStartUtc = QDateTime::fromString(ongoingQuery.value(0).toString(), Qt::ISODateWithMs);
        const QDateTime savedEndUtc   = QDateTime::fromString(ongoingQuery.value(1).toString(), Qt::ISODateWithMs);

        // Start time is preserved exactly
        QCOMPARE(savedStartUtc, snapshotStart.toUTC());

        // End-time must be strictly > snapshotEnd (refreshed, not stale)
        QVERIFY2(savedEndUtc > snapshotEnd.toUTC(),
                 "savedEndUtc must be > snapshotEnd: end-time was not refreshed");

        QVERIFY(!ongoingQuery.next());
        db.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_historydialog_save_failed_db_replace_keeps_runtime_state_unchanged()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime now = fixedNow();
    const QDateTime memStart = now.addSecs(-30);
    const QDateTime memEnd = now.addSecs(-20);
    const QDateTime dbStart = now.addSecs(-19);
    const QDateTime dbEnd = now.addSecs(-10);

    // Kept as raw sessionState_dbg() writes/reads deliberately (P5f-1 trap 9):
    // this arranges mode_ == None with a set anchor, which
    // checkDurationInvariants()'s invariant 4 (timetracker.cpp:~303) reports
    // via qWarning -- but the tracker is never started here, and saveChanges()
    // below fails at replaceAll() under the test's own BEGIN EXCLUSIVE lock
    // and returns at historydialog.cpp:226, before commitEditedTimeline()
    // (:255) -- the only call on this path that would run the invariant
    // check -- ever runs. A behavioural route (a real Start + triggerCheckpoint)
    // would replace the fixed "checkpoint-before-save" identity/time this
    // test pins with real engine-chosen values, which is a bigger change than
    // this pass makes; left for a future pass to decide deliberately.
    tracker.sessionState_dbg().durations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));
    const SegmentId checkpointSegmentBeforeSave = SegmentId::fromString("checkpoint-before-save");
    const QDateTime checkpointStartBeforeSave = now.addSecs(-5);
    tracker.sessionState_dbg().segment_id = checkpointSegmentBeforeSave;
    tracker.sessionState_dbg().segment_start_time = checkpointStartBeforeSave;

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, dbStart, dbEnd));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].segment_id.toString()), true);
    QCOMPARE(dialog.editSession_dbg().originIsMemory().value(dialog.editSession_dbg().pendingTimelines()[0].completed()[1].segment_id.toString()), false);

    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    const QString lockConnName = "historydialog_save_failure_lock";
    {
        QSqlDatabase lockDb = QSqlDatabase::addDatabase("QSQLITE", lockConnName);
        lockDb.setDatabaseName(db_path_);
        QVERIFY(lockDb.open());

        QSqlQuery lockQuery(lockDb);
        QVERIFY(lockQuery.exec("BEGIN EXCLUSIVE TRANSACTION"));

        // Skip QMessageBox::critical() to avoid offscreen QPA propagateSizeHints() warnings.
        // The save failure is still tested: debugSkipCritical_ suppresses only the dialog pop-up,
        // saveChanges() still returns false and leaves TimeTracker state unchanged.
        dialog.debugSkipCritical_ = true;
        dialog.done(QDialog::Accepted);
        dialog.saveChanges();

        QVERIFY(lockQuery.exec("ROLLBACK"));
        lockDb.close();
    }
    QSqlDatabase::removeDatabase(lockConnName);

    QCOMPARE(tracker.sessionState_dbg().durations.size(), static_cast<size_t>(1));
    QCOMPARE(tracker.sessionState_dbg().durations[0].type, DurationType::Activity);
    QCOMPARE(tracker.sessionState_dbg().durations[0].startTime, memStart);
    QCOMPARE(tracker.sessionState_dbg().durations[0].endTime, memEnd);
    QCOMPARE(tracker.sessionState_dbg().segment_id.toString(), checkpointSegmentBeforeSave.toString());
    QCOMPARE(tracker.sessionState_dbg().segment_start_time, checkpointStartBeforeSave);

    const QString verifyConnName = "historydialog_save_failure_verify";
    {
        QSqlDatabase verifyDb = QSqlDatabase::addDatabase("QSQLITE", verifyConnName);
        verifyDb.setDatabaseName(db_path_);
        QVERIFY(verifyDb.open());

        QSqlQuery verifyQuery(verifyDb);
        QVERIFY(verifyQuery.exec("SELECT type, start_utc, end_utc FROM durations ORDER BY id"));
        QVERIFY(verifyQuery.next());
        QCOMPARE(verifyQuery.value(0).toInt(), static_cast<int>(DurationType::Pause));

        const QDateTime savedStartUtc = QDateTime::fromString(verifyQuery.value(1).toString(), Qt::ISODateWithMs);
        const QDateTime savedEndUtc   = QDateTime::fromString(verifyQuery.value(2).toString(), Qt::ISODateWithMs);

        QCOMPARE(savedStartUtc, dbStart.toUTC());
        QCOMPARE(savedEndUtc, dbEnd.toUTC());
        QVERIFY(!verifyQuery.next());

        verifyDb.close();
    }
    QSqlDatabase::removeDatabase(verifyConnName);
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
// Test R — round-trip type toggle via accept updates TimeTracker
// ============================================================================

void HistoryDialogTest::test_R_round_trip_type_toggle_via_accept()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    {
        HistoryDialog dialog(tracker, settings);
        QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].type, DurationType::Activity);

        // Toggle to Pause via Timeline API
        dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));
        QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed()[0].type, DurationType::Pause);

        dialog.done(QDialog::Accepted);
        dialog.saveChanges();
    }

    // TimeTracker reflects the change
    QCOMPARE(tracker.completedTimeline().completed().size(), static_cast<size_t>(1));
    QCOMPARE(tracker.completedTimeline().completed()[0].type, DurationType::Pause);
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

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    {
        HistoryDialog dialog(tracker, settings);
        // Toggle to Pause but cancel
        dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

        dialog.done(QDialog::Rejected);
        dialog.saveChanges();  // saveChanges checks result() and returns early when Rejected
    }

    // TimeTracker is unchanged — still Activity
    QCOMPARE(tracker.completedTimeline().completed().size(), static_cast<size_t>(1));
    QCOMPARE(tracker.completedTimeline().completed()[0].type, DurationType::Activity);
}

void HistoryDialogTest::test_saveChanges_deduplicates_cross_bucket_overlaps()
{
    // DB history row (today): Activity 10:00-10:30
    // In-memory session row (today): Activity 10:15-10:45
    // After save+normalize, expect exactly one Activity row covering 10:00-10:45.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate today = QDate::currentDate();
    const QDateTime dbStart(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime dbEnd(today, QTime(10, 30, 0), Qt::UTC);
    const QDateTime memStart(today, QTime(10, 15, 0), Qt::UTC);
    const QDateTime memEnd(today, QTime(10, 45, 0), Qt::UTC);

    // Seed DB with today's finalized history row
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, dbStart, dbEnd));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    // Seed in-memory session row (overlaps the DB row)
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryDialog dialog(tracker, settings);

    // A10: an editless OK is a no-op. Make a genuine same-type edit — split the
    // memory row into two Activity halves — so the save runs and normalization
    // still merges the whole overlapping cluster into one 10:00-10:45 row.
    dialog.applySplit_dbg(0, 0, DurationType::Activity, DurationType::Activity);

    dialog.done(QDialog::Accepted);

    // Bypass QMessageBox::question() to avoid offscreen QPA propagateSizeHints() warnings.
    // The merge confirmation is still exercised: debugOverlapAnswer_ supplies "Yes" so the
    // overlap is merged and saved, exactly as if the user clicked Yes.
    dialog.debugOverlapAnswer_ = QMessageBox::Yes;
    dialog.saveChanges();

    // Read back DB and verify exactly one Activity row covering 10:00-10:45
    const QString connName = "historydialog_cross_bucket_dedup";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());

        QSqlQuery countQuery(sqlDb);
        QVERIFY(countQuery.exec("SELECT COUNT(*) FROM durations WHERE type = " +
            QString::number(static_cast<int>(DurationType::Activity))));
        QVERIFY(countQuery.next());
        QCOMPARE(countQuery.value(0).toInt(), 1);

        QSqlQuery rowQuery(sqlDb);
        QVERIFY(rowQuery.exec("SELECT start_utc, end_utc FROM durations WHERE type = " +
            QString::number(static_cast<int>(DurationType::Activity))));
        QVERIFY(rowQuery.next());
        const QDateTime savedStart = QDateTime::fromString(rowQuery.value(0).toString(), Qt::ISODateWithMs);
        const QDateTime savedEnd   = QDateTime::fromString(rowQuery.value(1).toString(), Qt::ISODateWithMs);
        QCOMPARE(savedStart.toUTC().time(), QTime(10, 0, 0));
        QCOMPARE(savedEnd.toUTC().time(), QTime(10, 45, 0));

        // A9: the merged completed row is finalized on save (nothing unfinalized).
        QSqlQuery unfinalizedQuery(sqlDb);
        QVERIFY(unfinalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 0 AND type = " +
            QString::number(static_cast<int>(DurationType::Activity))));
        QVERIFY(unfinalizedQuery.next());
        QCOMPARE(unfinalizedQuery.value(0).toInt(), 0);

        QSqlQuery finalizedQuery(sqlDb);
        QVERIFY(finalizedQuery.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 1 AND type = " +
            QString::number(static_cast<int>(DurationType::Activity))));
        QVERIFY(finalizedQuery.next());
        QCOMPARE(finalizedQuery.value(0).toInt(), 1);

        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // The merged 10:00-10:45 row must be retained in the live session after save.
    // Before fix #6: session loses the row entirely (bug — current-session time drops to 0).
    // After fix #6: session contains exactly the merged row (inflated from 30 min to 45 min).
    const auto completedAfterSave = tracker.completedTimeline();
    QCOMPARE(completedAfterSave.completed().size(), static_cast<size_t>(1));
    QCOMPARE(completedAfterSave.completed()[0].type, DurationType::Activity);
    QCOMPARE(completedAfterSave.completed()[0].startTime.toUTC().time(), QTime(10, 0, 0));
    QCOMPARE(completedAfterSave.completed()[0].endTime.toUTC().time(), QTime(10, 45, 0));
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
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryDialog dialog(tracker, settings);
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
    QVERIFY(tracker.replaceAll(Timeline(dbRows, std::nullopt), Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);
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

void HistoryDialogTest::test_H3_ongoing_type_edit_preserved_when_engine_stops()
{
    // Invariant: engine is still running when saveChanges() is called.
    // refreshOngoing() must advance the end-time from the engine but preserve
    // any type edit the user made to the pending ongoing segment.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing().has_value());
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].ongoing()->type, DurationType::Activity);

    // User edits ongoing type to Pause (same as the checkbox lambda in historydialog.cpp).
    QVERIFY(dialog.editSession_dbg().setOngoingType(0, DurationType::Pause));

    // Engine is still running (Activity mode).
    QVERIFY(tracker.isActive());

    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    // User's Pause type is preserved in the DB (unfinalized row), not overwritten
    // by the engine's Activity type refresh.
    const QString connName = "historydialog_h3_check";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());
        QSqlQuery q(sqlDb);
        QVERIFY(q.exec("SELECT type FROM durations WHERE is_finalized = 0"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), static_cast<int>(DurationType::Pause));
        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);
}

void HistoryDialogTest::test_H4_overlap_cancel_aborts_save()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDate today = QDate::currentDate();
    const QDateTime dbStart(today, QTime(11, 0, 0), Qt::UTC);
    const QDateTime dbEnd(today, QTime(11, 30, 0), Qt::UTC);
    const QDateTime memStart(today, QTime(11, 15, 0), Qt::UTC);
    const QDateTime memEnd(today, QTime(11, 45, 0), Qt::UTC);

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, dbStart, dbEnd));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    HistoryDialog dialog(tracker, settings);

    // Bypass QMessageBox::question() to avoid offscreen QPA propagateSizeHints() warnings.
    // Supplies "No" — same as user declining the merge — so saveChanges() aborts.
    dialog.debugOverlapAnswer_ = QMessageBox::No;
    dialog.done(QDialog::Accepted);
    dialog.saveChanges();

    // Save aborted — DB should still have exactly 1 finalized row (original)
    const QString connName = "historydialog_h4_cancel";
    {
        QSqlDatabase sqlDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        sqlDb.setDatabaseName(db_path_);
        QVERIFY(sqlDb.open());
        QSqlQuery q(sqlDb);
        QVERIFY(q.exec("SELECT COUNT(*) FROM durations WHERE is_finalized = 1"));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 1);
        sqlDb.close();
    }
    QSqlDatabase::removeDatabase(connName);

    // TimeTracker session unchanged: still has 1 memory row
    QCOMPARE(tracker.completedTimeline().completed().size(), static_cast<size_t>(1));
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
        // DB may not exist yet — seed it via replaceAll first.
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }
    // Seed DB to create the schema.
    QVERIFY(tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt)).ok());
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
    // It is display-only (see pages[]/crossMidnight above) and dropped from the
    // save payload (MUST-FIX1, review packet C3) rather than being preserved.
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
    QVERIFY(tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt)).ok());
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

    const QDateTime now = fixedNow();
    // Row 0 is Pause, row 1 is Activity — exercise both directions of the preset.
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-20), now.addSecs(-14)));
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-12), now.addSecs(-4)));

    HistoryDialog dialog(tracker, settings);

    // splitDialogPresetFirstType_dbg() runs onSplitRow()'s real preset step
    // (dlg.setFirstSegmentType(duration.type)) without realizing the dialog, so this
    // asserts the production source-row-type -> preset wiring, not a value we supply.
    QCOMPARE(dialog.splitDialogPresetFirstType_dbg(0, 0), DurationType::Pause);
    QCOMPARE(dialog.splitDialogPresetFirstType_dbg(1, 0), DurationType::Activity);
}

// ---------------------------------------------------------------------------
// F1: a failed history load must refuse destructive edited saves
// ---------------------------------------------------------------------------

void HistoryDialogTest::test_F1_saveChanges_refuses_when_history_load_failed()
{
    // Money test: a failed history load must never allow saveChanges() to
    // reach replaceAll() (which would DELETE FROM durations and rewrite only
    // the rows that happened to load, wiping the rest).
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
    // Make an edit so the save path is not skipped by the editless no-op guard.
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);

    QVERIFY(!dialog.saveChanges());
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 0);
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
    // The dialog's payload.violation refusal branch (historydialog.cpp) must
    // still block a save when the user's OWN edit creates the invalid shape
    // -- MUST-FIX2 only narrows the pre-existing, unedited case.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime now = fixedNow();
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
    dialog.editSession_dbg().setPageTimeline(0, Timeline(edited, std::nullopt));

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);

    QVERIFY(!dialog.saveChanges());
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 0);
}

void HistoryDialogTest::test_C3_saveChanges_drops_pre_existing_overlap_and_succeeds()
{
    // MUST-FIX2 (user decision, review packet C3): a pre-existing Activity/
    // Pause overlap already in loaded history must NOT fail an unrelated
    // save -- this was the regression: before C3, this save succeeded, and
    // wholesale-rejecting it left the user with no route to save an edit
    // except Cancel (losing it). HistoryEditSession now drops the
    // later-starting conflicting row (deterministic tie-break) instead of
    // flagging a violation, so the unrelated edit still saves.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime now = fixedNow();
    // Pre-existing DB overlap: Activity 400-300s ago, Pause 350-250s ago.
    const SegmentId activityId = SegmentId::mint();
    const SegmentId pauseId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-400), now.addSecs(-300), activityId));
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-350), now.addSecs(-250), pauseId));

    TimeTracker tracker(settings, fakeDb);
    // An unrelated, non-overlapping row that the user edits.
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    // Edit the unrelated row (index 0 on page 0 is the memory row) — the
    // overlapping pair above is untouched.
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);

    // M3 Step 2.6 also asks before this save, since it drops a pre-existing
    // overlap row; answer Yes so the save proceeds as before.
    dialog.debugDropAnswer_ = QMessageBox::Yes;
    QVERIFY(dialog.saveChanges());
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 1);
    // The earlier-starting Activity row survives; the later-starting Pause
    // row was dropped as the pre-existing conflict.
    QVERIFY(fakeDb.committedSegmentIds.contains(activityId.toString()));
    QVERIFY(!fakeDb.committedSegmentIds.contains(pauseId.toString()));
}

void HistoryDialogTest::test_R13_001_multiboundary_row_dropped_by_unrelated_save()
{
    // R13-001's literal headline trigger: a legacy row spanning MORE THAN ONE
    // calendar boundary (manually introduced -- uTimer never writes one)
    // alongside an ordinary row the user edits. buildFromTimer() already
    // skips a >1-boundary row entirely rather than carrying it forward as
    // display-only, so it never reaches pendingTimelines_ or the save
    // payload. Under the narrowed C3 contract this data loss for
    // out-of-contract data is explicitly accepted (plan section 2/C3 step 5)
    // -- no split/pass-through/preservation logic is added. This test pins
    // the real physical DB outcome after a genuine close and reopen: the
    // multi-boundary row is GONE.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // A row spanning >1 calendar boundary: starts two days ago at 23:00,
    // ends today at 01:00 -- crosses both the day-2→day-1 and day-1→today
    // midnights.
    const QDate twoDaysAgo = QDate::currentDate().addDays(-2);
    const QDateTime multiBoundaryStart(twoDaysAgo, QTime(23, 0, 0));
    const QDateTime multiBoundaryEnd(QDate::currentDate(), QTime(1, 0, 0));

    // An ordinary same-day row the user will edit.
    const QDateTime ordinaryStart = fixedNow().addSecs(-100);
    const QDateTime ordinaryEnd   = fixedNow().addSecs(-50);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, ordinaryStart, ordinaryEnd));

    // Insert the multi-boundary row directly via SQL (bypassing the same-day
    // invariant every uTimer write path enforces).
    SegmentId multiBoundaryId;
    {
        const QString connName = QString("test_multiboundary_%1").arg(
            reinterpret_cast<quintptr>(&db));
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        multiBoundaryId = TimeDuration::createSegmentId();
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations "
            "(segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :start_utc, :end_utc, 1)"
        );
        q.bindValue(":sid", multiBoundaryId.toString());
        q.bindValue(":start_utc", multiBoundaryStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":end_utc", multiBoundaryEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    // buildFromTimer() logs (via qWarning, not Logger::Log) when it skips a
    // >1-boundary row entirely -- expected here, not a regression.
    const QByteArray expectedSkipWarning = QString(
        "[HISTORY] buildFromTimer: skipping segment spanning >1 calendar boundary (segment_id: %1)")
        .arg(multiBoundaryId.toString()).toUtf8();
    QTest::ignoreMessage(QtWarningMsg, expectedSkipWarning.constData());

    HistoryDialog dialog(tracker, settings);
    // The multi-boundary row never became display-only -- it is absent from
    // every page, unlike a single-boundary cross-midnight row.
    for (const auto& page : dialog.editSession_dbg().pages()) {
        QCOMPARE(page.crossMidnight.size(), static_cast<size_t>(0));
        QCOMPARE(page.continuations.size(), static_cast<size_t>(0));
    }

    // Edit the unrelated ordinary row.
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    dialog.done(QDialog::Accepted);
    QVERIFY(dialog.saveChanges());

    // Assert: after a genuine close and reopen, the multi-boundary row is
    // physically gone -- dropped by the wholesale-replacement save, exactly
    // as plan section 2/C3 step 5 accepts for out-of-contract data.
    SqliteSessionStore db2(settings);
    QVERIFY(db2.ensureOpen_dbg());
    QSqlQuery check(db2.rawDb_dbg());
    check.prepare("SELECT COUNT(*) FROM durations WHERE segment_id = :sid");
    check.bindValue(":sid", multiBoundaryId.toString());
    QVERIFY(check.exec());
    QVERIFY(check.next());
    QCOMPARE(check.value(0).toInt(), 0);
    db2.closeConnection_dbg();
}

void HistoryDialogTest::test_C3_single_boundary_cross_midnight_dropped_by_unrelated_save()
{
    // Finding 3 (C3 re-review): unlike the >1-boundary row above, a legacy
    // SINGLE-boundary cross-midnight row becomes display-only (crossMidnight
    // on both pages) and, since MUST-FIX1 (C3), is silently dropped from the
    // save payload rather than preserved. That drop was only pinned at
    // payload level (FakeSessionStore) before this test; this pins the real
    // physical DB outcome after a genuine close and reopen: the row is GONE.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // A single-boundary cross-midnight row: yesterday 23:50 -> today 00:10.
    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime crossMidnightStart(yesterday, QTime(23, 50, 0));
    const QDateTime crossMidnightEnd(QDate::currentDate(), QTime(0, 10, 0));

    // An ordinary same-day row the user will edit.
    const QDateTime ordinaryStart = fixedNow().addSecs(-100);
    const QDateTime ordinaryEnd   = fixedNow().addSecs(-50);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, ordinaryStart, ordinaryEnd));

    // Insert the cross-midnight row directly via SQL (bypassing the same-day
    // invariant every uTimer write path enforces).
    SegmentId crossMidnightId;
    {
        const QString connName = QString("test_singleboundary_%1").arg(
            reinterpret_cast<quintptr>(&db));
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        crossMidnightId = TimeDuration::createSegmentId();
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations "
            "(segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :start_utc, :end_utc, 1)"
        );
        q.bindValue(":sid", crossMidnightId.toString());
        q.bindValue(":start_utc", crossMidnightStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":end_utc", crossMidnightEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    HistoryDialog dialog(tracker, settings);

    // Sanity: unlike the >1-boundary case, this row is display-only on
    // yesterday's page (whichever page index that lands on).
    bool sawDisplayOnly = false;
    for (const auto& page : dialog.editSession_dbg().pages())
        if (page.crossMidnight.size() == 1)
            sawDisplayOnly = true;
    QVERIFY(sawDisplayOnly);

    // Edit the unrelated ordinary row.
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    dialog.done(QDialog::Accepted);
    // M3 Step 2.6 also asks before this save, since it drops a display-only
    // cross-midnight row; answer Yes so the save proceeds as before.
    dialog.debugDropAnswer_ = QMessageBox::Yes;
    QVERIFY(dialog.saveChanges());

    // Assert: after a genuine close and reopen, the single-boundary row is
    // physically gone -- dropped by the wholesale-replacement save, exactly
    // as MUST-FIX1/C3 step 5 accepts for out-of-contract data.
    SqliteSessionStore db2(settings);
    QVERIFY(db2.ensureOpen_dbg());
    QSqlQuery check(db2.rawDb_dbg());
    check.prepare("SELECT COUNT(*) FROM durations WHERE segment_id = :sid");
    check.bindValue(":sid", crossMidnightId.toString());
    QVERIFY(check.exec());
    QVERIFY(check.next());
    QCOMPARE(check.value(0).toInt(), 0);
    db2.closeConnection_dbg();
}

void HistoryDialogTest::test_C4_historydialog_shows_own_critical_when_already_latched()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Arrange: latch the tracker BEFORE opening the dialog (a Fatal from an
    // unrelated earlier operation, e.g. a checkpoint write).
    fakeDb.replaceDurationsResult = SessionStoreResult::fatal("earlier fatal failure");
    tracker.replaceAll(Timeline({}, std::nullopt), Timeline({}, std::nullopt));
    QVERIFY(tracker.isFatalLatched());

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    // This call's own replaceAll() also returns Fatal (the latch was
    // already set, so enterFatalState() logs only -- no dialog of its own).
    fakeDb.replaceDurationsResult = SessionStoreResult::fatal("this call's fatal failure");
    dialog.debugSkipCritical_ = true; // observe via debugCriticalShownCount_, not a real popup
    dialog.done(QDialog::Accepted);

    QVERIFY(!dialog.saveChanges());
    QCOMPARE(dialog.debugCriticalShownCount_, 1);
}

void HistoryDialogTest::test_C4_historydialog_suppresses_own_critical_when_this_call_latches()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    // Arrange: NOT latched before this dialog's own save.
    QVERIFY(!tracker.isFatalLatched());

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    // This call's replaceAll() is the one that raises the latch, so
    // TimeTracker's own fatalError modal already fired -- HistoryDialog
    // must show none of its own for the same failure.
    fakeDb.replaceDurationsResult = SessionStoreResult::fatal("this call's fatal failure");
    dialog.debugSkipCritical_ = true; // observe via debugCriticalShownCount_, not a real popup
    dialog.done(QDialog::Accepted);

    QVERIFY(!dialog.saveChanges());
    QCOMPARE(dialog.debugCriticalShownCount_, 0);
    QVERIFY(tracker.isFatalLatched());
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
    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-200), now.addSecs(-150)));

    // Act: the dialog is opened only now, strictly after the shutdown/
    // cancellation above -- D5's own requirement (History works immediately,
    // no new Start needed) is exercised by the Start above already having
    // succeeded; this proves the dialog itself is not still refused.
    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    fakeDb.callLog.clear();
    dialog.done(QDialog::Accepted);

    // Assert
    QVERIFY(dialog.saveChanges());
    QVERIFY(fakeDb.callLog.contains("replaceAll"));
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
    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-200), now.addSecs(-150)));

    // The dialog opens while the session is still live -- its snapshot
    // predates the shutdown that is about to run.
    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

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
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 0);
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

    QDateTime start = fixedNow().addSecs(-10);
    QDateTime end = fixedNow().addSecs(-2);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    HistoryDialog dialog(tracker, settings);

    QCOMPARE(dialog.table_dbg()->rowCount(), 1);
    QVERIFY(dialog.editSession_dbg().pageTitle(0).contains("entries: 1"));
    QVERIFY(!dialog.editSession_dbg().pageModified(0));

    // D9 review (finding 2): applySplit_dbg() must share onSplitRow()'s
    // production tail (session_.splitRow() + updateTable()) so a debug-only
    // path cannot silently diverge from what a real split renders.
    dialog.applySplit_dbg(0, 0, DurationType::Activity, DurationType::Pause);

    // Model side: count/title/dirty state update immediately.
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(2));
    QVERIFY(dialog.editSession_dbg().pageTitle(0).contains("entries: 2"));
    QVERIFY(dialog.editSession_dbg().pageModified(0));

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

    QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);

    QVERIFY(!dialog.editSession_dbg().pageModified(0));

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);
    box->setChecked(false);

    QVERIFY(dialog.editSession_dbg().pageModified(0));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background().color(), QColor(180, 216, 228, 255));
    QCOMPARE(dialog.table_dbg()->item(0, 1)->background().color(), QColor(180, 216, 228, 255));
    QCOMPARE(dialog.table_dbg()->item(0, 2)->background().color(), QColor(180, 216, 228, 255));
}

void HistoryDialogTest::test_D9_ongoing_toggle_highlights_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing().has_value());

    // Ongoing is the only row on this page (no completed rows), so it is row 0.
    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);

    // R14-002: previously the ongoing checkbox updated text/totals but never
    // applied a dirty highlight, unlike completed rows.
    box->setChecked(false);

    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].ongoing()->type, DurationType::Pause);
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background().color(), QColor(180, 216, 228, 255));

    tracker.useTimerViaButton(Button::Stop);
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
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

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

    QDateTime start = fixedNow().addSecs(-10);
    QDateTime end = fixedNow().addSecs(-2);
    tracker.seedCompletedDuration_dbg(TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

    {
        HistoryDialog dialog(tracker, settings);
        dialog.applySplit_dbg(0, 0, DurationType::Activity, DurationType::Pause);
        QVERIFY(dialog.editSession_dbg().pageModified(0));

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
    QVERIFY(!fakeDb.callLog.contains("replaceAll"));

    // Runtime is unchanged — still the original single Activity row.
    QCOMPARE(tracker.completedTimeline().completed().size(), static_cast<size_t>(1));
    QCOMPARE(tracker.completedTimeline().completed()[0].type, DurationType::Activity);
    QCOMPARE(tracker.completedTimeline().completed()[0].startTime, start);
    QCOMPARE(tracker.completedTimeline().completed()[0].endTime, end);
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

    QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);

    box->setChecked(false); // Activity -> Pause
    box->setChecked(true);  // Pause -> Activity: back to the original value

    QVERIFY(!dialog.editSession_dbg().pageModified(0));
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

    QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);

    QCheckBox* box = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(0, 3));
    QVERIFY(box != nullptr);

    box->setChecked(false);
    QVERIFY(dialog.editSession_dbg().pageModified(0));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background().color(), QColor(180, 216, 228, 255));

    box->setChecked(true); // same net effect as the pair above, checked separately
    QVERIFY(!dialog.editSession_dbg().pageModified(0));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background(), QBrush());
}

void HistoryDialogTest::test_D9_ongoing_toggle_then_navigate_away_and_back_highlights_ongoing_row()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 30));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // A completed row plus the live ongoing row, both on today's page. Start
    // then Pause: Pause finalizes the Activity segment into session_.durations
    // (1 completed row) and begins a new ongoing Pause segment. (Pushing a
    // completed row via sessionState_dbg() before Start from Mode::None does
    // not work -- Start() calls session_.resetForNewSession(), clearing it.)
    // The clock advances (was QTest::qWait(20) x2) are load-bearing: a
    // zero-elapsed Start->Pause would make addDuration() discard the
    // Activity segment as NonPositive (timetracker.cpp:1548-1552), and the
    // completed row this test depends on would silently vanish.
    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);
    tracker.useTimerViaButton(Button::Pause);
    clock.advance(20);

    // A historical page so Older/Newer has somewhere to go.
    const QDateTime histStart(QDate::currentDate().addDays(-1), QTime(10, 0, 0));
    QVERIFY(tracker.replaceAll(
        Timeline({TimeDuration::fromPersistedRow(DurationType::Activity, histStart, histStart.addSecs(300))}, std::nullopt),
        Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pages().size() >= static_cast<size_t>(2));

    // Ongoing is row 1 (after the one completed row).
    QCheckBox* ongoingBox = qobject_cast<QCheckBox*>(dialog.table_dbg()->cellWidget(1, 3));
    QVERIFY(ongoingBox != nullptr);
    // Ongoing starts as Pause (unchecked) after Start->Pause above; check it
    // to flip to Activity -- setChecked(false) on an already-unchecked box
    // would not emit stateChanged() at all.
    QVERIFY(!ongoingBox->isChecked());
    ongoingBox->setChecked(true);
    QVERIFY(dialog.editSession_dbg().pageModified(0));

    dialog.onOlder_dbg();  // page 1
    dialog.onNewer_dbg();  // back to page 0 — a full updateTable(0) rerender

    // Highlighting is page-level (pageModified() is page-scoped, see D9
    // review finding 1): once page 0 is dirty, BOTH editable rows -- the
    // untouched completed row and the toggled ongoing row -- are highlighted
    // by the same rule. Before the fix, only the completed row was (the
    // per-row loop in updateTable() never highlighted the ongoing row at all).
    QVERIFY(dialog.editSession_dbg().pageModified(0));
    QCOMPARE(dialog.table_dbg()->item(0, 0)->background().color(), QColor(180, 216, 228, 255));
    QCOMPARE(dialog.table_dbg()->item(1, 0)->background().color(), QColor(180, 216, 228, 255));

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryDialogTest::test_D9_split_too_short_row_does_not_open_modal()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // Anchored to a fixed local noon-ish time on today's date (not real now)
    // so the row can never straddle local midnight -- see the house
    // convention at test_timer.cpp:78 and git show 5e63eae. "Today" is still
    // needed here: pendingTimelines()[0] below is the current-day page.
    const QDateTime end(QDate::currentDate(), QTime(12, 0, 0), Qt::LocalTime);
    const QDateTime start = end.addMSecs(-2500); // 2.5s -- under the 3s minimum
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, start, end));

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

    dialog.setContextMenuTarget_dbg(0, 0);
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
    QCOMPARE(dialog.editSession_dbg().pendingTimelines()[0].completed().size(), static_cast<size_t>(1));
}

// ---------------------------------------------------------------------------
// M2: a successful save whose destructive replace ran without its pre-write
// safety backup must warn the user, not stay silent.
// ---------------------------------------------------------------------------

void HistoryDialogTest::test_M2_saveChanges_warns_when_backup_missing()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    SessionStoreResult r = SessionStoreResult::success();
    r.backupMissing = true;
    fakeDb.replaceDurationsResult = r;

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);

    QVERIFY(dialog.saveChanges());
    QCOMPARE(dialog.debugBackupWarningShownCount_, 1);
    QCOMPARE(dialog.debugCriticalShownCount_, 0);
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 1);
}

void HistoryDialogTest::test_M2_saveChanges_silent_when_backup_present()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);

    QVERIFY(dialog.saveChanges());
    QCOMPARE(dialog.debugBackupWarningShownCount_, 0);
    QCOMPARE(dialog.debugCriticalShownCount_, 0);
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 1);
}

// ========================================================================
// M3: a destructive History save that would silently drop a display-only
// cross-midnight row or a pre-existing overlap row must ask first.
// ========================================================================

void HistoryDialogTest::test_M3_saveChanges_decline_cross_midnight_drop_preserves_rows()
{
    // Same real-store fixture as
    // test_C3_single_boundary_cross_midnight_dropped_by_unrelated_save(),
    // but declining the new Step 2.6 confirmation this time: the save must
    // abort before any DB write, leaving the cross-midnight row in place.
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // A single-boundary cross-midnight row: yesterday 23:50 -> today 00:10.
    const QDate yesterday = QDate::currentDate().addDays(-1);
    const QDateTime crossMidnightStart(yesterday, QTime(23, 50, 0));
    const QDateTime crossMidnightEnd(QDate::currentDate(), QTime(0, 10, 0));

    // An ordinary same-day row the user will edit.
    const QDateTime ordinaryStart = fixedNow().addSecs(-100);
    const QDateTime ordinaryEnd   = fixedNow().addSecs(-50);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, ordinaryStart, ordinaryEnd));

    // Insert the cross-midnight row directly via SQL (bypassing the same-day
    // invariant every uTimer write path enforces).
    SegmentId crossMidnightId;
    {
        const QString connName = QString("test_m3_singleboundary_decline_%1").arg(
            reinterpret_cast<quintptr>(&db));
        QSqlDatabase rawDb = QSqlDatabase::addDatabase("QSQLITE", connName);
        rawDb.setDatabaseName(db_path_);
        QVERIFY(rawDb.open());
        crossMidnightId = TimeDuration::createSegmentId();
        QSqlQuery q(rawDb);
        q.prepare(
            "INSERT INTO durations "
            "(segment_id, type, start_utc, end_utc, is_finalized) "
            "VALUES (:sid, 0, :start_utc, :end_utc, 1)"
        );
        q.bindValue(":sid", crossMidnightId.toString());
        q.bindValue(":start_utc", crossMidnightStart.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":end_utc", crossMidnightEnd.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(q.exec());
        rawDb.close();
        rawDb = QSqlDatabase();
        QSqlDatabase::removeDatabase(connName);
    }

    HistoryDialog dialog(tracker, settings);

    // Edit the unrelated ordinary row.
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    dialog.done(QDialog::Accepted);
    dialog.debugDropAnswer_ = QMessageBox::No;

    QVERIFY(!dialog.saveChanges());

    // The DB was never touched: the cross-midnight row still exists (count 1)
    // and no other row was written.
    SqliteSessionStore db2(settings);
    QVERIFY(db2.ensureOpen_dbg());
    QSqlQuery check(db2.rawDb_dbg());
    check.prepare("SELECT COUNT(*) FROM durations WHERE segment_id = :sid");
    check.bindValue(":sid", crossMidnightId.toString());
    QVERIFY(check.exec());
    QVERIFY(check.next());
    QCOMPARE(check.value(0).toInt(), 1);

    QSqlQuery totalCheck(db2.rawDb_dbg());
    totalCheck.prepare("SELECT COUNT(*) FROM durations");
    QVERIFY(totalCheck.exec());
    QVERIFY(totalCheck.next());
    QCOMPARE(totalCheck.value(0).toInt(), 1);
    db2.closeConnection_dbg();
}

void HistoryDialogTest::test_M3_saveChanges_decline_pre_existing_overlap_preserves_rows()
{
    // Same FakeSessionStore fixture as
    // test_C3_saveChanges_drops_pre_existing_overlap_and_succeeds(), but
    // declining the new Step 2.6 confirmation: replaceAll() must never be
    // called and both original rows must remain.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDateTime now = fixedNow();
    const SegmentId activityId = SegmentId::mint();
    const SegmentId pauseId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-400), now.addSecs(-300), activityId));
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-350), now.addSecs(-250), pauseId));

    TimeTracker tracker(settings, fakeDb);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);
    dialog.debugDropAnswer_ = QMessageBox::No;

    QVERIFY(!dialog.saveChanges());
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 0);

    // replaceAll() was never called, so loadDurations() still projects the
    // preset rows unchanged -- both survive.
    const auto reload = fakeDb.loadDurations();
    bool activityPresent = false;
    bool pausePresent = false;
    for (const auto& d : reload.durations) {
        if (d.segment_id == activityId) activityPresent = true;
        if (d.segment_id == pauseId) pausePresent = true;
    }
    QVERIFY(activityPresent);
    QVERIFY(pausePresent);
}

void HistoryDialogTest::test_M3_saveChanges_combined_drop_causes_single_prompt()
{
    // Both category-2 (single-boundary cross-midnight) and category-3
    // (pre-existing overlap) drops occur in the same save, alongside an
    // unrelated edited row. Exactly one confirmation is asked, and its text
    // carries both clauses.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDate today = QDate::currentDate();
    const QDate yesterday = today.addDays(-1);

    // One single-boundary cross-midnight row: yesterday 23:50 -> today 00:10.
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity,
            QDateTime(yesterday, QTime(23, 50, 0)), QDateTime(today, QTime(0, 10, 0))));

    // One pre-existing Activity/Pause overlap pair, both persisted+unedited.
    const QDateTime now = fixedNow();
    const SegmentId activityId = SegmentId::mint();
    const SegmentId pauseId = SegmentId::mint();
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-400), now.addSecs(-300), activityId));
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-350), now.addSecs(-250), pauseId));

    TimeTracker tracker(settings, fakeDb);
    // An unrelated row the user edits.
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);
    dialog.debugDropAnswer_ = QMessageBox::Yes;

    QVERIFY(dialog.saveChanges());
    QVERIFY(!dialog.debugDropAnswer_.has_value());
    QVERIFY(dialog.debugDropPromptText_.contains("1 entries spanning midnight"));
    QVERIFY(dialog.debugDropPromptText_.contains("1 pre-existing overlapping"));
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 1);
}

void HistoryDialogTest::test_M3_saveChanges_no_prompt_when_nothing_is_replaced()
{
    // Both save buckets can be empty (nothing will actually be replaced)
    // while hasEdits() is still true and a display-only cross-midnight row
    // exists -- the Step 2.6 prompt must not fire a false "will be removed"
    // claim, and must not consume the debug answer seam.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;

    const QDate today = QDate::currentDate();
    const QDate yesterday = today.addDays(-1);
    // A display-only cross-midnight row (never enters either save bucket).
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity,
            QDateTime(yesterday, QTime(23, 50, 0)), QDateTime(today, QTime(0, 10, 0))));

    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    tracker.useTimerViaButton(Button::Start);
    clock.advance(30000); // positive-length ongoing at dialog-open

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().pendingTimelines()[0].ongoing().has_value());

    // A8 shape: toggle the ongoing row's type (a real edit), then jump the
    // clock backward past the segment start. saveChanges()'s refreshOngoing()
    // then re-anchors it to a zero-length row that enters neither save
    // bucket -- so hasEdits() is true but willReplace ends up false.
    QVERIFY(dialog.editSession_dbg().setOngoingType(0, DurationType::Pause));
    clock.advance(-120000);

    fakeDb.callLog.clear();
    dialog.debugSkipCritical_ = true;
    dialog.done(QDialog::Accepted);
    dialog.debugDropAnswer_ = QMessageBox::No;

    QVERIFY(dialog.saveChanges());
    QVERIFY(dialog.debugDropAnswer_.has_value()); // not consumed -- prompt never shown
    QVERIFY(dialog.debugDropPromptText_.isEmpty());
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 0);

    // Cleanup: restore the clock ahead of the segment start before stopping.
    clock.setNow(fixedNow().addSecs(300));
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

    // Save a Pause row to the DB (historical, earlier today)
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-200), now.addSecs(-150)));

    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(now);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Write the historical row to the DB via replaceAll
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

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
    dialog.editSession_dbg().setPageTimeline(0,
        dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Activity));

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
    // Add an Activity row earlier today (same-day, so it's on page 0)
    fakeDb.loadDurationsResult.durations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

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
    dialog.editSession_dbg().setPageTimeline(0,
        dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

    // Arrange the finished spy
    QSignalSpy finished(&dialog, &QDialog::finished);

    // Act: emit the revoke signal
    emit tracker.exclusiveEditRevoked();

    // Wait for the finished signal to reach count 1 (the queued close)
    QTRY_COMPARE(finished.count(), 1);

    // Now call accept(), which sets result to Accepted and calls saveChanges()
    fakeDb.callLog.clear();
    dialog.accept();

    // Assert: replaceAll was never called
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 0);

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
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-100), now.addSecs(-50)));

    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(now);
    TimeTracker tracker(settings, fakeDb);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(5); // Ensure positive-length ongoing

    HistoryDialog dialog(tracker, settings);
    // No edits made: exercises saveChanges()'s no-edit early return.

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
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 0);
    QCOMPARE(fakeDb.callLog.count("commitSession"), 0);
}

void HistoryDialogTest::test_historydialog_construction_loads_store_exactly_once()
{
    // S1: the constructor builds the displayed model via
    // session_.buildFromTimer() (which itself calls loadHistoryFromStore()
    // once) and must not read the store a second time just for a log count.
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

    QCOMPARE(fakeDb.callLog.count("loadDurations"), 1);

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

    // Historical Pause row earlier the same day.
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause,
        segStart.addSecs(-200), segStart.addSecs(-150)));

    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(segStart);
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    tracker.useTimerViaButton(Button::Start);
    QVERIFY(tracker.isActive());
    const SegmentId liveSegmentId = tracker.getOngoingDuration()->segment_id;

    const QDateTime cutoff(today, QTime(23, 59, 59, 500), Qt::LocalTime);

    {
        HistoryDialog dialog(tracker, settings);

        // Flip the historical Pause row to Activity (an edit); no prompt raised.
        dialog.editSession_dbg().setPageTimeline(0,
            dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Activity));

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

    // Arrange: a completed segment to edit.
    const QDateTime now = clock.now();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-200), now.addSecs(-150)));

    // Open the dialog and begin an edit.
    HistoryDialog dialog(tracker, settings);
    dialog.editSession_dbg().setPageTimeline(0,
        dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(0, DurationType::Pause));

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
    QCOMPARE(fakeDb.callLog.count("replaceAll"), 0);
    QCOMPARE(dialogFinished.count(), 1); // No finished(Accepted) emitted.
}
