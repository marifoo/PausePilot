#include "test_appshell.h"
#include "fakesessionstore.h"
#include "fakeclock.h"
#include "mainwin.h"
#include "shutdowncoordinator.h"
#include "timetracker.h"
#include "startuprecovery.h"
#include "lockdecision.h"
#include "unixsignalbridge.h"
#include <QtTest>
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QSocketNotifier>
#include <QTimer>
#include <functional>
#include <memory>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#ifdef Q_OS_LINUX
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#endif

using TestCommon::createSettingsFile;

namespace {

// The production binary's location is not derivable from this test binary's
// own applicationDirPath() in a shadow build (a build directory separate
// from the checkout): the two targets need not share a parent directory.
// UTIMER_PRODUCTION_BINARY is injected by apptest.pro/apptest.vcxproj as an
// absolute, checkout-relative path resolved at qmake/project time instead;
// UTIMER_BIN lets the harness override it for a binary built elsewhere.
QString productionExecutable()
{
    const QString overridePath = qEnvironmentVariable("UTIMER_BIN");
    if (!overridePath.isEmpty())
        return QDir::fromNativeSeparators(overridePath);
    return QStringLiteral(UTIMER_PRODUCTION_BINARY);
}

// Names the exact resolved path a failed start attempted, so a bare Qt
// "execve: No such file or directory" (which does not itself name a path)
// can never appear unexplained -- notably for an apptest-only shadow build
// with no sibling production binary.
QString startFailureMessage(const QProcess& process)
{
    return QString("could not start production binary \"%1\" (%2); "
                    "set UTIMER_BIN to override the resolved path")
        .arg(productionExecutable(), process.errorString());
}

QProcessEnvironment productionEnvironment(const QString& dataDirectory)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("QT_QPA_PLATFORM", "offscreen");
    environment.insert("UTIMER_DATA_DIR", dataDirectory);
    return environment;
}

bool startProduction(QProcess& process, const QString& dataDirectory)
{
    process.setProgram(productionExecutable());
    process.setProcessEnvironment(productionEnvironment(dataDirectory));
    process.start();
    return process.waitForStarted(5000);
}

bool waitForFile(const QString& path, int timeoutMsec)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMsec) {
        if (QFileInfo::exists(path))
            return true;
        QTest::qWait(20);
    }
    return QFileInfo::exists(path);
}

// Polls a QLockFile's first line (its PID field) until it names `pid`. Used
// to prove a stale lock was actually reclaimed and rewritten by the new
// owner, as opposed to merely still existing on disk (true from the very
// first poll when the test itself wrote the seed file before starting the
// process).
bool waitForLockPid(const QString& path, qint64 pid, int timeoutMsec)
{
    const QByteArray expected = QByteArray::number(pid);
    QElapsedTimer elapsed;
    elapsed.start();
    do {
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            if (file.readLine().trimmed() == expected)
                return true;
        }
        QTest::qWait(20);
    } while (elapsed.elapsed() < timeoutMsec);
    return false;
}

bool stopProduction(QProcess& process)
{
    if (process.state() == QProcess::NotRunning)
        return true;
    process.terminate();
    if (process.waitForFinished(5000))
        return true;
    process.kill();
    return process.waitForFinished(5000);
}

struct ProcessGuard
{
    QProcess process;

    ~ProcessGuard()
    {
        stopProduction(process);
    }
};

