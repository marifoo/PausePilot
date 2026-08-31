#ifndef TEST_SHUTDOWNCOORDINATOR_H
#define TEST_SHUTDOWNCOORDINATOR_H

#include <QObject>
#include "testcommon.h"

class ShutdownCoordinatorTest : public QObject
{
    Q_OBJECT

private slots:
    // Test G: happy path — stop, flush, marker
    void test_G_happy_path_stop_flush_marker();

    // Test H: idempotent — second run() is a no-op
    void test_H_idempotent_second_run_is_noop();

    // Test I: force-direct path skips the retry loop
    void test_I_force_direct_skips_retry_loop();

    // Test J: pump actually drains queued events
    void test_J_pump_drains_queued_events();

    // Test K: cancelShutdown() re-arms the guard so a second shutdown
    // flushes again (A4), and clears the tracker's terminal flag (D5)
    void test_K_cancelShutdown_allows_second_shutdown();

    // Test L: a re-entrant run() dispatched inside the pump flushes only once (A14)
    void test_L_nested_run_during_pump_flushes_once();

    // Test M: timer already stopped → coordinator writes the marker after flush
    void test_M_marker_after_flush_when_already_stopped();

    // Test N: a START click during the pump is re-stopped (A14 pump re-entrancy)
    void test_N_restart_during_pump_is_restopped();

    // ========================================================================
    // A3 (R11-001): ShutdownCoordinator::run()/retryDrain() consume the typed
    // DrainResult from TimeTracker::drainForShutdown(). Clean-marker
    // eligibility comes from that result, not from mode_ == None.
    // ========================================================================

    // run() reports an incomplete drain; a later retryDrain() (what a user
    // Retry performs) resolves it once the store recovers.
    void test_A3_run_incomplete_then_retryDrain_succeeds();
    // Same setup, but retryDrain() also fails: still incomplete, no marker.
    void test_A3_run_incomplete_then_retryDrain_fails();
    // Already-stopped with no pending payload: a no-op success.
    void test_A3_run_already_stopped_no_pending_is_noop_success();
    // Direct and DrainEvents modes produce the same persistence result for
    // an identical failing setup.
    void test_A3_direct_and_drainevents_modes_produce_same_result();
    // Forced OS shutdown (Direct) on failure: no user warning, no marker.
    void test_A3_direct_mode_failure_emits_no_warning_and_no_marker();

    // A4 (R05-001): an unresolved checkpoint-discard intent blocks clean
    // marking exactly like a pending commit, including under a forced OS
    // shutdown (best-effort termination: no warning, but still no marker).
    void test_A4_forced_shutdown_with_unresolved_discard_writes_no_marker();

    // ========================================================================
    // D5 (R15-002): cancelShutdown() is one lifecycle transition owned by
    // ShutdownCoordinator that reverses shutdown()'s terminal state.
    // ========================================================================

    // History (wasShutDown()) accepts saves immediately after cancellation,
    // with no new Start in between.
    void test_D5_cancelShutdown_makes_history_available_without_new_start();
    // Cancelling a shutdown that ran while the tracker was already stopped
    // (no live segment) still clears the terminal flag.
    void test_D5_cancelShutdown_while_already_stopped();
};

#endif // TEST_SHUTDOWNCOORDINATOR_H
