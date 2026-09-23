#include "test_lockstatewatcher.h"
#include "fakeclock.h"
#include <QtTest>
#include <vector>

void LockStateWatcherTest::test_lockstatewatcher_debounce_logic()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // feed majority lock pattern
    std::deque<LockQueryResult> pattern = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    LockEvent event = LockEvent::None;
    for (LockQueryResult r : pattern) {
        event = watcher.determineLockEvent_dbg(r);
    }
    QCOMPARE(event, LockEvent::Lock);

    // feed majority unlock pattern
    pattern = {
        LockQueryResult::Locked,   LockQueryResult::Locked,
        LockQueryResult::Unlocked, LockQueryResult::Unlocked, LockQueryResult::Unlocked };
    for (LockQueryResult r : pattern) {
        event = watcher.determineLockEvent_dbg(r);
    }
    QCOMPARE(event, LockEvent::Unlock);
}

// ItemE: shouldEmitLongLock — Unknown never matures regardless of elapsed time.
void LockStateWatcherTest::test_shouldEmitLongLock_unknown_does_not_mature()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // Arrange: elapsed well past the backpause threshold (15 min default = 900000 ms).
    const qint64 pastThreshold = settings.getBackpauseMsec() + 1000;

    // Act + Assert: Unknown must never trigger long-lock maturation.
    QVERIFY(!watcher.shouldEmitLongLock_dbg(LockQueryResult::Unknown, pastThreshold));
}

// ItemE: shouldEmitLongLock — Locked with elapsed >= backpause DOES mature.
void LockStateWatcherTest::test_shouldEmitLongLock_locked_matures()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // Arrange: elapsed exactly at threshold.
    const qint64 atThreshold = settings.getBackpauseMsec();

    // Act + Assert: Locked result at or past threshold triggers long-lock.
    QVERIFY(watcher.shouldEmitLongLock_dbg(LockQueryResult::Locked, atThreshold));
}

// ItemE: shouldEmitLongLock — Locked with elapsed < backpause does NOT mature.
void LockStateWatcherTest::test_shouldEmitLongLock_locked_short_elapsed_does_not_mature()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // Arrange: elapsed one millisecond short of the threshold.
    const qint64 belowThreshold = settings.getBackpauseMsec() - 1;

    // Act + Assert: not enough time elapsed — no long-lock emission.
    QVERIFY(!watcher.shouldEmitLongLock_dbg(LockQueryResult::Locked, belowThreshold));
}

// 9.5: Unknown (query failure) must not modify the debounce buffer.
// Scenario: buffer is in a partially-locked state (3 of 5 = Locked).
// Feeding Unknown should not trigger a false Unlock transition.
void LockStateWatcherTest::test_lockstatewatcher_unknown_does_not_unlock()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // Prime the buffer toward the Lock pattern:
    // [Unlocked, Unlocked, Locked, Locked, Locked] → triggers Lock
    std::deque<LockQueryResult> primePattern = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    for (LockQueryResult r : primePattern) {
        watcher.determineLockEvent_dbg(r);
    }
    // Buffer is now the lock pattern — next Unknown must not change it.

    // Feeding Unknown should leave the buffer unchanged (no Unlock event).
    LockEvent event = watcher.determineLockEvent_dbg(LockQueryResult::Unknown);
    QCOMPARE(event, LockEvent::None);

    // Feeding another Unknown also leaves buffer unchanged.
    event = watcher.determineLockEvent_dbg(LockQueryResult::Unknown);
    QCOMPARE(event, LockEvent::None);

    // A real Unlock sequence must still work after the Unknown samples.
    LockEvent finalEvent = LockEvent::None;
    std::deque<LockQueryResult> unlockPattern = {
        LockQueryResult::Locked,   LockQueryResult::Locked,
        LockQueryResult::Unlocked, LockQueryResult::Unlocked, LockQueryResult::Unlocked };
    for (LockQueryResult r : unlockPattern) {
        finalEvent = watcher.determineLockEvent_dbg(r);
    }
    QCOMPARE(finalEvent, LockEvent::Unlock);
}

