#ifndef TEST_PERSISTENCE_CONTRACT_H
#define TEST_PERSISTENCE_CONTRACT_H

#include <QObject>
#include "testcommon.h"

/**
 * PersistenceContractTest -- verifies normalize/upsert/uniqueness/reconcile
 * behaviors against the REAL SqliteSessionStore (no spy or duplicate logic).
 *
 * These tests were extracted from FakeSessionStore when it was simplified to a
 * pure spy.  They confirm that SqliteSessionStore itself satisfies the persistence
 * contracts that tests previously relied on the fake to mirror.
 */
class PersistenceContractTest : public QObject
{
    Q_OBJECT

private:
    QString db_path_;
    QTemporaryDir data_dir_;

    void resetDatabaseFile() const {
        if (QFile::exists(db_path_))
            QFile::remove(db_path_);
    }

private slots:
    void initTestCase();
    void cleanupTestCase();

    // commitSession normalizes adjacent same-type segments before writing.
    void test_commitSession_normalizes_adjacent_same_type();

    // commitSession upserts (updates) a segment that already exists in the DB.
    void test_commitSession_upserts_existing_segment_id();

    // commitSession removes a segment_id from the DB when normalization merges it away.
    void test_commitSession_removes_merged_away_segment_id();
};

#endif // TEST_PERSISTENCE_CONTRACT_H
