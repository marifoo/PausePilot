#include "test_history_commit.h"
#include "fakeclock.h"
#include <QtTest>
#include <QMessageBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <algorithm>

#include "../historydialog.h"

using TestCommon::createSettingsFile;
using TestCommon::fixedNow;

void HistoryCommitTest::initTestCase()
{
    QVERIFY(data_dir_.isValid());
    TestCommon::setDataDirOverride(data_dir_.path());
    db_path_ = QDir(data_dir_.path()).filePath("uTimer.sqlite");
}

void HistoryCommitTest::cleanupTestCase()
{
    TestCommon::clearDataDirOverride();
}

void HistoryCommitTest::resetDatabaseFile() const
{
    if (QFile::exists(db_path_))
        QFile::remove(db_path_);
}

// ---------------------------------------------------------------------------
// accept() blocks close on DB failure
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_accept_stays_open_on_db_failure()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // WP4b: not Today-specific -- both rows moved onto a past-date page
    // seeded in the store (page 1; page 0 is the now-empty Today page),
    // rather than today's memory + replaceAll rows.
    const QDateTime yesterday = fixedNow().addDays(-1);
    const TimeDuration unaffected =
        TimeDuration::fromPersistedRow(DurationType::Activity, yesterday.addSecs(-30), yesterday.addSecs(-20));
    const TimeDuration edited =
        TimeDuration::fromPersistedRow(DurationType::Pause, yesterday.addSecs(-19), yesterday.addSecs(-10));
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(unaffected);
    dbDurations.push_back(edited);
    TestCommon::seedStoredRows(db, dbDurations, true);

    HistoryDialog dialog(tracker, settings);

    // A10: an editless OK is a no-op, so make a genuine edit (toggle the second
    // row's type) to exercise the save path that must fail under the exclusive lock.
    QVERIFY(dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(1, DurationType::Activity)));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // Hold an exclusive DB transaction to force the date-scoped save to fail.
    const QString lockConnName = "commit_test_db_lock";
    {
        QSqlDatabase lockDb = QSqlDatabase::addDatabase("QSQLITE", lockConnName);
        lockDb.setDatabaseName(db_path_);
        QVERIFY(lockDb.open());
        QSqlQuery lockQuery(lockDb);
        QVERIFY(lockQuery.exec("BEGIN EXCLUSIVE TRANSACTION"));

        // Skip QMessageBox::critical() to avoid offscreen QPA propagateSizeHints() warnings.
        // The DB-failure path is still fully exercised: the date-scoped save fails,
        // debugSkipCritical_ suppresses only the dialog, and accept() is expected to
        // NOT close the dialog.
        dialog.debugSkipCritical_ = true;
        dialog.accept();

        QVERIFY(lockQuery.exec("ROLLBACK"));
        lockDb.close();
    }
    QSqlDatabase::removeDatabase(lockConnName);

    // accept() must NOT have closed the dialog — result() should still be the
    // default (not QDialog::Accepted).
    QVERIFY(dialog.result() != QDialog::Accepted);

    // Store state must be unchanged: the failed date-scoped save wrote nothing.
    const auto loadedAfterFailedAccept = db.loadDurations();
    QCOMPARE(static_cast<int>(loadedAfterFailedAccept.size()), 2);
    const auto& rowAfterFailedAccept = *std::find_if(
        loadedAfterFailedAccept.durations.begin(), loadedAfterFailedAccept.durations.end(),
        [&](const TimeDuration& d) { return d.segment_id == unaffected.segment_id; });
    QCOMPARE(rowAfterFailedAccept.type, DurationType::Activity);
    const auto& editedRowAfterFailedAccept = *std::find_if(
        loadedAfterFailedAccept.durations.begin(), loadedAfterFailedAccept.durations.end(),
        [&](const TimeDuration& d) { return d.segment_id == edited.segment_id; });
    QCOMPARE(editedRowAfterFailedAccept.type, DurationType::Pause);
}

