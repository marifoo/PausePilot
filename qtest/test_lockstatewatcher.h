#ifndef TEST_LOCKSTATEWATCHER_H
#define TEST_LOCKSTATEWATCHER_H

#include <QObject>
#include "testcommon.h"

class LockStateWatcherTest : public QObject
{
    Q_OBJECT

private slots:
    void test_lockstatewatcher_debounce_logic();
    // 9.5: Unknown query result must not change the debounce buffer
    void test_lockstatewatcher_unknown_does_not_unlock();
    // ItemE: shouldEmitLongLock — Unknown never matures; Locked does; short elapsed does not
    void test_shouldEmitLongLock_unknown_does_not_mature();
    void test_shouldEmitLongLock_locked_matures();
    void test_shouldEmitLongLock_locked_short_elapsed_does_not_mature();
    // Phase 2 seam: stubbed query probe drives update(); now-provider records lock_time_.
    void test_seam_query_provider_drives_update_and_records_lock_time();
    // A2: LongOngoingLock decision uses the wall-clock lock instant, so it matures
    // across a suspend gap the monotonic lock_timer_ would miss.
    void test_A2_longlock_uses_wallclock_across_suspend();
    // H3: lock_time_ is stamped from the first Locked sample of a run, not the
    // accepting (third) sample, so a suspend between the first sample and the flip
    // does not move the lock instant to the resume time.
    void test_H3_lock_anchor_survives_suspend_during_debounce();
    void test_H3_lock_time_is_first_locked_sample_not_third();
    void test_H3_unknown_samples_do_not_move_the_lock_anchor();
    // A5: hysteresis debounce — a single flaky sample no longer permanently
    // misses the Lock; Lock/Unlock strictly alternate; LongOngoingLock at most once.
    void test_A5_noisy_transition_emits_exactly_one_lock();
    void test_A5_lock_unlock_strictly_alternate();
    void test_A5_longlock_fires_at_most_once_per_lock_session();

    // D3/R12-002: pure WTSINFOEX Level-1 SessionFlags -> tri-state translation.
    void test_D3_translateWtsSessionState_data();
    void test_D3_translateWtsSessionState();

    // D3/R12-001: Linux backend chain failover/backoff, driven by a fake chain.
    void test_D3_linux_preferred_backend_failure_falls_back();
    void test_D3_linux_recovers_once_preferred_backend_works_again();
    void test_D3_linux_active_backend_failure_falls_back_to_working_backend();
    void test_D3_linux_all_backends_fail_retains_unknown();
    void test_D3_linux_backoff_skips_dbus_until_window_elapses();
    void test_D3_linux_backoff_grows_and_is_bounded();
    void test_D3_linux_backoff_does_not_block_heartbeat_with_repeated_calls();
    void test_D3_linux_flapping_fallback_does_not_defeat_backoff();
};

#endif // TEST_LOCKSTATEWATCHER_H
