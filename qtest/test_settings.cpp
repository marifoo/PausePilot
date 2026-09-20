#include "test_settings.h"
#include "timeformat.h"
#include <QtTest>
#include <QFile>

// 9.4: unknown keys must survive a Settings constructor round-trip.
void SettingsTest::test_settings_unknown_key_survives_roundtrip()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = QDir(tempDir.path()).filePath("settings.ini");

    // Seed an unknown key into the file before Settings reads it.
    {
        QSettings seed(settingsPath, QSettings::IniFormat);
        seed.setValue("uTimer/history_days_to_keep", 30);
        seed.setValue("uTimer/unknown_future_key", "preserve_me");
        seed.sync();
    }

    // Constructing Settings must not drop the unknown key.
    Settings s(settingsPath);
    QCOMPARE(s.syncStatus(), QSettings::NoError);

    // Re-read the file directly and verify the unknown key survived.
    QSettings verify(settingsPath, QSettings::IniFormat);
    QVERIFY2(verify.contains("uTimer/unknown_future_key"),
             "Unknown key must survive a Settings load/save round-trip");
    QCOMPARE(verify.value("uTimer/unknown_future_key").toString(), QString("preserve_me"));
}

// 9.4: a sync failure must be visible via syncStatus().
void SettingsTest::test_settings_sync_failure_reported()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    const QString settingsPath = QDir(tempDir.path()).filePath("readonly.ini");

    // Create the file and make it read-only so sync() cannot write.
    {
        QFile f(settingsPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();
    }
    QFile::setPermissions(settingsPath, QFile::ReadOwner | QFile::ReadUser);

    // Skip this test if we are root (root ignores read-only permissions).
    if (QFile(settingsPath).open(QIODevice::WriteOnly)) {
        QSKIP("Running as root — read-only file permission test not meaningful");
    }

    Settings s(settingsPath);
    QVERIFY2(s.syncStatus() != QSettings::NoError,
             "syncStatus() must be non-zero when the file cannot be written");

    // Restore write permission for temp dir cleanup.
    QFile::setPermissions(settingsPath,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
}

void SettingsTest::test_settings_defaults_and_bounds()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

    // Write nothing: should load defaults
    Settings defaults(settingsPath);
    QCOMPARE(defaults.getHistoryDays(), 99);
    QCOMPARE(defaults.isAutopauseEnabled(), true);
    QCOMPARE(defaults.logToFile(), false);
    QCOMPARE(defaults.getBackpauseMsec(), convMinToMsec(15));
    QCOMPARE(defaults.getCheckpointIntervalMsec(), convMinToMsec(5));

    // Negative history clamps to 0
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/history_days_to_keep", -5);
    writer.sync();
    Settings clamped(settingsPath);
    QCOMPARE(clamped.getHistoryDays(), 0);

    // Bounds for checkpoint interval [0,60]
    writer.setValue("uTimer/checkpoint_interval_minutes", 120);
    writer.sync();
    Settings capped(settingsPath);
    QCOMPARE(capped.getCheckpointIntervalMsec(), convMinToMsec(60));
}

// A14: boot_time_sec_ is the only numeric key without a qBound; a garbage/huge
// value must be capped rather than silently voiding the boot credit.
void SettingsTest::test_settings_boot_time_sec_bound()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

    // Arrange: seed a garbage (way-too-large) boot_time_seconds value.
    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/boot_time_seconds", 999999u);
    writer.sync();

    // Act: red before the fix — getBootTimeSec() returned 999999 unbounded.
    Settings capped(settingsPath);

    // Assert: value is capped at 3600 seconds (1 hour).
    QCOMPARE(capped.getBootTimeSec(), 3600u);
}

