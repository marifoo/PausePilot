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

namespace {
// C4 (R05-003): the fake deliberately does not duplicate the concrete
// store's day-validity/overlap SQL policy, but a duplicate segment_id is an
// IDENTITY contract fact, not a policy decision -- both implementations must
// agree that it is CallerBug, not a silently-accepted spy call.
bool hasDuplicateSegmentId(const std::deque<TimeDuration>& rows)
{
    QSet<QString> seen;
    for (const auto& d : rows) {
        const QString id = d.segment_id.toString();
        if (seen.contains(id))
            return true;
        seen.insert(id);
    }
    return false;
}
}

FakeSessionStore::FakeSessionStore()
    : startupRecoveryResult{}
{
}

SessionStoreResult FakeSessionStore::commitSession(const Timeline& session)
{
    callLog.append("commitSession");
    if (hasDuplicateSegmentId(session.completed())) {
        return SessionStoreResult::callerBug("commitSession: duplicate segment_id in payload");
    }
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
    // Uniqueness is checked across the COMBINED payload (C4/R05-003): a
    // history row and a session row sharing an id is also a CallerBug, not
    // just a duplicate within one bucket.
    std::deque<TimeDuration> combined = history.completed();
    combined.insert(combined.end(), session.completed().begin(), session.completed().end());
    if (hasDuplicateSegmentId(combined)) {
        return SessionStoreResult::callerBug("replaceAll: duplicate segment_id in payload");
    }
    // Disabled is a contractual no-op, exactly like every other failure
    // category -- only Success may mutate fake state (R05-005/R02-003).
    if (replaceDurationsResult.ok()) {
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
        // checkpoint (that would mean TimeTracker submitted the same id to two separate
        // paths). C4 fix round 3 (F7): match SqliteSessionStore::saveCheckpoint, which
        // classifies this as CallerBug (see test_saveCheckpoint_callerBug_finalized_
        // segmentId in test_contract_matrix.cpp) rather than aborting the process --
        // hasDuplicateSegmentId() exists precisely so both implementations agree on
        // this identity fact.
        if (committedSegmentIds.contains(segmentId.toString())) {
            return SessionStoreResult::callerBug(
                "segment_id already committed via commitSession");
        }
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

MarkCleanShutdownResult FakeSessionStore::markCleanShutdown()
{
    // D6/M1: marker write, then -- only if it succeeded -- the durability
    // barrier. Mirrors SqliteSessionStore::markCleanShutdown() so tests can
    // inject marker/flush failures independently via setMarkerResult and
    // flushToDiscResult and observe both call order, short-circuiting, and
    // which step (markerWritten) a failure is attributed to.
    const SessionStoreResult markerResult = setLastCleanShutdownMarker(QDateTime());
    if (!markerResult.ok()) {
        return {markerResult, false};
    }
    return {flushToDisc(), true};
}

StartupRecoveryResult FakeSessionStore::recoverStartupCheckpoints(const QDateTime& /*now*/)
{
    callLog.append("recoverStartupCheckpoints");
    return startupRecoveryResult;
}