// Phase 2 clock/query seam: drive update() through the stubbed lock-state probe
// (no DBus/WTS) and prove the injected now-provider records the wall-clock lock
// instant. A scripted [U,U,L,L,L] sequence must produce exactly one Lock event.
void LockStateWatcherTest::test_seam_query_provider_drives_update_and_records_lock_time()
{
    // Arrange: watcher with an injected clock and a scripted query probe.
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    const QDateTime lockInstant(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(lockInstant);
    watcher.setNowProvider_dbg([&clock]() { return clock.now(); });

    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    watcher.setQueryProvider_dbg([&script](const QDateTime &) {
        LockQueryResult r = script.front();
        script.pop_front();
        return r;
    });

    // Capture emitted events without QSignalSpy (LockEvent has no metatype).
    std::vector<LockEvent> events;
    QObject::connect(&watcher, &LockStateWatcher::desktopLockEvent,
                     [&events](LockEvent e) { events.push_back(e); });

    // Act: run the 100 ms poll five times over the scripted samples.
    for (int i = 0; i < 5; ++i)
        watcher.update();

    // Assert: exactly one Lock event, and lock_time_ captured from the fake clock.
    QCOMPARE(events.size(), static_cast<size_t>(1));
    QCOMPARE(events[0], LockEvent::Lock);
    QCOMPARE(watcher.lockTime_dbg(), lockInstant);
}

// A2: the long-lock maturation must be driven by the wall-clock lock instant
// (lock_time_), not the monotonic lock_timer_ which freezes during a suspend.
// Lock at 09:00, then the wall clock jumps 20 min forward (simulated suspend +
// resume) while no real monotonic time elapses; the next Locked probe must
// mature the lock and emit exactly one LongOngoingLock.
void LockStateWatcherTest::test_A2_longlock_uses_wallclock_across_suspend()
{
    // Arrange: watcher with an injected clock and a scripted query probe.
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    const QDateTime lockInstant(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(lockInstant);
    watcher.setNowProvider_dbg([&clock]() { return clock.now(); });

    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked,
        LockQueryResult::Locked };
    watcher.setQueryProvider_dbg([&script](const QDateTime &) {
        LockQueryResult r = script.front();
        script.pop_front();
        return r;
    });

    std::vector<LockEvent> events;
    QObject::connect(&watcher, &LockStateWatcher::desktopLockEvent,
                     [&events](LockEvent e) { events.push_back(e); });

    // Act 1: establish the lock at 09:00.
    for (int i = 0; i < 5; ++i)
        watcher.update();
    QCOMPARE(events.size(), static_cast<size_t>(1));
    QCOMPARE(events[0], LockEvent::Lock);

    // Act 2: 20 min of wall time passes (well past the 15 min default backpause)
    // via a suspend that the monotonic timer does not observe. One more probe.
    clock.advance(20ll * 60ll * 1000ll);
    watcher.update();

    // Assert: the lock matured off the wall clock.
    QCOMPARE(events.size(), static_cast<size_t>(2));
    QCOMPARE(events[1], LockEvent::LongOngoingLock);
}

// H3: lock_time_ is stamped from the first Locked sample of a run, not the
// accepting (third) sample. A suspend between the first sample and the flip
// does not move the lock instant to the resume time.
void LockStateWatcherTest::test_H3_lock_anchor_survives_suspend_during_debounce()
{
    // Arrange: watcher with an injected clock and a scripted query probe.
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    watcher.setNowProvider_dbg([&clock]() { return clock.now(); });

    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    watcher.setQueryProvider_dbg([&script](const QDateTime &) {
        LockQueryResult r = script.front();
        script.pop_front();
        return r;
    });

    // Capture both signal arguments: event and lockTime.
    std::vector<std::pair<LockEvent, QDateTime>> events;
    QObject::connect(&watcher, &LockStateWatcher::desktopLockEvent,
                     [&events](LockEvent e, const QDateTime &lt) { events.push_back({e, lt}); });

    // Act 1: first update at t0 (U).
    watcher.update();

    // Act 2: advance 100 ms and update (U).
    clock.advance(100);
    watcher.update();

    // Act 3: advance 100 ms and update (first L at t0+200).
    clock.advance(100);
    watcher.update();

    // Act 4: suspend—advance by backpause + 60 seconds, update (would be 3rd L, triggers flip).
    clock.advance(settings.getBackpauseMsec() + 60000);
    watcher.update();

    // Act 5: advance 100 ms and update (flip after suspend).
    clock.advance(100);
    watcher.update();

    // Assert: two events (Lock and LongOngoingLock), both stamped at t0+200 (the first Locked sample).
    QCOMPARE(events.size(), static_cast<size_t>(2));
    QCOMPARE(events[0].first, LockEvent::Lock);
    QCOMPARE(events[0].second, t0.addMSecs(200));
    QCOMPARE(events[1].first, LockEvent::LongOngoingLock);
    QCOMPARE(events[1].second, t0.addMSecs(200));
}

// H3: lock_time_ is stamped from the first Locked sample, not the accepting
// (third) sample, even in the healthy case with no suspend.
void LockStateWatcherTest::test_H3_lock_time_is_first_locked_sample_not_third()
{
    // Arrange: watcher with an injected clock and a scripted query probe.
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    watcher.setNowProvider_dbg([&clock]() { return clock.now(); });

    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    watcher.setQueryProvider_dbg([&script](const QDateTime &) {
        LockQueryResult r = script.front();
        script.pop_front();
        return r;
    });

    // Capture both signal arguments.
    std::vector<std::pair<LockEvent, QDateTime>> events;
    QObject::connect(&watcher, &LockStateWatcher::desktopLockEvent,
                     [&events](LockEvent e, const QDateTime &lt) { events.push_back({e, lt}); });

    // Act: five updates with 100 ms advances between them (U, U, L, L, L).
    for (int i = 0; i < 5; ++i) {
        if (i > 0)
            clock.advance(100);
        watcher.update();
    }

    // Assert: one Lock event, recorded at t0+200 (first Locked sample), not t0+400 (third sample).
    QCOMPARE(events.size(), static_cast<size_t>(1));
    QCOMPARE(events[0].first, LockEvent::Lock);
    QCOMPARE(events[0].second, t0.addMSecs(200));
    QCOMPARE(watcher.lockTime_dbg(), t0.addMSecs(200));
}

// H3: Unknown samples do not move the lock anchor; the anchor is stamped
// from the first Locked sample before the Unknown samples arrive.
void LockStateWatcherTest::test_H3_unknown_samples_do_not_move_the_lock_anchor()
{
    // Arrange: watcher with an injected clock and a scripted query probe.
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    watcher.setNowProvider_dbg([&clock]() { return clock.now(); });

    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Unknown, LockQueryResult::Unknown,
        LockQueryResult::Locked,   LockQueryResult::Locked };
    watcher.setQueryProvider_dbg([&script](const QDateTime &) {
        LockQueryResult r = script.front();
        script.pop_front();
        return r;
    });

    // Capture both signal arguments.
    std::vector<std::pair<LockEvent, QDateTime>> events;
    QObject::connect(&watcher, &LockStateWatcher::desktopLockEvent,
                     [&events](LockEvent e, const QDateTime &lt) { events.push_back({e, lt}); });

    // Act: seven updates with 100 ms advances between them.
    for (int i = 0; i < 7; ++i) {
        if (i > 0)
            clock.advance(100);
        watcher.update();
    }

    // Assert: one Lock event, recorded at t0+200 (the first Locked sample),
    // proving Unknown samples do not move the anchor forward.
    QCOMPARE(events.size(), static_cast<size_t>(1));
    QCOMPARE(events[0].first, LockEvent::Lock);
    QCOMPARE(events[0].second, t0.addMSecs(200));
}

