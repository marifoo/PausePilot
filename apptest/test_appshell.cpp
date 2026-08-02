#include "test_appshell.h"
#include "fakesessionstore.h"
#include "mainwin.h"
#include "shutdowncoordinator.h"
#include "timetracker.h"
#include "startuprecovery.h"
#include <QtTest>
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <functional>
#include <memory>

using TestCommon::createSettingsFile;

namespace {

/// Owning handle for a modal-click poller. Destroying it stops and destroys
/// the QTimer, so a poller armed by one test can never survive into a later
/// one and click *that* test's modal. Every test must keep the returned
/// handle alive for exactly as long as it wants the arming to apply.
using ModalPoller = std::unique_ptr<QTimer>;

/// Number of distinct modals this process has clicked. Tests that care about
/// "exactly once" semantics reset it during Arrange and assert on it.
int g_modalsClicked = 0;

// Arms a click on the button matching `role` in the next QMessageBox that
// becomes the active modal widget. MainWin::shutdown() blocks synchronously
// inside QMessageBox::exec(), so the click can only be delivered from a
// callback running in the SAME thread's (nested) event loop; this polls every
// 5 ms rather than assuming the modal is already active on the first queued
// callback (its "active" state is set only once exec()'s own show() event has
// been processed, which can race a same-tick QTimer::singleShot(0)).
// `beforeClick` runs immediately before the click (e.g. to fix the store for
// a Retry). Bounded by a watchdog so a regression that never shows the modal
// fails the test instead of hanging the whole suite.
ModalPoller armModalClick(QMessageBox::ButtonRole role, std::function<void()> beforeClick = {})
{
    ModalPoller poll(new QTimer());
    QTimer* raw = poll.get();
    raw->setInterval(5);
    auto attempts = std::make_shared<int>(0);
    QObject::connect(raw, &QTimer::timeout, [raw, role, beforeClick, attempts]() {
        QMessageBox* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (!box) {
            if (++(*attempts) > 1000) {  // ~5 s
                qWarning("armModalClick: no modal appeared within the watchdog window");
                raw->stop();
            }
            return;
        }
        raw->stop();
        ++g_modalsClicked;
        if (beforeClick) beforeClick();
        const auto buttons = box->buttons();
        for (QAbstractButton* btn : buttons) {
            if (box->buttonRole(btn) == role) {
                btn->click();
                return;
            }
        }
        qWarning("armModalClick: no button with the requested role found");
    });
    raw->start();
    return poll;
}

/// Runs `action` (which must not block on a modal) and fails the test if any
/// application-modal widget becomes active while it runs or shortly after.
/// Used for paths that are REQUIRED to stay silent -- notably OS-forced
/// shutdown. A watchdog dismisses an unexpected modal so a regression fails
/// the assertion instead of hanging the suite forever.
bool runExpectingNoModal(const std::function<void()>& action)
{
    bool sawModal = false;
    QTimer sentinel;
    sentinel.setInterval(5);
    QObject::connect(&sentinel, &QTimer::timeout, [&sawModal]() {
        if (QMessageBox* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            sawModal = true;
            // Dismiss it so a blocking exec() cannot hang the run. Prefer the
            // DestructiveRole ("Exit Anyway") button: clicking an AcceptRole
            // ("Retry") button would re-enter the retry loop and re-show the
            // modal forever while the injected store failure persists.
            const auto buttons = box->buttons();
            if (buttons.isEmpty()) return;
            QAbstractButton* escape = buttons.last();
            for (QAbstractButton* btn : buttons) {
                if (box->buttonRole(btn) == QMessageBox::DestructiveRole
                    || box->buttonRole(btn) == QMessageBox::RejectRole) {
                    escape = btn;
                    break;
                }
            }
            escape->click();
        }
    });
    sentinel.start();
    action();
    // Give a queued/late modal a chance to appear before declaring silence.
    QTest::qWait(60);
    sentinel.stop();
    return !sawModal;
}

} // namespace

// ============================================================================
// A3 (R11-001): close/Retry/Exit-Anyway composition, tested at the
// application-shell seam rather than inside TimeTracker or ShutdownCoordinator
// directly.
// ============================================================================

