/**
 * FakeSessionStore -- spy test double for SessionStore.
 *
 * Records every call made to it (callLog) so tests can assert which
 * operations were performed and in what order.  Stores raw segments passed
 * to commitSession/replaceAll for inspection.  Return values are
 * configurable per-method to simulate failures.
 *
 * Intentionally contains NO normalization, upsert, overlap, or reconcile
 * policy — those behaviors are verified against the real SqliteSessionStore
 * in test_persistence_contract.cpp.
 *
 * Typical usage:
 *     FakeSessionStore fakeDb;
 *     TimeTracker tracker(settings, fakeDb);
 *     // ... exercise tracker ...
 *     QCOMPARE(fakeDb.callLog.count("saveCheckpoint"), 1);
 *     QVERIFY(fakeDb.callLog.contains("commitSession"));
 */

#ifndef FAKESESSIONSTORE_H
#define FAKESESSIONSTORE_H

#include "sessionstore.h"
#include "timeline.h"
#include <QSet>
#include <QStringList>
#include <deque>
#include <vector>

class FakeSessionStore : public SessionStore
{
public:
    FakeSessionStore();
    ~FakeSessionStore() override = default;

    // ---- SessionStore interface ----

    SessionStoreResult commitSession(const Timeline& session) override;
    SessionStoreResult replaceAll(const Timeline& history, const Timeline& session) override;
    LoadResult loadDurations() override;
    EntriesForDateResult hasEntriesForDate(const QDate& date) override;
    SessionStoreResult saveCheckpoint(DurationType type, const QDateTime& startTime,
                                      const QDateTime& endTime, const SegmentId& segmentId) override;
    SessionStoreResult discardCheckpoint(const SegmentId& segmentId) override;
    SchemaStatus checkSchemaOnStartup() override;
    SessionStoreResult flushToDisc() override;
    SessionStoreResult setLastCleanShutdownMarker(const QDateTime& timestamp) override;
    MarkCleanShutdownResult markCleanShutdown() override;
    StartupRecoveryResult recoverStartupCheckpoints(const QDateTime& now) override;

    // ---- Call log (for assertions) ----

    /// Ordered log of method names invoked, e.g. "saveCheckpoint".
    QStringList callLog;

    // ---- In-memory data store ----

    /// All stored durations (populated by saveDurations/updateDurationsById).
    std::deque<TimeDuration> storedDurations;

    /// Last checkpoint saved (for inspection).
    struct CheckpointRecord {
        DurationType type;
        QDateTime startTime;
        QDateTime endTime;
        SegmentId segmentId;
    };
    std::vector<CheckpointRecord> savedCheckpoints;

    // ---- Configurable return values ----

    SessionStoreResult commitSessionResult = SessionStoreResult::success();
    SessionStoreResult replaceDurationsResult = SessionStoreResult::success();
    SessionStoreResult saveCheckpointResult = SessionStoreResult::success();
    /// Result returned by discardCheckpoint(). On anything other than
    /// Success (including Disabled) savedCheckpoints is left untouched --
    /// Disabled and every failure category must be a no-op, not a mutation.
    SessionStoreResult discardCheckpointResult = SessionStoreResult::success();
    SchemaStatus checkSchemaResult = SchemaStatus::Ready;
    SessionStoreResult setMarkerResult = SessionStoreResult::success();
    SessionStoreResult flushToDiscResult = SessionStoreResult::success();

    /// Set of segment_ids seen in commitSession() calls.
    /// Populated on success for cross-path uniqueness checks in saveCheckpoint.
    QSet<QString> committedSegmentIds;

    /// Pre-loaded durations returned by loadDurations(). Tests populate this.
    LoadResult loadDurationsResult;

    /// Result returned by hasEntriesForDate(). Default: No entries.
    EntriesForDateResult entriesForDateResult = EntriesForDateResult::No;

    /// Result returned by recoverStartupCheckpoints().
    /// Default: success with zero recovery and no notification.
    StartupRecoveryResult startupRecoveryResult;
};

#endif // FAKESESSIONSTORE_H