bool waitForEither(QProcess& first, QProcess& second, int timeoutMsec)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeoutMsec) {
        if (first.state() == QProcess::NotRunning || second.state() == QProcess::NotRunning)
            return true;
        QTest::qWait(20);
    }
    return first.state() == QProcess::NotRunning || second.state() == QProcess::NotRunning;
}

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
ModalPoller armModalClick(QMessageBox::ButtonRole role, std::function<void()> beforeClick = {},
                           QString* capturedText = nullptr)
{
    ModalPoller poll(new QTimer());
    QTimer* raw = poll.get();
    raw->setInterval(5);
    auto attempts = std::make_shared<int>(0);
    QObject::connect(raw, &QTimer::timeout, [raw, role, beforeClick, attempts, capturedText]() {
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
        if (capturedText) *capturedText = box->text();
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

    // Assert: an incomplete drain bypasses the clean MARKER (D6), but the
    // durability barrier still runs on its own (Medium-2) -- Exit Anyway
    // "leaves existing durable checkpoints available for ... recovery"
    // (plan section 2), which requires this fsync to still happen.
    QVERIFY(fakeDb.callLog.contains("flushToDisc"));
    QVERIFY(!fakeDb.callLog.contains("setLastCleanShutdownMarker"));
    QVERIFY(tracker.hasPendingCommit_dbg());

    // Cleanup: resolve the payload so this test double's tracker does not
    // carry unresolved state past the test (run() already called drain once,
    // so no orchestration-bug assert is at risk here either way).
    fakeDb.commitSessionResult = SessionStoreResult::success();
    QVERIFY(tracker.drainForShutdown().clean());
}

// D6/Medium-3 (R05-002 follow-up): a marker-only failure (session data DID
// commit; only the clean-shutdown record failed) must be presented
// truthfully -- MainWin must not tell the user their session was not saved
// when it was.
void AppShellTest::test_close_marker_only_failure_shows_data_was_saved()
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
    // The drain itself (session data commit) succeeds; only the marker fails.
    fakeDb.setMarkerResult = SessionStoreResult::transient("injected marker failure");

    // Act: capture the modal's text and dismiss via Exit Anyway.
    QString shownText;
    auto poller = armModalClick(QMessageBox::DestructiveRole, {}, &shownText);
    mainWin.close();

    // Assert: the modal must say the session WAS saved, not that it wasn't.
    QVERIFY(!tracker.hasPendingCommit_dbg());
    QVERIFY(shownText.contains("was saved", Qt::CaseInsensitive));
    QVERIFY(!shownText.contains("could not save the current session", Qt::CaseInsensitive));
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

void AppShellTest::test_lock_decision_maps_all_tryLock_failures()
{
    // Arrange
    const QLockFile::LockError errors[] = {
        QLockFile::LockFailedError,
        QLockFile::PermissionError,
        QLockFile::UnknownError
    };

    // Act / Assert
    QCOMPARE(LockDecision::classify(errors[0]), LockDecision::Outcome::AnotherInstance);
    QVERIFY(LockDecision::message(LockDecision::Outcome::AnotherInstance)
                .contains("Another instance"));
    QCOMPARE(LockDecision::exitCode(LockDecision::Outcome::AnotherInstance), 2);

    for (int i = 1; i < 3; ++i) {
        QCOMPARE(LockDecision::classify(errors[i]), LockDecision::Outcome::OwnershipUnknown);
        QVERIFY(LockDecision::message(LockDecision::Outcome::OwnershipUnknown)
                    .contains("ownership"));
        QCOMPARE(LockDecision::exitCode(LockDecision::Outcome::OwnershipUnknown), 3);
    }
}

// Covers only the "lock file cannot be created" half of R15-001's trigger.
// The other half (the existing SQLite database remains accessible while the
// lock cannot be created) would need the data directory unwritable for the
// lock file but the database file within it still writable, which is not
// reachable via a single directory permission without root.
void AppShellTest::test_production_permission_failure_fails_closed()
{
#ifndef Q_OS_UNIX
    QSKIP("filesystem permission subprocess coverage is Unix-specific");
#else
    // Arrange
    if (geteuid() == 0)
        QSKIP("root can bypass the directory permission under test");
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QVERIFY(QFile::setPermissions(tempDir.path(),
                                  QFileDevice::ReadOwner | QFileDevice::ExeOwner
                                  | QFileDevice::ReadGroup | QFileDevice::ExeGroup
                                  | QFileDevice::ReadOther | QFileDevice::ExeOther));

    // Act
    ProcessGuard denied;
    QVERIFY2(startProduction(denied.process, tempDir.path()),
             qPrintable(startFailureMessage(denied.process)));
    QVERIFY2(denied.process.waitForFinished(5000),
             qPrintable(denied.process.errorString()));

    // Assert
    const QByteArray output = denied.process.readAllStandardError()
        + denied.process.readAllStandardOutput();
    const QString diagnostics = QString::fromLocal8Bit(output);
    QCOMPARE(denied.process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(denied.process.exitCode(), 3);
    QVERIFY2(diagnostics.contains("exclusive ownership"), output.constData());
    QVERIFY(!QFileInfo::exists(QDir(tempDir.path()).filePath("uTimer.lock")));
    QVERIFY(!QFileInfo::exists(QDir(tempDir.path()).filePath("uTimer.sqlite")));
    QVERIFY(!QFileInfo::exists(QDir(tempDir.path()).filePath("user-settings.ini")));
#endif
}

void AppShellTest::test_production_unwritable_log_path_disables_logging_once()
{
#ifndef Q_OS_UNIX
    QSKIP("directory-as-log-file open-failure simulation relies on POSIX file semantics");
#else
    // Arrange: logging enabled, but the log file's path is occupied by a
    // directory, so Logger's QFile::open() fails while the database,
    // settings and lock files (different names, same writable directory)
    // are unaffected.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QSettings settings(createSettingsFile(tempDir.path(), 7), QSettings::IniFormat);
    settings.setValue("uTimer/debug_log_to_file", true);
    settings.sync();
    QVERIFY(QDir(tempDir.path()).mkdir("uTimer.log"));

    // Act
    ProcessGuard production;
    QVERIFY2(startProduction(production.process, tempDir.path()),
             qPrintable(startFailureMessage(production.process)));
    // LockStateWatcher's constructor issues the first-ever Logger::Log()
    // call in this process; the database is opened right after it, so its
    // appearance proves that first call (and Logger's open() failure) has
    // already happened.
    QVERIFY2(waitForFile(QDir(tempDir.path()).filePath("uTimer.sqlite"), 5000),
             "production did not get past LockStateWatcher's startup log call");
    QTest::qWait(100);

    // Assert
    const QByteArray diagnostics = production.process.readAllStandardError();
    QCOMPARE(production.process.state(), QProcess::Running);
    QVERIFY2(diagnostics.contains("[Logger]"), diagnostics.constData());
    QCOMPARE(diagnostics.count(QByteArray("[Logger]")), 1);
    QVERIFY2(QFileInfo(QDir(tempDir.path()).filePath("uTimer.log")).isDir(),
             "the unwritable log path must be left untouched");
#endif
}

void AppShellTest::test_production_start_reclaims_stale_lock()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString lockPath = QDir(tempDir.path()).filePath("uTimer.lock");
    // unlock() removes the seed file, so write a valid-looking stale payload
    // directly instead of trying to read it after releasing the seed lock.
    const QByteArray staleLockData =
        // Empty host/host-id/boot-id fields make QLockFile use the local
        // process check, where this deliberately nonexistent PID is stale.
        "999999999\nuTimer\n\n\n\n";
    QFile staleLock(lockPath);
    QVERIFY(staleLock.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(staleLock.write(staleLockData), qint64(staleLockData.size()));
    staleLock.close();

    // Act
    ProcessGuard owner;
    QVERIFY2(startProduction(owner.process, tempDir.path()), qPrintable(startFailureMessage(owner.process)));
    const bool databaseReady = waitForFile(QDir(tempDir.path()).filePath("uTimer.sqlite"), 5000);
    const QByteArray ownerDiagnostics = owner.process.readAllStandardError()
        + owner.process.readAllStandardOutput();
    QVERIFY2(databaseReady, ownerDiagnostics.constData());

    // Assert: the lock file's PID now names the new owner process, proving
    // QLockFile actually reclaimed and rewrote the stale seed file above.
    // waitForFile(lockPath, ...) cannot show this: this test wrote lockPath
    // itself before the process ever started, so its mere existence is true
    // on the very first poll regardless of whether reclaim happened.
    QVERIFY2(waitForLockPid(lockPath, owner.process.processId(), 5000),
             "production process did not reclaim the stale lock (PID unchanged)");
    QVERIFY2(owner.process.state() == QProcess::Running,
             qPrintable(owner.process.readAllStandardError()));
}

// Seeds press_start_button_on_app_start=false in the owner's settings file.
// The default (true) makes MainWin::start() press Start, entering
// Mode::Activity; LockStateWatcher then polls the real desktop session over
// the inherited D-Bus address, and if the host happens to be locked,
// timetracker.cpp writes a checkpoint a few hundred ms later. That is
// designed behavior gated on Mode::Activity, not a defect, but it makes the
// data directory's mutation depend on the host's desktop-lock state instead
// of the loser under test. With autostart disabled, saveCheckpoint_locked()
// and the ordinary checkpoint timer both stay inactive, so the owner's
// database is provably static and a real byte comparison across the loser's
// run is meaningful evidence, not a source of flakiness.
void AppShellTest::test_production_active_owner_survives_old_lock_timestamp()
{
#ifndef Q_OS_UNIX
    QSKIP("stale-lock threshold regression is Unix-specific");
#else
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString lockPath = QDir(tempDir.path()).filePath("uTimer.lock");
    const QString dbPath = QDir(tempDir.path()).filePath("uTimer.sqlite");
    QSettings ownerSettings(createSettingsFile(tempDir.path(), 7), QSettings::IniFormat);
    ownerSettings.setValue("uTimer/press_start_button_on_app_start", false);
    ownerSettings.sync();

    ProcessGuard owner;
    QVERIFY2(startProduction(owner.process, tempDir.path()), qPrintable(startFailureMessage(owner.process)));
    QVERIFY2(waitForFile(lockPath, 5000), "production owner did not acquire the lock");
    QVERIFY2(waitForFile(dbPath, 5000), "production owner did not initialize the database");
    QTest::qWait(150);
    QFile dbBeforeFile(dbPath);
    QVERIFY(dbBeforeFile.open(QIODevice::ReadOnly));
    const QByteArray dbBefore = dbBeforeFile.readAll();
    dbBeforeFile.close();

    QFile oldLock(lockPath);
    QVERIFY(oldLock.open(QIODevice::ReadWrite));
    QVERIFY(oldLock.setFileTime(QDateTime::currentDateTime().addSecs(-60),
                                QFileDevice::FileModificationTime));
    oldLock.close();
    QVERIFY(QFileInfo(lockPath).lastModified()
            <= QDateTime::currentDateTime().addSecs(-30));

    // Act
    ProcessGuard loser;
    QVERIFY2(startProduction(loser.process, tempDir.path()), qPrintable(startFailureMessage(loser.process)));
    QVERIFY2(loser.process.waitForFinished(5000), qPrintable(loser.process.errorString()));

    // Assert
    const QByteArray loserOutput = loser.process.readAllStandardError()
        + loser.process.readAllStandardOutput();
    const QString loserStderr = QString::fromLocal8Bit(loserOutput);
    QCOMPARE(loser.process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(loser.process.exitCode(), 2);
    QVERIFY2(loserStderr.contains("Another instance"), loserOutput.constData());
    QVERIFY(owner.process.state() == QProcess::Running);

    QFile dbAfterFile(dbPath);
    QVERIFY(dbAfterFile.open(QIODevice::ReadOnly));
    const QByteArray dbAfter = dbAfterFile.readAll();
    dbAfterFile.close();
    QCOMPARE(dbAfter, dbBefore);
#endif
}

void AppShellTest::test_production_loser_cannot_mutate_database_with_active_owner()
{
    // Arrange: press_start_button_on_app_start=false (see the comment above
    // test_production_active_owner_survives_old_lock_timestamp) keeps the
    // owner's database static so the byte comparison below is meaningful.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString dbPath = QDir(tempDir.path()).filePath("uTimer.sqlite");
    QSettings ownerSettings(createSettingsFile(tempDir.path(), 7), QSettings::IniFormat);
    ownerSettings.setValue("uTimer/press_start_button_on_app_start", false);
    ownerSettings.sync();

    ProcessGuard owner;
    QVERIFY2(startProduction(owner.process, tempDir.path()), qPrintable(startFailureMessage(owner.process)));
    QVERIFY2(waitForFile(QDir(tempDir.path()).filePath("uTimer.lock"), 5000),
             "production owner did not acquire the lock");
    QVERIFY2(waitForFile(dbPath, 5000), "production owner did not initialize the database");
    QTest::qWait(150);
    QFile dbBeforeFile(dbPath);
    QVERIFY(dbBeforeFile.open(QIODevice::ReadOnly));
    const QByteArray dbBefore = dbBeforeFile.readAll();
    dbBeforeFile.close();

    // Act
    ProcessGuard loser;
    QVERIFY2(startProduction(loser.process, tempDir.path()), qPrintable(startFailureMessage(loser.process)));
    QVERIFY2(loser.process.waitForFinished(5000), qPrintable(loser.process.errorString()));

    // Assert
    const QByteArray loserOutput = loser.process.readAllStandardError()
        + loser.process.readAllStandardOutput();
    const QString loserStderr = QString::fromLocal8Bit(loserOutput);
    QCOMPARE(loser.process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(loser.process.exitCode(), 2);
    QVERIFY2(loserStderr.contains("Another instance"), loserOutput.constData());
    QVERIFY(owner.process.state() == QProcess::Running);

    QFile dbAfterFile(dbPath);
    QVERIFY(dbAfterFile.open(QIODevice::ReadOnly));
    const QByteArray dbAfter = dbAfterFile.readAll();
    dbAfterFile.close();
    QCOMPARE(dbAfter, dbBefore);
}

void AppShellTest::test_two_simultaneous_starts_have_one_owner_and_one_loser()
{
    // Arrange
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    ProcessGuard first;
    ProcessGuard second;
    QVERIFY2(startProduction(first.process, tempDir.path()), qPrintable(startFailureMessage(first.process)));
    QVERIFY2(startProduction(second.process, tempDir.path()), qPrintable(startFailureMessage(second.process)));

    // Act
    QVERIFY2(waitForEither(first.process, second.process, 5000), "neither simultaneous child exited");

    // Assert
    QProcess* loser = first.process.state() == QProcess::NotRunning
        ? &first.process : &second.process;
    QProcess* owner = loser == &first.process ? &second.process : &first.process;
    const QByteArray loserOutput = loser->readAllStandardError()
        + loser->readAllStandardOutput();
    const QString loserStderr = QString::fromLocal8Bit(loserOutput);
    QVERIFY2(loser->exitStatus() == QProcess::NormalExit, qPrintable(loserStderr));
    QCOMPARE(loser->exitCode(), 2);
    QVERIFY2(loserStderr.contains("Another instance"), loserOutput.constData());
    QVERIFY(owner->state() == QProcess::Running);
}

// ---------------------------------------------------------------------------
// D4 (R15-003): Unix signal bridge -- nonblocking, coalesced, cleaned up.
// ---------------------------------------------------------------------------

#ifdef Q_OS_LINUX
namespace {

// Pumps the event loop in short bursts until predicate() is true or
// budgetMs elapses, mirroring ShutdownCoordinator::pumpEvents()'s bounded
// style rather than a real timed wait.
bool pumpUntil(const std::function<bool()>& predicate, int budgetMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < budgetMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

// Restores a saved RLIMIT_NOFILE on scope exit, including when a
// QVERIFY/QCOMPARE above it fails and returns from the test slot early --
// otherwise a clamped descriptor limit leaks into every later test (M-1).
struct RlimitGuard {
    struct rlimit original;
    ~RlimitGuard() { ::setrlimit(RLIMIT_NOFILE, &original); }
};

} // namespace
#endif

void AppShellTest::test_setup_creates_nonblocking_closeonexec_descriptors()
{
#ifdef Q_OS_LINUX
    // Arrange
    UnixSignalBridge bridge;

    // Act
    const bool ok = bridge.setup();

    // Assert
    QVERIFY(ok);
    const int readFd = bridge.notifierFd();
    const int writeFd = bridge.writeFd_dbg();
    QVERIFY(readFd != -1);
    QVERIFY(writeFd != -1);
    QVERIFY2((fcntl(readFd, F_GETFL, 0) & O_NONBLOCK) != 0, "read end must be nonblocking");
    QVERIFY2((fcntl(writeFd, F_GETFL, 0) & O_NONBLOCK) != 0, "write end must be nonblocking");
    QVERIFY2((fcntl(readFd, F_GETFD, 0) & FD_CLOEXEC) != 0, "read end must be close-on-exec");
    QVERIFY2((fcntl(writeFd, F_GETFD, 0) & FD_CLOEXEC) != 0, "write end must be close-on-exec");

    bridge.teardown();
#else
    QSKIP("Linux-only signal bridge");
#endif
}

void AppShellTest::test_each_signal_is_delivered_and_drained_data()
{
#ifdef Q_OS_LINUX
    QTest::addColumn<int>("signum");
    QTest::newRow("SIGTERM") << SIGTERM;
    QTest::newRow("SIGINT") << SIGINT;
    QTest::newRow("SIGHUP") << SIGHUP;
#endif
}

void AppShellTest::test_each_signal_is_delivered_and_drained()
{
#ifdef Q_OS_LINUX
    QFETCH(int, signum);

    // Arrange
    UnixSignalBridge bridge;
    QVERIFY(bridge.setup());

    // Act
    ::raise(signum);

    // Assert
    QVERIFY(bridge.drainPending());
    QVERIFY(!bridge.drainPending());

    bridge.teardown();
#else
    QSKIP("Linux-only signal bridge");
#endif
}

void AppShellTest::test_repeated_signals_coalesce_into_one_drain()
{
#ifdef Q_OS_LINUX
    // Arrange
    UnixSignalBridge bridge;
    QVERIFY(bridge.setup());

    // Act: more real signals than drainPending()'s internal read buffer
    // (64 bytes) holds in one read(2) call, all delivered before anything
    // drains -- this makes the drain *loop* itself load-bearing (L-6): a
    // single read of a smaller batch would pass even if the loop were
    // replaced by one non-looping read.
    const int kSignals[] = { SIGTERM, SIGINT, SIGHUP };
    for (int i = 0; i < 200; ++i)
        ::raise(kSignals[i % 3]);

    // Assert: one drain call consumes every byte; a second call finds
    // nothing left, proving the channel was fully drained rather than
    // leaving bytes to trigger a spurious second shutdown.
    QVERIFY(bridge.drainPending());
    QVERIFY(!bridge.drainPending());

    bridge.teardown();
#else
    QSKIP("Linux-only signal bridge");
#endif
}

#ifdef Q_OS_LINUX
namespace {

int g_fakeWriteCalls = 0;
std::function<ssize_t(int, const void*, size_t)> g_fakeWrite;

ssize_t fakeWriteTrampoline(int fd, const void* buf, size_t count)
{
    ++g_fakeWriteCalls;
    return g_fakeWrite(fd, buf, count);
}

// Restores the real write(2) syscall on scope exit, including when a
// QVERIFY/QCOMPARE above it fails and returns from the test slot early --
// otherwise a fake write left installed by one failing test silently
// corrupts every later test's real writes (M-1).
struct WriteSyscallGuard {
    ~WriteSyscallGuard() { UnixSignalBridge::setWriteSyscall_dbg(nullptr); }
};

} // namespace
#endif

void AppShellTest::test_write_retries_only_eintr()
{
#ifdef Q_OS_LINUX
    // Arrange: the first two attempts are interrupted (EINTR), the third
    // succeeds.
    g_fakeWriteCalls = 0;
    g_fakeWrite = [](int, const void*, size_t) -> ssize_t {
        if (g_fakeWriteCalls <= 2) { errno = EINTR; return -1; }
        return 1;
    };
    WriteSyscallGuard writeSyscallGuard;
    UnixSignalBridge::setWriteSyscall_dbg(&fakeWriteTrampoline);

    // Act
    const UnixSignalBridge::WriteOutcome outcome = UnixSignalBridge::writeSignalByte_dbg(-1, 1);

    // Assert
    QCOMPARE(outcome, UnixSignalBridge::WriteOutcome::Written);
    QCOMPARE(g_fakeWriteCalls, 3);
#else
    QSKIP("Linux-only signal bridge");
#endif
}

void AppShellTest::test_write_treats_eagain_as_already_pending()
{
#ifdef Q_OS_LINUX
    // Arrange
    g_fakeWriteCalls = 0;
    g_fakeWrite = [](int, const void*, size_t) -> ssize_t {
        errno = EAGAIN;
        return -1;
    };
    WriteSyscallGuard writeSyscallGuard;
    UnixSignalBridge::setWriteSyscall_dbg(&fakeWriteTrampoline);

    // Act
    const UnixSignalBridge::WriteOutcome outcome = UnixSignalBridge::writeSignalByte_dbg(-1, 1);

    // Assert: EAGAIN means a byte is already queued for the Qt side, so
    // this is a benign outcome, not a retry target.
    QCOMPARE(outcome, UnixSignalBridge::WriteOutcome::AlreadyPending);
    QCOMPARE(g_fakeWriteCalls, 1);
#else
    QSKIP("Linux-only signal bridge");
#endif
}

void AppShellTest::test_write_preserves_errno()
{
#ifdef Q_OS_LINUX
    // Arrange: a distinctive errno set before the call, and a fake write
    // that reports EINTR once, then EAGAIN.
    g_fakeWriteCalls = 0;
    g_fakeWrite = [](int, const void*, size_t) -> ssize_t {
        if (g_fakeWriteCalls == 1) { errno = EINTR; return -1; }
        errno = EAGAIN;
        return -1;
    };
    WriteSyscallGuard writeSyscallGuard;
    UnixSignalBridge::setWriteSyscall_dbg(&fakeWriteTrampoline);
    errno = ENOMEM;

    // Act
    UnixSignalBridge::writeSignalByte_dbg(-1, 1);

    // Assert: errno reflects whatever the caller had before the handler
    // ran, not a value the write attempts left behind.
    QCOMPARE(errno, ENOMEM);
#else
    QSKIP("Linux-only signal bridge");
#endif
}

void AppShellTest::test_full_channel_does_not_block()
{
#ifdef Q_OS_LINUX
    // Arrange: a real, nonblocking socketpair with a tiny send buffer so it
    // fills after a handful of bytes.
    int fds[2] = {-1, -1};
    QVERIFY(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);
    for (int fd : fds) {
        const int flags = fcntl(fd, F_GETFL, 0);
        QVERIFY(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
    }
    const int tinyBuf = 1;
    ::setsockopt(fds[0], SOL_SOCKET, SO_SNDBUF, &tinyBuf, sizeof(tinyBuf));

    // Act: write until the channel reports it is full instead of blocking.
    int written = 0;
    UnixSignalBridge::WriteOutcome outcome = UnixSignalBridge::WriteOutcome::Written;
    for (int i = 0; i < 10000 && outcome == UnixSignalBridge::WriteOutcome::Written; ++i) {
        outcome = UnixSignalBridge::writeSignalByte_dbg(fds[0], 1);
        if (outcome == UnixSignalBridge::WriteOutcome::Written)
            ++written;
    }

    // Assert: the loop above returned (no block), the channel eventually
    // reported AlreadyPending, and at least one byte got through.
    QCOMPARE(outcome, UnixSignalBridge::WriteOutcome::AlreadyPending);
    QVERIFY(written > 0);

    ::close(fds[0]);
    ::close(fds[1]);
#else
    QSKIP("Linux-only signal bridge");
#endif
}

void AppShellTest::test_setup_failure_leaks_no_descriptors()
{
#ifdef Q_OS_LINUX
    // Arrange: lower the descriptor limit to the current usage so the next
    // socketpair() call fails with EMFILE. RlimitGuard restores the
    // original limit on scope exit even if an assertion below fails and
    // returns early -- otherwise a clamped descriptor limit leaks into
    // every later test (M-1).
    struct rlimit originalLimit;
    QVERIFY(getrlimit(RLIMIT_NOFILE, &originalLimit) == 0);
    RlimitGuard rlimitGuard{originalLimit};
    int highestFd = -1;
    for (int fd = 0; fd < 4096; ++fd) {
        if (::fcntl(fd, F_GETFD) != -1)
            highestFd = fd;
    }
    struct rlimit tightLimit = originalLimit;
    tightLimit.rlim_cur = static_cast<rlim_t>(highestFd + 1);
    QVERIFY(setrlimit(RLIMIT_NOFILE, &tightLimit) == 0);

    // Act
    UnixSignalBridge bridge;
    const bool ok = bridge.setup();

    // Assert: setup failed and left no descriptor or notifier fd behind.
    QVERIFY(!ok);
    QCOMPARE(bridge.notifierFd(), -1);
    QCOMPARE(bridge.writeFd_dbg(), -1);

    // Restore the limit and prove the bridge (and a fresh one) can still
    // succeed -- no descriptor was leaked by the failed attempt.
    QVERIFY(setrlimit(RLIMIT_NOFILE, &originalLimit) == 0);
    QVERIFY(bridge.setup());
    bridge.teardown();
#else
    QSKIP("Linux-only signal bridge");
#endif
}

void AppShellTest::test_qt_receipt_invokes_shutdown_exactly_once()
{
#ifdef Q_OS_LINUX
    // Arrange: wire the bridge to a QSocketNotifier the way main.cpp does.
    UnixSignalBridge bridge;
    QVERIFY(bridge.setup());
    QSocketNotifier notifier(bridge.notifierFd(), QSocketNotifier::Read);
    int shutdownCalls = 0;
    QObject::connect(&notifier, &QSocketNotifier::activated, [&](int) {
        if (bridge.drainPending())
            ++shutdownCalls;
    });

    // Act: two signals arrive together; the event loop must observe them
    // as a single readable notification cycle that drains both.
    ::raise(SIGTERM);
    ::raise(SIGINT);
    QVERIFY(pumpUntil([&]() { return shutdownCalls > 0; }, 2000));
    // Give any further (spurious) activations a chance to fire before
    // asserting there were none.
    pumpUntil([]() { return false; }, 100);

    // Assert
    QCOMPARE(shutdownCalls, 1);

    bridge.teardown();
#else
    QSKIP("Linux-only signal bridge");
#endif
}

// ============================================================================
// D5 (R15-002): MainWin::translateWindowsShutdownMessage() is a pure
// function -- unit-testable on Linux even though the Windows nativeEvent()
// path around it is #ifdef-ed out. WM_QUERYENDSESSION (0x0011) always
// decides Query; WM_ENDSESSION (0x0016) decides EndSessionEnding for a
// nonzero wParam and EndSessionCancelled for a zero wParam; any other
// message is Ignore.
// ============================================================================

void AppShellTest::test_D5_translateWindowsShutdownMessage_data()
{
    QTest::addColumn<quint32>("message");
    QTest::addColumn<quint64>("wParam");
    QTest::addColumn<int>("expected"); // WindowsShutdownDecision as int

    QTest::newRow("WM_QUERYENDSESSION, wParam=0")
        << quint32(0x0011) << quint64(0) << static_cast<int>(WindowsShutdownDecision::Query);
    QTest::newRow("WM_QUERYENDSESSION, wParam nonzero (ignored)")
        << quint32(0x0011) << quint64(1) << static_cast<int>(WindowsShutdownDecision::Query);
    QTest::newRow("WM_ENDSESSION, wParam=TRUE (1)")
        << quint32(0x0016) << quint64(1) << static_cast<int>(WindowsShutdownDecision::EndSessionEnding);
    QTest::newRow("WM_ENDSESSION, wParam=ENDSESSION_LOGOFF-style nonzero")
        << quint32(0x0016) << quint64(0x80000000ull) << static_cast<int>(WindowsShutdownDecision::EndSessionEnding);
    QTest::newRow("WM_ENDSESSION, wParam=FALSE (0) -- cancelled")
        << quint32(0x0016) << quint64(0) << static_cast<int>(WindowsShutdownDecision::EndSessionCancelled);
    QTest::newRow("unrelated message (WM_CLOSE)")
        << quint32(0x0010) << quint64(0) << static_cast<int>(WindowsShutdownDecision::Ignore);
}

void AppShellTest::test_D5_translateWindowsShutdownMessage()
{
    QFETCH(quint32, message);
    QFETCH(quint64, wParam);
    QFETCH(int, expected);

    // Act
    const WindowsShutdownDecision result = MainWin::translateWindowsShutdownMessage_dbg(message, wParam);

    // Assert
    QCOMPARE(static_cast<int>(result), expected);
}

// ============================================================================
// D10 (requirement 4): MainWin/ContentWidget wiring consumes the single
// TimeTracker::transitioned() event without double-acting on lock resume.
// ============================================================================

void AppShellTest::test_D10_lock_resume_updates_button_exactly_once()
{
    // Arrange: a real MainWin/ContentWidget wired to a real TimeTracker with
    // autopause enabled, Activity running and then long-locked into Pause.
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = createSettingsFile(tempDir.path(), 7);
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/autopause_enabled", true);
    writer.setValue("uTimer/autopause_threshold_minutes", 15);
    writer.sync();
    Settings settings(settingsPath);
    FakeSessionStore fakeDb;
    TimeTracker tracker(settings, fakeDb);
    FakeClock clock(QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime));
    tracker.setNowProvider_dbg([&clock]() { return clock.now(); });
    ShutdownCoordinator coordinator(tracker, fakeDb);
    MainWin mainWin(settings, tracker, fakeDb, coordinator);

    QPushButton* startPauseButton = mainWin.findChild<QPushButton*>("startPauseButton");
    QVERIFY2(startPauseButton, "ContentWidget's start/pause button must be reachable via objectName");

    tracker.useTimerViaButton(Button::Start);
    QCOMPARE(startPauseButton->text(), QString("PAUSE"));

    tracker.useTimerViaLockEvent(LockEvent::LongOngoingLock, clock.now());
    QVERIFY(tracker.isPaused());
    QCOMPARE(startPauseButton->text(), QString("CONTINUE"));

    // Count the ONE real transition ContentWidget reduces, so "exactly once"
    // is backed by an emission count rather than only an idempotent label
    // (a double emission would still leave the same final text).
    QSignalSpy transitionedSpy(&tracker, &TimeTracker::transitioned);

    // Act: desktop unlocks -- one transitioned(LockResume) event.
    tracker.useTimerViaLockEvent(LockEvent::Unlock);

    // Assert: the button reflects Activity again, from exactly one emission.
    // Before D10, this relied on ContentWidget separately ignoring
    // modeChanged(LockResume) after acting on started(true); now there is
    // structurally only one event to act on.
    QCOMPARE(transitionedSpy.count(), 1);
    QVERIFY(tracker.isActive());
    QCOMPARE(startPauseButton->text(), QString("PAUSE"));

    // Round-2 review (finding 1, M7b): ContentWidget's reducer must also
    // handle Mode::None -- an untested branch would leave the button
    // reading PAUSE forever after any stop, midnight cutoff, watchdog
    // discard, shutdown, or fatal latch.
    tracker.useTimerViaButton(Button::Stop);
    QCOMPARE(startPauseButton->text(), QString("START"));
}
