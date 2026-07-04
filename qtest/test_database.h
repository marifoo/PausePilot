#ifndef TEST_DATABASE_H
#define TEST_DATABASE_H

#include <QObject>
#include "testcommon.h"

class DatabaseTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QString db_backup_path_;
    
    void resetDatabaseFile() const
    {
        if (QFile::exists(db_path_)) {
            QFile::remove(db_path_);
        }
    }

private slots:
    void initTestCase();
    void cleanupTestCase();
    
    // Core database tests
    void test_database_backups_and_retention_and_disable();
    void test_databasemanager_write_failure();
    void test_database_transaction_rollback_on_insert_failure();
    void test_database_transaction_rollback_on_replace_failure();
    void test_database_checkpoint_single_row_per_segment();
    void test_database_checkpoint_missing_segment_reinserts_same_segment_id();
    void test_database_checkpoint_preserves_start_time();
    void test_database_update_by_id_insert_mode();
    void test_database_update_by_id_updates_existing_row_on_start_drift();
    void test_database_update_by_id_different_segments_same_start_both_exist();
    void test_database_update_by_id_empty_deque();
    void test_database_load_negative_duration();
    void test_database_load_start_after_end();
    void test_database_load_invalid_enum_type();
    void test_database_load_duration_mismatch_tolerance();
    void test_database_load_invalid_type_increments_skipped_and_omits_row();
    void test_database_load_200ms_mismatch_increments_repaired_and_uses_computed();
    void test_database_timezone_roundtrip();
    void test_database_millisecond_precision();
    void test_database_schema_validation_fresh_database();
    void test_database_schema_creates_idx_finalized_start();
    void test_database_backup_file_creation();
    void test_database_backup_preserves_contents();
    void test_database_backup_filenames_collision_free();

    // Schema/validation tests
    void test_exactMatching_upsertReplacesById();
    void test_explicitStartTimes_constructorComputesDuration();
    void test_explicitStartTimes_startTimePreservedAfterClean();
    void test_explicitStartTimes_mergeUpdatesAllFields();
    void test_splitPreservesStartTime();
    void test_zeroDurationNotAdded();
    void test_negativeDurationHandled();
    void test_checkpointPreservesStartTimeOnUpdateBySegmentId();
    void test_clockDriftResilience_durationStoredFromElapsed();
    void test_loadDurations_skipsNegativeDurationRows();

    // hasEntriesForDate tri-state result tests (T18)
    void test_hasEntriesForDate_returns_unknown_when_history_disabled();
    void test_hasEntriesForDate_returns_no_on_empty_database();
    void test_hasEntriesForDate_returns_yes_when_entries_exist();
    void test_hasEntriesForDate_utc_positive_offset();
    void test_hasEntriesForDate_utc_negative_offset();
    void test_hasEntriesForDate_utc_regression();

    // Retention cleanup once-per-session tests (T22)
    void test_retention_cleanup_runs_once_across_multiple_opens();
    void test_retention_cleanup_retries_after_failure();

    // Connection name uniqueness (T25)
    void test_connection_names_unique_across_100_instances();

    // Step 6: long-lived connection + WAL guard
    void test_connection_open_after_construction();
    void test_pragma_synchronous_normal_set_after_construction();

    // commitSession and replaceAll contract tests
    void test_T_commitSession_upserts_by_segment_id();
    void test_U_commitSession_orphan_cleanup_is_internal();
    void test_V_replaceAll_wipes_and_rewrites();
    void test_W_backup_created_before_replaceAll_not_commitSession();

    // flushToDisc durable heartbeat (Issue 2)
    void test_flushToDisc_writes_heartbeat_row();
    void test_flushToDisc_idempotent();

    // Step 2 (S1): saveDurations(Append) upserts on segment_id collision
    void test_database_saveDurations_append_upserts_existing_segment_id();

    // Step 3 (S2 + T3): atomic overlap-guarded finalisation.
    void test_finalizeIfNoOverlap_succeeds_when_no_overlap();
    void test_finalizeIfNoOverlap_rejects_when_overlapping_finalized_row_exists();
    void test_finalizeIfNoOverlap_leaves_row_unchanged_on_overlap();
    void test_reconcileUnfinalizedCheckpoints_reports_finalized_and_dropped();
    void test_reconcileUnfinalizedCheckpoints_outright_drops_are_deleted();

    // SchemaStatus enum: validation of existing DB shape
    void test_schema_status_ready_on_existing_valid_db();
    void test_schema_status_outdated_legacy_columns();
    void test_schema_status_outdated_missing_column();
    void test_schema_status_outdated_missing_unique_constraint();
    void test_schema_status_ready_recreates_missing_idx_start_utc();
    void test_schema_status_ready_creates_missing_app_settings_and_idx_segment_id();
    void test_schema_status_inaccessible_readonly_file();

    // Timer::initializeFromStore isolation: no side effects before recovery is called
    void test_constructor_only_does_not_consume_marker();
    void test_outdated_schema_does_not_mutate_db();

    // saveCheckpoint demotion guard: must not overwrite finalized rows
    void test_saveCheckpoint_does_not_demote_finalized_row();

    // Item C: validate schema before any additive DDL
    void test_outdated_schema_no_additive_ddl_on_incompatible_db();

    // Item D: marker must survive reconciliation failure
    void test_recoverStartupCheckpoints_marker_intact_on_reconcile_failure();
    // Finding 2: marker must survive orphan load failure
    void test_recoverStartupCheckpoints_marker_intact_on_load_failure();

    // A11: notification compares marker against the newest finalized orphan
    void test_recoverStartupCheckpoints_notifies_when_marker_covers_only_older_orphan();
    // A12: empty segment_id rows are treated as corrupt at load
    void test_loadDurations_skips_empty_segment_id_row();
};

#endif // TEST_DATABASE_H
