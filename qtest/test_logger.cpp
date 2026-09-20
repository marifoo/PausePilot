#include "test_logger.h"
#include <QtTest>
#include <QSettings>
#include "logger.h"
#include "settings.h"

void LoggerTest::initTestCase()
{
    QVERIFY(settingsDir_.isValid());
    // D13a: point AppPaths::logFile() (and thus Logger's process-wide
    // singleton, opened lazily on the first enabled Log() call below) at our
    // own temp dir instead of applicationDirPath() -- otherwise this suite
    // leaves a uTimer.log artifact next to the qtest binary.
    TestCommon::setDataDirOverride(settingsDir_.path());

    // settingsOff_: defaults — logToFile is false by default (verified in test_settings)
    settingsOff_ = new Settings(settingsDir_.filePath("off.ini"));
    QCOMPARE(settingsOff_->logToFile(), false);

    // settingsOn_: write debug_log_to_file = true before constructing
    {
        QSettings ini(settingsDir_.filePath("on.ini"), QSettings::IniFormat);
        ini.setValue("uTimer/debug_log_to_file", true);
        ini.sync();
    }
    settingsOn_ = new Settings(settingsDir_.filePath("on.ini"));
    QCOMPARE(settingsOn_->logToFile(), true);

    // Start each run with logging off so earlier test suites that call Logger::Log
    // don't accidentally create the file before Test A.
    Logger::configure(settingsOff_->logToFile());
}

void LoggerTest::cleanupTestCase()
{
    // Leave logging off for the rest of the process (later test suites don't
    // call Logger themselves, but this keeps the process-wide state tidy).
    Logger::configure(false);
    delete settingsOff_;
    delete settingsOn_;
    settingsOff_ = nullptr;
    settingsOn_  = nullptr;
    TestCommon::clearDataDirOverride();
}

// Test A: with logToFile = false, 100 Log() calls must not create or grow the log file.
void LoggerTest::test_a_disabled_logging_is_silent()
{
    Logger::configure(settingsOff_->logToFile());

    const QString logPath = Logger::logFilePath();
    const qint64 sizeBefore = QFile::exists(logPath) ? QFileInfo(logPath).size() : -1;

    for (int i = 0; i < 100; i++) {
        Logger::Log(QString("test_a_should_not_appear_%1").arg(i));
    }

    if (sizeBefore == -1) {
        QVERIFY2(!QFile::exists(logPath), "Log file must not be created when logging is disabled");
    } else {
        QCOMPARE(QFileInfo(logPath).size(), sizeBefore);
    }
}

// Test A1: seed the log file with more than max_lines_ (1024) lines before
// Logger ever opens it, then trigger the first (and only, for the process)
// open. Falsified by corrupting the trim in Logger::Logger()
// (`lines = lines.mid(lines.size() - max_lines_);` in logger.cpp): the
// oldest seeded line would then still be present after rotation.
void LoggerTest::test_a1_rotation_trims_oldest_lines_on_first_open()
{
    const QString logPath = Logger::logFilePath();
    QFile seed(logPath);
    QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text));
    QTextStream seedOut(&seed);
    for (int i = 0; i < 1100; i++) {
        seedOut << QString("seed_line_%1").arg(i) << "\n";
    }
    seedOut.flush();
    seed.close();

    Logger::configure(settingsOn_->logToFile());
    const QString marker = QString("test_a1_marker_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(marker);

    QFile f(logPath);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    const QStringList fileLines = content.split('\n');
    f.close();

    // Exact-line membership, not substring containment: "seed_line_75" is a
    // *prefix* of surviving lines like "seed_line_750", so a plain
    // QString::contains() would pass even if rotation dropped nothing.
    QVERIFY2(!fileLines.contains("seed_line_0"), "Oldest pre-rotation line must be trimmed");
    QVERIFY2(!fileLines.contains("seed_line_75"), "Line beyond the 1024-line window must be trimmed");
    QVERIFY2(fileLines.contains("seed_line_76"), "First line inside the 1024-line window must survive rotation");
    QVERIFY2(fileLines.contains("seed_line_1099"), "Most recent pre-rotation line must survive rotation");
    QVERIFY2(content.contains("uTimer Startup"), "Startup marker must follow rotation");
    QVERIFY2(content.contains(marker), "The triggering Log() call must still be written");
}

