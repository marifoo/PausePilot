#ifndef TEST_HISTORY_COMMIT_H
#define TEST_HISTORY_COMMIT_H

#include <QObject>
#include "testcommon.h"

/**
 * HistoryCommitTest — accept() blocks close on DB failure,
 * and TimerExclusiveEditGuard restores checkpoints on all exit paths.
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
};

#endif // TEST_HISTORY_COMMIT_H
