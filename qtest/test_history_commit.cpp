#include "test_history_commit.h"
#include <QtTest>
#include <QMessageBox>
#include <algorithm>

#include "../historydialog.h"
#include "../historyeditsession.h"

using TestCommon::createSettingsFile;

void HistoryCommitTest::initTestCase()
{
    db_path_ = QDir(QCoreApplication::applicationDirPath()).filePath("uTimer.sqlite");
    if (QFile::exists(db_path_)) {
        db_backup_path_ = db_path_ + ".bak_commit_test";
        QFile::remove(db_backup_path_);
        QVERIFY(QFile::rename(db_path_, db_backup_path_));
    }
}

void HistoryCommitTest::cleanupTestCase()
{
    if (!db_path_.isEmpty())
        QFile::remove(db_path_);
    if (!db_backup_path_.isEmpty()) {
        QFile::remove(db_path_);
        QVERIFY(QFile::rename(db_backup_path_, db_path_));
    }
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

    const QDateTime now = QDateTime::currentDateTime();
    const TimeDuration seeded =
        TimeDuration::fromPersistedRow(DurationType::Activity, now.addSecs(-30), now.addSecs(-20));
    tracker.seedCompletedDuration_dbg(seeded);

    std::deque<TimeDuration> dbDurations;
    dbDurations.push_back(TimeDuration::fromPersistedRow(DurationType::Pause, now.addSecs(-19), now.addSecs(-10)));
    QVERIFY(tracker.replaceAll(Timeline(dbDurations, std::nullopt), Timeline({}, std::nullopt)).ok());

    HistoryDialog dialog(tracker, settings);

    // A10: an editless OK is a no-op, so make a genuine edit (toggle the DB row's
    // type) to exercise the save path that must fail under the exclusive lock.
    dialog.editSession_dbg().setPageTimeline(0, dialog.editSession_dbg().pendingTimelines()[0].withSegmentType(1, DurationType::Activity));

    // Hold an exclusive DB transaction to force replaceAll to fail.
    const QString lockConnName = "commit_test_db_lock";
    {
        QSqlDatabase lockDb = QSqlDatabase::addDatabase("QSQLITE", lockConnName);
        lockDb.setDatabaseName(db_path_);
        QVERIFY(lockDb.open());
        QSqlQuery lockQuery(lockDb);
        QVERIFY(lockQuery.exec("BEGIN EXCLUSIVE TRANSACTION"));

        // Skip QMessageBox::critical() to avoid offscreen QPA propagateSizeHints() warnings.
        // The DB-failure path is still fully exercised: replaceAll fails, debugSkipCritical_
        // suppresses only the dialog, and accept() is expected to NOT close the dialog.
        dialog.debugSkipCritical_ = true;
        dialog.accept();

        QVERIFY(lockQuery.exec("ROLLBACK"));
        lockDb.close();
    }
    QSqlDatabase::removeDatabase(lockConnName);

    // accept() must NOT have closed the dialog — result() should still be the
    // default (not QDialog::Accepted).
    QVERIFY(dialog.result() != QDialog::Accepted);

    // TimeTracker state must be unchanged.
    const auto completedAfterFailedAccept = tracker.completedTimeline();
    QCOMPARE(completedAfterFailedAccept.completed().size(), static_cast<size_t>(1));
    const auto& rowAfterFailedAccept = *std::find_if(
        completedAfterFailedAccept.completed().begin(), completedAfterFailedAccept.completed().end(),
        [&](const TimeDuration& d) { return d.segment_id == seeded.segment_id; });
    QCOMPARE(rowAfterFailedAccept.type, DurationType::Activity);
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

    // A10: an editless OK is a no-op. Make a genuine same-type edit — split the
    // memory row into two Activity halves — so the save runs and the overlap
    // merge confirmation is still triggered.
    dialog.applySplit_dbg(0, 0, DurationType::Activity, DurationType::Activity);

    // Bypass QMessageBox::question() to avoid offscreen QPA propagateSizeHints() warnings.
    // Supplies "No" — same as user declining the merge — so accept() should not close the dialog.
    dialog.debugOverlapAnswer_ = QMessageBox::No;
    dialog.accept();

    // Dialog must remain open (result still not Accepted).
    QVERIFY(dialog.result() != QDialog::Accepted);

    // TimeTracker session unchanged.
    QCOMPARE(tracker.completedTimeline().completed().size(), static_cast<size_t>(1));
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

    const QDateTime now = QDateTime::currentDateTime();
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
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(!tracker.isCheckpointTimerActive_dbg());

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
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(!tracker.isCheckpointTimerActive_dbg());

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
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", 1);
    writer.sync();

    Settings settings(settingsPath);
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    {
        HistoryDialog dialog(tracker, settings);
        QVERIFY(tracker.isDialogOpen_dbg());
        QVERIFY(!tracker.isCheckpointTimerActive_dbg());
        // Dialog is destroyed here without accept or reject.
    }

    // After destruction, exclusive edit must have ended.
    QVERIFY(!tracker.isDialogOpen_dbg());
    QVERIFY(tracker.isCheckpointTimerActive_dbg());

    tracker.useTimerViaButton(Button::Stop);
}

