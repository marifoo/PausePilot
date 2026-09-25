#ifndef TEST_SHUTDOWNCOORDINATOR_H
#define TEST_SHUTDOWNCOORDINATOR_H

#include <QObject>
#include "testcommon.h"
#include "fakeclock.h"

class ShutdownCoordinatorTest : public QObject
{
    Q_OBJECT

private:
    // D15 part 4: fixture-owned clock, pinned to TestCommon::fixedNow()
    // (local noon) so Start()'s real armScheduledStop() QTimer
    // (timetracker.cpp:654) is armed ~12 real hours out and cannot fire
    // during any test's run, regardless of when the real wall clock crosses
    // local midnight -- see test_shutdowncoordinator.cpp's file-header
    // comment for the class-wide mechanism this closes. Allocated in init()
    // and destroyed in cleanup(), which QtTest runs immediately before/after
    // every test function -- so clock_ always outlives every TimeTracker a
    // test constructs as a local (stack) variable within its own body,
    // satisfying the outlives-tracker declaration-order guarantee
    // automatically, including for tests added later without special care.
    FakeClock* clock_ = nullptr;

private slots:
    void init();
    void cleanup();

    // Test G: happy path — stop, flush, marker
    void test_G_happy_path_stop_flush_marker();

    // Test H: idempotent — second run() is a no-op
    void test_H_idempotent_second_run_is_noop();

    // Test K: cancelShutdown() re-arms the guard so a second shutdown
    // flushes again (A4), and clears the tracker's terminal flag (D5)
    void test_K_cancelShutdown_allows_second_shutdown();

    // Test M: timer already stopped -- coordinator still writes marker before flush (D6)
    void test_M_marker_before_flush_when_already_stopped();

    // C9b: shutdown is terminal -- both a direct Start and one queued after
    // run() returns are refused, and a second run() just returns the cached
    // result with no extra marker/flush call.
    void test_C9b_start_refused_after_run_direct_and_queued();

    // C9a: a Start queued (via QTimer::singleShot(0, ...)) before run() is
    // called must not be dispatched by run() itself -- run() no longer pumps
    // the event loop. Only once the caller's own processEvents() runs does
    // it dispatch, and C9b's terminal guard refuses it.
    void test_C9a_run_does_not_dispatch_start_queued_before_run();

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

    // ========================================================================
    // D6 (R05-002/R15-004): clean marking (marker write + FULL barrier) is
    // one durable shutdown/store operation, attempted only when the drain
    // itself is clean.
    // ========================================================================

    // Medium-2: an incomplete drain (Exit-Anyway/forced-shutdown-failure
    // shape) bypasses only the clean MARKER -- the durability barrier still
    // runs on its own so already-durable checkpoints stay covered by fsync.
    void test_D6_incomplete_drain_skips_marker_but_still_barriers();
    // High-1: a marker write failure must downgrade the result to
    // non-clean, tag it markerOnlyFailure (Medium-3), and never reach the
    // durability barrier at all.
    void test_D6_marker_failure_is_not_clean_and_skips_barrier();
    // High-1: a barrier failure AFTER a successful marker write must also
    // be non-clean, and is NOT markerOnlyFailure.
    void test_D6_barrier_failure_after_successful_marker_is_not_clean();
    // High-1/R05-005: Disabled history keeps the drain clean -- Disabled is
    // a no-op, not a failure.
    void test_D6_disabled_history_marker_keeps_drain_clean();
};

#endif // TEST_SHUTDOWNCOORDINATOR_H
