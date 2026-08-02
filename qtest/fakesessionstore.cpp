/**
 * FakeSessionStore -- spy test double for SessionStore.
 *
 * Records every call made to it (callLog) and stores raw segments
 * passed to commitSession/replaceAll so tests can inspect what was
 * written.  No normalization, upsert, or overlap logic is duplicated
 * here; those behaviors are verified against the real SqliteSessionStore
 * in test_persistence_contract.cpp.
 *
 * Return values come from the configurable *Result members set by the test.
 */

#include "fakesessionstore.h"
#include <algorithm>

FakeSessionStore::FakeSessionStore()
    : startupRecoveryResult{}
{
}

SessionStoreResult FakeSessionStore::commitSession(const Timeline& session)
{
    callLog.append("commitSession");
    if (commitSessionResult.ok()) {
        // Spy: record the segments as submitted, without normalization or upsert policy.
        // Tests that need normalization semantics should use SqliteSessionStore directly.
        for (const auto& d : session.completed()) {
            committedSegmentIds.insert(d.segment_id.toString());
            storedDurations.push_back(d);
        }
    }
    return commitSessionResult;
}

SessionStoreResult FakeSessionStore::replaceAll(const Timeline& history, const Timeline& session)
{
    callLog.append("replaceAll");
    if (replaceDurationsResult.ok() || replaceDurationsResult.category == SessionStoreResult::Disabled) {
        storedDurations.clear();
        committedSegmentIds.clear();
        for (const auto& d : history.completed()) {
            storedDurations.push_back(d);
            committedSegmentIds.insert(d.segment_id.toString());
        }
        for (const auto& d : session.completed()) {
            storedDurations.push_back(d);
            committedSegmentIds.insert(d.segment_id.toString());
        }
    }
    return replaceDurationsResult;
}

LoadResult FakeSessionStore::loadDurations()
{
    callLog.append("loadDurations");
    return loadDurationsResult;
}

EntriesForDateResult FakeSessionStore::hasEntriesForDate(const QDate& /*date*/)
{
    callLog.append("hasEntriesForDate");
    return entriesForDateResult;
}

SessionStoreResult FakeSessionStore::saveCheckpoint(DurationType type, const QDateTime& startTime,
                                                     const QDateTime& endTime, const SegmentId& segmentId)
{
    callLog.append("saveCheckpoint");
    if (segmentId.isEmpty()) {
        return SessionStoreResult::callerBug("empty segment_id");
    }
    if (saveCheckpointResult.ok()) {
        // Check whether this segment_id already has a checkpoint record.
        // Multiple saves for the same segment_id are expected (it is an upsert/update),
        // so we allow them — we only assert that no *different* finalized row already owns this id.
        bool alreadySaved = false;
        for (const auto& cp : savedCheckpoints) {
            if (cp.segmentId == segmentId) {
                alreadySaved = true;
                break;
            }
        }
        // A segment_id that was finalized via commitSession must not also appear as a
        // checkpoint (that would mean TimeTracker submitted the same id to two separate paths).
        Q_ASSERT_X(!committedSegmentIds.contains(segmentId.toString()),
                   "FakeSessionStore::saveCheckpoint",
                   "UNIQUE(segment_id) violation: segment_id already committed via commitSession");
        (void)alreadySaved; // Multiple checkpoint saves for the same id are fine (upsert).
        savedCheckpoints.push_back({type, startTime, endTime, segmentId});
    }
    return saveCheckpointResult;
}

SessionStoreResult FakeSessionStore::discardCheckpoint(const SegmentId& segmentId)
{
    callLog.append("discardCheckpoint");
    if (segmentId.isEmpty()) {
        return SessionStoreResult::callerBug("empty segment_id");
    }
    if (discardCheckpointResult.category != SessionStoreResult::Success) {
        // Disabled or a failure category: a deliberate no-op, not a mutation.
        return discardCheckpointResult;
    }
    // Mirror deleting the unfinalized rows: drop any recorded checkpoints for this id.
    savedCheckpoints.erase(
        std::remove_if(savedCheckpoints.begin(), savedCheckpoints.end(),
                       [&](const CheckpointRecord& cp) { return cp.segmentId == segmentId; }),
        savedCheckpoints.end());
    return discardCheckpointResult;
}

SchemaStatus FakeSessionStore::checkSchemaOnStartup()
{
    callLog.append("checkSchemaOnStartup");
    return checkSchemaResult;
}

SessionStoreResult FakeSessionStore::flushToDisc()
{
    callLog.append("flushToDisc");
    return flushToDiscResult;
}

SessionStoreResult FakeSessionStore::setLastCleanShutdownMarker(const QDateTime& /*timestamp*/)
{
    callLog.append("setLastCleanShutdownMarker");
    return setMarkerResult;
}

StartupRecoveryResult FakeSessionStore::recoverStartupCheckpoints(const QDateTime& /*now*/)
{
    callLog.append("recoverStartupCheckpoints");
    return startupRecoveryResult;
}
