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

    // Test K: reset() re-arms the guard so a second shutdown flushes again (A4)
    void test_K_reset_allows_second_shutdown();

    // Test L: a re-entrant run() dispatched inside the pump flushes only once (A14)
    void test_L_nested_run_during_pump_flushes_once();

    // Test M: timer already stopped → coordinator writes the marker after flush
    void test_M_marker_after_flush_when_already_stopped();

    // Test N: a START click during the pump is re-stopped (A14 pump re-entrancy)
    void test_N_restart_during_pump_is_restopped();
};

#endif // TEST_SHUTDOWNCOORDINATOR_H