void AppShellTest::test_close_happy_path_writes_marker_without_modal()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);
    MainWin mainWin(settings, tracker, fakeDb, coordinator);

    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);

    // Act: close with a healthy store. runExpectingNoModal() supplies the
    // watchdog: a regression that shows a modal here is dismissed and
    // reported, instead of blocking forever inside exec().
    const bool stayedSilent = runExpectingNoModal([&mainWin]() { mainWin.close(); });

    // Assert
    QVERIFY2(stayedSilent, "a successful drain must not show any modal");
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.hasPendingCommit_dbg());
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

void AppShellTest::test_close_retry_succeeds_after_failure()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);
    MainWin mainWin(settings, tracker, fakeDb, coordinator);

    mainWin.show();
    QVERIFY(QTest::qWaitForWindowExposed(&mainWin));
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");

    // Act: the initial drain (inside close()) fails and MainWin shows the
    // Retry/Exit Anyway modal. Schedule a click on Retry that also fixes the
    // store, so the one new drain attempt it triggers succeeds.
    auto poller = armModalClick(QMessageBox::AcceptRole, [&fakeDb]() {
        fakeDb.commitSessionResult = SessionStoreResult::success();
    });
    mainWin.close();

    // Assert: resolved via exactly the Retry path, marker written.
    QVERIFY(tracker.isStopped());
    QVERIFY(!tracker.hasPendingCommit_dbg());
    QVERIFY(fakeDb.callLog.contains("setLastCleanShutdownMarker"));
}

void AppShellTest::test_close_exit_anyway_stops_retrying_and_writes_no_marker()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);
    MainWin mainWin(settings, tracker, fakeDb, coordinator);

    mainWin.show();
    QVERIFY(QTest::qWaitForWindowExposed(&mainWin));
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");

    // Act: the initial drain fails; the user clicks Exit Anyway instead of
    // Retry -- the store is left broken, so a second attempt would also fail
    // if one were (wrongly) made.
    auto poller = armModalClick(QMessageBox::DestructiveRole);
    mainWin.close();

    // Assert: exactly one coordinator run total (the initial run() -- Exit
    // Anyway must not trigger ShutdownCoordinator::retryDrain()). A single
    // drainForShutdown() call may itself submit the failing commit twice
    // (the Stop-time commit, then its own immediate pending retry), but
    // flushToDisc()/the marker are only reached once per coordinator run.
    QCOMPARE(fakeDb.callLog.count("flushToDisc"), 1);
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Cleanup: resolve the payload so this test double's tracker does not
    // carry unresolved state past the test (run() already called drain once,
    // so no orchestration-bug assert is at risk here either way).
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// Regression: "Exit Anyway" is a decision about the whole termination, not
// about one entry point. The real app reaches MainWin::shutdown() twice on a
// normal quit -- closeEvent (X / Alt+F4) and then QCoreApplication::
// aboutToQuit once the last window closes (main.cpp wires onAboutToQuit) --
// and SIGTERM likewise calls onAboutToQuit() before quit() re-emits it.
// ShutdownCoordinator::run() is guarded and replays its cached Incomplete
// result, so without a latched decline the user is asked a second time.
void AppShellTest::test_exit_anyway_is_not_asked_again_on_aboutToQuit()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);
    MainWin mainWin(settings, tracker, fakeDb, coordinator);

    mainWin.show();
    QVERIFY(QTest::qWaitForWindowExposed(&mainWin));
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    g_modalsClicked = 0;

    // Act: the drain fails, the user declines with Exit Anyway...
    {
        auto poller = armModalClick(QMessageBox::DestructiveRole);
        mainWin.close();
    }
    QCOMPARE(g_modalsClicked, 1);

    // ...and then the application's aboutToQuit path enters shutdown again.
    // The store is still broken, so the cached result is still Incomplete.
    const bool stayedSilent = runExpectingNoModal([&mainWin]() { mainWin.onAboutToQuit(); });

    // Assert: the user is asked exactly once across BOTH entry points.
    QVERIFY2(stayedSilent, "Exit Anyway must not be re-asked on the aboutToQuit path");
    QCOMPARE(g_modalsClicked, 1);
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));

    // Cleanup: resolve the payload so destruction finds no unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// ============================================================================
