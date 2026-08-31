#ifndef TEST_APPSHELL_H
#define TEST_APPSHELL_H

#include <QObject>
#include "testcommon.h"

/**
 * AppShellTest -- application-shell composition tests (A3).
 *
 * Narrow on purpose: this is the seam where MainWin's Retry/Exit Anyway
 * modal loop is actually exercised end-to-end (qtest's own aggregate never
 * links mainwin.cpp/contentwidget.cpp, so it cannot compose MainWin with a
 * real ShutdownCoordinator/TimeTracker). B2 and C1 add their own cases here;
 * D13 later extends it to full startup/shutdown composition coverage.
 */
class AppShellTest : public QObject
{
    Q_OBJECT

private slots:
    // Close with a healthy store: no modal, marker written.
    void test_close_happy_path_writes_marker_without_modal();
    // Close fails once; the user clicks Retry and the store has recovered:
    // exactly one new drain attempt resolves it.
    void test_close_retry_succeeds_after_failure();
    // Close fails; the user clicks Exit Anyway: no further retry, no marker.
    void test_close_exit_anyway_stops_retrying_and_writes_no_marker();
    // Regression: Exit Anyway is latched, so the second shutdown entry point
    // (aboutToQuit after the last window closes) must not ask again.
    void test_exit_anyway_is_not_asked_again_on_aboutToQuit();
    // OS-forced (Direct) shutdown failure must show no modal at all.
    void test_forced_os_shutdown_failure_shows_no_modal();

    // C1 (R15-001): startup must fail closed whenever ownership is not
    // established, while QLockFile still recovers a stale lock.
    void test_lock_decision_maps_all_tryLock_failures();
    void test_production_permission_failure_fails_closed();
    void test_production_start_reclaims_stale_lock();
    void test_production_active_owner_survives_old_lock_timestamp();
    void test_production_loser_cannot_mutate_database_with_active_owner();
    void test_two_simultaneous_starts_have_one_owner_and_one_loser();

    // B2 (R08-001..003,005, R05-004): runStartupRecovery()'s Retry/Quit loop,
    // exercised before any MainWin/ContentWidget exists -- proving no
    // tracking is structurally possible before recovery succeeds.
    //
    // Recovery fails once; Retry repeats the whole atomic recovery and
    // succeeds: exactly two recoverStartupCheckpoints() calls.
    void test_startup_recovery_retry_repeats_recovery_until_success();
    // Recovery fails; Quit declines without a second attempt.
    void test_startup_recovery_quit_declines_without_retry();
    // Recovery succeeds immediately: no modal at all.
    void test_startup_recovery_success_shows_no_modal();

    // D4 (R15-003): the Unix signal bridge must be nonblocking, coalesced,
    // and clean up after itself.
    void test_setup_creates_nonblocking_closeonexec_descriptors();
    void test_each_signal_is_delivered_and_drained_data();
    void test_each_signal_is_delivered_and_drained();
    void test_repeated_signals_coalesce_into_one_drain();
    void test_write_retries_only_eintr();
    void test_write_treats_eagain_as_already_pending();
    void test_write_preserves_errno();
    void test_full_channel_does_not_block();
    void test_setup_failure_leaks_no_descriptors();
    void test_qt_receipt_invokes_shutdown_exactly_once();
};

#endif // TEST_APPSHELL_H