// ---------------------------------------------------------------------------
// A9: a merge-save must not demote finalized history rows to unfinalized, or a
// crash + recovery >24 h later would delete them as stale orphans.  Completed
// rows are finalized on save; only the ongoing row stays unfinalized.
// ---------------------------------------------------------------------------

void HistoryCommitTest::test_merge_save_survives_late_crash_recovery()
{
    resetDatabaseFile();
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    SqliteSessionStore db(settings);
    TimeTracker tracker(settings, db);

    // A finalized DB history row (today) 10:00–10:30.
    const QDate today = QDate::currentDate();
    const QDateTime dbStart(today, QTime(10, 0, 0), Qt::UTC);
    const QDateTime dbEnd(today, QTime(10, 30, 0), Qt::UTC);
    std::deque<TimeDuration> dbRows;
    dbRows.push_back(TimeDuration::fromPersistedRow(DurationType::Activity, dbStart, dbEnd));
    QVERIFY(tracker.replaceAll(Timeline(dbRows, std::nullopt), Timeline({}, std::nullopt)).ok());

    // An in-memory session row 10:15–10:45 that overlaps the DB row → merge.
    const QDateTime memStart(today, QTime(10, 15, 0), Qt::UTC);
    const QDateTime memEnd(today, QTime(10, 45, 0), Qt::UTC);
    tracker.seedCompletedDuration_dbg(
        TimeDuration::fromPersistedRow(DurationType::Activity, memStart, memEnd));

    // Build the save payload (normalization merges the two rows) and persist it,
    // exactly as HistoryDialog::saveChanges() does.
    HistoryEditSession session;
    session.buildFromTimer(tracker, QDate::currentDate());
    const auto payload = session.buildSavePayload();
    QVERIFY(payload.needsMergeConfirmation);
    QVERIFY(tracker.replaceAll(payload.historyTimeline, payload.sessionTimeline).ok());

    // Simulate a crash (no clean stop → no clean-shutdown marker) followed by a
    // relaunch >24 h later.  Stale-orphan policy would drop any unfinalized row.
    const QDateTime later = memEnd.addSecs(25 * 3600);
    const StartupRecoveryResult res = db.recoverStartupCheckpoints(later);
    QVERIFY(res.ok());

    // The merged row must survive: it was finalized on save, not demoted.
    const auto loaded = db.loadDurations();
    QCOMPARE(static_cast<int>(loaded.size()), 1);
    QCOMPARE(loaded.durations[0].startTime.toUTC().time(), QTime(10, 0, 0));
    QCOMPARE(loaded.durations[0].endTime.toUTC().time(), QTime(10, 45, 0));
}
