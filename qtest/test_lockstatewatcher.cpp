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