// ---------------------------------------------------------------------------
// accept() blocks close when user declines merge confirmation
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_accept_stays_open_on_merge_decline()
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

    // A10: an editless OK is a no-op. Make a genuine same-type edit — split the
    // first row into two Activity halves — so the save runs and the overlap
    // merge confirmation is still triggered against the second, overlapping row.
    dialog.applySplit_dbg(0, 1, DurationType::Activity, DurationType::Activity);
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // Bypass QMessageBox::question() to avoid offscreen QPA propagateSizeHints() warnings.
    // Supplies "No" — same as user declining the merge — so accept() should not close the dialog.
    dialog.debugOverlapAnswer_ = QMessageBox::No;
    dialog.accept();

    // Dialog must remain open (result still not Accepted).
    QVERIFY(dialog.result() != QDialog::Accepted);

    // Store unchanged: the declined merge never reached the date-scoped save.
    QCOMPARE(static_cast<int>(db.loadDurations().size()), 2);
}

// ---------------------------------------------------------------------------
// accept() closes on success
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_accept_closes_on_success()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime now = fixedNow();
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-30), now.addSecs(-10)));

    HistoryDialog dialog(tracker, settings);
    dialog.accept();

    // On success, QDialog::accept() sets result to Accepted.
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
}

// ---------------------------------------------------------------------------
// RAII guard: endExclusiveEdit called on accept path
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_raii_guard_resumes_on_accept()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    // D15/F1 (part 3, B2): the scheduled midnight stop is heartbeat-driven
    // (C5), with no dedicated QTimer of its own, and this test never calls
    // onTick() itself, so it cannot fire during this test's run regardless
    // of the real wall clock. Pinning to fixedNow() (local noon) still keeps
    // segment_start_time independent of when the test happens to run.
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(tracker.isCheckpointTimerActive_dbg());

        dialog.accept();
    }

    // After accept (and dialog destruction), exclusive edit must have ended.
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// RAII guard: endExclusiveEdit called on reject path
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_raii_guard_resumes_on_reject()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    // D15/F1 (part 3, B2): the scheduled midnight stop is heartbeat-driven
    // (C5), with no dedicated QTimer of its own, and this test never calls
    // onTick() itself, so it cannot fire during this test's run regardless
    // of the real wall clock. Pinning to fixedNow() (local noon) still keeps
    // segment_start_time independent of when the test happens to run.
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(tracker.isCheckpointTimerActive_dbg());

        dialog.reject();
    }

    // After reject, exclusive edit must have ended.
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// RAII guard: endExclusiveEdit called on destruction without accept/reject
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_raii_guard_resumes_on_destruction()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    // D15/F1 (part 3, B2): the scheduled midnight stop is heartbeat-driven
    // (C5), with no dedicated QTimer of its own, and this test never calls
    // onTick() itself, so it cannot fire during this test's run regardless
    // of the real wall clock. Pinning to fixedNow() (local noon) still keeps
    // segment_start_time independent of when the test happens to run.
    // FakeClock declared before TimeTracker so it outlives it (see the
    // outlives-tracker note at test_integration.cpp:614).
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    tracker.useTimerViaButton(Button::Start);
    clock.advance(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(tracker.isCheckpointTimerActive_dbg());
        // Dialog is destroyed here without accept or reject.
    }

    // After destruction, exclusive edit must have ended.
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// WP5 acceptance: a date-scoped save never touches backup/export sidecars.
// saveHistoryDays() never calls createBackup() (unlike replaceAll()), so a
// save through the dialog must leave the DB directory's file listing --
// including any pre-existing *.backup / *.durations.txt sidecars -- exactly
// as it was: same files, same sizes, same mtimes. This guards against a
// later change that adds a backup/export to the scoped path, or routes a
// save back through replaceAll.
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_save_leaves_sidecars_unchanged()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    const QDateTime yesterday = fixedNow().addDays(-1);
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, yesterday.addSecs(-30), yesterday.addSecs(-10)));
    TestCommon::seedStoredRows(db, dbDurations, true);

    // Seed old sidecar files named the way createBackup() would name them
    // (sqlitesessionstore.cpp: base + ".<timestamp>.backup" and
    // "...durations.txt"), in the DB's own directory.
    const QString backupPath = db_path_ + ".2020-01-01T00-00-00-000.backup";
    const QString durationsPath = db_path_ + ".2020-01-01T00-00-00-000.durations.txt";
    {
        QFile backupFile(backupPath);
        QVERIFY(backupFile.open(QIODevice::WriteOnly));
        backupFile.write("old backup contents");
        backupFile.close();
        QFile durationsFile(durationsPath);
        QVERIFY(durationsFile.open(QIODevice::WriteOnly));
        durationsFile.write("old durations contents");
        durationsFile.close();
    }

    const QDir dbDir(QFileInfo(db_path_).absoluteDir());
    const QStringList entriesBefore = dbDir.entryList(QDir::Files, QDir::Name);
    QVERIFY(entriesBefore.contains(QFileInfo(backupPath).fileName()));
    QVERIFY(entriesBefore.contains(QFileInfo(durationsPath).fileName()));
    const qint64 backupSizeBefore = QFileInfo(backupPath).size();
    const qint64 durationsSizeBefore = QFileInfo(durationsPath).size();
    const QDateTime backupMtimeBefore = QFileInfo(backupPath).lastModified();
    const QDateTime durationsMtimeBefore = QFileInfo(durationsPath).lastModified();

    HistoryDialog dialog(tracker, settings);
    QVERIFY(dialog.editSession_dbg().setPageTimeline(1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause)));
    QVERIFY(dialog.editSession_dbg().hasEdits());

    dialog.accept();

    // The save must have succeeded (Accepted), so the comparison below is of
    // a real save, not a no-op.
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));

    const QStringList entriesAfter = dbDir.entryList(QDir::Files, QDir::Name);
    QCOMPARE(entriesAfter, entriesBefore);
    QCOMPARE(QFileInfo(backupPath).size(), backupSizeBefore);
    QCOMPARE(QFileInfo(durationsPath).size(), durationsSizeBefore);
    QCOMPARE(QFileInfo(backupPath).lastModified(), backupMtimeBefore);
    QCOMPARE(QFileInfo(durationsPath).lastModified(), durationsMtimeBefore);
}

