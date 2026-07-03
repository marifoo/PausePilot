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
    watcher.setQueryProvider_dbg([&script]() {
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
    watcher.setQueryProvider_dbg([&script]() {
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
    watcher.setQueryProvider_dbg([&script]() {
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