// Test B: with logToFile = true, a Log() call must appear in the log file.
void LoggerTest::test_b_enabled_logging_writes()
{
    Logger::configure(settingsOn_->logToFile());

    const QString marker = QString("test_b_marker_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(marker);

    const QString logPath = Logger::logFilePath();
    QVERIFY2(QFile::exists(logPath), "Log file must be created when logging is enabled");

    QFile f(logPath);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QVERIFY2(content.contains(marker),
             qPrintable(QString("Log file must contain marker '%1'").arg(marker)));
}

// Test C: toggling the setting mid-run — messages before the flip must not appear,
// messages after must appear.
void LoggerTest::test_c_toggle_at_runtime_is_honoured()
{
    // Logging off: this message must NOT appear
    Logger::configure(settingsOff_->logToFile());
    const QString beforeMarker = QString("BEFORE_FLIP_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(beforeMarker);

    // Logging on: this message MUST appear
    Logger::configure(settingsOn_->logToFile());
    const QString afterMarker = QString("AFTER_FLIP_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(afterMarker);

    const QString logPath = Logger::logFilePath();
    QFile f(logPath);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QVERIFY2(!content.contains(beforeMarker),
             qPrintable(QString("Pre-flip message '%1' must not appear in log").arg(beforeMarker)));
    QVERIFY2(content.contains(afterMarker),
             qPrintable(QString("Post-flip message '%1' must appear in log").arg(afterMarker)));
}

// Test D: Logger::configure() takes the logToFile value itself, not a
// pointer to the Settings object it came from, so Logger must keep working
// correctly after that Settings object is destroyed. Falsified by reverting
// Logger::configure()/Logger::Log() to store and later dereference a
// `const Settings*` (the pre-D12 design): the read below would then run
// against freed memory instead of the captured bool.
void LoggerTest::test_d_settings_lifetime_survives_settings_destruction()
{
    Settings* transient = new Settings(settingsDir_.filePath("on.ini"));
    QVERIFY(transient->logToFile());
    Logger::configure(transient->logToFile());
    delete transient;

    const QString marker = QString("test_d_marker_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(marker);

    const QString logPath = Logger::logFilePath();
    QFile f(logPath);
    QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString content = QString::fromUtf8(f.readAll());
    f.close();

    QVERIFY2(content.contains(marker),
             qPrintable(QString("Log call after Settings destruction must still be written: '%1'").arg(marker)));
}

// Test E: the process's first write failure must disable file logging
// permanently and quietly. MUST run last in this suite: Logger's file handle
// is a process-wide singleton (a function-local static), so once this test
// disables it there is no way to re-enable it for a later test. Falsified by
// either: removing the `return;` right after reportFailure("write") in
// Logger::log() (the forced-failure marker would then still be written), or
// removing `s_disabled_ = true;` from Logger::reportFailure() (the
// after-failure marker would then be written too).
void LoggerTest::test_e_forced_write_failure_disables_logging_permanently()
{
    Logger::configure(settingsOn_->logToFile());

    const QString logPath = Logger::logFilePath();
    QFile before(logPath);
    QVERIFY(before.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString contentBefore = QString::fromUtf8(before.readAll());
    before.close();

    Logger::forceWriteFailure_dbg(true);
    const QString failedMarker = QString("test_e_failed_write_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(failedMarker);

    const QString afterMarker = QString("test_e_after_disable_%1").arg(QDateTime::currentMSecsSinceEpoch());
    Logger::Log(afterMarker);

    QFile after(logPath);
    QVERIFY(after.open(QIODevice::ReadOnly | QIODevice::Text));
    const QString contentAfter = QString::fromUtf8(after.readAll());
    after.close();

    QVERIFY2(!contentAfter.contains(failedMarker),
             "The message whose write was forced to fail must not appear in the log");
    QVERIFY2(!contentAfter.contains(afterMarker),
             "No message must be written after the first failure -- logging is permanently disabled");
    QCOMPARE(contentAfter.size(), contentBefore.size());
}