// D12-1: a malformed (non-numeric) int key must fall back to the documented
// default (5 min), not the 0 that QSettings::toInt() silently produces on
// failure (which would then wrongly clamp to 0 via qBound).
void SettingsTest::test_settings_malformed_int_uses_default()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/checkpoint_interval_minutes", "notanumber");
    writer.sync();

    Settings s(settingsPath);
    QCOMPARE(s.getCheckpointIntervalMsec(), convMinToMsec(5));
}

// D12-1: QVariant::toBool() has no ok-overload and treats any non-empty
// string that isn't "0"/"false" as true, so an unrecognized value would
// silently flip a default-false setting on. Must fall back to the default
// instead.
void SettingsTest::test_settings_malformed_bool_uses_default()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

    QSettings writer(settingsPath, QSettings::IniFormat);
    writer.setValue("uTimer/debug_log_to_file", "banana");
    writer.sync();

    Settings s(settingsPath);
    QCOMPARE(s.logToFile(), false);
}

// D12-3: on a successful write, setAutopauseState() reports NoError and the
// new value is visible via isAutopauseEnabled(). Falsified by corrupting
// settings.cpp's post-writeAndSync() success branch (i.e. leaving
// autopause_enabled_ reverted, or returning the pre-write status).
void SettingsTest::test_settings_setAutopauseState_success_updates_value()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

    Settings s(settingsPath);
    const bool prior = s.isAutopauseEnabled();
    QCOMPARE(s.setAutopauseState(!prior), QSettings::NoError);
    QCOMPARE(s.isAutopauseEnabled(), !prior);
}

// D12-3: a write failure must be reported and must NOT change the in-memory
// value. Falsified by corrupting settings.cpp's Settings::setAutopauseState()
// revert-on-failure line (autopause_enabled_ = prev;) -- without it the
// setter would keep the new value even though the file write failed.
void SettingsTest::test_settings_setAutopauseState_write_failure_retains_prior_value()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

    // Construct successfully first (file is writable), then make the file
    // read-only. The permission check happens at open(), so this must
    // happen AFTER construction's writeAndSync() and BEFORE the mutator's
    // own writeAndSync() reopens the file -- an fd opened before chmod
    // would stay writable and the test would not reproduce the failure.
    Settings s(settingsPath);
    const bool prior = s.isAutopauseEnabled();
    QFile::setPermissions(settingsPath, QFile::ReadOwner | QFile::ReadUser);

    // Skip this test if we are root (root ignores read-only permissions).
    if (QFile(settingsPath).open(QIODevice::WriteOnly)) {
        QFile::setPermissions(settingsPath,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
        QSKIP("Running as root — read-only file permission test not meaningful");
    }

    const QSettings::Status status = s.setAutopauseState(!prior);
    QVERIFY2(status != QSettings::NoError, "setAutopauseState() must report the write failure");
    QCOMPARE(s.isAutopauseEnabled(), prior);

    // Restore write permission for temp dir cleanup.
    QFile::setPermissions(settingsPath,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
}

// D12-3: same guarantee for setPinToTopState() -- falsified by corrupting
// settings.cpp's Settings::setPinToTopState() revert-on-failure line
// (start_pinned_to_top_ = prev;).
void SettingsTest::test_settings_setPinToTopState_write_failure_retains_prior_value()
{
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());
    QString settingsPath = QDir(tempDir.path()).filePath("user-settings.ini");

    Settings s(settingsPath);
    const bool prior = s.isPinnedStartEnabled();
    QFile::setPermissions(settingsPath, QFile::ReadOwner | QFile::ReadUser);

    if (QFile(settingsPath).open(QIODevice::WriteOnly)) {
        QFile::setPermissions(settingsPath,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
        QSKIP("Running as root — read-only file permission test not meaningful");
    }

    const QSettings::Status status = s.setPinToTopState(!prior);
    QVERIFY2(status != QSettings::NoError, "setPinToTopState() must report the write failure");
    QCOMPARE(s.isPinnedStartEnabled(), prior);

    QFile::setPermissions(settingsPath,
        QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);
}