// A5: a single flaky Unlocked sample in the middle of an otherwise-Locked
// transition must NOT permanently prevent Lock. Exact-pattern matching missed
// this; hysteresis on the last-3 debounced level recovers.
void LockStateWatcherTest::test_A5_noisy_transition_emits_exactly_one_lock()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // Noisy lock transition: one stray Unlocked between the Locked samples.
    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };

    int lockCount = 0;
    for (LockQueryResult r : script)
        if (watcher.determineLockEvent_dbg(r) == LockEvent::Lock)
            ++lockCount;

    QCOMPARE(lockCount, 1);
}

// A5: emitted Lock/Unlock events must strictly alternate — repeated Locked (or
// Unlocked) runs must not re-emit the same level.
void LockStateWatcherTest::test_A5_lock_unlock_strictly_alternate()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    // Both the unlock and the second lock transition carry a stray sample; a
    // correct debounce must still alternate Lock → Unlock → Lock.
    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked, // Lock
        LockQueryResult::Unlocked, LockQueryResult::Locked,                          // noise
        LockQueryResult::Unlocked, LockQueryResult::Unlocked, LockQueryResult::Unlocked, // Unlock
        LockQueryResult::Locked,   LockQueryResult::Unlocked,                        // noise
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked }; // Lock

    std::vector<LockEvent> events;
    for (LockQueryResult r : script) {
        LockEvent e = watcher.determineLockEvent_dbg(r);
        if (e != LockEvent::None)
            events.push_back(e);
    }

    QCOMPARE(events.size(), static_cast<size_t>(3));
    QCOMPARE(events[0], LockEvent::Lock);
    QCOMPARE(events[1], LockEvent::Unlock);
    QCOMPARE(events[2], LockEvent::Lock);
}

