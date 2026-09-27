#ifndef TEST_HISTORY_COMMIT_H
#define TEST_HISTORY_COMMIT_H

#include <QObject>
#include "testcommon.h"

/**
 * HistoryCommitTest — accept() blocks close on DB failure,
 * and TimerExclusiveEditGuard ends the exclusive edit on all exit paths.
 * WP6: checkpoints keep running for the dialog's whole lifetime, so these
 * RAII-guard tests assert the checkpoint timer stays active throughout.
 */
class HistoryCommitTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QTemporaryDir data_dir_;

    void resetDatabaseFile() const;

private slots:
    void initTestCase();
    void cleanupTestCase();

    // accept() blocks close on DB failure
    void test_accept_stays_open_on_db_failure();

    // accept() blocks close when user declines merge confirmation
    void test_accept_stays_open_on_merge_decline();

    // accept() closes on success
    void test_accept_closes_on_success();

    // RAII guard: endExclusiveEdit called on accept path
    void test_raii_guard_resumes_on_accept();

    // RAII guard: endExclusiveEdit called on reject path
    void test_raii_guard_resumes_on_reject();

    // RAII guard: endExclusiveEdit called on destruction without accept/reject
    void test_raii_guard_resumes_on_destruction();

    // A9: merge-save finalizes completed rows so a late crash-recovery keeps them
    void test_merge_save_survives_late_crash_recovery();

    // WP5 acceptance: a date-scoped save leaves backup/export sidecars untouched
    void test_save_leaves_sidecars_unchanged();

    // WP6 acceptance: a past-day edit while Activity continues preserves the
    // live checkpoint row byte-for-byte, and a later checkpoint still advances it.
    void test_wp6_past_day_edit_preserves_live_checkpoint_while_activity();

    // WP6 acceptance: the Pause variant -- no mode change, session_.durations
    // and the pending-commit payload are unaffected by the past-day save.
    void test_wp6_past_day_edit_while_paused_leaves_session_unchanged();

    // WP6/F3 replacement: a real transient SQL failure during a scoped save
    // (UNIQUE clash on the split's second half) leaves the live checkpoint and
    // segment untouched and the edits retained; a retry after the clash clears
    // succeeds and later checkpoints keep advancing the same live segment.
    void test_wp6_F3_transient_save_failure_then_retry_preserves_live_segment();
};

#endif // TEST_HISTORY_COMMIT_H
