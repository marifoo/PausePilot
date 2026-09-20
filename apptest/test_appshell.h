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
    // D6/Medium-3: a marker-only failure (data committed fine) must be
    // presented as "your session was saved", not as unsaved data.
    void test_close_marker_only_failure_shows_data_was_saved();
    // Regression: Exit Anyway is latched, so the second shutdown entry point
    // (aboutToQuit after the last window closes) must not ask again.
    void test_exit_anyway_is_not_asked_again_on_aboutToQuit();
    // OS-forced (Direct) shutdown failure must show no modal at all.
    void test_forced_os_shutdown_failure_shows_no_modal();

    // C1 (R15-001): startup must fail closed whenever ownership is not
    // established, while QLockFile still recovers a stale lock.
    void test_lock_decision_maps_all_tryLock_failures();
    void test_production_permission_failure_fails_closed();
    // D12-3 item 5: an unwritable log path must disable Logger with exactly
    // one stderr diagnostic and no crash -- exercised via a fresh production
    // process because Logger's file handle is a process-wide singleton
    // opened once, and qtest's own process already spends that one open on
    // the success path.
    void test_production_unwritable_log_path_disables_logging_once();
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

    // D5 (R15-002): the pure Windows shutdown-message decision seam --
    // testable on Linux even though the native WM_QUERYENDSESSION/
    // WM_ENDSESSION delivery itself is Windows-only.
    void test_D5_translateWindowsShutdownMessage_data();
    void test_D5_translateWindowsShutdownMessage();

    // D10 (requirement 4): ContentWidget reduces the single
    // TimeTracker::transitioned() event, wired up through MainWin. Exercised
    // here (not in qtest) because qtest's aggregate never links
    // contentwidget.cpp/mainwin.cpp.
    void test_D10_lock_resume_updates_button_exactly_once();

    // D13f: button-to-tracker-to-widget composition, driven by a REAL
    // QTest::mouseClick on the actual QPushButton -- not a hand-invoked call
    // to ContentWidget::pressedStartPauseButton()/pressedStopButton() or
    // TimeTracker::useTimerViaButton(). Proves the whole chain (button
    // clicked() -> ContentWidget slot -> startPausePressed()/stopPressed()
    // -> MainWin's connect() -> TimeTracker::onStartPausePressed()/
    // onStopPressed() -> transitioned() -> ContentWidget::onTransitioned())
    // is actually wired, not merely that each link works in isolation.
    void test_start_button_click_starts_tracker_and_updates_widget();
    void test_stop_button_click_stops_tracker_and_updates_widget();

    // D13f: quit/shutdown composition through the REAL application entry
    // point (main.cpp), not the narrowed MainWin/ShutdownCoordinator seam
    // above. A real SIGTERM exercises the full chain UnixSignalBridge's
    // signal handler -> self-pipe -> QSocketNotifier::activated (a real Qt
    // signal fired by the real event loop, not a hand-invoked slot call) ->
    // main.cpp's lambda -> MainWin::onAboutToQuit() -> shutdown() ->
    // ShutdownCoordinator -> SqliteSessionStore -> QCoreApplication::quit().
    void test_production_sigterm_drains_and_writes_clean_shutdown_marker();

    // D13f (F3, R02-001 follow-up): the mainwin.cpp:48 connect(&timetracker_,
    // &TimeTracker::stopped, health_monitor_, &HealthMonitor::reset) wiring,
    // compiled through the REAL MainWin owner instead of
    // qtest/test_healthmonitor.cpp's hand-rolled duplicate of the same
    // connect() (its own comment says it exists only because "MainWin is
    // GUI/not in the test build" -- apptest links mainwin.cpp, so that
    // workaround is no longer necessary here).
    void test_mainwin_stopped_rearms_health_monitor_through_real_wiring();
};

#endif // TEST_APPSHELL_H