// A5: LongOngoingLock fires at most once per lock session even when update() is
// polled repeatedly while the session stays locked past the threshold.
void LockStateWatcherTest::test_A5_longlock_fires_at_most_once_per_lock_session()
{
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);

    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);
    watcher.setNowProvider_dbg([&clock]() { return clock.now(); });

    // Noisy lock-establishing samples, then stay Locked indefinitely.
    std::deque<LockQueryResult> script = {
        LockQueryResult::Unlocked, LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Unlocked,
        LockQueryResult::Locked,   LockQueryResult::Locked, LockQueryResult::Locked };
    const int establishPolls = static_cast<int>(script.size());
    watcher.setQueryProvider_dbg([&script](const QDateTime &) {
        if (!script.empty()) { LockQueryResult r = script.front(); script.pop_front(); return r; }
        return LockQueryResult::Locked;
    });

    std::vector<LockEvent> events;
    QObject::connect(&watcher, &LockStateWatcher::desktopLockEvent,
                     [&events](LockEvent e) { events.push_back(e); });

    // Establish the lock at 09:00 despite the noisy sample.
    for (int i = 0; i < establishPolls; ++i)
        watcher.update();

    // Past the threshold, poll many times while still locked.
    clock.advance(20ll * 60ll * 1000ll);
    for (int i = 0; i < 5; ++i)
        watcher.update();

    int longLockCount = 0;
    for (LockEvent e : events)
        if (e == LockEvent::LongOngoingLock)
            ++longLockCount;
    QCOMPARE(longLockCount, 1);
}