// B2 (R08-001..003,005, R05-004): runStartupRecovery()'s Retry/Quit loop.
// Production composition (main.cpp) calls this BEFORE ShutdownCoordinator or
// MainWin is constructed, so a Quit here structurally means no window, no
// buttons, and no possible tracking ever existed for this launch -- these
// tests exercise runStartupRecovery() directly, at the same narrow seam A3
// established for the shutdown modal loop.
// ============================================================================

void AppShellTest::test_startup_recovery_retry_repeats_recovery_until_success()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.startupRecoveryResult = StartupRecoveryResult::failure(SessionStoreResult::TransientError, "injected recovery failure");
    TimeTracker tracker(settings, fakeDb);

    // Act: the initial recovery (inside runStartupRecovery()'s first
    // initializeFromStore() call) fails and shows the Retry/Quit modal.
    // Schedule a click on Retry that also fixes the store, so the one new
    // recovery attempt it triggers succeeds.
    auto poller = armModalClick(QMessageBox::AcceptRole, [&fakeDb]() {
        fakeDb.startupRecoveryResult = StartupRecoveryResult{};
    });
    const bool proceeded = runStartupRecovery(tracker);

    // Assert: recovery ultimately succeeded via exactly one Retry -- two
    // total recoverStartupCheckpoints() calls.
    QVERIFY(proceeded);
    QCOMPARE(fakeDb.callLog.count("recoverStartupCheckpoints"), 2);
}

void AppShellTest::test_startup_recovery_quit_declines_without_retry()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    fakeDb.startupRecoveryResult = StartupRecoveryResult::failure(SessionStoreResult::TransientError, "injected recovery failure");
    TimeTracker tracker(settings, fakeDb);

    // Act: the initial recovery fails; the user clicks Quit instead of Retry.
    auto poller = armModalClick(QMessageBox::DestructiveRole);
    const bool proceeded = runStartupRecovery(tracker);

    // Assert: Quit is honored without a second attempt -- existing rows/
    // marker are left exactly as recoverStartupCheckpoints() (a no-op on
    // TransientError) already left them. Production (main.cpp) exits here
    // before ShutdownCoordinator or MainWin is ever constructed, so no
    // tracking session could have started this launch.
    QVERIFY(!proceeded);
    QCOMPARE(fakeDb.callLog.count("recoverStartupCheckpoints"), 1);
}

void AppShellTest::test_startup_recovery_success_shows_no_modal()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;  // default startupRecoveryResult: Success, zero facts.
    TimeTracker tracker(settings, fakeDb);

    // Act
    bool proceeded = false;
    const bool stayedSilent = runExpectingNoModal([&]() { proceeded = runStartupRecovery(tracker); });

    // Assert
    QVERIFY2(stayedSilent, "a successful recovery must not show any modal");
    QVERIFY(proceeded);
    QCOMPARE(fakeDb.callLog.count("recoverStartupCheckpoints"), 1);
}

// The OS-forced (Direct) path must attempt once and exit with NO warning and
// no clean marker; data loss is accepted for this case only. The mechanism
// that enforces silence is the `mode == Direct` early return in
// MainWin::shutdown() -- so assert on the absence of a MODAL, not merely on
// the absence of a userWarning signal (which that code path never emits
// anyway and therefore cannot detect a regression).
void AppShellTest::test_forced_os_shutdown_failure_shows_no_modal()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    Settings settings(createSettingsFile(tempDir.path(), 7));
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    ShutdownCoordinator coordinator(tracker, fakeDb);
    MainWin mainWin(settings, tracker, fakeDb, coordinator);

    mainWin.show();
    QVERIFY(QTest::qWaitForWindowExposed(&mainWin));
    tracker.useTimerViaButton(Button::Start);
    QTest::qWait(20);
    fakeDb.commitSessionResult = SessionStoreResult::transient("injected failure");
    g_modalsClicked = 0;

    // Act: OS-forced shutdown with a failing store.
    const bool stayedSilent = runExpectingNoModal(
        [&mainWin]() { mainWin.shutdownForTest_dbg(ShutdownMode::Direct); });

    // Assert: silent, no clean marker, and the loss is simply accepted.
    QVERIFY2(stayedSilent, "a forced OS shutdown must never show a modal, even on failure");
    QCOMPARE(g_modalsClicked, 0);
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Cleanup: resolve the payload so destruction finds no unresolved commit.
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}