// ---------------------------------------------------------------------------
// WP6: checkpoints keep running while History is open. A past-day edit while
// Activity continues must not touch the live checkpoint row: the scoped save
// only ever writes the selected date's rows, so the live row's id, start,
// type, end and is_finalized=0 must be byte-identical across the save. A
// later ordinary checkpoint may still advance its end.
// ---------------------------------------------------------------------------

namespace {

struct LiveCheckpointRow {
    QString segmentId, type, startUtc, endUtc, isFinalized;
    bool found = false;
};

LiveCheckpointRow readLiveCheckpointRow(SqliteSessionStore& db, const QString& segmentId)
{
    LiveCheckpointRow row;
    if (!db.ensureOpen_dbg()) return row;
    QSqlQuery query(db.rawDb_dbg());
    query.prepare("SELECT segment_id, type, start_utc, end_utc, is_finalized FROM durations "
                  "WHERE segment_id = :segment_id AND is_finalized = 0");
    query.bindValue(":segment_id", segmentId);
    if (!query.exec() || !query.next()) return row;
    row.segmentId = query.value(0).toString();
    row.type = query.value(1).toString();
    row.startUtc = query.value(2).toString();
    row.endUtc = query.value(3).toString();
    row.isFinalized = query.value(4).toString();
    row.found = true;
    return row;
}

} // namespace

void HistoryCommitTest::test_wp6_past_day_edit_preserves_live_checkpoint_while_activity()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    // FakeClock declared before TimeTracker so it outlives it.
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // A past-date row to edit (page 1; page 0 is the empty Today page).
    const QDateTime yesterday = fixedNow().addDays(-1);
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, yesterday.addSecs(-30), yesterday.addSecs(-10)));
    TestCommon::seedStoredRows(db, dbDurations, true);

    // Arrange: Activity with a persisted live checkpoint.
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60 * 1000);
    tracker.triggerCheckpoint_dbg();
    const QString liveSegmentId = tracker.getOngoingDuration()->segment_id.toString();

    const LiveCheckpointRow before = readLiveCheckpointRow(db, liveSegmentId);
    QVERIFY(before.found);
    QCOMPARE(before.isFinalized, QString("0"));

    // Act: open History, edit the past date, hold the clock, save.
    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(tracker.isCheckpointTimerActive_dbg());

        QVERIFY(dialog.editSession_dbg().setPageTimeline(
            1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause)));
        QVERIFY(dialog.editSession_dbg().hasEdits());

        dialog.accept();
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    }

    // Assert: the live row is untouched by the scoped save.
    const LiveCheckpointRow afterSave = readLiveCheckpointRow(db, liveSegmentId);
    QVERIFY(afterSave.found);
    QCOMPARE(afterSave.segmentId, before.segmentId);
    QCOMPARE(afterSave.type, before.type);
    QCOMPARE(afterSave.startUtc, before.startUtc);
    QCOMPARE(afterSave.endUtc, before.endUtc);
    QCOMPARE(afterSave.isFinalized, before.isFinalized);
    QVERIFY(tracker.isActive());

    // A later ordinary checkpoint still advances the same row's end.
    clock.advance(60 * 1000);
    tracker.triggerCheckpoint_dbg();
    const LiveCheckpointRow afterCheckpoint = readLiveCheckpointRow(db, liveSegmentId);
    QVERIFY(afterCheckpoint.found);
    QCOMPARE(afterCheckpoint.segmentId, before.segmentId);
    QCOMPARE(afterCheckpoint.startUtc, before.startUtc);
    QVERIFY(afterCheckpoint.endUtc != before.endUtc);

    tracker.useTimerViaButton(Button::Stop);
}