// D3/R12-002: translateWtsSessionState() is a pure function -- unit-testable
// on Linux even though the Windows WTS query path around it is #ifdef-ed out.
// Only exact WTS_SESSIONSTATE_LOCK (0) / WTS_SESSIONSTATE_UNLOCK (1) at
// Level 1 map to Locked/Unlocked; the documented WTS_SESSIONSTATE_UNKNOWN
// (0xFFFFFFFF), any other flags value, and any non-1 level (a malformed
// response) all remain Unknown.
void LockStateWatcherTest::test_D3_translateWtsSessionState_data()
{
    QTest::addColumn<quint32>("level");
    QTest::addColumn<quint32>("sessionFlags");
    QTest::addColumn<int>("expected"); // LockQueryResult as int for QTest::addColumn simplicity

    QTest::newRow("Level1 Lock")            << quint32(1) << quint32(0)          << static_cast<int>(LockQueryResult::Locked);
    QTest::newRow("Level1 Unlock")          << quint32(1) << quint32(1)          << static_cast<int>(LockQueryResult::Unlocked);
    QTest::newRow("Level1 WTS Unknown")     << quint32(1) << quint32(0xFFFFFFFFu) << static_cast<int>(LockQueryResult::Unknown);
    QTest::newRow("Level1 other value")     << quint32(1) << quint32(42)         << static_cast<int>(LockQueryResult::Unknown);
    QTest::newRow("malformed: wrong level") << quint32(2) << quint32(0)          << static_cast<int>(LockQueryResult::Unknown);
}

void LockStateWatcherTest::test_D3_translateWtsSessionState()
{
    QFETCH(quint32, level);
    QFETCH(quint32, sessionFlags);
    QFETCH(int, expected);

    // Act
    const LockQueryResult result = LockStateWatcher::translateWtsSessionState_dbg(level, sessionFlags);

    // Assert
    QCOMPARE(static_cast<int>(result), expected);
}

#ifdef Q_OS_LINUX
namespace {
LockStateWatcher::LinuxLockBackend countingBackend(const QString &name, int *calls, LockQueryResult result)
{
    return LockStateWatcher::LinuxLockBackend{ name, [calls, result]() { ++(*calls); return result; } };
}
} // namespace
#endif

// D3/R12-001: the preferred (first) backend fails its real query; the chain
// must fall over to the next working backend in the same operation rather
// than getting stuck Unknown.
void LockStateWatcherTest::test_D3_linux_preferred_backend_failure_falls_back()
{
#ifdef Q_OS_LINUX
    // Arrange
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);
    int callsA = 0, callsB = 0;
    watcher.setLinuxBackends_dbg({
        countingBackend("A", &callsA, LockQueryResult::Unknown),
        countingBackend("B", &callsB, LockQueryResult::Locked),
    });
    const QDateTime now(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);

    // Act
    const LockQueryResult result = watcher.queryLinuxBackends_dbg(now);

    // Assert
    QCOMPARE(result, LockQueryResult::Locked);
    QVERIFY(watcher.hasActiveLinuxBackend_dbg());
    QCOMPARE(callsA, 1);
    QCOMPARE(callsB, 1);
#else
    QSKIP("Linux-only backend chain");
#endif
}

// D3/R12-001: after an all-backends failure episode backs off, the same
// backend recovering on the next reprobe is selected again. Recovering via a
// rescan is not by itself proof of stability, so backoff is NOT cleared by
// that success (round-3 review M2) -- it only clears once the now-active
// backend goes on to answer directly, without needing another scan.
void LockStateWatcherTest::test_D3_linux_recovers_once_preferred_backend_works_again()
{
#ifdef Q_OS_LINUX
    // Arrange
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);
    int calls = 0;
    bool healthy = false;
    watcher.setLinuxBackends_dbg({
        { "A", [&calls, &healthy]() { ++calls; return healthy ? LockQueryResult::Unlocked : LockQueryResult::Unknown; } },
    });
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);

    // Act 1: first probe fails -> all backends failed, backoff scheduled.
    LockQueryResult r1 = watcher.queryLinuxBackends_dbg(clock.now());
    QCOMPARE(r1, LockQueryResult::Unknown);
    QVERIFY(!watcher.hasActiveLinuxBackend_dbg());
    const int backoffAfterFailure = watcher.linuxBackoffMs_dbg();
    QVERIFY(backoffAfterFailure > 0);

    // Act 2: backend becomes healthy and the backoff window elapses ->
    // recovered via a rescan.
    healthy = true;
    clock.advance(backoffAfterFailure);
    const LockQueryResult r2 = watcher.queryLinuxBackends_dbg(clock.now());

    // Assert: recovered and selected, but backoff is NOT cleared by a
    // rescan-path success -- only a later direct-active success clears it.
    QCOMPARE(r2, LockQueryResult::Unlocked);
    QVERIFY(watcher.hasActiveLinuxBackend_dbg());
    QVERIFY(watcher.linuxBackoffMs_dbg() > 0);
    QCOMPARE(calls, 2);

    // Act 3: the now-active backend answers directly (no rescan needed) ->
    // this is what actually clears backoff.
    clock.advance(100);
    const LockQueryResult r3 = watcher.queryLinuxBackends_dbg(clock.now());

    // Assert
    QCOMPARE(r3, LockQueryResult::Unlocked);
    QCOMPARE(watcher.linuxBackoffMs_dbg(), 0);
    QCOMPARE(calls, 3);
#else
    QSKIP("Linux-only backend chain");
#endif
}

// D3/R12-001 round-3 review M1: this is R12-001's exact trigger -- a backend
// already selected and working for a while later fails its real per-session
// query, while a different backend is available and working. Failover must
// happen immediately (no backoff wait) because a healthy, directly-answering
// active backend keeps backoff at 0, and this test proves the mutant that
// simply returns Unknown instead of falling through to a rescan is caught.
// callsA/callsB are pinned exactly (round-4 review F2), derived from the
// policy: Act1's cold scan finds A first (callsA=1, B untouched); Act2's
// direct success is a second A call (callsA=2); Act3's failing direct query
// is a third A call (callsA=3) whose fallback scan excludes A (L1) and
// queries only B once (callsB=1) -- proving the just-failed backend is not
// re-queried within the same tick's scan.
void LockStateWatcherTest::test_D3_linux_active_backend_failure_falls_back_to_working_backend()
{
#ifdef Q_OS_LINUX
    // Arrange: A answers healthily at first (selected as preferred); B also
    // works, but is never touched while A stays healthy (index-0 preference).
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);
    bool aHealthy = true;
    int callsA = 0, callsB = 0;
    watcher.setLinuxBackends_dbg({
        { "A", [&callsA, &aHealthy]() { ++callsA; return aHealthy ? LockQueryResult::Locked : LockQueryResult::Unknown; } },
        countingBackend("B", &callsB, LockQueryResult::Unlocked),
    });
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);

    // Act 1: cold scan selects A, the preferred (first) backend.
    const LockQueryResult r1 = watcher.queryLinuxBackends_dbg(clock.now());
    QCOMPARE(r1, LockQueryResult::Locked);
    QVERIFY(watcher.hasActiveLinuxBackend_dbg());
    QCOMPARE(callsB, 0); // A alone answered; B untouched while A is healthy

    // Act 2: A keeps answering directly for a while -- this is the real
    // steady state, and it clears any transient backoff from the cold scan.
    clock.advance(100);
    watcher.queryLinuxBackends_dbg(clock.now());
    QCOMPARE(watcher.linuxBackoffMs_dbg(), 0);

    // Act 3: A's real per-session query now fails while B is available and
    // working (R12-001's exact trigger). Because backoff is clear, failover
    // must happen in this same call, not be deferred behind a backoff wait.
    aHealthy = false;
    clock.advance(100);
    const LockQueryResult r3 = watcher.queryLinuxBackends_dbg(clock.now());

    // Assert: failed over to B without waiting for a backoff window, and the
    // just-failed A is not re-queried inside this same tick's fallback scan.
    QCOMPARE(r3, LockQueryResult::Unlocked);
    QVERIFY(watcher.hasActiveLinuxBackend_dbg());
    QCOMPARE(callsA, 3);
    QCOMPARE(callsB, 1);
#else
    QSKIP("Linux-only backend chain");
#endif
}