void HistoryCommitTest::test_wp6_past_day_edit_while_paused_leaves_session_unchanged()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    const QDateTime yesterday = fixedNow().addDays(-1);
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(
        TimeDuration::fromPersistedRow(DurationType::Activity, yesterday.addSecs(-30), yesterday.addSecs(-10)));
    TestCommon::seedStoredRows(db, dbDurations, true);

    // Arrange: Pause, with no mode change expected across the edit.
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60 * 1000);
    tracker.useTimerViaButton(Button::Pause);
    QVERIFY(tracker.isPaused());

    const std::deque<TimeDuration> durationsBefore = tracker.sessionState_dbg().durations;
    const std::deque<TimeDuration> pendingBefore = tracker.pendingCommitPayload_dbg();

    // Act: open and save while paused.
    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(dialog.editSession_dbg().setPageTimeline(
            1, dialog.editSession_dbg().pendingTimelines()[1].withSegmentType(0, DurationType::Pause)));
        QVERIFY(dialog.editSession_dbg().hasEdits());

        dialog.accept();
        QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    }

    // Assert: mode, memory rows and the pending-commit payload are unaffected.
    QVERIFY(tracker.isPaused());
    QCOMPARE(tracker.sessionState_dbg().durations.size(), durationsBefore.size());
    for (size_t i = 0; i < durationsBefore.size(); ++i) {
        QCOMPARE(tracker.sessionState_dbg().durations[i].segment_id.toString(),
                 durationsBefore[i].segment_id.toString());
    }
    QCOMPARE(tracker.pendingCommitPayload_dbg().size(), pendingBefore.size());

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// WP6/F3 replacement (simplification_plan.md "F3 regression replacement"):
// a real transient SQL failure during the scoped save -- a UNIQUE clash on
// the split's second half -- must leave the selected date byte-identical, the
// live segment and its checkpoint untouched, and the edits retained in the
// dialog. A retry after the clash clears must then succeed, touching only
// the selected date, with the same live segment_id still unfinalized and a
// later checkpoint still advancing it.
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_wp6_F3_transient_save_failure_then_retry_preserves_live_segment()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    FakeClock clock(fixedNow());
    TimeTracker tracker(settings, db);
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Arrange: a live Activity segment with a known segment_id and a
    // persisted checkpoint.
    tracker.useTimerViaButton(Button::Start);
    clock.advance(60 * 1000);
    tracker.triggerCheckpoint_dbg();
    const QString liveSegmentId = tracker.getOngoingDuration()->segment_id.toString();
    const LiveCheckpointRow checkpointBefore = readLiveCheckpointRow(db, liveSegmentId);
    QVERIFY(checkpointBefore.found);

    // A past-date row (page 1) long enough to split.
    const QDateTime yesterday = fixedNow().addDays(-1);
    const QDateTime rowStart = yesterday.addSecs(-60);
    const QDateTime rowEnd = yesterday;
    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, rowStart, rowEnd));
    TestCommon::seedStoredRows(db, dbDurations, true);

    HistoryDialog dialog(tracker, settings);
    dialog.debugSkipCritical_ = true;

    const QDateTime splitAt = rowStart.addSecs(30);
    const auto outcome = dialog.editSession_dbg().splitRow(1, 0, splitAt, DurationType::Activity, DurationType::Pause);
    QVERIFY(outcome.applied);
    QVERIFY(dialog.editSession_dbg().hasEdits());
    const QString secondId = outcome.secondId.toString();

    // Snapshot the selected date's rows before the first (failing) attempt.
    auto readDateRows = [&](const QDateTime& start, const QDateTime& end) {
        QStringList rows;
        if (!db.ensureOpen_dbg()) return rows;
        QSqlQuery q(db.rawDb_dbg());
        q.prepare("SELECT segment_id, type, start_utc, end_utc, is_finalized FROM durations "
                  "WHERE start_utc < :end AND end_utc > :start ORDER BY start_utc");
        q.bindValue(":start", start.toUTC().toString(Qt::ISODateWithMs));
        q.bindValue(":end", end.toUTC().toString(Qt::ISODateWithMs));
        if (!q.exec()) return rows;
        while (q.next()) {
            rows << QString("%1|%2|%3|%4|%5")
                        .arg(q.value(0).toString(), q.value(1).toString(), q.value(2).toString(),
                             q.value(3).toString(), q.value(4).toString());
        }
        return rows;
    };
    const QDateTime dayStart(yesterday.date(), QTime(0, 0, 0), Qt::LocalTime);
    const QDateTime dayEnd = dayStart.addDays(1);
    const QStringList selectedDateBefore = readDateRows(dayStart, dayEnd);

    // Inject a real transient failure: raw-insert a finalized row on a
    // different date whose segment_id equals the split's secondId, so the
    // scoped save's INSERT hits UNIQUE(segment_id) after its UPDATE.
    const QDateTime otherDate = yesterday.addDays(-5);
    {
        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery clash(db.rawDb_dbg());
        clash.prepare("INSERT INTO durations (segment_id, type, start_utc, end_utc, is_finalized) "
                      "VALUES (:segment_id, 0, :start_utc, :end_utc, 1)");
        clash.bindValue(":segment_id", secondId);
        clash.bindValue(":start_utc", otherDate.addSecs(-20).toUTC().toString(Qt::ISODateWithMs));
        clash.bindValue(":end_utc", otherDate.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(clash.exec());
    }

    // First attempt: the save must fail with TransientError.
    dialog.accept();
    QVERIFY(dialog.result() != QDialog::Accepted);
    QCOMPARE(readDateRows(dayStart, dayEnd), selectedDateBefore);
    QVERIFY(tracker.isActive());
    QCOMPARE(tracker.getOngoingDuration()->segment_id.toString(), liveSegmentId);
    const LiveCheckpointRow checkpointAfterFailure = readLiveCheckpointRow(db, liveSegmentId);
    QVERIFY(checkpointAfterFailure.found);
    QCOMPARE(checkpointAfterFailure.startUtc, checkpointBefore.startUtc);
    QCOMPARE(checkpointAfterFailure.endUtc, checkpointBefore.endUtc);
    QVERIFY(dialog.editSession_dbg().hasEdits());

    // Remove the clashing row.
    {
        QVERIFY(db.ensureOpen_dbg());
        QSqlQuery cleanup(db.rawDb_dbg());
        cleanup.prepare("DELETE FROM durations WHERE segment_id = :segment_id AND start_utc < :cutoff");
        cleanup.bindValue(":segment_id", secondId);
        cleanup.bindValue(":cutoff", dayStart.toUTC().toString(Qt::ISODateWithMs));
        QVERIFY(cleanup.exec());
    }

    // Second attempt: succeeds, only the selected date changed.
    dialog.accept();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QCOMPARE(readDateRows(dayStart, dayEnd).size(), 2);

    // The same live segment_id is present as an unfinalized row in SQLite
    // and in the engine's ongoing view.
    const LiveCheckpointRow checkpointAfterSave = readLiveCheckpointRow(db, liveSegmentId);
    QVERIFY(checkpointAfterSave.found);
    QCOMPARE(tracker.getOngoingDuration()->segment_id.toString(), liveSegmentId);

    // A later checkpoint advances its end.
    clock.advance(60 * 1000);
    tracker.triggerCheckpoint_dbg();
    const LiveCheckpointRow checkpointAfterLater = readLiveCheckpointRow(db, liveSegmentId);
    QVERIFY(checkpointAfterLater.found);
    QVERIFY(checkpointAfterLater.endUtc != checkpointAfterSave.endUtc);

    tracker.useTimerViaButton(Button::Stop);
}