// D3/R12-001 round-3 review M2: a permanently-broken preferred backend (A)
// combined with a flapping fallback (B, alternating success/failure on every
// real query) must not degenerate into a full synchronous chain scan on
// every 100 ms heartbeat tick. The backoff window rate-limits the scan
// itself -- not just the "every backend still down" outcome -- so B's
// transient rescan-path successes cannot keep resetting backoff to 0 and
// defeating it. The pre-fix code measured callsA=50, callsB=99, backoff=0
// under this exact scenario.
//
// Counts re-derived from the policy after round-4 review F1 (a successful
// scan now restarts the backoff ladder at its floor, 1000ms, instead of
// keeping the grown value): each cycle is a 1000ms period once the ladder
// settles at the floor -- t=0 (cold scan, growth 0->1000): A fails, B
// succeeds (selected). t=100: active B queried directly, fails (its
// alternation), scan blocked (still inside the [t=0, t=1000) window). t=200..900:
// blocked, no calls. t=1000 (window elapsed, growth 1000->2000 before the
// scan, then reset to 1000 on B's success): A fails, B succeeds -> next
// window is [t=1000, t=2000). This 1000ms cycle (2 backend calls: one A,
// one B during the scan; one B during the failed direct re-check) repeats
// every 1000ms. Over 5000ms (50 ticks of 100ms) that is 5 cycles ->
// callsA=5 (one per cycle's scan), callsB=10 (one scan success + one failed
// direct re-check per cycle).
void LockStateWatcherTest::test_D3_linux_flapping_fallback_does_not_defeat_backoff()
{
#ifdef Q_OS_LINUX
    // Arrange
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);
    int callsA = 0, callsB = 0;
    bool bHealthy = true; // flips on every real query -- B "flaps"
    watcher.setLinuxBackends_dbg({
        countingBackend("A", &callsA, LockQueryResult::Unknown), // permanently broken
        { "B", [&callsB, &bHealthy]() {
            ++callsB;
            const LockQueryResult r = bHealthy ? LockQueryResult::Locked : LockQueryResult::Unknown;
            bHealthy = !bHealthy;
            return r; } },
    });
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);

    // Act: 50 ticks at the real 100 ms heartbeat cadence (5 seconds total).
    for (int i = 0; i < 50; ++i) {
        watcher.queryLinuxBackends_dbg(clock.now());
        clock.advance(100);
    }

    // Assert: exact, deterministic call counts under this fully deterministic
    // schedule -- far below the ~50/~99 the pre-fix code produced (a ~10x
    // reduction), and the backoff ladder is at its floor (recovered, not
    // pinned at the accumulated maximum) rather than 0 (not fully idle,
    // since B is still flapping).
    QCOMPARE(callsA, 5);
    QCOMPARE(callsB, 10);
    QCOMPARE(watcher.linuxBackoffMs_dbg(), 1000);
#else
    QSKIP("Linux-only backend chain");
#endif
}

// D3/R12-001: when every backend fails, Unknown is retained -- never
// synthesized as Unlock -- and no backend is left selected.
void LockStateWatcherTest::test_D3_linux_all_backends_fail_retains_unknown()
{
#ifdef Q_OS_LINUX
    // Arrange
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);
    int callsA = 0, callsB = 0;
    watcher.setLinuxBackends_dbg({
        countingBackend("A", &callsA, LockQueryResult::Unknown),
        countingBackend("B", &callsB, LockQueryResult::Unknown),
    });
    const QDateTime now(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);

    // Act
    const LockQueryResult result = watcher.queryLinuxBackends_dbg(now);

    // Assert
    QCOMPARE(result, LockQueryResult::Unknown);
    QVERIFY(!watcher.hasActiveLinuxBackend_dbg());
    QCOMPARE(callsA, 1);
    QCOMPARE(callsB, 1);
#else
    QSKIP("Linux-only backend chain");
#endif
}

// D3/R12-001: while backing off after an all-fail episode, further calls
// within the backoff window must not touch the (fake) DBus backends at all;
// only once the window elapses does a new full-chain reprobe run.
void LockStateWatcherTest::test_D3_linux_backoff_skips_dbus_until_window_elapses()
{
#ifdef Q_OS_LINUX
    // Arrange
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);
    int calls = 0;
    watcher.setLinuxBackends_dbg({ countingBackend("A", &calls, LockQueryResult::Unknown) });
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);

    // Act 1: first probe fails and schedules backoff.
    watcher.queryLinuxBackends_dbg(clock.now());
    QCOMPARE(calls, 1);
    const int backoff = watcher.linuxBackoffMs_dbg();
    QVERIFY(backoff > 0);

    // Act 2: calls before the window elapses must not invoke the backend.
    clock.advance(backoff - 1);
    watcher.queryLinuxBackends_dbg(clock.now());
    watcher.queryLinuxBackends_dbg(clock.now());

    // Assert: still exactly one call -- no synchronous per-tick reprobing.
    QCOMPARE(calls, 1);

    // Act 3: once the window elapses, the reprobe runs again.
    clock.advance(1);
    watcher.queryLinuxBackends_dbg(clock.now());

    // Assert
    QCOMPARE(calls, 2);
#else
    QSKIP("Linux-only backend chain");
#endif
}

// D3/R12-001: repeated all-fail episodes at most double the backoff each
// time (never decreasing), and it is bounded by a cap -- exercised by
// running enough episodes (1000 -> ... -> 30000, 5 doublings) that the cap
// must actually be hit, which is what the final QCOMPARE verifies.
void LockStateWatcherTest::test_D3_linux_backoff_grows_and_is_bounded()
{
#ifdef Q_OS_LINUX
    // Arrange
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);
    int calls = 0;
    watcher.setLinuxBackends_dbg({ countingBackend("A", &calls, LockQueryResult::Unknown) });
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);

    // Act + Assert: each all-fail episode is non-decreasing and at most
    // doubles the backoff.
    int previousBackoff = 0;
    for (int i = 0; i < 8; ++i) {
        clock.advance(watcher.linuxBackoffMs_dbg()); // jump straight to the next reprobe
        watcher.queryLinuxBackends_dbg(clock.now());
        const int backoff = watcher.linuxBackoffMs_dbg();
        QVERIFY(backoff > 0);
        QVERIFY(backoff >= previousBackoff);
        if (previousBackoff > 0)
            QVERIFY(backoff <= previousBackoff * 2); // at most doubling; less once capped
        previousBackoff = backoff;
    }
    // Bounded: the cap was reached well before 8 doublings from 1000ms.
    QCOMPARE(previousBackoff, 30000);
#else
    QSKIP("Linux-only backend chain");
#endif
}

// D3/R12-001: while all backends stay down, polling at the normal 100 ms
// heartbeat cadence must not query the backends on every tick -- the total
// call count across many ticks stays small, proving the GUI thread is not
// blocked by repeated synchronous DBus attempts.
void LockStateWatcherTest::test_D3_linux_backoff_does_not_block_heartbeat_with_repeated_calls()
{
#ifdef Q_OS_LINUX
    // Arrange
    Settings settings(TestCommon::createSettingsFile(QDir::tempPath(), 7));
    LockStateWatcher watcher(settings);
    int calls = 0;
    watcher.setLinuxBackends_dbg({ countingBackend("A", &calls, LockQueryResult::Unknown) });
    const QDateTime t0(QDate(2025, 6, 1), QTime(9, 0, 0), Qt::LocalTime);
    FakeClock clock(t0);

    // Act: 50 ticks at the real 100 ms heartbeat cadence (5 seconds total).
    for (int i = 0; i < 50; ++i) {
        watcher.queryLinuxBackends_dbg(clock.now());
        clock.advance(100);
    }

    // Assert: exact, deterministic call count under this schedule (scans at
    // t=0, 1000, 3000 ms; every other tick is skipped by backoff) -- far
    // fewer than the 50 a per-tick synchronous probe would produce.
    QCOMPARE(calls, 3);
#else
    QSKIP("Linux-only backend chain");
#endif
}
